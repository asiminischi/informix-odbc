#include "sqli/connection.h"
#include "sqli/version.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    const auto v = ifxsqli::version();
    std::printf("ifx_probe (ifx_sqli %d.%d.%d)\n", v.major, v.minor, v.patch);

    if (argc < 5) {
        std::printf("usage: ifx_probe <host> <port> <username> <password> [servername] [database]\n");
        return argc == 1 ? 0 : 1;
    }

    std::string host = argv[1];
    int port = std::atoi(argv[2]);
    std::string username = argv[3];
    std::string password = argv[4];
    std::string serverName = argc > 5 ? argv[5] : "informix";
    std::string database = argc > 6 ? argv[6] : "";

    ifxsqli::ConnectParams params;
    params.username = username;
    params.password = password;
    params.serverName = serverName;
    params.database = database;

    std::printf("connecting to %s:%d (server=%s, user=%s)...\n", host.c_str(), port, serverName.c_str(),
                username.c_str());

    auto conn = ifxsqli::SqliConnection::connect(host, port, params);
    if (!conn) {
        std::fprintf(stderr, "connect failed: %s\n", conn.error().message.c_str());
        return 1;
    }

    const auto& info = conn->serverInfo();
    std::printf("connected.\n");
    std::printf("  server version : %s\n", info.serverVersion.c_str());
    std::printf("  serial number  : %s\n", info.serialNumber.c_str());
    std::printf("  application id : %s\n", info.applicationId.c_str());
    std::printf("  capabilities   : %u %u %u\n", info.cap1, info.cap2, info.cap3);

    auto disc = conn->disconnectOrderly();
    if (!disc) {
        std::fprintf(stderr, "disconnect failed: %s\n", disc.error().message.c_str());
        return 1;
    }
    std::printf("disconnected cleanly.\n");
    return 0;
}
