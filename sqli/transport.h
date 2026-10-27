#pragma once

#include "sqli/error.h"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace ifxsqli {

// A blocking TCP connection to an Informix server. Owns the socket; not
// copyable, movable. All reads/writes are exact-count (no short reads) since
// the SQLI framing above this layer depends on that.
class TcpSocket {
public:
#if defined(_WIN32)
    using SocketHandle = unsigned long long; // SOCKET, avoids pulling in winsock2.h here
    static constexpr SocketHandle kInvalid = static_cast<SocketHandle>(~0ULL);
#else
    using SocketHandle = int;
    static constexpr SocketHandle kInvalid = -1;
#endif

    TcpSocket() = default;
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    // timeoutMs > 0 bounds every blocking connect/send/recv on this socket
    // (SO_RCVTIMEO/SO_SNDTIMEO); 0 means block indefinitely. A timed-out
    // read leaves the SQLI stream in an unknown state, so callers must treat
    // the connection as dead afterward rather than retrying on it.
    [[nodiscard]] static std::expected<TcpSocket, Error> connect(const std::string& host, int port,
                                                                  int timeoutMs = 0);

    // Re-applies (or, with 0, clears) the same per-operation send/recv bound
    // on an open socket. Note it limits each individual wait, so a query the
    // server spends longer than timeoutMs executing - it sends nothing
    // meanwhile - fails too, closing the stream the same way.
    void setTimeouts(int timeoutMs);

    [[nodiscard]] std::expected<void, Error> writeAll(std::span<const std::byte> data);
    [[nodiscard]] std::expected<void, Error> readExact(std::span<std::byte> out);

    // Convenience: read exactly n bytes into a freshly-sized buffer.
    [[nodiscard]] std::expected<std::vector<std::byte>, Error> readExactN(std::size_t n);

    void close();
    [[nodiscard]] bool isOpen() const { return fd_ != kInvalid; }

private:
    explicit TcpSocket(SocketHandle fd) : fd_(fd) {}

    SocketHandle fd_ = kInvalid;
};

} // namespace ifxsqli
