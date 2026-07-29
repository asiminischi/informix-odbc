#include <doctest/doctest.h>

#include "sqli/handshake.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace {

std::vector<std::byte> fromHex(std::string_view hex) {
    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        return 10 + (std::tolower(c) - 'a');
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(std::byte(nibble(hex[i]) * 16 + nibble(hex[i + 1])));
    }
    return out;
}

// The process-info block (tag 107) embeds the live PID, which naturally
// differs between the frozen capture and this test run. Zero those 4 bytes
// in-place (identified by the fixed "tag 107 + reserved u32(0)" prefix that
// immediately precedes them) so the rest of the encoding can still be
// compared byte-for-byte.
void zeroPidField(std::vector<std::byte>& buf) {
    const std::byte marker[] = {std::byte{0x00}, std::byte{0x6b}, std::byte{0x00},
                                 std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    auto it = std::search(buf.begin(), buf.end(), std::begin(marker), std::end(marker));
    REQUIRE(it != buf.end());
    auto pidStart = it + 6;
    REQUIRE(std::distance(pidStart, buf.end()) >= 4);
    std::fill_n(pidStart, 4, std::byte{0});
}

// Frozen regression fixture: the exact connect-request bytes our own encoder
// produced and sent to a live Informix server (IDS 14.10) during Phase 1
// development, captured via tests/fixtures/probe_db_select_and_exit.txt. The
// server accepted this request and completed the handshake successfully.
constexpr std::string_view kCapturedRequestHex =
        "014f013c0000006400650000003d0006494545454d00006c73716c6578656300"
        "000000000006302e312e3000000d4946584f444243233030303000000573716c"
        "69000000013c000000000000000000010009696e666f726d6978000007696e34"
        "6d6978000000003d6f6c00000000000000000001746c6974637000000068000b"
        "000000030009696e666f726d69780000000000000000000000006a0006000744"
        "42504154480000022e00000e434c49454e545f4c4f43414c4500000b656e5f55"
        "532e75746638000011434c4e545f50414d5f43415041424c4500000231000007"
        "44424441544500000659344d442d00000c4946585f5550444445534300000231"
        "0000094e4f4445464441430000036e6f00006b00000000000041540000000100"
        "0a6c6f63616c686f737400000000022e00007400180000000000000000000e69"
        "6e666f726d69782d6f64626300007f";

} // namespace

// Guards against accidental changes to the wire encoding (the live PID field
// is masked out since it's inherently non-reproducible).
TEST_CASE("encodeConnectRequest matches the known-good live capture") {
    ifxsqli::ConnectParams params;
    params.username = "informix";
    params.password = "in4mix";
    params.serverName = "informix";
    params.appName = "informix-odbc";

    auto actual = ifxsqli::encodeConnectRequest(params);
    auto expected = fromHex(kCapturedRequestHex);

    zeroPidField(actual);
    zeroPidField(expected);

    REQUIRE(actual.size() == expected.size());
    CHECK(actual == expected);
}
