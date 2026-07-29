#include "sqli/handshake.h"

#include "sqli/byte_io.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace ifxsqli {

namespace {

// Fixed protocol-identifying constants. These select the wire dialect
// (onsoctcp/TCP, "SQLI 0600" framing) and must match exactly for the server
// to understand the request - see docs/protocol-notes.md.
constexpr std::string_view kFloatType = "IEEEM";
constexpr std::string_view kApplType = "sqlexec";      // padded to 12 bytes
constexpr std::string_view kApplId = "sqli";
constexpr std::string_view kProtSqliOl = "ol";          // padded to 8 bytes
constexpr std::string_view kNetTlitcp = "tlitcp";       // padded to 8 bytes
constexpr std::uint32_t kProtocolLevel = 61;
constexpr std::uint32_t kProtocolVersion = 316;         // echoed back as Cap_1; confirmed load-bearing
constexpr std::uint16_t kTagAssoc = 100;
constexpr std::uint16_t kTagAscBinary = 101;
constexpr std::uint16_t kTagApplType = 108;
constexpr std::uint16_t kTagAscInitReq = 104;
constexpr std::uint16_t kXConnect = 11;
constexpr std::uint16_t kTagAscEnv = 106;
constexpr std::uint16_t kTagAscPInfo = 107;
constexpr std::uint16_t kTagAscMisc = 116;
constexpr std::uint16_t kTagAscEot = 127;

constexpr std::uint8_t kSlTypeConReq = 1;
constexpr std::uint8_t kSlAttrSqli0600 = 60;

constexpr std::uint8_t kSlTypeConAcc = 2;
constexpr std::uint8_t kSlTypeConAccWithErr = 3;
constexpr std::uint8_t kSlTypeRedirect = 13;

constexpr std::uint16_t kTagError = 102;
constexpr std::uint16_t kTagRedirect = 103;

// The minimal environment-variable set the real driver sends; unclear which
// (if any) are strictly required, so we mirror the observed set. See
// docs/protocol-notes.md.
struct EnvVar {
    std::string_view name;
    std::string_view value;
};
constexpr EnvVar kEnvVars[] = {
    {"DBPATH", "."},
    {"CLIENT_LOCALE", "en_US.utf8"},
    {"CLNT_PAM_CAPABLE", "1"},
    {"DBDATE", "Y4MD-"},
    {"IFX_UPDDESC", "1"},
    {"NODEFDAC", "no"},
};

std::vector<std::byte> encodeConnectBlob(const ConnectParams& params) {
    ByteWriter w;
    w.u16(kTagAssoc);
    w.u16(kTagAscBinary);
    w.u32(kProtocolLevel);
    w.lenPrefixedString(kFloatType);

    w.u16(kTagApplType);
    w.fixed(kApplType, 12);

    w.lenPrefixedString("0.1.0");
    w.lenPrefixedString("IFXODBC#0000");
    w.lenPrefixedString(kApplId);

    w.u32(kProtocolVersion);
    w.u32(0);
    w.u32(0);
    w.u16(1);

    w.lenPrefixedString(params.username);
    if (params.password.empty()) {
        w.emptyLenPrefixed();
    } else {
        w.lenPrefixedString(params.password);
    }

    w.u32(kProtocolLevel);
    w.fixed(kProtSqliOl, 8);
    w.u32(1);
    w.fixed(kNetTlitcp, 8);

    w.u16(kTagAscInitReq);
    w.u16(kXConnect);

    constexpr std::uint32_t kStmtOptions = 3;
    w.u32(kStmtOptions);

    w.lenPrefixedString(params.serverName);
    w.emptyLenPrefixed(); // database name: sent as a separate message post-connect, see notes

    w.u16(0);
    w.u16(0);
    w.u16(0);
    w.u16(0);

    w.u16(kTagAscEnv);
    w.u16(static_cast<std::uint16_t>(std::size(kEnvVars)));
    for (const auto& ev : kEnvVars) {
        w.lenPrefixedString(ev.name);
        w.lenPrefixedString(ev.value);
    }

    w.u16(kTagAscPInfo);
    w.u32(0);
#if defined(_WIN32)
    std::uint32_t pid = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    std::uint32_t pid = static_cast<std::uint32_t>(getpid());
#endif
    w.u32(pid);
    w.u32(1); // "thread id" - single-threaded per connection at this layer
    w.lenPrefixedString("localhost");
    w.u16(0);
    w.lenPrefixedString(".");

    const std::string& appName = params.appName;
    w.u16(kTagAscMisc);
    w.u16(static_cast<std::uint16_t>(10 + appName.size() + 1));
    w.u32(0);
    w.u32(0);
    w.lenPrefixedString(appName);

    w.u16(kTagAscEot);

    return std::vector<std::byte>(w.view().begin(), w.view().end());
}

} // namespace

std::vector<std::byte> encodeConnectRequest(const ConnectParams& params) {
    auto blob = encodeConnectBlob(params);

    ByteWriter header;
    header.u16(static_cast<std::uint16_t>(blob.size() + 6));
    header.u8(kSlTypeConReq);
    header.u8(kSlAttrSqli0600);
    header.u16(0);

    std::vector<std::byte> out(header.view().begin(), header.view().end());
    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

namespace {

std::expected<void, Error> sendConnectRequest(TcpSocket& sock, const ConnectParams& params) {
    return sock.writeAll(encodeConnectRequest(params));
}

std::expected<ConnectResult, Error> recvConnectResponse(TcpSocket& sock) {
    auto lenBytes = sock.readExactN(2);
    if (!lenBytes) return std::unexpected(lenBytes.error());
    ByteReader lenReader(*lenBytes);
    std::uint16_t totalLen = lenReader.u16();
    if (totalLen < 2) return std::unexpected(Error{ErrorKind::Protocol, "implausible response length"});

    auto body = sock.readExactN(totalLen - 2);
    if (!body) return std::unexpected(body.error());

    try {
        ByteReader r(*body);
        std::uint8_t slType = r.u8();

        if (slType == kSlTypeRedirect) {
            return std::unexpected(Error{ErrorKind::Server, "server requested HA/connection-manager redirect (unsupported)"});
        }
        if (slType != kSlTypeConAcc && slType != kSlTypeConAccWithErr) {
            return std::unexpected(Error{ErrorKind::Protocol, "unexpected SLType in connect response"});
        }

        r.skip(3); // slAttribute + slOptions
        if (r.u16() != kTagAssoc || r.u16() != kTagAscBinary) {
            return std::unexpected(Error{ErrorKind::Protocol, "missing ASSOC/ASCBINARY marker in connect response"});
        }

        r.skip(4); // protocol level marker
        std::uint16_t floatLen = r.u16();
        r.skip(floatLen);

        if (r.u16() != kTagApplType) {
            return std::unexpected(Error{ErrorKind::Protocol, "missing app-type tag in connect response"});
        }
        r.skip(12);

        ConnectResult result;
        result.serverVersion = r.lenPrefixedString();
        result.serialNumber = r.lenPrefixedString();
        result.applicationId = r.lenPrefixedString();
        result.cap1 = r.u32();
        result.cap2 = r.u32();
        result.cap3 = r.u32();

        r.skip(2);
        if (std::uint16_t lenA = r.u16(); lenA > 0) r.skip(lenA);
        if (std::uint16_t lenB = r.u16(); lenB > 0) r.skip(lenB);
        r.skip(24);

        // This trailing tag/diagnostic block is only load-bearing when the
        // outer envelope is SLType 3 (accepted-with-error); for a plain
        // SLType 2 accept the server always sends it - even on a fully
        // successful connection, as confirmed by capture - so it must not
        // fail the connection here. See docs/protocol-notes.md.
        std::uint16_t finalTag = r.u16();
        std::uint16_t svcError = 0;
        if (finalTag == kTagError) {
            r.skip(6);
            svcError = r.u16();
            (void)r.u16(); // osError, unused here
            (void)r.u16(); // warnings, unused here
        } else if (finalTag != kTagAscEot && finalTag != kTagRedirect) {
            return std::unexpected(Error{ErrorKind::Protocol, "unrecognized trailing tag in connect response"});
        }

        if (slType == kSlTypeConAccWithErr && svcError != 0) {
            return std::unexpected(Error{ErrorKind::Server, "server rejected connection (svcError=" +
                                                                  std::to_string(svcError) + ")"});
        }
        return result;
    } catch (const ProtocolError& e) {
        return std::unexpected(Error{ErrorKind::Protocol, e.what()});
    }
}

} // namespace

std::expected<ConnectResult, Error> performHandshake(TcpSocket& sock, const ConnectParams& params) {
    if (auto r = sendConnectRequest(sock, params); !r) return std::unexpected(r.error());
    return recvConnectResponse(sock);
}

} // namespace ifxsqli
