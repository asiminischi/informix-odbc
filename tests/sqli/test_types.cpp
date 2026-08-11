#include <doctest/doctest.h>

#include "sqli/types.h"

#include <cctype>
#include <vector>

namespace {

std::vector<std::byte> fromHex(std::string_view hexWithSpaces) {
    std::string hex;
    hex.reserve(hexWithSpaces.size());
    for (char c : hexWithSpaces) {
        if (!std::isspace(static_cast<unsigned char>(c))) hex.push_back(c);
    }
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

} // namespace

using namespace ifxsqli;

// Byte sequences below are column slices taken verbatim from a live capture
// against IDS 14.10.FC7W1DE (see docs/protocol-notes.md), except where noted
// (e.g. the empty-string VARCHAR case, which is a synthetic edge case never
// observed live).

TEST_CASE("SMALLINT") {
    auto v = decodeSmallInt(fromHex("3039"));
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK(**v == 12345);

    auto neg = decodeSmallInt(fromHex("cfc7"));
    REQUIRE(neg.has_value());
    CHECK(**neg == -12345);

    auto null = decodeSmallInt(fromHex("8000"));
    REQUIRE(null.has_value());
    CHECK(!null->has_value());

    // encodeSmallInt is confirmed (see sqli/types.h) to be the same plain
    // big-endian shape as the TUPLE decoder above, so it must round-trip
    // through decodeSmallInt exactly.
    CHECK(encodeSmallInt(12345) == fromHex("3039"));
    CHECK(encodeSmallInt(-12345) == fromHex("cfc7"));
}

TEST_CASE("INTEGER") {
    auto v = decodeInteger(fromHex("499602d2"));
    REQUIRE(v.has_value());
    CHECK(**v == 1234567890);

    auto neg = decodeInteger(fromHex("80000288"));
    REQUIRE(neg.has_value());
    CHECK(**neg == -2147483000);

    auto null = decodeInteger(fromHex("80000000"));
    REQUIRE(null.has_value());
    CHECK(!null->has_value());
}

TEST_CASE("BIGINT") {
    auto v = decodeBigInt(fromHex("0000704886 0ddf79"));
    REQUIRE(v.has_value());
    CHECK(**v == 123456789012345);

    auto neg = decodeBigInt(fromHex("ffff8fb779f22087"));
    REQUIRE(neg.has_value());
    CHECK(**neg == -123456789012345);

    auto null = decodeBigInt(fromHex("8000000000000000"));
    REQUIRE(null.has_value());
    CHECK(!null->has_value());

    // encodeBigInt is confirmed (see sqli/types.h) to be the same plain
    // big-endian shape as the TUPLE decoder above, so it must round-trip
    // through decodeBigInt exactly.
    CHECK(encodeBigInt(123456789012345) == fromHex("0000704886 0ddf79"));
    CHECK(encodeBigInt(-123456789012345) == fromHex("ffff8fb779f22087"));
}

TEST_CASE("CHAR") {
    auto v = decodeChar(fromHex("68656c6c6f2020202020")); // "hello     "
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK(**v == "hello     ");

    auto null = decodeChar(fromHex("00202020202020202020"));
    REQUIRE(null.has_value());
    CHECK(!null->has_value());
}

TEST_CASE("VARCHAR") {
    auto v = decodeVarchar(fromHex("0d776f726c642076617263686172")); // len 13, "world varchar"
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK(**v == "world varchar");

    auto empty = decodeVarchar(fromHex("00"));
    REQUIRE(empty.has_value());
    REQUIRE(empty->has_value());
    CHECK(**empty == "");

    auto null = decodeVarchar(fromHex("0100"));
    REQUIRE(null.has_value());
    CHECK(!null->has_value());
}

TEST_CASE("DATE") {
    auto v = decodeDateUnixDays(fromHex("0000b497")); // 2026-07-29, day count 46231
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK(**v == 46231 - 25568);

    auto v2 = decodeDateUnixDays(fromHex("00008ead")); // 2000-01-01, day count 36525
    REQUIRE(v2.has_value());
    CHECK(**v2 == 36525 - 25568);

    auto null = decodeDateUnixDays(fromHex("80000000"));
    REQUIRE(null.has_value());
    CHECK(!null->has_value());
}

TEST_CASE("DECIMAL positive") {
    auto v = decodeDecimal(fromHex("c301172d4300"), 10, 2); // 12345.67
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK(**v == "12345.67");

    auto small = decodeDecimal(fromHex("c00100000000"), 10, 2); // 0.01
    REQUIRE(small.has_value());
    CHECK(**small == "0.01");
}

TEST_CASE("DECIMAL negative") {
    auto v = decodeDecimal(fromHex("3e000100 0000"), 10, 2); // -99.99
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK(**v == "-99.99");

    auto v2 = decodeDecimal(fromHex("3c624c362100"), 10, 2); // -12345.67
    REQUIRE(v2.has_value());
    CHECK(**v2 == "-12345.67");

    auto v3 = decodeDecimal(fromHex("3f630000 0000"), 10, 2); // -0.01
    REQUIRE(v3.has_value());
    CHECK(**v3 == "-0.01");

    auto v4 = decodeDecimal(fromHex("3d630000 0000"), 10, 2); // -100
    REQUIRE(v4.has_value());
    CHECK(**v4 == "-100.00");
}

TEST_CASE("DECIMAL zero and NULL") {
    auto zero = decodeDecimal(fromHex("800000 0000 00"), 10, 2);
    REQUIRE(zero.has_value());
    REQUIRE(zero->has_value());
    CHECK(**zero == "0.00");

    auto null = decodeDecimal(fromHex("000000000000"), 10, 2);
    REQUIRE(null.has_value());
    CHECK(!null->has_value());
}

TEST_CASE("DATETIME YEAR TO SECOND") {
    // encodedLength = (14 << 8) | 0x0A: 14 total digits, qualifier
    // start=YEAR(0) end=SECOND(10).
    constexpr std::uint32_t kEncLen = (14u << 8) | 0x0A;

    auto v = decodeDatetime(fromHex("c7141a071d0c2238"), kEncLen); // 2026-07-29 12:34:56
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK((*v)->year == 2026);
    CHECK((*v)->month == 7);
    CHECK((*v)->day == 29);
    CHECK((*v)->hour == 12);
    CHECK((*v)->minute == 34);
    CHECK((*v)->second == 56);

    auto v2 = decodeDatetime(fromHex("c71400010100 0000"), kEncLen); // 2000-01-01 00:00:00
    REQUIRE(v2.has_value());
    REQUIRE(v2->has_value());
    CHECK((*v2)->year == 2000);
    CHECK((*v2)->month == 1);
    CHECK((*v2)->day == 1);
    CHECK((*v2)->hour == 0);
    CHECK((*v2)->minute == 0);
    CHECK((*v2)->second == 0);

    auto null = decodeDatetime(fromHex("0000000000000000"), kEncLen);
    REQUIRE(null.has_value());
    CHECK(!null->has_value());
}
