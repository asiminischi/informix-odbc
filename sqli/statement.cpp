#include "sqli/statement.h"

#include "sqli/byte_io.h"
#include "sqli/protocol.h"
#include "sqli/types.h"

#include <atomic>
#include <cstdio>

namespace ifxsqli {

namespace {

std::expected<std::uint16_t, Error> readU16(TcpSocket& sock) {
    auto b = sock.readExactN(2);
    if (!b) return std::unexpected(b.error());
    ByteReader r(*b);
    return r.u16();
}

std::expected<std::uint32_t, Error> readU32(TcpSocket& sock) {
    auto b = sock.readExactN(4);
    if (!b) return std::unexpected(b.error());
    ByteReader r(*b);
    return r.u32();
}

// Statement-protocol "char" field: u16 length (no NUL) + bytes + a pad byte
// consumed (and discarded) iff the length is odd. See ByteWriter::ifxChar2.
std::expected<std::string, Error> readIfxChar2(TcpSocket& sock) {
    auto lenR = readU16(sock);
    if (!lenR) return std::unexpected(lenR.error());
    std::uint16_t n = *lenR;
    std::string s;
    if (n > 0) {
        auto data = sock.readExactN(n);
        if (!data) return std::unexpected(data.error());
        s.assign(reinterpret_cast<const char*>(data->data()), n);
    }
    if (n & 1) {
        auto pad = sock.readExactN(1);
        if (!pad) return std::unexpected(pad.error());
    }
    return s;
}

// Splits one fetched row's raw byte blob into per-column buffers. Only the
// scalar types needed to prove the Phase 2 round trip are handled; anything
// else fails cleanly rather than silently misaligning - full type coverage
// is Phase 3.
std::expected<std::vector<std::vector<std::byte>>, Error> splitRow(std::span<const std::byte> row,
                                                                     const std::vector<ColumnDesc>& columns) {
    std::vector<std::vector<std::byte>> result;
    result.reserve(columns.size());
    std::size_t offset = 0;
    for (const auto& col : columns) {
        std::size_t width = 0;
        switch (col.ifxType) {
            case ifxtype::kChar:               // CHAR(n): fixed width
                width = col.encodedLength;
                break;
            case ifxtype::kSmallInt:
                width = 2;
                break;
            case ifxtype::kInteger:
            case ifxtype::kSerial:
            case ifxtype::kDate:
                width = 4;
                break;
            case ifxtype::kBigInt:
            case ifxtype::kBigSerial:
                width = 8;
                break;
            case ifxtype::kVarchar:
            case ifxtype::kNVarchar: {
                if (offset >= row.size()) {
                    return std::unexpected(Error{ErrorKind::Protocol, "row truncated reading varchar length byte"});
                }
                width = 1 + static_cast<std::size_t>(std::to_integer<unsigned char>(row[offset]));
                break;
            }
            case ifxtype::kDecimal: {  // 1 exponent byte + ceil(precision/2) mantissa bytes
                unsigned precision = (col.encodedLength >> 8) & 0xFF;
                width = 1 + (precision + 1) / 2;
                break;
            }
            case ifxtype::kDatetime: {  // 1 exponent byte + ceil(totalDigits/2) mantissa bytes
                unsigned totalDigits = (col.encodedLength >> 8) & 0xFF;
                width = 1 + (totalDigits + 1) / 2;
                break;
            }
            default:
                return std::unexpected(
                    Error{ErrorKind::Protocol,
                          "column type " + std::to_string(col.ifxType) + " not yet supported (Phase 3)"});
        }
        if (offset + width > row.size()) {
            return std::unexpected(Error{ErrorKind::Protocol, "row data shorter than column layout implies"});
        }
        result.emplace_back(row.begin() + static_cast<std::ptrdiff_t>(offset),
                             row.begin() + static_cast<std::ptrdiff_t>(offset + width));
        offset += width;
    }
    return result;
}

} // namespace

std::expected<SqliStatement, Error> SqliStatement::prepare(SqliConnection& conn, const std::string& sql) {
    SqliStatement stmt(conn);
    auto& sock = conn.transport();

    ByteWriter w;
    w.u16(opcode::kPrepare);
    // numqmarks - Phase 2 MVP has no bind parameters. Width (u16 vs u32)
    // follows the same negotiated narrow/wide toggle as everywhere else
    // (confirmed against the golden capture's SQ_PREPARE request).
    if (conn.has4ByteOffsets()) {
        w.u32(0);
    } else {
        w.u16(0);
    }
    w.ifxChar2(sql);
    w.u16(opcode::kNonCursorDescribe);
    w.u16(opcode::kWantDone);
    w.u16(opcode::kEot);
    if (auto r = sock.writeAll(w.view()); !r) return std::unexpected(r.error());

    auto batch = stmt.processResponseBatch();
    if (!batch) return std::unexpected(batch.error());

    return stmt;
}

std::expected<void, Error> SqliStatement::execute() {
    static std::atomic<std::uint64_t> counter{1};
    std::uint64_t n = counter.fetch_add(1);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "_odbc%013llu", static_cast<unsigned long long>(n));
    cursorName_ = buf;

    auto& sock = conn_->transport();
    ByteWriter w;
    w.u16(opcode::kId);
    w.u16(statementId_);
    w.u16(opcode::kCursorName);
    w.ifxChar2(cursorName_);
    w.u16(opcode::kOpen);
    w.u16(opcode::kEot);
    if (auto r = sock.writeAll(w.view()); !r) return std::unexpected(r.error());

    auto batch = processResponseBatch();
    if (!batch) return std::unexpected(batch.error());

    cursorOpen_ = true;
    typeSent_ = false;
    exhausted_ = false;
    pendingRows_.clear();
    return {};
}

std::expected<void, Error> SqliStatement::fetchNextBatch() {
    auto& sock = conn_->transport();
    ByteWriter w;
    w.u16(opcode::kId);
    w.u16(statementId_);
    if (!typeSent_) {
        // Tells the server how to render each column for this fetch
        // direction; required at least once whenever variable-length
        // columns are present (confirmed live - see docs/protocol-notes.md).
        w.u16(opcode::kReturnType);
        w.u16(1); // direction: 1 = fetch
        w.u16(static_cast<std::uint16_t>(columns_.size()));
        for (const auto& col : columns_) {
            w.u16(static_cast<std::uint16_t>(col.ifxType));
            w.u32(col.encodedLength);
        }
        typeSent_ = true;
    }
    w.u16(opcode::kFetch);
    // Tuple buffer size: u32 only when the server negotiated 2GB-fetch-buffer
    // support; otherwise u16 (see decompiled IfxSqli.sendFetch). Confirmed
    // against the golden capture: wide connections use u32 here and also
    // include a trailing "Fetch Array Size" u16(0) field afterward
    // (decompiled isFetchArr()); narrow connections use u16 and omit it.
    if (conn_->has4ByteOffsets()) {
        w.u32(4096);
        w.u16(0);
    } else {
        w.u16(4096);
    }
    w.u16(opcode::kEot);
    if (auto r = sock.writeAll(w.view()); !r) return std::unexpected(r.error());

    auto sawTuple = processResponseBatch();
    if (!sawTuple) return std::unexpected(sawTuple.error());
    if (!*sawTuple) exhausted_ = true;
    return {};
}

std::expected<std::optional<std::vector<std::vector<std::byte>>>, Error> SqliStatement::fetchRow() {
    if (pendingRows_.empty()) {
        if (exhausted_) return std::optional<std::vector<std::vector<std::byte>>>{};
        if (auto r = fetchNextBatch(); !r) return std::unexpected(r.error());
        if (pendingRows_.empty()) return std::optional<std::vector<std::vector<std::byte>>>{};
    }
    auto row = std::move(pendingRows_.front());
    pendingRows_.erase(pendingRows_.begin());
    return std::optional(std::move(row));
}

std::expected<void, Error> SqliStatement::close() {
    auto& sock = conn_->transport();
    ByteWriter w;
    w.u16(opcode::kId);
    w.u16(statementId_);
    w.u16(opcode::kClose);
    w.u16(opcode::kEot);
    if (auto r = sock.writeAll(w.view()); !r) return std::unexpected(r.error());

    auto batch = processResponseBatch();
    if (!batch) return std::unexpected(batch.error());
    cursorOpen_ = false;
    return {};
}

std::expected<void, Error> SqliStatement::release() {
    auto& sock = conn_->transport();
    ByteWriter w;
    w.u16(opcode::kId);
    w.u16(statementId_);
    w.u16(opcode::kRelease);
    w.u16(opcode::kEot);
    if (auto r = sock.writeAll(w.view()); !r) return std::unexpected(r.error());

    auto batch = processResponseBatch();
    if (!batch) return std::unexpected(batch.error());
    return {};
}

// Reads one batched server response: a stream of tagged records terminated
// by SQ_EOT. Handles the opcodes this driver currently produces (DESCRIBE,
// DONE, COST, TUPLE, ERROR) generically so every send-then-receive call site
// above can share it. Returns whether any TUPLE was seen, which fetchRow()
// uses to detect cursor exhaustion.
std::expected<bool, Error> SqliStatement::processResponseBatch() {
    auto& sock = conn_->transport();
    const bool wide = conn_->has4ByteOffsets();
    std::optional<Error> pendingError;
    bool sawTuple = false;

    for (;;) {
        auto tag = readU16(sock);
        if (!tag) return std::unexpected(tag.error());
        if (*tag == opcode::kEot) break;

        switch (*tag) {
            case opcode::kDescribe: {
                auto stype = readU16(sock);
                if (!stype) return std::unexpected(stype.error());
                statementType_ = *stype;

                auto sid = readU16(sock);
                if (!sid) return std::unexpected(sid.error());
                statementId_ = *sid;

                if (auto estCost = readU32(sock); !estCost) return std::unexpected(estCost.error()); // unused
                if (auto tupleSize = readU16(sock); !tupleSize) return std::unexpected(tupleSize.error()); // unused

                auto nfields = readU16(sock);
                if (!nfields) return std::unexpected(nfields.error());

                std::uint32_t stringTableSize;
                if (wide) {
                    auto v = readU32(sock);
                    if (!v) return std::unexpected(v.error());
                    stringTableSize = *v;
                } else {
                    auto v = readU16(sock);
                    if (!v) return std::unexpected(v.error());
                    stringTableSize = *v;
                }

                columns_.clear();
                columns_.resize(*nfields);
                for (std::uint16_t i = 0; i < *nfields; ++i) {
                    if (wide) {
                        if (!readU32(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (field index)"});
                    } else {
                        if (!readU16(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (field index)"});
                    }
                    if (wide) {
                        if (!readU32(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (column start)"});
                    } else {
                        if (!readU16(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (column start)"});
                    }

                    auto colType = readU16(sock);
                    if (!colType) return std::unexpected(colType.error());
                    columns_[i].ifxType = *colType & 0xFF;
                    columns_[i].notNull = (*colType & 0x100) != 0;

                    if (wide) {
                        if (!readU32(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (extended id)"});
                        if (auto owner = readIfxChar2(sock); !owner) return std::unexpected(owner.error());
                        if (auto extName = readIfxChar2(sock); !extName) return std::unexpected(extName.error());
                        if (!readU16(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (reference)"});
                        if (!readU16(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (alignment)"});
                        if (!readU32(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (source type)"});

                        auto encLen = readU32(sock);
                        if (!encLen) return std::unexpected(encLen.error());
                        columns_[i].encodedLength = *encLen;
                    } else {
                        auto encLen = readU16(sock);
                        if (!encLen) return std::unexpected(encLen.error());
                        columns_[i].encodedLength = *encLen;
                    }
                }

                if (stringTableSize > 0) {
                    auto strtab = sock.readExactN(stringTableSize);
                    if (!strtab) return std::unexpected(strtab.error());
                    if (stringTableSize & 1) {
                        auto pad = sock.readExactN(1);
                        if (!pad) return std::unexpected(pad.error());
                    }
                    std::size_t start = 0;
                    for (std::uint16_t i = 0; i < *nfields && start < strtab->size(); ++i) {
                        std::size_t end = start;
                        while (end < strtab->size() && (*strtab)[end] != std::byte{0}) ++end;
                        columns_[i].name.assign(reinterpret_cast<const char*>(strtab->data() + start), end - start);
                        start = end + 1;
                    }
                }
                break;
            }
            case opcode::kDone: {
                if (!readU16(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (warnings)"});
                if (!readU32(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (row count)"});
                if (!readU32(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (rowid)"});
                if (!readU32(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (serial id)"});
                break;
            }
            case opcode::kCost: {
                auto b = sock.readExactN(8);
                if (!b) return std::unexpected(b.error());
                break;
            }
            case opcode::kTuple: {
                if (!readU16(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (tuple warning)"});
                std::uint32_t size;
                if (wide) {
                    auto v = readU32(sock);
                    if (!v) return std::unexpected(v.error());
                    size = *v;
                } else {
                    auto v = readU16(sock);
                    if (!v) return std::unexpected(v.error());
                    size = *v;
                }
                if (size > 0) {
                    auto raw = sock.readExactN(size);
                    if (!raw) return std::unexpected(raw.error());
                    if (size & 1) {
                        auto pad = sock.readExactN(1);
                        if (!pad) return std::unexpected(pad.error());
                    }
                    auto row = splitRow(*raw, columns_);
                    if (!row) return std::unexpected(row.error());
                    pendingRows_.push_back(std::move(*row));
                    sawTuple = true;
                }
                break;
            }
            case opcode::kInsertDone: {
                // See SqliConnection::executeCommand's handling of the same
                // tag - confirmed live 18-byte layout (10-byte legacy "long
                // int" serial8 + plain 8-byte big-endian bigserial).
                auto serial8 = sock.readExactN(10);
                if (!serial8) return std::unexpected(serial8.error());
                lastSerialInserted_ = decodeLegacyLongInt(*serial8);

                auto bigSerial = sock.readExactN(8);
                if (!bigSerial) return std::unexpected(bigSerial.error());
                ByteReader r(*bigSerial);
                lastBigSerialInserted_ =
                    static_cast<std::int64_t>((static_cast<std::uint64_t>(r.u32()) << 32) | r.u32());
                break;
            }
            case opcode::kError: {
                auto sqlcodeRaw = readU16(sock);
                if (!sqlcodeRaw) return std::unexpected(sqlcodeRaw.error());
                auto sqlcode = static_cast<std::int16_t>(*sqlcodeRaw);

                if (sqlcode == -619) {
                    // Resync marker (decompiled receiveError's special case).
                    // Consume its trailing fields and keep draining to EOT.
                    if (!readU16(sock) || !readU16(sock) || !readU16(sock)) {
                        return std::unexpected(Error{ErrorKind::Transport, "read failed (resync fields)"});
                    }
                    if (auto msg = readIfxChar2(sock); !msg) return std::unexpected(msg.error());
                    pendingError = Error{ErrorKind::Server, "server requested resync (sqlcode -619)"};
                    break;
                }

                auto isamcode = readU16(sock);
                if (!isamcode) return std::unexpected(isamcode.error());
                auto offset = readU32(sock);
                if (!offset) return std::unexpected(offset.error());

                std::string message;
                if (sqlcode != -368) {
                    auto msg = readIfxChar2(sock);
                    if (!msg) return std::unexpected(msg.error());
                    message = *msg;
                }
                pendingError = Error{ErrorKind::Server, "sqlcode " + std::to_string(sqlcode) + ": " + message};
                break;
            }
            default:
                return std::unexpected(
                    Error{ErrorKind::Protocol, "unrecognized statement-protocol tag " + std::to_string(*tag)});
        }
    }

    if (pendingError) return std::unexpected(*pendingError);
    return sawTuple;
}

} // namespace ifxsqli
