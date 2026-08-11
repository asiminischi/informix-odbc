// Live integration test for transaction control (SQ_BEGIN/SQ_CMMTWORK/
// SQ_RBWORK, see sqli/protocol.h and SqliConnection::beginTransaction/
// commitTransaction/rollbackTransaction). Confirmed live: rollback makes an
// inserted row invisible even to the same connection, and commit makes it
// visible from a separate connection.
//
// Not registered with the default ctest run (see tests/CMakeLists.txt's
// IFX_ENABLE_INTEGRATION_TESTS option) since it requires a live server;
// enable with -DIFX_ENABLE_INTEGRATION_TESTS=ON against the Docker
// container in docker/docker-compose.yml.

#include "sqli/connection.h"
#include "sqli/statement.h"
#include "sqli/types.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using namespace ifxsqli;

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

// Returns the row count for `select count(*) from tx_probe where tag = ?`
// via a fresh, independent connection - so a commit test can confirm the
// row is visible outside the connection that made it, not just to itself.
[[nodiscard]] std::expected<std::int64_t, Error> countRows(const std::string& host, int port,
                                                             const ConnectParams& params, const std::string& tag) {
    auto conn = SqliConnection::connect(host, port, params);
    if (!conn) return std::unexpected(conn.error());

    auto stmt = SqliStatement::prepare(*conn, "select count(*) from tx_probe where tag = '" + tag + "'");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto r = stmt->execute(); !r) return std::unexpected(r.error());

    auto row = stmt->fetchRow();
    if (!row) return std::unexpected(row.error());
    if (!*row) return std::unexpected(Error{ErrorKind::Protocol, "count query returned no row"});

    // count(*) comes back as this server's default DECIMAL, not INTEGER or
    // BIGINT (9-byte buffer = 1 exponent + 8 mantissa bytes, i.e.
    // DECIMAL(16,0) - see sqli/types.h's decodeDecimal comment).
    auto count = decodeDecimal((**row)[0], 16, 0);
    if (!count) return std::unexpected(count.error());
    if (!*count) return std::unexpected(Error{ErrorKind::Protocol, "count column was NULL"});

    if (auto r = stmt->close(); !r) return std::unexpected(r.error());
    if (auto r = stmt->release(); !r) return std::unexpected(r.error());
    if (auto r = conn->disconnectOrderly(); !r) return std::unexpected(r.error());

    return std::stoll(**count);
}

[[nodiscard]] bool run(const std::string& host, int port, const ConnectParams& params) {
    auto setup = SqliConnection::connect(host, port, params);
    if (!setup) {
        std::fprintf(stderr, "connect failed: %s\n", setup.error().message.c_str());
        return false;
    }
    if (auto r = setup->executeCommand("drop table if exists tx_probe"); !r) {
        std::fprintf(stderr, "drop table failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = setup->executeCommand("create table tx_probe (tag varchar(32) not null)"); !r) {
        std::fprintf(stderr, "create table failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = setup->disconnectOrderly(); !r) {
        std::fprintf(stderr, "setup disconnect failed: %s\n", r.error().message.c_str());
        return false;
    }

    // Rollback: a row inserted inside a transaction that is then rolled
    // back must not be visible, even to the same connection.
    {
        std::printf("rollback test:\n");
        auto conn = SqliConnection::connect(host, port, params);
        if (!conn) {
            std::fprintf(stderr, "connect failed: %s\n", conn.error().message.c_str());
            return false;
        }
        if (auto r = conn->beginTransaction(); !r) {
            std::fprintf(stderr, "beginTransaction failed: %s\n", r.error().message.c_str());
            return false;
        }
        if (auto r = conn->executeCommand("insert into tx_probe (tag) values ('rollback-test')"); !r) {
            std::fprintf(stderr, "insert failed: %s\n", r.error().message.c_str());
            return false;
        }
        if (auto r = conn->rollbackTransaction(); !r) {
            std::fprintf(stderr, "rollbackTransaction failed: %s\n", r.error().message.c_str());
            return false;
        }
        if (auto r = conn->disconnectOrderly(); !r) {
            std::fprintf(stderr, "disconnect failed: %s\n", r.error().message.c_str());
            return false;
        }

        auto count = countRows(host, port, params, "rollback-test");
        if (!count) {
            std::fprintf(stderr, "countRows failed: %s\n", count.error().message.c_str());
            return false;
        }
        check("rolled-back row is absent", *count == 0);
    }

    // Commit: a row inserted inside a transaction that is then committed
    // must be visible, including from a separate connection.
    {
        std::printf("commit test:\n");
        auto conn = SqliConnection::connect(host, port, params);
        if (!conn) {
            std::fprintf(stderr, "connect failed: %s\n", conn.error().message.c_str());
            return false;
        }
        if (auto r = conn->beginTransaction(); !r) {
            std::fprintf(stderr, "beginTransaction failed: %s\n", r.error().message.c_str());
            return false;
        }
        if (auto r = conn->executeCommand("insert into tx_probe (tag) values ('commit-test')"); !r) {
            std::fprintf(stderr, "insert failed: %s\n", r.error().message.c_str());
            return false;
        }
        if (auto r = conn->commitTransaction(); !r) {
            std::fprintf(stderr, "commitTransaction failed: %s\n", r.error().message.c_str());
            return false;
        }
        if (auto r = conn->disconnectOrderly(); !r) {
            std::fprintf(stderr, "disconnect failed: %s\n", r.error().message.c_str());
            return false;
        }

        auto count = countRows(host, port, params, "commit-test");
        if (!count) {
            std::fprintf(stderr, "countRows failed: %s\n", count.error().message.c_str());
            return false;
        }
        check("committed row is visible from a separate connection", *count == 1);
    }

    auto cleanup = SqliConnection::connect(host, port, params);
    if (!cleanup) {
        std::fprintf(stderr, "cleanup connect failed: %s\n", cleanup.error().message.c_str());
        return false;
    }
    if (auto r = cleanup->executeCommand("drop table if exists tx_probe"); !r) {
        std::fprintf(stderr, "cleanup drop table failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = cleanup->disconnectOrderly(); !r) {
        std::fprintf(stderr, "cleanup disconnect failed: %s\n", r.error().message.c_str());
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
        std::printf("\ntransaction integration test FAILED to complete (see error above)\n");
        return 1;
    }

    if (g_failures > 0) {
        std::printf("\n%d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall assertions passed\n");
    return 0;
}
