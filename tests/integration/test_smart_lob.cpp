// Live integration test for smart LOB (BLOB/CLOB) create/write/read, at the
// sqli layer (SqliConnection::createSmartLob/fetchSmartLob, sqli/connection.h).
// Exists specifically to catch the class of regression this test was added
// after: an earlier refactor made appendFastPathLocatorParam double-length-
// prefix its payload, silently breaking fetchSmartLob for every locator
// (sqlcode -937 "blobrecv" on ifx_lo_open) - every other test in this suite
// still passed, since none of them touch a smart LOB. See
// docs/protocol-notes.md's Smart large objects section for the full
// writeup.
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
#include <span>
#include <string>
#include <vector>

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

std::vector<std::byte> toBytes(const std::string& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

// Binds `locator` (createSmartLob's own 72-byte return) into a fresh row's
// smart-LOB column, matching the ordinary-column-bind convention
// (ExtendedOwner="informix", precisionField=72, data=[4-byte length=72]
// [the 72 bytes]) - not the fastpath convention createSmartLob/fetchSmartLob
// use internally (ExtendedOwner="").
[[nodiscard]] std::expected<void, Error> insertRow(SqliConnection& conn, const std::string& table,
                                                     const std::string& column, int id,
                                                     std::span<const std::byte> locator, const std::string& extType) {
    auto stmt = SqliStatement::prepare(conn, "insert into " + table + " (id, " + column + ") values (?, ?)");
    if (!stmt) return std::unexpected(stmt.error());

    BindParam idParam;
    idParam.ifxType = ifxtype::kInteger;
    idParam.data = {std::byte{static_cast<unsigned char>((id >> 24) & 0xFF)},
                     std::byte{static_cast<unsigned char>((id >> 16) & 0xFF)},
                     std::byte{static_cast<unsigned char>((id >> 8) & 0xFF)},
                     std::byte{static_cast<unsigned char>(id & 0xFF)}};

    BindParam lobParam;
    lobParam.ifxType = ifxtype::kUdt;
    lobParam.extendedOwnerName = "informix";
    lobParam.extendedTypeName = extType;
    lobParam.precisionField = static_cast<std::uint16_t>(locator.size());
    lobParam.data.resize(4 + locator.size());
    std::uint32_t len = static_cast<std::uint32_t>(locator.size());
    lobParam.data[0] = static_cast<std::byte>((len >> 24) & 0xFF);
    lobParam.data[1] = static_cast<std::byte>((len >> 16) & 0xFF);
    lobParam.data[2] = static_cast<std::byte>((len >> 8) & 0xFF);
    lobParam.data[3] = static_cast<std::byte>(len & 0xFF);
    std::copy(locator.begin(), locator.end(), lobParam.data.begin() + 4);

    std::vector<BindParam> params{idParam, lobParam};
    return stmt->execute(params);
}

// Reads column `column` from `table` where id = `id`, on whatever
// connection is passed in, and fetches its smart-LOB content.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> readLob(SqliConnection& conn, const std::string& table,
                                                                     const std::string& column, int id) {
    auto stmt = SqliStatement::prepare(conn, "select " + column + " from " + table + " where id = " + std::to_string(id));
    if (!stmt) return std::unexpected(stmt.error());
    if (auto r = stmt->execute(); !r) return std::unexpected(r.error());

    auto row = stmt->fetchRow();
    if (!row) return std::unexpected(row.error());
    if (!*row) return std::unexpected(Error{ErrorKind::Protocol, "select returned no row"});

    auto bytes = (**row)[0];
    if (bytes.empty() || bytes[0] == std::byte{1}) {
        return std::unexpected(Error{ErrorKind::Protocol, "column was unexpectedly NULL"});
    }
    return conn.fetchSmartLob(std::span(bytes).subspan(1));
}

[[nodiscard]] bool runOneColumn(const std::string& host, int port, const ConnectParams& params,
                                 const std::string& table, const std::string& column, const std::string& extType,
                                 int id, const std::string& content) {
    std::printf("%s column:\n", extType.c_str());

    // Create + write on one connection.
    auto writer = SqliConnection::connect(host, port, params);
    if (!writer) {
        std::fprintf(stderr, "connect (writer) failed: %s\n", writer.error().message.c_str());
        return false;
    }
    auto locator = writer->createSmartLob(table + "." + column, toBytes(content));
    if (!locator) {
        std::fprintf(stderr, "createSmartLob failed: %s\n", locator.error().message.c_str());
        return false;
    }
    check("createSmartLob returns a 72-byte locator", locator->size() == 72);

    if (auto r = insertRow(*writer, table, column, id, *locator, extType); !r) {
        std::fprintf(stderr, "insert failed: %s\n", r.error().message.c_str());
        return false;
    }

    // Read back on the SAME connection.
    auto sameConnRead = readLob(*writer, table, column, id);
    if (!sameConnRead) {
        std::fprintf(stderr, "same-connection read-back failed: %s\n", sameConnRead.error().message.c_str());
        return false;
    }
    std::string sameConnStr(reinterpret_cast<const char*>(sameConnRead->data()), sameConnRead->size());
    check("same-connection read-back matches written content", sameConnStr == content);

    if (auto r = writer->disconnectOrderly(); !r) {
        std::fprintf(stderr, "writer disconnect failed: %s\n", r.error().message.c_str());
        return false;
    }

    // Read back on a SEPARATE, fresh connection - the case that would have
    // caught the double-length-prefix regression (it broke fetchSmartLob's
    // ifx_lo_open request encoding unconditionally, not just for
    // same-connection reads).
    auto reader = SqliConnection::connect(host, port, params);
    if (!reader) {
        std::fprintf(stderr, "connect (reader) failed: %s\n", reader.error().message.c_str());
        return false;
    }
    auto freshConnRead = readLob(*reader, table, column, id);
    if (!freshConnRead) {
        std::fprintf(stderr, "fresh-connection read-back failed: %s\n", freshConnRead.error().message.c_str());
        return false;
    }
    std::string freshConnStr(reinterpret_cast<const char*>(freshConnRead->data()), freshConnRead->size());
    check("fresh-connection read-back matches written content", freshConnStr == content);

    if (auto r = reader->disconnectOrderly(); !r) {
        std::fprintf(stderr, "reader disconnect failed: %s\n", r.error().message.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] bool run(const std::string& host, int port, const ConnectParams& params) {
    const std::string table = "smart_lob_probe";

    auto setup = SqliConnection::connect(host, port, params);
    if (!setup) {
        std::fprintf(stderr, "connect failed: %s\n", setup.error().message.c_str());
        return false;
    }
    if (auto r = setup->executeCommand("drop table if exists " + table); !r) {
        std::fprintf(stderr, "drop table failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = setup->executeCommand("create table " + table +
                                        " (id integer, b blob, c clob) put b in (sbspace1), c in (sbspace1)");
        !r) {
        std::fprintf(stderr, "create table failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = setup->disconnectOrderly(); !r) {
        std::fprintf(stderr, "setup disconnect failed: %s\n", r.error().message.c_str());
        return false;
    }

    bool ok = true;
    ok &= runOneColumn(host, port, params, table, "b", "blob", 1, "smart lob integration test (blob)");
    ok &= runOneColumn(host, port, params, table, "c", "clob", 2, "smart lob integration test (clob)");

    auto cleanup = SqliConnection::connect(host, port, params);
    if (!cleanup) {
        std::fprintf(stderr, "cleanup connect failed: %s\n", cleanup.error().message.c_str());
        return false;
    }
    if (auto r = cleanup->executeCommand("drop table if exists " + table); !r) {
        std::fprintf(stderr, "cleanup drop table failed: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = cleanup->disconnectOrderly(); !r) {
        std::fprintf(stderr, "cleanup disconnect failed: %s\n", r.error().message.c_str());
        return false;
    }
    return ok;
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
        std::printf("\nsmart LOB integration test FAILED to complete (see error above)\n");
        return 1;
    }

    if (g_failures > 0) {
        std::printf("\n%d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall assertions passed\n");
    return 0;
}
