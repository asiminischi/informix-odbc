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

private:
    SqliConnection(TcpSocket sock, ConnectResult info) : sock_(std::move(sock)), info_(std::move(info)) {}

    TcpSocket sock_;
    ConnectResult info_;
};

} // namespace ifxsqli
