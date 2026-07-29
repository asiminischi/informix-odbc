#pragma once

#include "sqli/error.h"
#include "sqli/handshake.h"
#include "sqli/transport.h"

#include <expected>
#include <string>

namespace ifxsqli {

// A live SQLI session: transport + completed handshake. Phase 1 scope only
// covers connect/disconnect; statement traffic is Phase 2.
class SqliConnection {
public:
    [[nodiscard]] static std::expected<SqliConnection, Error> connect(const std::string& host, int port,
                                                                       const ConnectParams& params);

    // Sends SQ_EXIT and waits for the server's acknowledgement, per the
    // confirmed disconnect sequence in docs/protocol-notes.md. Safe to call
    // at most once; the destructor closes the socket unconditionally as a
    // fallback if this wasn't called.
    [[nodiscard]] std::expected<void, Error> disconnectOrderly();

    [[nodiscard]] const ConnectResult& serverInfo() const { return info_; }

    // Whether the connection negotiated the "wide" SQ_PROTOCOLS capability
    // set. Despite the name, this gates more than just DESCRIBE's 4-byte
    // offset fields (bit 50) - it's used as a single proxy for several
    // distinct capability bits (isUSVER, is2GBFetchBufferSupported, isFetchArr,
    // etc.) that all happen to co-occur as true on every server tested so far.
    // Only bit 50 is actually inspected; the others are assumed to track it.
    // If a server is ever found where they diverge, this needs to become
    // several independent flags - see docs/protocol-notes.md.
    [[nodiscard]] bool has4ByteOffsets() const { return has4ByteOffsets_; }

    // Sends a simple, resultless SQL command (e.g. "set isolation to ...")
    // and waits for it to complete. Distinct from SqliStatement because the
    // wire shape is different: prepare+execute+release happen as one batch
    // rather than as separate steps.
    [[nodiscard]] std::expected<void, Error> executeCommand(const std::string& sql);

    // Exposed for SqliStatement (sqli/statement.h), which sits on top of the
    // same connection and needs to send/receive statement-protocol messages
    // over it directly.
    [[nodiscard]] TcpSocket& transport() { return sock_; }

private:
    SqliConnection(TcpSocket sock, ConnectResult info, bool has4ByteOffsets)
        : sock_(std::move(sock)), info_(std::move(info)), has4ByteOffsets_(has4ByteOffsets) {}

    TcpSocket sock_;
    ConnectResult info_;
    bool has4ByteOffsets_ = false;
};

} // namespace ifxsqli
