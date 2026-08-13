#include "sqli/connection.h"

#include "sqli/byte_io.h"
#include "sqli/protocol.h"

#include <cstdio>
#include <optional>

namespace ifxsqli {

namespace {
constexpr std::uint16_t kSqExit = 56;
constexpr std::uint16_t kSqEot = 12;
constexpr std::uint16_t kSqXactStat = 99;
constexpr std::uint16_t kOpSelectDatabase = 36;

// The exact 8-byte client capability offer the real driver sends in
// SQ_PROTOCOLS (confirmed byte-for-byte against tests/fixtures/
// handshake_capture_ids14.10.txt); the server ANDs it against its own
// supported set and echoes back the intersection. Byte[6] bit 0x20 (bit 50
// overall) is the one this driver currently cares about: whether DESCRIBE
// responses use 4-byte (vs 2-byte) offset/length fields. See
// docs/protocol-notes.md.
constexpr std::uint8_t kClientProtocols[8] = {0xFF, 0xFC, 0x7F, 0xFC, 0x3C, 0x8C, 0xAA, 0x97};

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
// consumed iff the length is odd (see ByteWriter::ifxChar2).
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

// Reads a SQ_ERROR(13) payload (sqlcode/isamcode/offset/message) for
// surfacing in an Error. Assumes the tag itself has already been consumed.
std::expected<Error, Error> readErrorRecord(TcpSocket& sock) {
    auto sqlcodeRaw = readU16(sock);
    if (!sqlcodeRaw) return std::unexpected(sqlcodeRaw.error());
    auto sqlcode = static_cast<std::int16_t>(*sqlcodeRaw);
    if (!readU16(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (isamcode)"});
    if (!readU32(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (offset)"});
    std::string message;
    if (sqlcode != -368) {
        auto msg = readIfxChar2(sock);
        if (!msg) return std::unexpected(msg.error());
        message = *msg;
    }
    return Error{ErrorKind::Server, "sqlcode " + std::to_string(sqlcode) + ": " + message};
}

// Sends SQ_PROTOCOLS(126) right after the handshake, per decompiled
// IfxSqliConnect.enhancedProtocolMechanism()/IfxSqli.{send,receive}Protocols,
// cross-checked against the golden capture. Unlike most send* helpers here,
// this one is NOT self-terminated by the SQ_PROTOCOLS payload alone: the real
// driver follows it with a bare SQ_EOT as a separate write, and the server's
// reply is the protocols response immediately followed by its own SQ_EOT in
// the same message. Omitting that trailing EOT leaves the server waiting for
// more input forever - it never replies at all.
std::expected<bool, Error> negotiateProtocols(TcpSocket& sock) {
    ByteWriter w;
    w.u16(opcode::kProtocols);
    w.u16(static_cast<std::uint16_t>(sizeof(kClientProtocols)));
    for (auto b : kClientProtocols) w.u8(b);
    w.u16(kSqEot);
    if (auto r = sock.writeAll(w.view()); !r) return std::unexpected(r.error());

    auto tag = readU16(sock);
    if (!tag) return std::unexpected(tag.error());
    if (*tag != opcode::kProtocols) {
        return std::unexpected(Error{ErrorKind::Protocol, "unexpected reply tag to SQ_PROTOCOLS"});
    }

    auto len = readU16(sock);
    if (!len) return std::unexpected(len.error());
    auto data = sock.readExactN(*len);
    if (!data) return std::unexpected(data.error());
    if (*len & 1) {
        auto pad = sock.readExactN(1);
        if (!pad) return std::unexpected(pad.error());
    }

    auto eot = readU16(sock);
    if (!eot) return std::unexpected(eot.error());
    if (*eot != kSqEot) {
        return std::unexpected(Error{ErrorKind::Protocol, "missing SQ_EOT after SQ_PROTOCOLS reply"});
    }

    // Bit 50 overall (byte[6] bit 0x20): whether the server uses 4-byte (vs
    // 2-byte) offset/length fields in DESCRIBE/TUPLE/etc - see
    // docs/protocol-notes.md for the full field-by-field breakdown.
    bool has4ByteOffsets = data->size() > 6 &&
                           (std::to_integer<unsigned char>((*data)[6]) & 0x20) != 0;
    return has4ByteOffsets;
}

// Sends SQ_INFO(81)/INFO_ENV(6) right after protocol negotiation, per
// decompiled IfxSqli.sendEnv() and confirmed against the golden capture. The
// server rejects an empty env set outright (sqlcode -208), so this sends the
// same DBTEMP/SUBQCACHESZ pair the real driver sends when no connInfo
// properties override them, plus an explicit DBDATE - confirmed live that
// without it, date string literals (e.g. `DATE('07/29/2026')`) are parsed
// against whatever locale the server defaults to (which turned out to be
// ISO-ish, not the Docker image's shell-level DBDATE used by dbaccess),
// which is surprising and not something a driver should leave to chance.
// `Y4MD-` pins it to an unambiguous ISO year-month-day order regardless of
// server default. The server replies with a bare SQ_EOT.
std::expected<void, Error> sendEnv(TcpSocket& sock) {
    constexpr std::uint16_t kOpInfo = 81;
    constexpr std::uint16_t kInfoEnv = 6;
    ByteWriter w;
    w.u16(kOpInfo);
    w.u16(kInfoEnv);
    w.u16(54); // total length (see docs/protocol-notes.md for the formula)
    w.u16(12); // longest name length ("SUBQCACHESZ", padded to even)
    w.u16(6);  // longest value length ("Y4MD-", padded to even)
    w.ifxChar2("DBTEMP");
    w.ifxChar2("/tmp");
    w.ifxChar2("SUBQCACHESZ");
    w.ifxChar2("10");
    w.ifxChar2("DBDATE");
    w.ifxChar2("Y4MD-");
    w.u16(0); // INFO_DONE terminator: name length 0
    w.u16(0); // INFO_DONE terminator: value length 0
    w.u16(kSqEot);
    if (auto r = sock.writeAll(w.view()); !r) return r;

    auto tag = readU16(sock);
    if (!tag) return std::unexpected(tag.error());
    if (*tag == opcode::kError) {
        auto err = readErrorRecord(sock);
        if (!err) return std::unexpected(err.error());
        return std::unexpected(*err);
    }
    if (*tag != kSqEot) {
        return std::unexpected(Error{ErrorKind::Protocol, "missing SQ_EOT after SQ_INFO/INFO_ENV reply"});
    }
    return {};
}

// Sends the "select database" message observed immediately after a
// successful handshake in the live capture (docs/protocol-notes.md). The
// reply's exact shape is Phase 2 (statement protocol) territory we don't yet
// parse, so we just scan for the SQ_EOT(12) terminator to stay in sync with
// the stream rather than fully decoding it. This is a narrow, deliberately
// minimal stand-in - full opcode dispatch replaces it in Phase 2.
std::expected<void, Error> selectDatabase(TcpSocket& sock, const std::string& database) {
    ByteWriter w;
    w.u16(kOpSelectDatabase);
    // Database name is the standard statement-protocol "char" field
    // (u16 length + raw bytes + pad-if-odd) - confirmed live: an unconditional
    // trailing NUL byte (this driver's original assumption, based on only ever
    // testing with the odd-length "sysmaster") breaks alignment for any
    // even-length database name.
    w.ifxChar2(database);
    w.u16(0);
    w.u16(kSqEot);
    if (auto r = sock.writeAll(w.view()); !r) return r;

    constexpr int kMaxFields = 256;
    for (int i = 0; i < kMaxFields; ++i) {
        auto b = sock.readExactN(2);
        if (!b) return std::unexpected(b.error());
        ByteReader r(*b);
        auto v = r.u16();
        if (v == kSqEot) return {};
    }
    return std::unexpected(Error{ErrorKind::Protocol, "select-database reply never terminated"});
}

} // namespace

std::expected<void, Error> SqliConnection::executeCommand(const std::string& sql) {
    ByteWriter w;
    w.u16(opcode::kCommand);
    // "# of values": decompiled IfxSqli.sendCommand shows writeSmallInt(0)
    // (u16), but the golden capture proves this field is u32 on connections
    // that negotiated wide offsets - the same narrow/wide toggle as every
    // other reserved/length field in this protocol (see
    // docs/protocol-notes.md). Trust the capture over the decompiled source.
    if (has4ByteOffsets_) {
        w.u32(0);
    } else {
        w.u16(0);
    }
    w.ifxChar2(sql);
    w.u16(opcode::kNonCursorDescribe);
    w.u16(opcode::kExecute);
    w.u16(opcode::kRelease);
    w.u16(opcode::kEot);
    if (auto r = sock_.writeAll(w.view()); !r) return r;

    lastSerialInserted_.reset();
    lastBigSerialInserted_.reset();

    std::optional<Error> pendingError;
    for (;;) {
        auto tag = readU16(sock_);
        if (!tag) return std::unexpected(tag.error());
        if (*tag == opcode::kEot) break;

        switch (*tag) {
            case opcode::kDescribe: {
                if (!readU16(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (statementType)"});
                if (!readU16(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (statementId)"});
                if (!readU32(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (estCost)"});
                if (!readU16(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (tupleSize)"});
                auto nfields = readU16(sock_);
                if (!nfields) return std::unexpected(nfields.error());

                std::uint32_t stringTableSize;
                if (has4ByteOffsets_) {
                    auto v = readU32(sock_);
                    if (!v) return std::unexpected(v.error());
                    stringTableSize = *v;
                } else {
                    auto v = readU16(sock_);
                    if (!v) return std::unexpected(v.error());
                    stringTableSize = *v;
                }

                for (std::uint16_t i = 0; i < *nfields; ++i) {
                    if (has4ByteOffsets_) {
                        if (!readU32(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (field index)"});
                    } else {
                        if (!readU16(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (field index)"});
                    }
                    if (has4ByteOffsets_) {
                        if (!readU32(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (column start)"});
                    } else {
                        if (!readU16(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (column start)"});
                    }
                    if (!readU16(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (colType)"});
                    if (has4ByteOffsets_) {
                        if (!readU32(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (extId)"});
                        if (auto owner = readIfxChar2(sock_); !owner) return std::unexpected(owner.error());
                        if (auto extName = readIfxChar2(sock_); !extName) return std::unexpected(extName.error());
                        if (!readU16(sock_) || !readU16(sock_)) {
                            return std::unexpected(Error{ErrorKind::Transport, "read failed (reference/alignment)"});
                        }
                        if (!readU32(sock_) || !readU32(sock_)) {
                            return std::unexpected(Error{ErrorKind::Transport, "read failed (sourceType/encLen)"});
                        }
                    } else {
                        if (!readU16(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (encLen)"});
                    }
                }
                if (stringTableSize > 0) {
                    auto strtab = sock_.readExactN(stringTableSize);
                    if (!strtab) return std::unexpected(strtab.error());
                    if (stringTableSize & 1) {
                        auto pad = sock_.readExactN(1);
                        if (!pad) return std::unexpected(pad.error());
                    }
                }
                break;
            }
            case opcode::kDone: {
                if (!readU16(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (warnings)"});
                if (!readU32(sock_) || !readU32(sock_) || !readU32(sock_)) {
                    return std::unexpected(Error{ErrorKind::Transport, "read failed (done fields)"});
                }
                break;
            }
            case opcode::kCost: {
                auto b = sock_.readExactN(8);
                if (!b) return std::unexpected(b.error());
                break;
            }
            case opcode::kError: {
                auto err = readErrorRecord(sock_);
                if (!err) return std::unexpected(err.error());
                pendingError = *err;
                break;
            }
            case opcode::kInsertDone: {
                // Confirmed live (18 bytes total): a 10-byte legacy "long
                // int" serial8Inserted field, then a plain 8-byte big-endian
                // bigSerialInserted field - see docs/protocol-notes.md.
                auto serial8 = sock_.readExactN(10);
                if (!serial8) return std::unexpected(serial8.error());
                lastSerialInserted_ = decodeLegacyLongInt(*serial8);

                auto bigSerial = sock_.readExactN(8);
                if (!bigSerial) return std::unexpected(bigSerial.error());
                ByteReader r(*bigSerial);
                std::uint32_t hi = r.u32();
                std::uint32_t lo = r.u32();
                lastBigSerialInserted_ = static_cast<std::int64_t>((static_cast<std::uint64_t>(hi) << 32) | lo);
                break;
            }
            default:
                return std::unexpected(Error{ErrorKind::Protocol, "unrecognized tag in executeCommand response"});
        }
    }

    if (pendingError) return std::unexpected(*pendingError);
    return {};
}

std::expected<std::vector<std::byte>, Error> SqliConnection::fetchSimpleLob(std::span<const std::byte> descriptor) {
    ByteWriter w;
    w.u16(opcode::kFetchBlob);
    w.bytes(descriptor);
    w.u16(opcode::kEot);
    if (auto r = sock_.writeAll(w.view()); !r) return std::unexpected(r.error());

    std::vector<std::byte> result;
    std::optional<Error> pendingError;
    for (;;) {
        auto tag = readU16(sock_);
        if (!tag) return std::unexpected(tag.error());
        if (*tag == opcode::kEot) break;

        switch (*tag) {
            case opcode::kBlob: {
                auto len = readU16(sock_);
                if (!len) return std::unexpected(len.error());
                if (*len > 0) {
                    auto chunk = sock_.readExactN(*len);
                    if (!chunk) return std::unexpected(chunk.error());
                    result.insert(result.end(), chunk->begin(), chunk->end());
                    if (*len & 1) {
                        auto pad = sock_.readExactN(1);
                        if (!pad) return std::unexpected(pad.error());
                    }
                }
                break;
            }
            case opcode::kError: {
                auto err = readErrorRecord(sock_);
                if (!err) return std::unexpected(err.error());
                pendingError = *err;
                break;
            }
            default:
                return std::unexpected(Error{ErrorKind::Protocol, "unrecognized tag in fetchSimpleLob response"});
        }
    }

    if (pendingError) return std::unexpected(*pendingError);
    return result;
}

namespace {

// Sends an opcode (with an optional trailing u16 field some opcodes require
// before the EOT - e.g. SQ_RBWORK's savepoint id) and reads back the generic
// tagged-record response, tolerating DONE/COST and surfacing ERROR - the
// shape executeCommand's own reply loop uses minus the DESCRIBE handling,
// since a transaction-control opcode carries no SQL text and nothing should
// ever prepare a statement in response to one. Any DESCRIBE or unrecognized
// tag is treated as a protocol error rather than silently ignored, so a
// future opcode reusing this helper with a reply shape it doesn't handle
// yet fails loudly instead of quietly desyncing the stream.
std::expected<void, Error> sendBareOpcodeAndReadReply(TcpSocket& sock, std::uint16_t op,
                                                        std::optional<std::uint16_t> extraField = std::nullopt) {
    ByteWriter w;
    w.u16(op);
    if (extraField) w.u16(*extraField);
    w.u16(kSqEot);
    if (auto r = sock.writeAll(w.view()); !r) return r;

    std::optional<Error> pendingError;
    for (;;) {
        auto tag = readU16(sock);
        if (!tag) return std::unexpected(tag.error());
        if (*tag == kSqEot) break;

        switch (*tag) {
            case opcode::kDone: {
                if (!readU16(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (warnings)"});
                if (!readU32(sock) || !readU32(sock) || !readU32(sock)) {
                    return std::unexpected(Error{ErrorKind::Transport, "read failed (done fields)"});
                }
                break;
            }
            case opcode::kCost: {
                auto b = sock.readExactN(8);
                if (!b) return std::unexpected(b.error());
                break;
            }
            case opcode::kError: {
                auto err = readErrorRecord(sock);
                if (!err) return std::unexpected(err.error());
                pendingError = *err;
                break;
            }
            case kSqXactStat: {
                // Confirmed live: SQ_BEGIN/SQ_CMMTWORK/SQ_RBWORK's reply all
                // lead with this tag, followed by 3 more u16 fields, the
                // same shape disconnectOrderly already tolerates around
                // SQ_EXIT.
                if (!readU16(sock) || !readU16(sock) || !readU16(sock)) {
                    return std::unexpected(Error{ErrorKind::Transport, "read failed (xactstat fields)"});
                }
                break;
            }
            default:
                return std::unexpected(Error{ErrorKind::Protocol,
                    "unrecognized tag in transaction-control response (opcode " + std::to_string(op) +
                    ", tag " + std::to_string(*tag) + ") - see protocol.h"});
        }
    }

    if (pendingError) return std::unexpected(*pendingError);
    return {};
}

} // namespace

std::expected<void, Error> SqliConnection::beginTransaction() {
    return sendBareOpcodeAndReadReply(sock_, opcode::kBegin);
}

std::expected<void, Error> SqliConnection::commitTransaction() {
    return sendBareOpcodeAndReadReply(sock_, opcode::kCommitWork);
}

std::expected<void, Error> SqliConnection::rollbackTransaction() {
    // SQ_RBWORK carries a trailing u16 savepoint id after the opcode, unlike
    // SQ_BEGIN/SQ_CMMTWORK - confirmed against the decompiled JDBC
    // reference's sendRollback(), which writes 20 then 0 before the caller's
    // later EOT. Without it the server waits for those two bytes forever
    // instead of replying. 0 means "roll back the whole transaction", not a
    // named savepoint - this driver doesn't support savepoints yet.
    return sendBareOpcodeAndReadReply(sock_, opcode::kRollbackWork, std::uint16_t{0});
}

std::expected<SqliConnection, Error> SqliConnection::connect(const std::string& host, int port,
                                                               const ConnectParams& params) {
    auto sock = TcpSocket::connect(host, port);
    if (!sock) return std::unexpected(sock.error());

    auto info = performHandshake(*sock, params);
    if (!info) return std::unexpected(info.error());

    auto has4ByteOffsets = negotiateProtocols(*sock);
    if (!has4ByteOffsets) return std::unexpected(has4ByteOffsets.error());

    if (auto r = sendEnv(*sock); !r) return std::unexpected(r.error());

    if (!params.database.empty()) {
        if (auto r = selectDatabase(*sock, params.database); !r) return std::unexpected(r.error());
    }

    return SqliConnection(std::move(*sock), std::move(*info), *has4ByteOffsets);
}

std::expected<void, Error> SqliConnection::disconnectOrderly() {
    ByteWriter w;
    w.u16(kSqExit);
    if (auto r = sock_.writeAll(w.view()); !r) return r;

    // The server sometimes acks with SQ_EXIT/SQ_EOT and sometimes just closes
    // the socket outright (observed live: depends on prior session activity -
    // see docs/protocol-notes.md). The real driver tolerates both by simply
    // swallowing any I/O error while waiting for this reply, so we do too:
    // a closed connection here is a successful disconnect, not a failure.
    auto readU16 = [this]() -> std::expected<std::uint16_t, Error> {
        auto b = sock_.readExactN(2);
        if (!b) return std::unexpected(b.error());
        ByteReader r(*b);
        return r.u16();
    };

    auto last = readU16();
    while (last.has_value() && *last == kSqXactStat) {
        for (int i = 0; i < 3 && last.has_value(); ++i) (void)readU16();
        last = readU16();
    }

    sock_.close();
    return {};
}

} // namespace ifxsqli
