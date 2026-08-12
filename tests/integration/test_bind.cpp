// Live integration test for SQ_BIND parameter encoding (sqli/types.h's
// encodeSmallInt/encodeInteger/encodeBigInt/encodeChar, see
// driver/bind_encode.cpp for the ODBC-surface mirror). Confirms the full
// round trip: encode -> SQ_BIND wire bytes -> server storage -> plain
// SELECT fetch -> decode, for each bind-parameter type currently supported.

#include "sqli/connection.h"
#include "sqli/statement.h"
#include "sqli/types.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace ifxsqli;

namespace {

int g_failures = 0;

std::string envOr(const char* name, const std::string& fallback) {
    if (const char* v = std::getenv(name)) return v;
    return fallback;
}

void check(const char* label, bool condition) {
    if (condition) {
        std::printf("  [ok] %s\n", label);
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", label);
    }
}

// Inserts `literal` into a fresh row's col_decimal (every other column left
// NULL) and fetches it straight back, on its own connection. Used to
// exercise DECIMAL's positive and zero exponent-byte shapes live, distinct
// paths from the general negative case the main round trip below already
// covers - encodeDecimal's positive/zero branches were previously
// validated only by decodeDecimal round-tripping its own output, which
// can't catch "self-consistent bytes the server actually rejects."
[[nodiscard]] bool checkDecimalRoundTrip(const std::string& host, int port, const ConnectParams& params,
                                          const char* literal) {
    auto conn = SqliConnection::connect(host, port, params);
    if (!conn) {
        std::fprintf(stderr, "connect failed: %s\n", conn.error().message.c_str());
        return false;
    }
    auto encoded = encodeDecimal(literal);
    if (!encoded) {
        std::fprintf(stderr, "encodeDecimal failed: %s\n", encoded.error().message.c_str());
        return false;
    }
    auto insertStmt = SqliStatement::prepare(*conn, "insert into bind_probe (col_decimal) values (?)");
    if (!insertStmt) {
        std::fprintf(stderr, "prepare insert failed: %s\n", insertStmt.error().message.c_str());
        return false;
    }
    std::vector<BindParam> params2{BindParam{ifxtype::kDecimal, false, encoded->data, encoded->scale}};
    if (auto r = insertStmt->execute(params2); !r) {
        std::fprintf(stderr, "execute insert failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = insertStmt->close(); !r) return false;
    if (auto r = insertStmt->release(); !r) return false;

    auto selectStmt = SqliStatement::prepare(
        *conn, "select col_decimal from bind_probe where col_decimal is not null order by col_decimal desc");
    if (!selectStmt) {
        std::fprintf(stderr, "prepare select failed: %s\n", selectStmt.error().message.c_str());
        return false;
    }
    if (auto r = selectStmt->execute(); !r) {
        std::fprintf(stderr, "execute select failed: %s\n", r.error().message.c_str());
        return false;
    }
    auto row = selectStmt->fetchRow();
    if (!row || !*row) {
        std::fprintf(stderr, "fetchRow failed or no row\n");
        return false;
    }
    auto decoded = decodeDecimal((**row)[0], 10, 2);
    if (auto r = selectStmt->close(); !r) return false;
    if (auto r = selectStmt->release(); !r) return false;
    // Consume every not-null col_decimal row (including run()'s own,
    // already-checked row on the first call here) so the next call's
    // "order by ... desc" pick is unambiguous rather than racing against
    // whatever earlier literals are still sitting in the table.
    if (auto r = conn->executeCommand("delete from bind_probe where col_decimal is not null"); !r) return false;
    if (auto r = conn->disconnectOrderly(); !r) return false;
    if (!decoded) {
        std::fprintf(stderr, "decode failed: %s\n", decoded.error().message.c_str());
        return false;
    }
    check((std::string("bound DECIMAL round-trips (") + literal + ")").c_str(),
          decoded->has_value() && **decoded == literal);
    return true;
}

// Live round trip for DATETIME's FRACTION support (col_datetime_frac is
// YEAR TO FRACTION(5)): encode -> SQ_BIND -> server storage -> plain
// SELECT -> decode, on its own connection/row so it doesn't interact with
// run()'s or checkDecimalRoundTrip's rows. Distinct from run()'s plain
// col_datetime check (YEAR TO SECOND, no fraction) - this exercises the
// exponent byte's fixed whole-field-pair-count encoding (confirmed live to
// stay pinned at 7 even though the mantissa carries 10 pairs total once
// FRACTION is appended) and the odd-digit-count zero-pad convention.
[[nodiscard]] bool checkDatetimeFractionRoundTrip(const std::string& host, int port, const ConnectParams& params) {
    auto conn = SqliConnection::connect(host, port, params);
    if (!conn) {
        std::fprintf(stderr, "connect failed: %s\n", conn.error().message.c_str());
        return false;
    }
    constexpr std::uint32_t kYearToFraction5 = (19u << 8) | 0x0F;
    DatetimeValue value{2024, 6, 15, 13, 45, 52, 12345, 5}; // 2024-06-15 13:45:52.12345
    auto encoded = encodeDatetime(value, kYearToFraction5);
    if (!encoded) {
        std::fprintf(stderr, "encodeDatetime failed: %s\n", encoded.error().message.c_str());
        return false;
    }
    auto insertStmt = SqliStatement::prepare(*conn, "insert into bind_probe (col_datetime_frac) values (?)");
    if (!insertStmt) {
        std::fprintf(stderr, "prepare insert failed: %s\n", insertStmt.error().message.c_str());
        return false;
    }
    std::vector<BindParam> params2{
        BindParam{ifxtype::kDatetime, false, *encoded, static_cast<std::uint16_t>(kYearToFraction5)}};
    if (auto r = insertStmt->execute(params2); !r) {
        std::fprintf(stderr, "execute insert failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = insertStmt->close(); !r) return false;
    if (auto r = insertStmt->release(); !r) return false;

    auto selectStmt =
        SqliStatement::prepare(*conn, "select col_datetime_frac from bind_probe where col_datetime_frac is not null");
    if (!selectStmt) {
        std::fprintf(stderr, "prepare select failed: %s\n", selectStmt.error().message.c_str());
        return false;
    }
    if (auto r = selectStmt->execute(); !r) {
        std::fprintf(stderr, "execute select failed: %s\n", r.error().message.c_str());
        return false;
    }
    auto row = selectStmt->fetchRow();
    if (!row || !*row) {
        std::fprintf(stderr, "fetchRow failed or no row\n");
        return false;
    }
    auto decoded = decodeDatetime((**row)[0], kYearToFraction5);
    if (auto r = selectStmt->close(); !r) return false;
    if (auto r = selectStmt->release(); !r) return false;
    if (auto r = conn->executeCommand("delete from bind_probe where col_datetime_frac is not null"); !r) return false;
    if (auto r = conn->disconnectOrderly(); !r) return false;
    if (!decoded) {
        std::fprintf(stderr, "decode failed: %s\n", decoded.error().message.c_str());
        return false;
    }
    check("bound DATETIME YEAR TO FRACTION(5) round-trips",
          decoded->has_value() && (*decoded)->year == 2024 && (*decoded)->month == 6 && (*decoded)->day == 15 &&
              (*decoded)->hour == 13 && (*decoded)->minute == 45 && (*decoded)->second == 52 &&
              (*decoded)->fraction == 12345 && (*decoded)->fractionDigits == 5);
    return true;
}

// Confirms SQ_BIND's NULL path: binds a NULL indicator (-1) into every
// non-char column with the column's real ifxType attached (mirroring the
// fix in driver/statement.cpp's buildBindParams, which used to leave
// ifxType defaulted to 0/kChar for every NULL regardless of the target
// column). Runs on its own connection/table row so it doesn't interact
// with run()'s or checkDecimalRoundTrip's rows.
[[nodiscard]] bool checkNullBind(const std::string& host, int port, const ConnectParams& params) {
    auto conn = SqliConnection::connect(host, port, params);
    if (!conn) {
        std::fprintf(stderr, "connect failed: %s\n", conn.error().message.c_str());
        return false;
    }
    auto insertStmt = SqliStatement::prepare(
        *conn, "insert into bind_probe (col_smallint, col_integer, col_bigint, col_date, col_decimal,"
               " col_datetime, col_float, col_smallfloat, col_boolean) values (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!insertStmt) {
        std::fprintf(stderr, "prepare insert failed: %s\n", insertStmt.error().message.c_str());
        return false;
    }
    // BOOLEAN's NULL bind still needs the extended owner/name fields
    // (confirmed live that omitting them hangs the connection, same as a
    // non-NULL BOOLEAN bind - see BindParam's doc comment and
    // driver/statement.cpp's buildBindParams NULL branch, which has the
    // same special case for the ODBC-surface path).
    std::vector<BindParam> params2{
        BindParam{ifxtype::kSmallInt, true, {}, 0}, BindParam{ifxtype::kInteger, true, {}, 0},
        BindParam{ifxtype::kBigInt, true, {}, 0},   BindParam{ifxtype::kDate, true, {}, 0},
        BindParam{ifxtype::kDecimal, true, {}, 0},  BindParam{ifxtype::kDatetime, true, {}, 0},
        BindParam{ifxtype::kFloat, true, {}, 0},    BindParam{ifxtype::kSmallFloat, true, {}, 0},
        BindParam{ifxtype::kBooleanBindType, true, {}, 0, std::string("informix"), std::string("boolean")}};
    if (auto r = insertStmt->execute(params2); !r) {
        std::fprintf(stderr, "execute insert failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = insertStmt->close(); !r) return false;
    if (auto r = insertStmt->release(); !r) return false;

    auto selectStmt = SqliStatement::prepare(
        *conn, "select col_smallint, col_integer, col_bigint, col_date, col_decimal, col_datetime,"
               " col_float, col_smallfloat, col_boolean"
               " from bind_probe where col_char is null and col_varchar is null");
    if (!selectStmt) {
        std::fprintf(stderr, "prepare select failed: %s\n", selectStmt.error().message.c_str());
        return false;
    }
    if (auto r = selectStmt->execute(); !r) {
        std::fprintf(stderr, "execute select failed: %s\n", r.error().message.c_str());
        return false;
    }
    auto row = selectStmt->fetchRow();
    if (!row || !*row) {
        std::fprintf(stderr, "fetchRow failed or no row\n");
        return false;
    }
    auto smallintVal = decodeSmallInt((**row)[0]);
    auto integerVal = decodeInteger((**row)[1]);
    auto bigintVal = decodeBigInt((**row)[2]);
    auto dateVal = decodeDateUnixDays((**row)[3]);
    auto decimalVal = decodeDecimal((**row)[4], 10, 2);
    auto datetimeVal = decodeDatetime((**row)[5], (14u << 8) | 0x0A);
    auto floatVal = decodeFloat((**row)[6]);
    auto smallfloatVal = decodeSmallFloat((**row)[7]);
    auto booleanVal = decodeBoolean((**row)[8]);
    if (auto r = selectStmt->close(); !r) return false;
    if (auto r = selectStmt->release(); !r) return false;
    if (auto r = conn->executeCommand("delete from bind_probe where col_char is null and col_varchar is null");
        !r)
        return false;
    if (auto r = conn->disconnectOrderly(); !r) return false;
    if (!smallintVal || !integerVal || !bigintVal || !dateVal || !decimalVal || !datetimeVal || !floatVal ||
        !smallfloatVal || !booleanVal) {
        std::fprintf(stderr, "decode failed\n");
        return false;
    }
    check("NULL bound with real ifxType round-trips (SMALLINT)", !smallintVal->has_value());
    check("NULL bound with real ifxType round-trips (INTEGER)", !integerVal->has_value());
    check("NULL bound with real ifxType round-trips (BIGINT)", !bigintVal->has_value());
    check("NULL bound with real ifxType round-trips (DATE)", !dateVal->has_value());
    check("NULL bound with real ifxType round-trips (DECIMAL)", !decimalVal->has_value());
    check("NULL bound with real ifxType round-trips (DATETIME)", !datetimeVal->has_value());
    check("NULL bound with real ifxType round-trips (FLOAT)", !floatVal->has_value());
    check("NULL bound with real ifxType round-trips (SMALLFLOAT)", !smallfloatVal->has_value());
    check("NULL bound with real ifxType round-trips (BOOLEAN)", !booleanVal->has_value());
    return true;
}

[[nodiscard]] bool run(const std::string& host, int port, const ConnectParams& params) {
    auto setup = SqliConnection::connect(host, port, params);
    if (!setup) {
        std::fprintf(stderr, "connect failed: %s\n", setup.error().message.c_str());
        return false;
    }
    if (auto r = setup->executeCommand("drop table if exists bind_probe"); !r) {
        std::fprintf(stderr, "drop table failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = setup->executeCommand(
            "create table bind_probe (col_smallint smallint, col_integer integer, col_bigint bigint,"
            " col_char char(20), col_varchar varchar(32), col_date date, col_decimal decimal(10,2),"
            " col_datetime datetime year to second, col_float float, col_smallfloat smallfloat,"
            " col_boolean boolean, col_datetime_frac datetime year to fraction(5))");
        !r) {
        std::fprintf(stderr, "create table failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = setup->disconnectOrderly(); !r) {
        std::fprintf(stderr, "setup disconnect failed: %s\n", r.error().message.c_str());
        return false;
    }

    auto conn = SqliConnection::connect(host, port, params);
    if (!conn) {
        std::fprintf(stderr, "connect failed: %s\n", conn.error().message.c_str());
        return false;
    }

    auto insertStmt =
        SqliStatement::prepare(*conn, "insert into bind_probe values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!insertStmt) {
        std::fprintf(stderr, "prepare insert failed: %s\n", insertStmt.error().message.c_str());
        return false;
    }

    // Deliberately odd byte length (7): exercises writeBindBlock's
    // even-alignment pad path, which the prior "hello bind" (10 bytes,
    // even) value never touched - this protocol's own history is full of
    // odd-length bugs masked until a differently-shaped value hit them.
    std::string_view charValue = "hi bind";
    std::string_view varcharValue = "varchar!";
    std::int32_t dateUnixDays = 10957; // 2000-01-01
    auto decimalEncoded = encodeDecimal("-12345.67");
    if (!decimalEncoded) {
        std::fprintf(stderr, "encodeDecimal failed: %s\n", decimalEncoded.error().message.c_str());
        return false;
    }
    DatetimeValue datetimeValue{2026, 7, 29, 12, 34, 56};
    constexpr std::uint32_t kYearToSecond = (14u << 8) | 0x0A;
    auto datetimeEncoded = encodeDatetime(datetimeValue, kYearToSecond);
    if (!datetimeEncoded) {
        std::fprintf(stderr, "encodeDatetime failed: %s\n", datetimeEncoded.error().message.c_str());
        return false;
    }
    std::vector<BindParam> params2;
    params2.push_back(BindParam{ifxtype::kSmallInt, false, encodeSmallInt(-12345), kSmallIntBindPrecision});
    params2.push_back(BindParam{ifxtype::kInteger, false, encodeInteger(1234567890), kIntegerBindPrecision});
    params2.push_back(
        BindParam{ifxtype::kBigInt, false, encodeBigInt(123456789012345), kBigIntBindPrecision});
    params2.push_back(BindParam{ifxtype::kChar, false, encodeChar(charValue),
                                 static_cast<std::uint16_t>(charValue.size())});
    params2.push_back(BindParam{ifxtype::kVarchar, false, encodeChar(varcharValue),
                                 static_cast<std::uint16_t>(varcharValue.size())});
    params2.push_back(BindParam{ifxtype::kDate, false, encodeDateUnixDays(dateUnixDays), 4});
    params2.push_back(BindParam{ifxtype::kDecimal, false, decimalEncoded->data, decimalEncoded->scale});
    params2.push_back(BindParam{ifxtype::kDatetime, false, *datetimeEncoded,
                                 static_cast<std::uint16_t>(kYearToSecond)});
    params2.push_back(BindParam{ifxtype::kFloat, false, encodeFloat(3.14159), kFloatBindPrecision});
    params2.push_back(BindParam{ifxtype::kSmallFloat, false, encodeSmallFloat(2.5f), kSmallFloatBindPrecision});
    params2.push_back(BindParam{ifxtype::kBooleanBindType, false, encodeBoolean(true), kBooleanBindPrecision,
                                 std::string("informix"), std::string("boolean")});
    // col_datetime_frac isn't exercised here - see checkDatetimeFractionRoundTrip.
    params2.push_back(BindParam{ifxtype::kDatetime, true, {}, 0});
    if (auto r = insertStmt->execute(params2); !r) {
        std::fprintf(stderr, "execute insert failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = insertStmt->close(); !r) return false;
    if (auto r = insertStmt->release(); !r) return false;

    auto selectStmt = SqliStatement::prepare(*conn,
                                              "select col_smallint, col_integer, col_bigint, col_char, col_varchar,"
                                              " col_date, col_decimal, col_datetime, col_float, col_smallfloat,"
                                              " col_boolean"
                                              " from bind_probe");
    if (!selectStmt) {
        std::fprintf(stderr, "prepare select failed: %s\n", selectStmt.error().message.c_str());
        return false;
    }
    if (auto r = selectStmt->execute(); !r) {
        std::fprintf(stderr, "execute select failed: %s\n", r.error().message.c_str());
        return false;
    }
    auto row = selectStmt->fetchRow();
    if (!row) {
        std::fprintf(stderr, "fetchRow failed: %s\n", row.error().message.c_str());
        return false;
    }
    if (!*row) {
        std::fprintf(stderr, "no row came back\n");
        return false;
    }

    auto smallintVal = decodeSmallInt((**row)[0]);
    auto integerVal = decodeInteger((**row)[1]);
    auto bigintVal = decodeBigInt((**row)[2]);
    auto charVal = decodeChar((**row)[3]);
    auto varcharVal = decodeVarchar((**row)[4]);
    auto dateVal = decodeDateUnixDays((**row)[5]);
    auto decimalVal = decodeDecimal((**row)[6], 10, 2);
    auto datetimeVal = decodeDatetime((**row)[7], kYearToSecond);
    auto floatVal = decodeFloat((**row)[8]);
    auto smallfloatVal = decodeSmallFloat((**row)[9]);
    auto booleanVal = decodeBoolean((**row)[10]);
    if (!smallintVal || !integerVal || !bigintVal || !charVal || !varcharVal || !dateVal || !decimalVal ||
        !datetimeVal || !floatVal || !smallfloatVal || !booleanVal) {
        std::fprintf(stderr, "decode failed\n");
        return false;
    }
    check("bound SMALLINT round-trips", smallintVal->has_value() && **smallintVal == -12345);
    check("bound INTEGER round-trips", integerVal->has_value() && **integerVal == 1234567890);
    check("bound BIGINT round-trips", bigintVal->has_value() && **bigintVal == 123456789012345);
    // CHAR(20) is space-padded to the declared width on the wire (see
    // decodeChar's doc comment) - compute the expected padded form instead
    // of hand-counting trailing spaces.
    std::string expectedChar = std::string(charValue) + std::string(20 - charValue.size(), ' ');
    check("bound CHAR round-trips (odd byte length)", charVal->has_value() && **charVal == expectedChar);
    check("bound VARCHAR round-trips", varcharVal->has_value() && **varcharVal == "varchar!");
    check("bound DATE round-trips", dateVal->has_value() && **dateVal == dateUnixDays);
    check("bound DECIMAL round-trips", decimalVal->has_value() && **decimalVal == "-12345.67");
    check("bound DATETIME round-trips", datetimeVal->has_value() && (*datetimeVal)->year == 2026 &&
                                             (*datetimeVal)->month == 7 && (*datetimeVal)->day == 29 &&
                                             (*datetimeVal)->hour == 12 && (*datetimeVal)->minute == 34 &&
                                             (*datetimeVal)->second == 56);
    check("bound FLOAT round-trips", floatVal->has_value() && **floatVal == 3.14159);
    check("bound SMALLFLOAT round-trips", smallfloatVal->has_value() && **smallfloatVal == 2.5f);
    check("bound BOOLEAN round-trips", booleanVal->has_value() && **booleanVal == true);

    if (auto r = selectStmt->close(); !r) return false;
    if (auto r = selectStmt->release(); !r) return false;
    if (auto r = conn->disconnectOrderly(); !r) return false;

    // bind_probe is dropped by main() after the standalone DECIMAL checks
    // below, which reuse this same table.
    return true;
}

} // namespace

int main() {
    std::string host = envOr("IFX_HOST", "127.0.0.1");
    int port = std::atoi(envOr("IFX_PORT", "9088").c_str());

    ConnectParams params;
    params.username = envOr("IFX_USER", "informix");
    params.password = envOr("IFX_PASSWORD", "in4mix");
    params.serverName = envOr("IFX_SERVER", "informix");
    params.database = envOr("IFX_DATABASE", "ifxodbc_test");

    std::printf("connecting to %s:%d (server=%s, db=%s)...\n", host.c_str(), port, params.serverName.c_str(),
                params.database.c_str());

    if (!run(host, port, params)) {
        std::printf("\nbind integration test FAILED to complete (see error above)\n");
        return 1;
    }

    // DECIMAL's positive and zero exponent-byte shapes are structurally
    // different from the negative case run() just exercised (sign bit set
    // vs. clear vs. the hardcoded 0x80 zero special case) - check them
    // live too rather than trusting the unit tests' decodeDecimal
    // round-trip, which can't catch bytes the server itself rejects.
    if (!checkDecimalRoundTrip(host, port, params, "12345.67")) {
        std::printf("\nbind integration test FAILED to complete (see error above)\n");
        return 1;
    }
    if (!checkDecimalRoundTrip(host, port, params, "0.00")) {
        std::printf("\nbind integration test FAILED to complete (see error above)\n");
        return 1;
    }
    if (!checkNullBind(host, port, params)) {
        std::printf("\nbind integration test FAILED to complete (see error above)\n");
        return 1;
    }
    if (!checkDatetimeFractionRoundTrip(host, port, params)) {
        std::printf("\nbind integration test FAILED to complete (see error above)\n");
        return 1;
    }

    {
        auto cleanup = SqliConnection::connect(host, port, params);
        if (!cleanup) {
            std::fprintf(stderr, "cleanup connect failed: %s\n", cleanup.error().message.c_str());
            return 1;
        }
        if (auto r = cleanup->executeCommand("drop table if exists bind_probe"); !r) {
            std::fprintf(stderr, "cleanup drop table failed: %s\n", r.error().message.c_str());
            return 1;
        }
        if (auto r = cleanup->disconnectOrderly(); !r) {
            std::fprintf(stderr, "cleanup disconnect failed: %s\n", r.error().message.c_str());
            return 1;
        }
    }

    if (g_failures > 0) {
        std::printf("\n%d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall assertions passed\n");
    return 0;
}
