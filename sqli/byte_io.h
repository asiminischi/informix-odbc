#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

} // namespace ifxsqli
