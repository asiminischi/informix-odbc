#include "sqli/connection.h"

#include "sqli/byte_io.h"

namespace ifxsqli {

namespace {
constexpr std::uint16_t kSqExit = 56;
constexpr std::uint16_t kSqEot = 12;
constexpr std::uint16_t kSqXactStat = 99;
constexpr std::uint16_t kOpSelectDatabase = 36;

// Sends the "select database" message observed immediately after a
// successful handshake in the live capture (docs/protocol-notes.md). The
// reply's exact shape is Phase 2 (statement protocol) territory we don't yet
// parse, so we just scan for the SQ_EOT(12) terminator to stay in sync with
// the stream rather than fully decoding it. This is a narrow, deliberately
// minimal stand-in - full opcode dispatch replaces it in Phase 2.
std::expected<void, Error> selectDatabase(TcpSocket& sock, const std::string& database) {
    ByteWriter w;
    w.u16(kOpSelectDatabase);
    w.u16(static_cast<std::uint16_t>(database.size()));
    w.raw(database);
    w.u8(0);
    w.u16(0);
    w.u16(kSqEot);
    if (auto r = sock.writeAll(w.view()); !r) return r;

    constexpr int kMaxFields = 256;
    for (int i = 0; i < kMaxFields; ++i) {
        auto b = sock.readExactN(2);
        if (!b) return std::unexpected(b.error());
        ByteReader r(*b);
        if (r.u16() == kSqEot) return {};
    }
    return std::unexpected(Error{ErrorKind::Protocol, "select-database reply never terminated"});
}
} // namespace

std::expected<SqliConnection, Error> SqliConnection::connect(const std::string& host, int port,
                                                               const ConnectParams& params) {
    auto sock = TcpSocket::connect(host, port);
    if (!sock) return std::unexpected(sock.error());

    auto info = performHandshake(*sock, params);
    if (!info) return std::unexpected(info.error());

    if (!params.database.empty()) {
        if (auto r = selectDatabase(*sock, params.database); !r) return std::unexpected(r.error());
    }

    return SqliConnection(std::move(*sock), std::move(*info));
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
