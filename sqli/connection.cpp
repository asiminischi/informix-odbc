#include "sqli/connection.h"

#include "sqli/byte_io.h"
#include "sqli/protocol.h"
#include "sqli/types.h"

#include <cstdio>
#include <optional>
#include <utility>

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
struct ProtocolCapabilities {
    bool has4ByteOffsets = false;
    // Byte[6] bit 0x08 (bit 52 overall, decompiled IfxSqliConnect's
    // handleProtocolByte_7) - whether SQ_GETROUTINE/receiveHandle use the
    // named-parameters wire framing (a leading isRoutineById byte plus a
    // db-name field) or the plain signature-only framing. Needed for the
    // fastpath RPC subsystem smart LOBs use (sqli/fastpath.cpp) - not
    // consulted anywhere else yet. Confirmed live against this project's
    // IDS 14.10 dev container: set.
    bool namedParametersSupported = false;
};

std::expected<ProtocolCapabilities, Error> negotiateProtocols(TcpSocket& sock) {
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

    ProtocolCapabilities caps;
    if (data->size() > 6) {
        unsigned char byte6 = std::to_integer<unsigned char>((*data)[6]);
        // Bit 50 overall (byte[6] bit 0x20): whether the server uses 4-byte
        // (vs 2-byte) offset/length fields in DESCRIBE/TUPLE/etc - see
        // docs/protocol-notes.md for the full field-by-field breakdown.
        caps.has4ByteOffsets = (byte6 & 0x20) != 0;
        caps.namedParametersSupported = (byte6 & 0x08) != 0;
    }
    return caps;
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

// SQ_GETROUTINE(101): resolves a fastpath function signature string to a
// numeric routine handle, returning it alongside the database name the
// server echoes back (EXFPROUTINE needs this verbatim - see
// sendExfpRoutine below). Confirmed live: the signature field uses a
// 4-byte length prefix (ifxChar4), not the 2-byte ifxChar2 this driver
// uses almost everywhere else - gated by SQ_PROTOCOLS capability bit 62
// (isRemove64KLimitSupported, byte[7] & 0x02, true on every server tested
// so far), per the decompiled reference's getJavaToIfxCharBytes(). This
// driver doesn't track that bit as its own capability flag yet (only
// GETROUTINE needs it so far) - always sending ifxChar4 here is safe
// unconditionally since the request framing this project actually tested
// live only exists on a server where the bit is set; if a server without
// it is ever encountered, this needs to become capability-gated like
// has4ByteOffsets(). The reply's echoed database name, in contrast, uses
// the plain 2-byte ifxChar2 - confirmed live this asymmetry is real, not
// a mistake (see readIfxChar2 above). Both the request and the reply are
// terminated by an explicit SQ_EOT despite neither showing one in
// IfxSqli.java's own getHandle()/dispatchMsg() source - the same "trust
// the live capture over an incomplete send* method" situation
// SQ_PROTOCOLS above already has.
std::expected<std::pair<std::string, std::int32_t>, Error> getRoutineHandle(TcpSocket& sock,
                                                                              const std::string& signature) {
    ByteWriter w;
    w.u16(opcode::kGetRoutine);
    w.u8(0); // isRoutineById = false (plain signature lookup, not a cached numeric id)
    w.ifxChar4(signature);
    w.u16(0); // request_fparam = false - this driver never needs the FParam/named-parameter-metadata mechanism
    w.u16(opcode::kEot);
    if (auto r = sock.writeAll(w.view()); !r) return std::unexpected(r.error());

    auto tag = readU16(sock);
    if (!tag) return std::unexpected(tag.error());
    if (*tag == opcode::kError) {
        auto err = readErrorRecord(sock);
        if (!err) return std::unexpected(err.error());
        return std::unexpected(*err);
    }
    if (*tag != opcode::kGetRoutine) {
        return std::unexpected(Error{ErrorKind::Protocol, "unexpected reply tag to SQ_GETROUTINE"});
    }

    auto dbName = readIfxChar2(sock);
    if (!dbName) return std::unexpected(dbName.error());
    auto handle = readU32(sock);
    if (!handle) return std::unexpected(handle.error());

    auto eot = readU16(sock);
    if (!eot) return std::unexpected(eot.error());
    if (*eot != opcode::kEot) {
        return std::unexpected(Error{ErrorKind::Protocol, "missing SQ_EOT after SQ_GETROUTINE reply"});
    }

    return std::make_pair(*dbName, static_cast<std::int32_t>(*handle));
}

// Appends one SQ_BIND-shaped plain INTEGER fastpath parameter body (no
// extended owner/name - ifxtype::kInteger is 2, below sendBind's `type >
// 18` extended-type threshold). The precision field is 2560, confirmed
// live via a JDBC golden-capture trace for every INTEGER fastpath
// parameter and return value this driver uses (informix.ifx_lo_open's
// mode, informix.ifx_lo_close's fd/status) - deliberately NOT this
// driver's own established plain-INTEGER SQ_BIND precision (4, used for
// ordinary column binds elsewhere via driver/bind_encode.cpp) - a genuine
// asymmetry between fastpath and ordinary bind parameters, copied from the
// trace rather than assumed to match.
void appendFastPathIntParam(ByteWriter& w, std::int32_t value) {
    w.u16(ifxtype::kInteger);
    w.u16(0); // indicator: not NULL
    w.u16(2560);
    w.bytes(encodeInteger(value));
}

// Appends one SQ_BIND-shaped UDT-wrapped fastpath parameter body carrying
// a smart LOB locator. `payload` must be exactly [4-byte big-endian
// length=72][72-byte locator bytes] - see fetchSmartLob's doc comment.
// ExtendedOwner is the empty string, not "informix" - confirmed live via
// the golden trace (unlike BOOLEAN's bind, which does use "informix"; see
// ifxtype::kUdt's doc comment). ExtendedTypeName is unconditionally
// "blob", matching informix.ifx_lo_open's own function signature
// (`function informix.ifx_lo_open(blob ,integer)`) regardless of whether
// the real column is BLOB or CLOB - confirmed live.
void appendFastPathLocatorParam(ByteWriter& w, std::span<const std::byte> payload) {
    w.u16(ifxtype::kUdt);
    w.ifxChar2("");
    w.ifxChar2("blob");
    w.u16(0); // indicator: not NULL
    w.u16(0); // precision
    w.bytes(payload);
    if (payload.size() & 1) w.u8(0);
}

// SQ_EXFPROUTINE(102) + its embedded SQ_BIND sub-block, sent as one
// combined write with a single trailing SQ_EOT - confirmed live this is
// NOT two separate EOT-terminated writes (one after the EXFPROUTINE
// header, another after the bind block); `paramBodies` is the
// concatenation of appendFastPathIntParam/appendFastPathLocatorParam
// calls, `paramCount` how many.
std::expected<void, Error> sendExfpRoutine(TcpSocket& sock, const std::string& dbName, std::int32_t handle,
                                            std::span<const std::byte> paramBodies, std::uint16_t paramCount) {
    ByteWriter w;
    w.u16(opcode::kExfpRoutine);
    w.ifxChar2(dbName);
    w.u32(static_cast<std::uint32_t>(handle));
    w.u16(paramCount);
    w.u16(0); // request_fparam = false
    w.u16(opcode::kBind);
    w.u16(paramCount);
    w.bytes(paramBodies);
    w.u16(opcode::kEot);
    return sock.writeAll(w.view());
}

// Reads an SQ_FPROUTINE(103) reply expected to carry exactly one plain
// INTEGER return value - the only return shape informix.ifx_lo_open (the
// file descriptor) and informix.ifx_lo_close (a status code this driver
// doesn't need) produce. Confirmed live: the return value's precision
// field is also 2560, matching the equally-nonstandard bind-direction
// value above, not the 4 an ordinary INTEGER column fetch would imply.
std::expected<std::int32_t, Error> receiveFpRoutineIntReturn(TcpSocket& sock) {
    auto tag = readU16(sock);
    if (!tag) return std::unexpected(tag.error());
    if (*tag == opcode::kError) {
        auto err = readErrorRecord(sock);
        if (!err) return std::unexpected(err.error());
        return std::unexpected(*err);
    }
    if (*tag != opcode::kFpRoutine) {
        return std::unexpected(Error{ErrorKind::Protocol, "unexpected reply tag to SQ_EXFPROUTINE"});
    }

    auto count = readU16(sock);
    if (!count) return std::unexpected(count.error());
    if (*count != 1) {
        return std::unexpected(Error{ErrorKind::Protocol, "unexpected SQ_FPROUTINE return count"});
    }

    auto type = readU16(sock);
    if (!type) return std::unexpected(type.error());
    if (*type != ifxtype::kInteger) {
        return std::unexpected(Error{ErrorKind::Protocol, "unexpected SQ_FPROUTINE return type"});
    }

    auto ind = readU16(sock);
    if (!ind) return std::unexpected(ind.error());
    if (!readU16(sock)) return std::unexpected(Error{ErrorKind::Transport, "read failed (fproutine return precision)"});

    std::int32_t value = 0;
    if (static_cast<std::int16_t>(*ind) != -1) {
        auto v = readU32(sock);
        if (!v) return std::unexpected(v.error());
        value = static_cast<std::int32_t>(*v);
    }

    auto eot = readU16(sock);
    if (!eot) return std::unexpected(eot.error());
    if (*eot != opcode::kEot) {
        return std::unexpected(Error{ErrorKind::Protocol, "missing SQ_EOT after SQ_FPROUTINE reply"});
    }

    return value;
}

} // namespace

std::expected<std::vector<std::byte>, Error> SqliConnection::fetchSmartLob(std::span<const std::byte> locatorPayload) {
    auto openHandle = getRoutineHandle(sock_, "function informix.ifx_lo_open(blob ,integer)");
    if (!openHandle) return std::unexpected(openHandle.error());

    // LO_RDWR(8), not LO_RDONLY(4) - the exact mode value confirmed live via
    // the JDBC golden-capture trace; LO_RDONLY is untested and, since the
    // mode value isn't otherwise validated by this driver, not worth an
    // extra live check to swap in for a fetch-only code path.
    constexpr std::int32_t kLoOpenMode = 8;
    ByteWriter openParams;
    appendFastPathLocatorParam(openParams, locatorPayload);
    appendFastPathIntParam(openParams, kLoOpenMode);
    if (auto r = sendExfpRoutine(sock_, openHandle->first, openHandle->second, openParams.view(), 2); !r) {
        return std::unexpected(r.error());
    }
    auto fd = receiveFpRoutineIntReturn(sock_);
    if (!fd) return std::unexpected(fd.error());

    // SQ_LODATA(97)/LO_READ(0): LO_FD is a smallint on the wire (not a full
    // int, despite arriving from SQ_FPROUTINE as one) - confirmed live and
    // matches the decompiled reference's own writeSmallInt((short)loFd).
    // The requested byte count is just an upper bound, not a guess that has
    // to be right: the server's reply carries the true total in its own
    // `fileSize` field (confirmed live), so requesting a value comfortably
    // larger than any real smart LOB is safe.
    ByteWriter loReq;
    loReq.u16(opcode::kLoData);
    loReq.u16(0); // LO_READ
    loReq.u16(static_cast<std::uint16_t>(*fd));
    loReq.u32(0x7FFFFFFF);
    loReq.u16(32000); // R_W_BUFSIZE - the fixed value the golden trace showed
    loReq.u16(opcode::kEot);
    if (auto r = sock_.writeAll(loReq.view()); !r) return std::unexpected(r.error());

    std::vector<std::byte> content;
    {
        auto tag = readU16(sock_);
        if (!tag) return std::unexpected(tag.error());
        if (*tag != opcode::kLoData) {
            return std::unexpected(Error{ErrorKind::Protocol, "unexpected reply tag to SQ_LODATA"});
        }
        if (!readU16(sock_)) return std::unexpected(Error{ErrorKind::Transport, "read failed (lodata optype)"});
        auto fileSizeR = readU32(sock_);
        if (!fileSizeR) return std::unexpected(fileSizeR.error());
        auto fileSize = static_cast<std::int32_t>(*fileSizeR);
        if (fileSize < 0) {
            return std::unexpected(Error{ErrorKind::Server,
                "SQ_LODATA reported a negative size (isamcode " + std::to_string(fileSize) + ")"});
        }
        // Chunks are simple length-prefixed records (u16 length + that many
        // bytes, padded to even), read back-to-back until the total the
        // reply announced has been consumed - no zero-length terminator
        // chunk the way fetchSimpleLob's SQ_BLOB stream has one (confirmed
        // live and matches the decompiled reference's receiveLODATA, which
        // tracks a remaining-byte counter rather than watching for a
        // sentinel).
        std::int64_t remaining = fileSize;
        while (remaining > 0) {
            auto chunkLen = readU16(sock_);
            if (!chunkLen) return std::unexpected(chunkLen.error());
            auto chunk = sock_.readExactN(*chunkLen);
            if (!chunk) return std::unexpected(chunk.error());
            content.insert(content.end(), chunk->begin(), chunk->end());
            if (*chunkLen & 1) {
                auto pad = sock_.readExactN(1);
                if (!pad) return std::unexpected(pad.error());
            }
            remaining -= *chunkLen;
        }

        auto eot = readU16(sock_);
        if (!eot) return std::unexpected(eot.error());
        if (*eot != opcode::kEot) {
            return std::unexpected(Error{ErrorKind::Protocol, "missing SQ_EOT after SQ_LODATA reply"});
        }
    }

    auto closeHandle = getRoutineHandle(sock_, "function informix.ifx_lo_close(integer)");
    if (!closeHandle) return std::unexpected(closeHandle.error());
    ByteWriter closeParams;
    appendFastPathIntParam(closeParams, *fd);
    if (auto r = sendExfpRoutine(sock_, closeHandle->first, closeHandle->second, closeParams.view(), 1); !r) {
        return std::unexpected(r.error());
    }
    auto closeStatus = receiveFpRoutineIntReturn(sock_);
    if (!closeStatus) return std::unexpected(closeStatus.error());

    return content;
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

    auto caps = negotiateProtocols(*sock);
    if (!caps) return std::unexpected(caps.error());

    if (auto r = sendEnv(*sock); !r) return std::unexpected(r.error());

    if (!params.database.empty()) {
        if (auto r = selectDatabase(*sock, params.database); !r) return std::unexpected(r.error());
    }

    return SqliConnection(std::move(*sock), std::move(*info), caps->has4ByteOffsets, caps->namedParametersSupported);
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
