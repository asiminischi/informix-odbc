#include "sqli/version.h"

#include <cstdio>

int main() {
    const auto v = ifxsqli::version();
    std::printf("ifx_probe (ifx_sqli %d.%d.%d)\n", v.major, v.minor, v.patch);
    std::printf("usage: ifx_probe <host> <port> <username> <password> [servername] [database]\n");
    std::printf("(handshake support not implemented yet - Phase 1)\n");
    return 0;
}
