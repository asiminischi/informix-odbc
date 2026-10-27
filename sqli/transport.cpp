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

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ifxsqli {

namespace {

// Opt-in raw wire trace for protocol debugging (IFX_SQLI_TRACE=1). Prints a
// hex dump of every send()/recv() to stderr - see docs/protocol-notes.md's
// "raw byte capture" debugging note.
bool traceEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("IFX_SQLI_TRACE");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

void traceBytes(const char* direction, std::span<const std::byte> data) {
    if (!traceEnabled()) return;
    std::fprintf(stderr, "[trace] %s %zu bytes:", direction, data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i % 16 == 0) std::fprintf(stderr, "\n  ");
        std::fprintf(stderr, "%02x ", std::to_integer<unsigned char>(data[i]));
    }
    std::fprintf(stderr, "\n");
}

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
#if defined(_WIN32)
    bool timedOut = code == WSAETIMEDOUT;
#else
    bool timedOut = code == EAGAIN || code == EWOULDBLOCK;
#endif
    // Only reachable with a timeout set (see TcpSocket::setTimeouts) - every
    // socket here is otherwise blocking.
    if (timedOut) return Error{ErrorKind::Transport, what + ": timed out waiting for the server"};
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

// Bounds every blocking send()/recv() (and, on Linux, connect() itself) to
// `timeoutMs` (0 clears the bound - a zero timeval/DWORD means "block
// indefinitely" on both platforms), so a server that stops answering mid-exchange - or a wire
// desync that leaves this side waiting for bytes that never come - surfaces
// as a Transport error instead of blocking the calling thread forever.
static void applyTimeouts(TcpSocket::SocketHandle fd, int timeoutMs) {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(timeoutMs);
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
#endif
    setsockopt(toNative(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(toNative(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
}

void TcpSocket::setTimeouts(int timeoutMs) {
    if (fd_ != kInvalid) applyTimeouts(fd_, timeoutMs);
}

std::expected<TcpSocket, Error> TcpSocket::connect(const std::string& host, int port, int timeoutMs) {
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
        if (timeoutMs > 0) applyTimeouts(fromNative(s), timeoutMs);
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            fd = fromNative(s);
            break;
        }
        closesocket(s);
#else
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        if (timeoutMs > 0) applyTimeouts(fromNative(s), timeoutMs);
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
    traceBytes("SEND", data);
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
    traceBytes("RECV", out);
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
