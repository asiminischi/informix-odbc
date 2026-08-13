#include "sqli/statement.h"

#include "sqli/byte_io.h"
#include "sqli/protocol.h"
#include "sqli/types.h"

#include <algorithm>
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
            // Simple LOB (BYTE/TEXT): the TUPLE row carries a fixed-size
            // opaque descriptor here, not the value itself - same
            // fixed-width shape as kChar for splitRow's purposes (DESCRIBE
            // reports encodedLength=56, confirmed live, including for a
            // NULL row - the descriptor doesn't shrink), just interpreted
            // completely differently downstream (see
            // SqliConnection::fetchSimpleLob, sqli/types.h's
            // kSimpleLobDescriptorSize doc comment).
            case ifxtype::kByte:
            case ifxtype::kText:
                width = col.encodedLength;
                break;
            case ifxtype::kBoolean: {
                // Self-describing, not fixed-width despite encodedLength=1 -
                // see decodeBoolean's doc comment for the confirmed shape.
                if (offset >= row.size()) {
                    return std::unexpected(Error{ErrorKind::Protocol, "row truncated reading boolean null flag"});
                }
                width = (row[offset] == std::byte{1}) ? 5 : 6;
                break;
            }
            case ifxtype::kSmallInt:
                width = 2;
                break;
            case ifxtype::kInteger:
            case ifxtype::kSerial:
            case ifxtype::kDate:
            case ifxtype::kSmallFloat:
                width = 4;
                break;
            case ifxtype::kBigInt:
            case ifxtype::kBigSerial:
            case ifxtype::kFloat:
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
            case ifxtype::kInterval: {
                // 1 exponent byte + mantissa bytes, counted per field (see
                // decodeIntervalDayFraction's doc comment): the leading
                // field and FRACTION each round up their own width
                // independently, so a single ceil(totalDigits/2) can
                // undercount by 1 when both the leading field and
                // FRACTION are independently odd-width (their oddness
                // could cancel out in the combined totalDigits parity).
                unsigned totalDigits = (col.encodedLength >> 8) & 0xFF;
                unsigned qualifierByte = col.encodedLength & 0xFF;
                unsigned start = (qualifierByte >> 4) & 0xF;
                unsigned end = qualifierByte & 0xF;
                unsigned wholeEnd = std::min(end, 10u);
                unsigned fractionDigits = end > 10 ? end - 10 : 0;
                unsigned leadingWidth = totalDigits - (wholeEnd - start) - fractionDigits;
                unsigned leadingPairBytes = (leadingWidth + 1) / 2;
                unsigned wholeFieldCount = (wholeEnd - start) / 2;
                unsigned fractionPairBytes = (fractionDigits + 1) / 2;
                width = 1 + leadingPairBytes + wholeFieldCount + fractionPairBytes;
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

// SQ_BIND: u16(5), u16(count), then per parameter: u16(ifxType),
// u16(indicator: -1 = NULL, 0 = has a value), then u16(precisionField) -
// always sent, NULL or not - and - only when non-NULL - the pre-encoded
// value bytes (padded to even length, matching every other variable-length
// field in this protocol).
//
// A NULL parameter's precisionField was originally believed to be omitted
// entirely (the indicator alone conveys NULL), but that was never actually
// confirmed live for a NULL that isn't the bind block's last parameter -
// the only NULL binds exercised so far were either the sole parameter or
// the last one, both cases where an omitted 2-byte field gets silently
// absorbed by whatever follows instead of causing a visible failure. A
// NULL bound ahead of another parameter hung the connection (same failure
// mode as the SQ_RBWORK savepoint-id omission - see docs/protocol-notes.md's
// Transactions section): the server was left waiting for 2 bytes that never
// arrived. Re-reading IfxSqli.sendBind's isNull() branch shows it writes a
// precision field of 0 right after the -1 indicator, matching every other
// parameter's shape (type, indicator, precision, [data]) rather than
// truncating it - confirmed live after adding it back.
void writeBindBlock(ByteWriter& w, const std::vector<BindParam>& params) {
    w.u16(opcode::kBind);
    w.u16(static_cast<std::uint16_t>(params.size()));
    for (const auto& p : params) {
        w.u16(static_cast<std::uint16_t>(p.ifxType));
        // Extended-type owner/name (confirmed live for BOOLEAN, hangs the
        // connection if omitted) - written unconditionally whenever
        // present, even for a NULL bind, matching the decompiled
        // sendBind's ordering (owner/name before the null check).
        if (p.extendedOwnerName && p.extendedTypeName) {
            w.ifxChar2(*p.extendedOwnerName);
            w.ifxChar2(*p.extendedTypeName);
        }
        if (p.isNull) {
            w.u16(static_cast<std::uint16_t>(-1));
            w.u16(0);
            continue;
        }
        w.u16(0);
        w.u16(p.precisionField);
        w.bytes(p.data);
        if (p.data.size() & 1) w.u8(0);
    }
}

// Counts "?" parameter markers in `sql`, skipping anything inside single- or
// double-quoted string literals (so a literal like '?' or a column named
// "?" - however unlikely - doesn't get miscounted as a bind parameter).
// Reimplemented from scratch against the documented behavior of the
// decompiled JDBC driver's own qmark-counting scan (a simple 3-state
// scanner: normal / single-quoted / double-quoted), not copied from it.
std::size_t countParamMarkers(const std::string& sql) {
    std::size_t count = 0;
    char quote = '\0'; // '\0' = not inside a quoted literal
    for (char c : sql) {
        if (quote != '\0') {
            if (c == quote) quote = '\0';
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '?') {
            ++count;
        }
    }
    return count;
}

} // namespace

std::expected<SqliStatement, Error> SqliStatement::prepare(SqliConnection& conn, const std::string& sql) {
    SqliStatement stmt(conn);
    stmt.paramCount_ = countParamMarkers(sql);
    auto& sock = conn.transport();

    ByteWriter w;
    w.u16(opcode::kPrepare);
    // numqmarks: the real "?" count (confirmed correct via a live bind-
    // parameter round trip - Phase 5). This field is two separate u16s in
    // wide mode (count, then a reserved/unused u16), NOT one u32 as
    // earlier phases assumed - every prior capture only ever carried
    // numqmarks=0, where "one u32 zero" and "two u16 zeros" are
    // byte-identical, so the two layouts were indistinguishable until a
    // live nonzero count actually exercised this field. Confirmed live:
    // treating it as one u32(1) sends `00 00 00 01`, which the server
    // reads as count=0/reserved=1 and then blocks forever waiting for the
    // rest of a message it considers incomplete (see docs/protocol-notes.md's
    // SQ_BIND section). In narrow mode there's no reserved half at all.
    if (conn.has4ByteOffsets()) {
        w.u16(static_cast<std::uint16_t>(stmt.paramCount_));
        w.u16(0);
    } else {
        w.u16(static_cast<std::uint16_t>(stmt.paramCount_));
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

std::expected<void, Error> SqliStatement::execute(const std::vector<BindParam>& params) {
    auto& sock = conn_->transport();

    if (!hasResultSet()) {
        // Non-SELECT (DDL/DML) prepared statement: no result set means no
        // cursor to open. Confirmed live: this path previously used
        // columns_.empty() as the discriminator, which is wrong once bind
        // parameters are involved - DESCRIBE's nfields/per-field records
        // describe *input parameter* placeholders instead of output
        // columns for a non-SELECT statement with "?" markers (confirmed
        // live: a 1-parameter INSERT reports nfields=1), so columns_ is
        // non-empty here even though there's still no result set. Opening
        // a cursor on a CREATE/DROP TABLE or INSERT statement fails live
        // (sqlcode -363, or a confusing "-255 not in transaction" once a
        // bind parameter is involved) - see hasResultSet()'s doc comment.
        // Matches the decompiled JDBC driver's own statementType-based
        // branch: a statement with a result set opens a cursor, one
        // without just gets SQ_EXECUTE directly.
        ByteWriter w;
        w.u16(opcode::kId);
        w.u16(statementId_);
        if (!params.empty()) writeBindBlock(w, params);
        w.u16(opcode::kExecute);
        w.u16(opcode::kEot);
        if (auto r = sock.writeAll(w.view()); !r) return std::unexpected(r.error());

        auto batch = processResponseBatch();
        if (!batch) return std::unexpected(batch.error());

        cursorOpen_ = false;
        return {};
    }

    static std::atomic<std::uint64_t> counter{1};
    std::uint64_t n = counter.fetch_add(1);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "_odbc%013llu", static_cast<unsigned long long>(n));
    cursorName_ = buf;

    ByteWriter w;
    w.u16(opcode::kId);
    w.u16(statementId_);
    if (!params.empty()) writeBindBlock(w, params);
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
        //
        // Extended/opaque types (BOOLEAN and friends - anything DESCRIBE gave
        // a nonzero extendedId) need a different entry shape than plain
        // scalars: the *extended* type code (not the base ifxType) plus the
        // owner/type-name pair DESCRIBE reported, echoed straight back -
        // reverse-engineered from the decompiled IfxSqli.sendType(), which
        // remaps extendedId {1,3,4,5,other} -> sqltype {43,46,47,45,44} and
        // writes owner+name (as ifxChar2 fields) before encodedLength for any
        // such column. Omitting this for BOOLEAN causes the server to
        // misparse the rest of the request and drop the connection
        // (confirmed live: a bare SQ_RETURN_TYPE for a BOOLEAN column gets a
        // lone tag-56 reply followed by the socket closing).
        w.u16(opcode::kReturnType);
        w.u16(1); // direction: 1 = fetch
        w.u16(static_cast<std::uint16_t>(columns_.size()));
        for (const auto& col : columns_) {
            bool extended = col.extendedId != 0 && col.ifxType != ifxtype::kVarchar &&
                             col.ifxType != ifxtype::kNVarchar;
            if (extended) {
                std::uint16_t remapped;
                switch (col.extendedId) {
                    case 1: remapped = 43; break;
                    case 3: remapped = 46; break;
                    case 4: remapped = 47; break;
                    case 5: remapped = 45; break;
                    default: remapped = 44; break;
                }
                w.u16(remapped);
                w.ifxChar2(col.extendedOwnerName);
                w.ifxChar2(col.extendedTypeName);
            } else {
                w.u16(static_cast<std::uint16_t>(col.ifxType));
            }
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
    // A non-SELECT execute() never opens a cursor (see execute()'s
    // columns_.empty() branch) - SQ_CLOSE with no matching SQ_OPEN has
    // nothing to do here and isn't worth a round trip to the server.
    if (!cursorOpen_) return {};

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
                        auto extId = readU32(sock);
                        if (!extId) return std::unexpected(Error{ErrorKind::Transport, "read failed (extended id)"});
                        columns_[i].extendedId = *extId;
                        auto owner = readIfxChar2(sock);
                        if (!owner) return std::unexpected(owner.error());
                        columns_[i].extendedOwnerName = *owner;
                        auto extName = readIfxChar2(sock);
                        if (!extName) return std::unexpected(extName.error());
                        columns_[i].extendedTypeName = *extName;
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
                auto rowCount = readU32(sock);
                if (!rowCount) return std::unexpected(Error{ErrorKind::Transport, "read failed (row count)"});
                rowCount_ = *rowCount;
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
                std::uint32_t hi = r.u32();
                std::uint32_t lo = r.u32();
                lastBigSerialInserted_ = static_cast<std::int64_t>((static_cast<std::uint64_t>(hi) << 32) | lo);
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
