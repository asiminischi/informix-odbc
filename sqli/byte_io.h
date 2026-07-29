#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ifxsqli {

// Thrown by ByteReader on truncated/malformed input. Caught at the boundary
// of each public sqli operation and turned into an Error.
struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Appends fields in the big-endian, length-prefixed-string style the SQLI
// wire protocol uses throughout.
class ByteWriter {
public:
    void u8(std::uint8_t v) { buf_.push_back(std::byte{v}); }

    void u16(std::uint16_t v) {
        std::uint16_t be = std::endian::native == std::endian::big ? v : std::byteswap(v);
        append(&be, sizeof(be));
    }

    void u32(std::uint32_t v) {
        std::uint32_t be = std::endian::native == std::endian::big ? v : std::byteswap(v);
        append(&be, sizeof(be));
    }

    void bytes(std::span<const std::byte> b) {
        buf_.insert(buf_.end(), b.begin(), b.end());
    }

    void raw(std::string_view s) {
        for (char c : s) buf_.push_back(std::byte(c));
    }

    // A fixed-width field padded with NUL bytes to `width` (used for the
    // handshake's constant-width tags like "sqlexec\0\0\0\0\0").
    void fixed(std::string_view s, std::size_t width) {
        raw(s);
        for (std::size_t i = s.size(); i < width; ++i) u8(0);
    }

    // u16 length-prefix (string length + 1 for the NUL) + bytes + explicit NUL.
    // This is the recurring "length-prefixed string" shape used for
    // usernames, server/database names, version strings, env var names, etc.
    void lenPrefixedString(std::string_view s) {
        u16(static_cast<std::uint16_t>(s.size() + 1));
        raw(s);
        u8(0);
    }

    // Same shape but with a length of 0 and no bytes at all, used when a
    // field (e.g. password, database name) is absent.
    void emptyLenPrefixed() { u16(0); }

    // The statement protocol's "char" field shape (distinct from the
    // handshake blob's lenPrefixedString above): a length prefix that does
    // NOT include a NUL, the raw bytes, then a single zero pad byte if and
    // only if the string length is odd (2-byte wire alignment - confirmed
    // against a live capture, see docs/protocol-notes.md). ifxChar2 uses a
    // u16 length (e.g. SQ_COMMAND text, database/cursor names); ifxChar4
    // uses a u32 length (e.g. SQ_PREPARE's SQL text).
    void ifxChar2(std::string_view s) {
        u16(static_cast<std::uint16_t>(s.size()));
        raw(s);
        if (s.size() & 1) u8(0);
    }

    void ifxChar4(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        raw(s);
        if (s.size() & 1) u8(0);
    }

    [[nodiscard]] std::span<const std::byte> view() const { return buf_; }
    [[nodiscard]] std::size_t size() const { return buf_.size(); }

private:
    void append(const void* p, std::size_t n) {
        auto b = static_cast<const std::byte*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }

    std::vector<std::byte> buf_;
};

// Reads fields from a fixed buffer, in the same shapes ByteWriter produces.
// Throws ProtocolError on underflow.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::uint8_t u8() {
        require(1);
        return std::to_integer<std::uint8_t>(data_[pos_++]);
    }

    [[nodiscard]] std::uint16_t u16() {
        require(2);
        std::uint16_t be;
        std::memcpy(&be, data_.data() + pos_, 2);
        pos_ += 2;
        return std::endian::native == std::endian::big ? be : std::byteswap(be);
    }

    [[nodiscard]] std::uint32_t u32() {
        require(4);
        std::uint32_t be;
        std::memcpy(&be, data_.data() + pos_, 4);
        pos_ += 4;
        return std::endian::native == std::endian::big ? be : std::byteswap(be);
    }

    [[nodiscard]] std::span<const std::byte> bytes(std::size_t n) {
        require(n);
        auto out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }

    void skip(std::size_t n) {
        require(n);
        pos_ += n;
    }

    // Reads a NUL-terminated string of exactly `n` bytes total (as produced
    // by lenPrefixedString: the length prefix already includes the NUL).
    [[nodiscard]] std::string cstring(std::size_t n) {
        if (n == 0) return {};
        auto b = bytes(n);
        std::size_t len = (n > 0 && std::to_integer<unsigned char>(b[n - 1]) == 0) ? n - 1 : n;
        std::string s(len, '\0');
        std::memcpy(s.data(), b.data(), len);
        return s;
    }

    // Reads a u16 length prefix then that many bytes as a NUL-terminated
    // string (the common "length-prefixed string" field shape).
    [[nodiscard]] std::string lenPrefixedString() {
        std::uint16_t n = u16();
        return cstring(n);
    }

    // Statement-protocol "char" field: u16 length (no NUL), that many raw
    // bytes, then a pad byte skipped if the length was odd. Mirrors
    // ByteWriter::ifxChar2 - see its comment for the confirmed padding rule.
    [[nodiscard]] std::string ifxChar2() {
        std::uint16_t n = u16();
        auto b = bytes(n);
        std::string s(n, '\0');
        if (n > 0) std::memcpy(s.data(), b.data(), n);
        if (n & 1) skip(1);
        return s;
    }

    [[nodiscard]] std::size_t remaining() const { return data_.size() - pos_; }
    [[nodiscard]] std::size_t position() const { return pos_; }

private:
    void require(std::size_t n) const {
        if (pos_ + n > data_.size()) {
            throw ProtocolError("truncated SQLI message");
        }
    }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

// Decodes the legacy "long int" 10-byte format used by SQ_INSERTDONE's
// serial8Inserted field (confirmed live - see docs/protocol-notes.md): a
// u16 sign (0 = no value/sentinel, 1 = positive, -1 = negative) followed by
// two 4-byte halves stored in an unusual order (bytes [6..9] then [2..5]
// relative to the 10-byte buffer's start) that get reassembled high-half-
// first into a plain 64-bit value, then negated if the sign was -1. Distinct
// from the plain 8-byte big-endian BIGINT column encoding - this format is
// only used for this one legacy field.
[[nodiscard]] inline std::optional<std::int64_t> decodeLegacyLongInt(std::span<const std::byte> b) {
    auto byte = [&](std::size_t i) { return std::to_integer<std::uint64_t>(b[i]); };
    std::int16_t sign = static_cast<std::int16_t>((byte(0) << 8) | byte(1));
    if (sign == 0) return std::nullopt;
    std::uint64_t v = byte(6);
    v = (v << 8) | byte(7);
    v = (v << 8) | byte(8);
    v = (v << 8) | byte(9);
    v = (v << 8) | byte(2);
    v = (v << 8) | byte(3);
    v = (v << 8) | byte(4);
    v = (v << 8) | byte(5);
    auto result = static_cast<std::int64_t>(v);
    return sign < 0 ? -result : result;
}

} // namespace ifxsqli
