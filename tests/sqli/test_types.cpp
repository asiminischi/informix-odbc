#include <doctest/doctest.h>

#include "sqli/types.h"

#include <cctype>
#include <span>
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

TEST_CASE("BOOLEAN") {
    // Captured live from a 3-row bool_probe fixture (true/false/NULL) - see
    // decodeBoolean's doc comment in sqli/types.h for the shape this
    // confirms (matches the decompiled IfxBoolean.toIfxTuple() byte-for-byte).
    auto t = decodeBoolean(fromHex("000000000101"));
    REQUIRE(t.has_value());
    REQUIRE(t->has_value());
    CHECK(**t == true);

    auto f = decodeBoolean(fromHex("000000000100"));
    REQUIRE(f.has_value());
    REQUIRE(f->has_value());
    CHECK(**f == false);

    auto null = decodeBoolean(fromHex("0100000000"));
    REQUIRE(null.has_value());
    CHECK(!null->has_value());
}

TEST_CASE("FLOAT") {
    // Captured live from a 3-row float_probe fixture (3.14159/-0.001/NULL).
    auto v = decodeFloat(fromHex("400921f9f01b866e"));
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK(**v == doctest::Approx(3.14159));

    auto v2 = decodeFloat(fromHex("bf50624dd2f1a9fc"));
    REQUIRE(v2.has_value());
    REQUIRE(v2->has_value());
    CHECK(**v2 == doctest::Approx(-0.001));

    auto null = decodeFloat(fromHex("ffffffffffffffff"));
    REQUIRE(null.has_value());
    CHECK(!null->has_value());
}

TEST_CASE("SMALLFLOAT") {
    // Captured live from the same float_probe fixture (2.5/-100.25/NULL).
    auto v = decodeSmallFloat(fromHex("40200000"));
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK(**v == doctest::Approx(2.5f));

    auto v2 = decodeSmallFloat(fromHex("c2c88000"));
    REQUIRE(v2.has_value());
    REQUIRE(v2->has_value());
    CHECK(**v2 == doctest::Approx(-100.25f));

    auto null = decodeSmallFloat(fromHex("ffffffff"));
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

namespace {

// Round-trips a decimal literal through encodeDecimal then decodeDecimal
// (stripping encodeDecimal's SQ_BIND-only 2-byte length prefix first,
// which decodeDecimal's TUPLE-format input never has), using exactly the
// mantissa width encodeDecimal itself chose - it deliberately doesn't pad
// to any declared column precision (see sqli/types.h), so a fixed
// precision argument would be wrong for one of these test cases and right
// for another.
std::string roundTripDecimal(std::string_view literal) {
    auto encoded = encodeDecimal(literal);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->data.size() >= 3);
    std::span<const std::byte> withoutLengthPrefix(encoded->data.data() + 2, encoded->data.size() - 2);
    unsigned mantissaBytes = static_cast<unsigned>(withoutLengthPrefix.size() - 1);
    auto decoded = decodeDecimal(withoutLengthPrefix, mantissaBytes * 2, encoded->scale);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    return **decoded;
}

} // namespace

TEST_CASE("DECIMAL bind encoding round-trips through decodeDecimal") {
    CHECK(roundTripDecimal("12345.67") == "12345.67");
    CHECK(roundTripDecimal("0.01") == "0.01");
    CHECK(roundTripDecimal("-99.99") == "-99.99");
    CHECK(roundTripDecimal("-12345.67") == "-12345.67");
    CHECK(roundTripDecimal("-0.01") == "-0.01");
    CHECK(roundTripDecimal("-100.00") == "-100.00");
    CHECK(roundTripDecimal("0.00") == "0.00");
    CHECK(roundTripDecimal("100") == "100");
}

TEST_CASE("DECIMAL bind encoding matches hand-derived bytes for -0.01") {
    // Independently derived from convFromBigDecimal's algorithm (see
    // sqli/types.h's encodeDecimal doc comment): unscaledValue "1", scale
    // 2, exp = 1 - 2 = -1 (odd) -> prepend a zero digit, trueExp = 0.
    // Mantissa digit before complementing: [01]. Positive low7 = 64 -> 0x40.
    // Negative: exponent = 0x7F - 0x40 = 0x3F; mantissa hundred's-complement
    // of [1] is [99]=0x63 (99-1=98, +1 carry -> 99).
    auto encoded = encodeDecimal("-0.01");
    REQUIRE(encoded.has_value());
    CHECK(encoded->scale == 2);
    CHECK(encoded->data == fromHex("0002 3f 63"));
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

TEST_CASE("DATETIME YEAR TO SECOND bind encoding") {
    constexpr std::uint32_t kEncLen = (14u << 8) | 0x0A;

    // encodeDatetime's only difference from decodeDatetime's TUPLE-format
    // shape is a 2-byte length prefix (dec_ndgts + 1 = 8) - so this must
    // produce exactly the same exponent+mantissa bytes as the confirmed
    // live decode test above for the same value, just with that prefix
    // prepended.
    DatetimeValue value{2026, 7, 29, 12, 34, 56};
    auto encoded = encodeDatetime(value, kEncLen);
    REQUIRE(encoded.has_value());
    CHECK(*encoded == fromHex("0008 c7141a071d0c2238"));

    auto decoded = decodeDatetime(std::span<const std::byte>(encoded->data() + 2, encoded->size() - 2), kEncLen);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    CHECK((*decoded)->year == 2026);
    CHECK((*decoded)->month == 7);
    CHECK((*decoded)->day == 29);
    CHECK((*decoded)->hour == 12);
    CHECK((*decoded)->minute == 34);
    CHECK((*decoded)->second == 56);
}

TEST_CASE("DATETIME YEAR TO FRACTION(5)") {
    // encodedLength = (19 << 8) | 0x0F: 19 total digits (14 whole-field +
    // 5 fraction), qualifier start=YEAR(0) end=FRACTION(5)=15. Bytes are a
    // live wire capture (IFX_SQLI_TRACE) of "2024-06-15 13:45:52.12345":
    // exponent 0xc7 encodes 7 (the YEAR..SECOND pair count, *not* the
    // total mantissa byte count of 10 - DATETIME's implicit "decimal
    // point" always sits right after SECOND, matching DECIMAL's own
    // exponent semantics), then 10 mantissa bytes, the last 3 of which are
    // the FRACTION(5) pairs [12][34][50] - the trailing digit of the
    // 5-digit (odd) fraction is zero-padded on the wire, not left-padded.
    constexpr std::uint32_t kEncLen = (19u << 8) | 0x0F;

    auto v = decodeDatetime(fromHex("c71418060f0d2d340c2232"), kEncLen);
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    CHECK((*v)->year == 2024);
    CHECK((*v)->month == 6);
    CHECK((*v)->day == 15);
    CHECK((*v)->hour == 13);
    CHECK((*v)->minute == 45);
    CHECK((*v)->second == 52);
    CHECK((*v)->fraction == 12345);
    CHECK((*v)->fractionDigits == 5);

    // An even fraction digit count needs no trailing zero-pad byte.
    auto v2 = decodeDatetime(fromHex("c71418060f0d2d340c22"), (18u << 8) | 0x0E); // .1234
    REQUIRE(v2.has_value());
    REQUIRE(v2->has_value());
    CHECK((*v2)->fraction == 1234);
    CHECK((*v2)->fractionDigits == 4);
}

TEST_CASE("DATETIME YEAR TO FRACTION(5) bind encoding") {
    constexpr std::uint32_t kEncLen = (19u << 8) | 0x0F;

    // Same value as the live decode test above - encodeDatetime must
    // reproduce the exact same exponent+mantissa bytes, confirmed live via
    // a bind -> server storage -> plain SELECT -> decode round trip
    // (tests/integration/test_bind.cpp).
    DatetimeValue value{2024, 6, 15, 13, 45, 52, 12345, 5};
    auto encoded = encodeDatetime(value, kEncLen);
    REQUIRE(encoded.has_value());
    CHECK(*encoded == fromHex("000b c71418060f0d2d340c2232"));

    auto decoded = decodeDatetime(std::span<const std::byte>(encoded->data() + 2, encoded->size() - 2), kEncLen);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    CHECK((*decoded)->fraction == 12345);
    CHECK((*decoded)->fractionDigits == 5);
}

TEST_CASE("INTERVAL YEAR TO MONTH") {
    // Four live wire captures (IFX_SQLI_TRACE) via ifx_probe against
    // throwaway fixture tables, covering every YEAR-MONTH-class qualifier
    // shape and both signs:

    // INTERVAL YEAR(4) TO MONTH holding 3-07 (3 years, 7 months): the
    // leading field's high digit-pair is zero, so the wire's floating
    // representation strips it - exponent 0xC6's biased magnitude (6) is
    // one less than the unnormalized formula's 7 (see types.h's doc
    // comment for the exact math), and the mantissa's freed slot is
    // backfilled with a trailing zero pair: [3,7,0], not the naive
    // [0,3,7].
    {
        constexpr std::uint32_t kEncLen = (6u << 8) | 0x02; // totalDigits=6, YEAR(0) to MONTH(2)
        auto v = decodeIntervalYearMonth(fromHex("c6 03 07 00"), kEncLen);
        REQUIRE(v.has_value());
        REQUIRE(v->has_value());
        CHECK(**v == 43); // 3*12 + 7
    }
    // Same qualifier and magnitude, negative: hundred's-complement decodes
    // to the identical digit array and exponent as the positive case above
    // (confirmed live - this is what the sign bit alone should flip).
    {
        constexpr std::uint32_t kEncLen = (6u << 8) | 0x02;
        auto v = decodeIntervalYearMonth(fromHex("39 60 5d 00"), kEncLen);
        REQUIRE(v.has_value());
        REQUIRE(v->has_value());
        CHECK(**v == -43);
    }
    // INTERVAL YEAR(2) TO MONTH holding 47-07: the leading field (47)
    // fills its full 2-digit width, so no stripping occurs - exponent's
    // biased magnitude matches the unnormalized formula directly, and the
    // mantissa is the field values as-is: [47, 7].
    {
        constexpr std::uint32_t kEncLen = (4u << 8) | 0x02; // totalDigits=4, YEAR(0) to MONTH(2)
        auto v = decodeIntervalYearMonth(fromHex("c6 2f 07"), kEncLen);
        REQUIRE(v.has_value());
        REQUIRE(v->has_value());
        CHECK(**v == 571); // 47*12 + 7
    }
    // INTERVAL MONTH(3) TO MONTH holding 43: a single total-months field,
    // no years/12 split (the decompiled reference's separate start==MONTH
    // branch). totalDigits=3 is odd - this is the case that caught a real
    // bug: the extra padding digit slot from ceil(3/2)*2=4 raw digits has
    // to be dropped from the *front* ("043" -> 43), not the back ("004"
    // -> 4, wrong).
    {
        constexpr std::uint32_t kEncLen = (3u << 8) | 0x22; // totalDigits=3, MONTH(2) to MONTH(2)
        auto v = decodeIntervalYearMonth(fromHex("c5 2b 00"), kEncLen);
        REQUIRE(v.has_value());
        REQUIRE(v->has_value());
        CHECK(**v == 43);
    }
    {
        constexpr std::uint32_t kEncLen = (3u << 8) | 0x22;
        auto v = decodeIntervalYearMonth(fromHex("3a 39 00"), kEncLen);
        REQUIRE(v.has_value());
        REQUIRE(v->has_value());
        CHECK(**v == -43);
    }
    // INTERVAL YEAR(3) TO YEAR holding 43 (years, no month remainder field
    // at all - end==YEAR not MONTH) = 516 months. Also odd totalDigits,
    // confirming the front-padding fix generalizes beyond MONTH TO MONTH.
    {
        constexpr std::uint32_t kEncLen = (3u << 8) | 0x00; // totalDigits=3, YEAR(0) to YEAR(0)
        auto v = decodeIntervalYearMonth(fromHex("c6 2b 00"), kEncLen);
        REQUIRE(v.has_value());
        REQUIRE(v->has_value());
        CHECK(**v == 516); // 43*12
    }
    // A genuine zero value, distinct from NULL: like DECIMAL, INTERVAL's
    // canonical zero is exponent 0x80 (sign bit set, magnitude 0) with an
    // all-zero mantissa, not all-zero bytes - confirmed live via `INTERVAL
    // MONTH(3) TO MONTH` holding 0. Worth checking explicitly: the
    // all-zero-bytes NULL check below could otherwise have silently
    // swallowed a real zero if this convention didn't hold.
    {
        constexpr std::uint32_t kEncLen = (3u << 8) | 0x22; // totalDigits=3, MONTH(2) to MONTH(2)
        auto v = decodeIntervalYearMonth(fromHex("80 00 00"), kEncLen);
        REQUIRE(v.has_value());
        REQUIRE(v->has_value());
        CHECK(**v == 0);
    }
    // Multi-pair stripping: INTERVAL YEAR(4) TO MONTH holding 0-05 (zero
    // years, both leading digit-pairs zero) - a bigger un-normalization
    // shift than the single-pair case above, confirmed to still land on
    // the right digits.
    {
        constexpr std::uint32_t kEncLen = (6u << 8) | 0x02; // totalDigits=6, YEAR(0) to MONTH(2)
        auto v = decodeIntervalYearMonth(fromHex("c5 05 00 00"), kEncLen);
        REQUIRE(v.has_value());
        REQUIRE(v->has_value());
        CHECK(**v == 5);
    }

    auto null = decodeIntervalYearMonth(fromHex("00 00 00 00"), (6u << 8) | 0x02);
    REQUIRE(null.has_value());
    CHECK(!null->has_value());
}
