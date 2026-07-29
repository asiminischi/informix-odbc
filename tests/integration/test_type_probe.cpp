// Live integration test for the Phase 3 scalar type codecs. Unlike
// tests/sqli/test_types.cpp (which calls the decoders directly on frozen
// captured bytes and never touches the network), this test exercises the
// full connect -> DDL/DML -> prepare -> execute -> fetch -> decode path
// against a real Informix server, per the plan's Phase 3 done-bar ("a
// live-query integration test hitting a real test table with all MVP
// types, including NULLs").
//
// Not registered with the default `ctest` run (see tests/CMakeLists.txt's
// IFX_ENABLE_INTEGRATION_TESTS option) since it requires a live server;
// enable with -DIFX_ENABLE_INTEGRATION_TESTS=ON against the Docker
// container in docker/docker-compose.yml. The table is created and torn
// down by this test itself (see fixture.sql in this directory for the
// same DDL/rows kept as a human-readable reference copy) so the whole
// thing is reproducible from a clean container with no manual setup step.

#include "sqli/connection.h"
#include "sqli/statement.h"
#include "sqli/types.h"

#include <cstdio>
#include <cstdlib>
#include <expected>
#include <optional>
#include <string>

namespace {

using namespace ifxsqli;

int g_failures = 0;

std::string envOr(const char* name, const std::string& fallback) {
    if (const char* v = std::getenv(name)) return v;
    return fallback;
}

template <class T>
void checkEq(const char* label, const T& actual, const T& expected) {
    if (actual == expected) {
        std::printf("  [ok] %s\n", label);
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", label);
    }
}

void checkNull(const char* label, bool isNull) {
    if (isNull) {
        std::printf("  [ok] %s is NULL\n", label);
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s expected NULL, got a value\n", label);
    }
}

// Unwraps a decodeXxx() result, reporting a decode-level protocol error as a
// test failure (rather than throwing, which would abort the whole test on
// std::expected::value()) and returning nullopt so the caller's checkEq/
// checkNull calls degrade gracefully to a visible failure instead of a crash.
template <class T>
std::optional<T> unwrap(const char* label, std::expected<std::optional<T>, Error> r) {
    if (!r) {
        ++g_failures;
        std::printf("  [FAIL] %s: decode error: %s\n", label, r.error().message.c_str());
        return std::nullopt;
    }
    return *r;
}

[[nodiscard]] bool run(const std::string& host, int port, const ConnectParams& params) {
    auto conn = SqliConnection::connect(host, port, params);
    if (!conn) {
        std::fprintf(stderr, "connect failed: %s\n", conn.error().message.c_str());
        return false;
    }

    // Best-effort: the table may not exist on a fresh database. Informix
    // supports DROP TABLE IF EXISTS (confirmed live), so this never needs
    // to tolerate an error.
    if (auto r = conn->executeCommand("drop table if exists type_probe"); !r) {
        std::fprintf(stderr, "drop table failed: %s\n", r.error().message.c_str());
        return false;
    }

    if (auto r = conn->executeCommand(
            "create table type_probe ("
            "  id serial not null, col_smallint smallint, col_integer integer,"
            "  col_bigint bigint, col_char char(10), col_varchar varchar(32),"
            "  col_decimal decimal(10,2), col_date date,"
            "  col_datetime datetime year to second)");
        !r) {
        std::fprintf(stderr, "create table failed: %s\n", r.error().message.c_str());
        return false;
    }

    if (auto r = conn->executeCommand(
            "insert into type_probe (col_smallint, col_integer, col_bigint, col_char,"
            " col_varchar, col_decimal, col_date, col_datetime) values (12345, 1234567890,"
            " 123456789012345, 'hello', 'world varchar', 12345.67, DATE('2026-07-29'),"
            " DATETIME(2026-07-29 12:34:56) YEAR TO SECOND)");
        !r) {
        std::fprintf(stderr, "insert (positive row) failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = conn->executeCommand(
            "insert into type_probe (col_smallint, col_integer, col_bigint, col_char,"
            " col_varchar, col_decimal, col_date, col_datetime) values (-12345, -2147483000,"
            " -123456789012345, 'ab', 'x', -99.99, DATE('2000-01-01'),"
            " DATETIME(2000-01-01 00:00:00) YEAR TO SECOND)");
        !r) {
        std::fprintf(stderr, "insert (negative row) failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = conn->executeCommand(
            "insert into type_probe (col_smallint, col_integer, col_bigint, col_char,"
            " col_varchar, col_decimal, col_date, col_datetime) values (null, null, null,"
            " null, null, null, null, null)");
        !r) {
        std::fprintf(stderr, "insert (NULL row) failed: %s\n", r.error().message.c_str());
        return false;
    }

    auto stmt = SqliStatement::prepare(*conn,
        "select id, col_smallint, col_integer, col_bigint, col_char, col_varchar,"
        " col_decimal, col_date, col_datetime from type_probe order by id");
    if (!stmt) {
        std::fprintf(stderr, "prepare failed: %s\n", stmt.error().message.c_str());
        return false;
    }
    if (auto r = stmt->execute(); !r) {
        std::fprintf(stderr, "execute failed: %s\n", r.error().message.c_str());
        return false;
    }

    const auto& cols = stmt->columns();
    int rowNum = 0;
    for (;;) {
        auto row = stmt->fetchRow();
        if (!row) {
            std::fprintf(stderr, "fetch failed: %s\n", row.error().message.c_str());
            return false;
        }
        if (!*row) break;
        ++rowNum;
        const auto& r = **row;
        std::printf("row %d:\n", rowNum);

        unsigned precision = (cols[6].encodedLength >> 8) & 0xFF;
        unsigned scale = cols[6].encodedLength & 0xFF;

        if (rowNum == 1) {
            checkEq("id", unwrap("id", decodeInteger(r[0])), std::optional<std::int32_t>(1));
            checkEq("col_smallint", unwrap("col_smallint", decodeSmallInt(r[1])), std::optional<std::int16_t>(12345));
            checkEq("col_integer", unwrap("col_integer", decodeInteger(r[2])),
                    std::optional<std::int32_t>(1234567890));
            checkEq("col_bigint", unwrap("col_bigint", decodeBigInt(r[3])),
                    std::optional<std::int64_t>(123456789012345));
            checkEq("col_char", unwrap("col_char", decodeChar(r[4])), std::optional<std::string>("hello     "));
            checkEq("col_varchar", unwrap("col_varchar", decodeVarchar(r[5])),
                    std::optional<std::string>("world varchar"));
            checkEq("col_decimal", unwrap("col_decimal", decodeDecimal(r[6], precision, scale)),
                    std::optional<std::string>("12345.67"));
            checkEq("col_date", unwrap("col_date", decodeDateUnixDays(r[7])),
                    std::optional<std::int32_t>(46231 - 25568));
            auto dt = unwrap("col_datetime", decodeDatetime(r[8], cols[8].encodedLength));
            if (dt) {
                checkEq("col_datetime.year", dt->year, 2026);
                checkEq("col_datetime.month", dt->month, 7);
                checkEq("col_datetime.day", dt->day, 29);
                checkEq("col_datetime.hour", dt->hour, 12);
                checkEq("col_datetime.minute", dt->minute, 34);
                checkEq("col_datetime.second", dt->second, 56);
            } else {
                ++g_failures;
                std::printf("  [FAIL] col_datetime expected a value, got NULL\n");
            }
        } else if (rowNum == 2) {
            checkEq("id", unwrap("id", decodeInteger(r[0])), std::optional<std::int32_t>(2));
            checkEq("col_smallint", unwrap("col_smallint", decodeSmallInt(r[1])),
                    std::optional<std::int16_t>(-12345));
            checkEq("col_integer", unwrap("col_integer", decodeInteger(r[2])),
                    std::optional<std::int32_t>(-2147483000));
            checkEq("col_bigint", unwrap("col_bigint", decodeBigInt(r[3])),
                    std::optional<std::int64_t>(-123456789012345));
            checkEq("col_char", unwrap("col_char", decodeChar(r[4])), std::optional<std::string>("ab        "));
            checkEq("col_varchar", unwrap("col_varchar", decodeVarchar(r[5])), std::optional<std::string>("x"));
            checkEq("col_decimal", unwrap("col_decimal", decodeDecimal(r[6], precision, scale)),
                    std::optional<std::string>("-99.99"));
            checkEq("col_date", unwrap("col_date", decodeDateUnixDays(r[7])),
                    std::optional<std::int32_t>(36525 - 25568));
            auto dt = unwrap("col_datetime", decodeDatetime(r[8], cols[8].encodedLength));
            if (dt) {
                checkEq("col_datetime.year", dt->year, 2000);
                checkEq("col_datetime.month", dt->month, 1);
                checkEq("col_datetime.day", dt->day, 1);
                checkEq("col_datetime.hour", dt->hour, 0);
                checkEq("col_datetime.minute", dt->minute, 0);
                checkEq("col_datetime.second", dt->second, 0);
            } else {
                ++g_failures;
                std::printf("  [FAIL] col_datetime expected a value, got NULL\n");
            }
        } else if (rowNum == 3) {
            checkNull("col_smallint", !unwrap("col_smallint", decodeSmallInt(r[1])).has_value());
            checkNull("col_integer", !unwrap("col_integer", decodeInteger(r[2])).has_value());
            checkNull("col_bigint", !unwrap("col_bigint", decodeBigInt(r[3])).has_value());
            checkNull("col_char", !unwrap("col_char", decodeChar(r[4])).has_value());
            checkNull("col_varchar", !unwrap("col_varchar", decodeVarchar(r[5])).has_value());
            checkNull("col_decimal", !unwrap("col_decimal", decodeDecimal(r[6], precision, scale)).has_value());
            checkNull("col_date", !unwrap("col_date", decodeDateUnixDays(r[7])).has_value());
            checkNull("col_datetime", !unwrap("col_datetime", decodeDatetime(r[8], cols[8].encodedLength)).has_value());
        } else {
            ++g_failures;
            std::printf("  [FAIL] unexpected extra row\n");
        }
    }
    checkEq("row count", rowNum, 3);

    if (auto r = stmt->close(); !r) {
        std::fprintf(stderr, "close failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = stmt->release(); !r) {
        std::fprintf(stderr, "release failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = conn->executeCommand("drop table if exists type_probe"); !r) {
        std::fprintf(stderr, "cleanup drop table failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = conn->disconnectOrderly(); !r) {
        std::fprintf(stderr, "disconnect failed: %s\n", r.error().message.c_str());
        return false;
    }
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
        std::printf("\nintegration test FAILED to complete (see error above)\n");
        return 1;
    }

    if (g_failures > 0) {
        std::printf("\n%d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall assertions passed\n");
    return 0;
}
