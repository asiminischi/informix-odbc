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
            " col_char char(20), col_varchar varchar(32), col_date date)");
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

    auto insertStmt = SqliStatement::prepare(*conn, "insert into bind_probe values (?, ?, ?, ?, ?, ?)");
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
    if (auto r = insertStmt->execute(params2); !r) {
        std::fprintf(stderr, "execute insert failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = insertStmt->close(); !r) return false;
    if (auto r = insertStmt->release(); !r) return false;

    auto selectStmt = SqliStatement::prepare(
        *conn,
        "select col_smallint, col_integer, col_bigint, col_char, col_varchar, col_date from bind_probe");
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
    if (!smallintVal || !integerVal || !bigintVal || !charVal || !varcharVal || !dateVal) {
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

    if (auto r = selectStmt->close(); !r) return false;
    if (auto r = selectStmt->release(); !r) return false;
    if (auto r = conn->executeCommand("drop table if exists bind_probe"); !r) return false;
    if (auto r = conn->disconnectOrderly(); !r) return false;

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

    if (g_failures > 0) {
        std::printf("\n%d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall assertions passed\n");
    return 0;
}
