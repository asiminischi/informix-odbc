#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "driver/bind_encode.h"
#include "sqli/types.h"

using namespace ifxodbc;

// intervalEncodedLengthFor reconstructs the wire qualifier from
// SQLBindParameter's ParameterType/ColumnSize/DecimalDigits triple - the
// one piece of new logic in the INTERVAL bind path that isn't already
// covered by sqli::encodeIntervalYearMonth/encodeIntervalDayFraction's own
// live-verified tests (tests/sqli/test_types.cpp). Expected values below
// are the exact collength values driver/catalog.cpp's SQLColumns work
// captured live against real INTERVAL columns (see docs/protocol-notes.md's
// "INTERVAL in SQLColumns" section) - if this function reconstructs the
// same qualifier a live column of that shape actually has, it's right.
TEST_CASE("intervalEncodedLengthFor matches live-captured qualifiers") {
    // INTERVAL YEAR(4) TO MONTH - collength 1538 confirmed live.
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_YEAR_TO_MONTH, 4, 0) == 1538u);
    // INTERVAL DAY(4) TO FRACTION(3) - collength 3405 confirmed live.
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_DAY_TO_SECOND, 4, 3) == 3405u);
    // INTERVAL MONTH(2) TO MONTH - collength 546 confirmed live.
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_MONTH, 2, 0) == 546u);
}

TEST_CASE("intervalEncodedLengthFor covers every qualifier shape") {
    // YEAR-MONTH class.
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_YEAR, 4, 0) == ((4u << 8) | 0x00));
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_MONTH, 3, 0) == ((3u << 8) | 0x22));

    // DAY-anchored.
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_DAY, 2, 0) == ((2u << 8) | 0x44));
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_DAY_TO_HOUR, 2, 0) == ((4u << 8) | 0x46));
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_DAY_TO_MINUTE, 2, 0) == ((6u << 8) | 0x48));
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_DAY_TO_SECOND, 2, 0) == ((8u << 8) | 0x4A));

    // HOUR/MINUTE/SECOND-anchored, matching the qualifiers this session's
    // formatIntervalDayFraction fix live-captured (fmt_probe).
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_HOUR, 3, 0) == ((3u << 8) | 0x66));
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_HOUR_TO_MINUTE, 3, 0) == ((5u << 8) | 0x68));
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_MINUTE, 3, 0) == ((3u << 8) | 0x88));
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_MINUTE_TO_SECOND, 3, 0) == ((5u << 8) | 0x8A));
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_SECOND, 3, 0) == ((3u << 8) | 0xAA));
    // SECOND(2) TO FRACTION(3) - matches fmt_probe's col_ssf.
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_SECOND, 2, 3) == ((5u << 8) | 0xAD));
}

TEST_CASE("intervalEncodedLengthFor rejects malformed input") {
    // Not a recognized SQL_INTERVAL_* constant.
    CHECK(!intervalEncodedLengthFor(SQL_INTEGER, 4, 0).has_value());
    // ColumnSize outside Informix's own 1-9 leading-field digit-width limit.
    CHECK(!intervalEncodedLengthFor(SQL_INTERVAL_YEAR, 0, 0).has_value());
    CHECK(!intervalEncodedLengthFor(SQL_INTERVAL_YEAR, 10, 0).has_value());
    // DecimalDigits on a shape with no seconds field at all is silently
    // ignored, not rejected - matches this driver's existing lenient-
    // encode conventions elsewhere (e.g. DATE's unvalidated
    // precisionField).
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_YEAR_TO_MONTH, 4, 3) == 1538u);
    // Informix caps FRACTION at 5 digits - past that the qualifier byte's
    // low nibble (10 + fractionDigits) would overflow past 0xF and
    // silently corrupt the wire qualifier, so this is rejected rather than
    // truncated.
    CHECK(intervalEncodedLengthFor(SQL_INTERVAL_SECOND, 2, 5).has_value());
    CHECK(!intervalEncodedLengthFor(SQL_INTERVAL_SECOND, 2, 6).has_value());
}

// encodeBindParam's SQL_INTERVAL_STRUCT read path isn't exercised by
// intervalEncodedLengthFor's tests above (that function only computes the
// wire qualifier, never touches the value) - these two cases cover the
// value-reading logic that has no other durable test (the throwaway ODBC
// probe used to verify it live this session isn't part of the repo). Both
// compare encodeBindParam's output against calling sqli's own encoder
// directly with the expected already-reduced value, so a regression in
// either the SQL_INTERVAL_STRUCT field-reading or the sqli encoder itself
// would fail this, not just a drift between two copies of the same bug.
TEST_CASE("encodeBindParam: MONTH TO MONTH reads month as the total, not year*12+month") {
    // shape->start == 2 is the one YEAR-MONTH shape where `year` isn't
    // read at all (see bind_encode.cpp's encodeBindParam doc comment) -
    // setting year=5 here alongside month=43 is the actual test: if the
    // code mistakenly fell through to the year*12+month branch, this would
    // encode 103 months instead of 43.
    SQL_INTERVAL_STRUCT s{};
    s.interval_type = SQL_IS_MONTH;
    s.interval_sign = SQL_FALSE;
    s.intval.year_month.year = 5;
    s.intval.year_month.month = 43;

    auto result = encodeBindParam(SQL_C_DEFAULT, SQL_INTERVAL_MONTH, &s, 0, /*columnSize=*/2, /*decimalDigits=*/0);
    REQUIRE(result.status == BindEncodeStatus::kOk);

    auto encodedLength = intervalEncodedLengthFor(SQL_INTERVAL_MONTH, 2, 0);
    REQUIRE(encodedLength.has_value());
    auto expected = ifxsqli::encodeIntervalYearMonth(43, *encodedLength);
    REQUIRE(expected.has_value());
    CHECK(result.param.data == *expected);

    // Negative sign flips the total, same single field.
    s.interval_sign = SQL_TRUE;
    auto negResult = encodeBindParam(SQL_C_DEFAULT, SQL_INTERVAL_MONTH, &s, 0, 2, 0);
    REQUIRE(negResult.status == BindEncodeStatus::kOk);
    auto negExpected = ifxsqli::encodeIntervalYearMonth(-43, *encodedLength);
    REQUIRE(negExpected.has_value());
    CHECK(negResult.param.data == *negExpected);
}

TEST_CASE("encodeBindParam: DAY-FRACTION scales SQL_DAY_SECOND_STRUCT's nanosecond fraction down, truncating") {
    // SQL_DAY_SECOND_STRUCT.fraction is nanoseconds (same convention
    // SQL_TIMESTAMP_STRUCT.fraction uses) - a FRACTION(3) column keeps only
    // the top 3 digits, dividing by 10^(9-3). 678999999 ns is deliberately
    // not an exact multiple of 10^6, so a value of 678 (not 679) confirms
    // this truncates rather than rounds.
    SQL_INTERVAL_STRUCT s{};
    s.interval_type = SQL_IS_SECOND;
    s.interval_sign = SQL_FALSE;
    s.intval.day_second.second = 45;
    s.intval.day_second.fraction = 678999999;

    auto result = encodeBindParam(SQL_C_DEFAULT, SQL_INTERVAL_SECOND, &s, 0, /*columnSize=*/2, /*decimalDigits=*/3);
    REQUIRE(result.status == BindEncodeStatus::kOk);

    auto encodedLength = intervalEncodedLengthFor(SQL_INTERVAL_SECOND, 2, 3);
    REQUIRE(encodedLength.has_value());
    ifxsqli::IntervalDayFractionValue v;
    v.second = 45;
    v.fraction = 678;
    v.fractionDigits = 3;
    auto expected = ifxsqli::encodeIntervalDayFraction(v, *encodedLength);
    REQUIRE(expected.has_value());
    CHECK(result.param.data == *expected);
}
