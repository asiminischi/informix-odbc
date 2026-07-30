#include "sqli/transport.h"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cstring>

namespace ifxsqli {

namespace {

#if defined(_WIN32)
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() { WSACleanup(); }
};

void ensureWinsock() {
    static WinsockInit init;
    (void)init;
}

SOCKET toNative(TcpSocket::SocketHandle h) { return static_cast<SOCKET>(h); }
TcpSocket::SocketHandle fromNative(SOCKET s) { return static_cast<TcpSocket::SocketHandle>(s); }
#else
int toNative(TcpSocket::SocketHandle h) { return h; }
TcpSocket::SocketHandle fromNative(int s) { return s; }
#endif

Error lastSocketError(const std::string& what) {
#if defined(_WIN32)
    int code = WSAGetLastError();
#else
    int code = errno;
#endif
    return Error{ErrorKind::Transport, what + " (errno " + std::to_string(code) + ")"};
}

} // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalid; }

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = kInvalid;
    }
    return *this;
}

std::expected<TcpSocket, Error> TcpSocket::connect(const std::string& host, int port) {
#if defined(_WIN32)
    ensureWinsock();
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        return std::unexpected(Error{ErrorKind::Transport, "DNS/address resolution failed for " + host});
    }

    SocketHandle fd = kInvalid;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
#if defined(_WIN32)
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            fd = fromNative(s);
            break;
        }
        closesocket(s);
#else
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        if (::connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
            fd = fromNative(s);
            break;
        }
        ::close(s);
#endif
    }
    freeaddrinfo(result);

    if (fd == kInvalid) {
        return std::unexpected(lastSocketError("connect() failed for " + host + ":" + portStr));
    }

    // TCP_NODELAY: the SQLI framing is small-message/round-trip heavy.
    int one = 1;
    setsockopt(toNative(fd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    return TcpSocket(fd);
}

std::expected<void, Error> TcpSocket::writeAll(std::span<const std::byte> data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        int n = send(toNative(fd_), reinterpret_cast<const char*>(data.data() + sent),
                      static_cast<int>(data.size() - sent), 0);
        if (n == SOCKET_ERROR) return std::unexpected(lastSocketError("send() failed"));
#else
        ssize_t n = ::send(toNative(fd_), data.data() + sent, data.size() - sent, 0);
        if (n < 0) return std::unexpected(lastSocketError("send() failed"));
#endif
        sent += static_cast<std::size_t>(n);
    }
    return {};
}

std::expected<void, Error> TcpSocket::readExact(std::span<std::byte> out) {
    std::size_t received = 0;
    while (received < out.size()) {
#if defined(_WIN32)
        int n = recv(toNative(fd_), reinterpret_cast<char*>(out.data() + received),
                      static_cast<int>(out.size() - received), 0);
        if (n == SOCKET_ERROR) return std::unexpected(lastSocketError("recv() failed"));
#else
        ssize_t n = ::recv(toNative(fd_), out.data() + received, out.size() - received, 0);
        if (n < 0) return std::unexpected(lastSocketError("recv() failed"));
#endif
        if (n == 0) {
            return std::unexpected(Error{ErrorKind::Transport, "connection closed by peer"});
        }
        received += static_cast<std::size_t>(n);
    }
    return {};
}

std::expected<std::vector<std::byte>, Error> TcpSocket::readExactN(std::size_t n) {
    std::vector<std::byte> buf(n);
    auto r = readExact(buf);
    if (!r) return std::unexpected(r.error());
    return buf;
}

void TcpSocket::close() {
    if (fd_ != kInvalid) {
#if defined(_WIN32)
        closesocket(toNative(fd_));
#else
        ::close(toNative(fd_));
#endif
        fd_ = kInvalid;
    }
}

} // namespace ifxsqli
