#pragma once

#include "sqli/error.h"
#include "sqli/transport.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace ifxsqli {

struct ConnectParams {
    std::string username;
    std::string password;
    std::string serverName;   // Informix INFORMIXSERVER (e.g. "informix")
    std::string database;     // optional; empty = no database selected
    std::string appName = "informix-odbc";
};

struct ConnectResult {
    std::string serverVersion;
    std::string serialNumber;
    std::string applicationId;
    std::uint32_t cap1 = 0;
    std::uint32_t cap2 = 0;
    std::uint32_t cap3 = 0;
};

// Performs the SQLI connect handshake (SLTYPE_CONREQ) over an already-open
// socket. See docs/protocol-notes.md "Connect handshake" for the byte layout
// this implements, confirmed against a live server capture.
[[nodiscard]] std::expected<ConnectResult, Error> performHandshake(TcpSocket& sock, const ConnectParams& params);

// Builds the raw connect-request bytes (SL header + binary blob) without
// sending them anywhere. Exposed for regression testing against a frozen
// known-good capture; see tests/sqli/test_handshake_encode.cpp.
[[nodiscard]] std::vector<std::byte> encodeConnectRequest(const ConnectParams& params);

} // namespace ifxsqli
