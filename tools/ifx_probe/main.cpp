#include "sqli/connection.h"
#include "sqli/statement.h"
#include "sqli/types.h"
#include "sqli/version.h"

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>

namespace {

// Decodes and prints one column's value using the sqli/types.h codecs,
// exercising them against whatever the live server actually sends (rather
// than just the frozen bytes the unit tests use).
void printColumnValue(const ifxsqli::ColumnDesc& col, std::span<const std::byte> bytes) {
    using namespace ifxsqli;
    switch (col.ifxType) {
        case ifxtype::kSmallInt: {
            auto v = decodeSmallInt(bytes);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("%d", **v);
            return;
        }
        case ifxtype::kInteger:
        case ifxtype::kSerial: {
            auto v = decodeInteger(bytes);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("%d", **v);
            return;
        }
        case ifxtype::kBigInt:
        case ifxtype::kBigSerial: {
            auto v = decodeBigInt(bytes);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("%lld", static_cast<long long>(**v));
            return;
        }
        case ifxtype::kFloat: {
            auto v = decodeFloat(bytes);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("%g", **v);
            return;
        }
        case ifxtype::kSmallFloat: {
            auto v = decodeSmallFloat(bytes);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("%g", static_cast<double>(**v));
            return;
        }
        case ifxtype::kChar: {
            auto v = decodeChar(bytes);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("\"%s\"", (*v)->c_str());
            return;
        }
        case ifxtype::kVarchar:
        case ifxtype::kNVarchar: {
            auto v = decodeVarchar(bytes);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("\"%s\"", (*v)->c_str());
            return;
        }
        case ifxtype::kDate: {
            auto v = decodeDateUnixDays(bytes);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("<%d unix days>", **v);
            return;
        }
        case ifxtype::kDecimal: {
            unsigned precision = (col.encodedLength >> 8) & 0xFF;
            unsigned scale = col.encodedLength & 0xFF;
            auto v = decodeDecimal(bytes, precision, scale);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("%s", (*v)->c_str());
            return;
        }
        case ifxtype::kBoolean: {
            auto v = decodeBoolean(bytes);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("%s", **v ? "true" : "false");
            return;
        }
        case ifxtype::kDatetime: {
            auto v = decodeDatetime(bytes, col.encodedLength);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            std::printf("%04d-%02d-%02d %02d:%02d:%02d", (*v)->year, (*v)->month, (*v)->day, (*v)->hour,
                        (*v)->minute, (*v)->second);
            if ((*v)->fractionDigits > 0) {
                std::printf(".%0*d", (*v)->fractionDigits, (*v)->fraction);
            }
            return;
        }
        case ifxtype::kInterval: {
            unsigned startCode = (col.encodedLength >> 4) & 0xF;
            if (startCode <= 2) {
                auto v = decodeIntervalYearMonth(bytes, col.encodedLength);
                if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
                if (!*v) { std::printf("NULL"); return; }
                std::int64_t months = **v;
                bool negative = months < 0;
                if (negative) months = -months;
                std::printf("%s%lld-%02lld months", negative ? "-" : "", static_cast<long long>(months / 12),
                            static_cast<long long>(months % 12));
                return;
            }
            auto v = decodeIntervalDayFraction(bytes, col.encodedLength);
            if (!v) { std::printf("<error: %s>", v.error().message.c_str()); return; }
            if (!*v) { std::printf("NULL"); return; }
            const auto& iv = **v;
            std::printf("%s%lld %02lld:%02lld:%02lld", iv.negative ? "-" : "", static_cast<long long>(iv.day),
                        static_cast<long long>(iv.hour), static_cast<long long>(iv.minute),
                        static_cast<long long>(iv.second));
            if (iv.fractionDigits > 0) {
                std::printf(".%0*d", iv.fractionDigits, iv.fraction);
            }
            return;
        }
        default:
            break;
    }
    std::printf("0x");
    for (auto b : bytes) std::printf("%02x", std::to_integer<unsigned char>(b));
}

} // namespace

int main(int argc, char** argv) {
    const auto v = ifxsqli::version();
    std::printf("ifx_probe (ifx_sqli %d.%d.%d)\n", v.major, v.minor, v.patch);

    if (argc < 5) {
        std::printf("usage: ifx_probe <host> <port> <username> <password> [servername] [database] [sql]\n");
        return argc == 1 ? 0 : 1;
    }

    std::string host = argv[1];
    int port = std::atoi(argv[2]);
    std::string username = argv[3];
    std::string password = argv[4];
    std::string serverName = argc > 5 ? argv[5] : "informix";
    std::string database = argc > 6 ? argv[6] : "";
    std::string sql = argc > 7 ? argv[7] : "";

    ifxsqli::ConnectParams params;
    params.username = username;
    params.password = password;
    params.serverName = serverName;
    params.database = database;

    std::printf("connecting to %s:%d (server=%s, user=%s)...\n", host.c_str(), port, serverName.c_str(),
                username.c_str());

    auto conn = ifxsqli::SqliConnection::connect(host, port, params);
    if (!conn) {
        std::fprintf(stderr, "connect failed: %s\n", conn.error().message.c_str());
        return 1;
    }

    const auto& info = conn->serverInfo();
    std::printf("connected.\n");
    std::printf("  server version : %s\n", info.serverVersion.c_str());
    std::printf("  serial number  : %s\n", info.serialNumber.c_str());
    std::printf("  application id : %s\n", info.applicationId.c_str());
    std::printf("  capabilities   : %u %u %u\n", info.cap1, info.cap2, info.cap3);

    if (!sql.empty()) {
        if (auto r = conn->executeCommand("set isolation to dirty read"); !r) {
            std::fprintf(stderr, "set isolation failed: %s\n", r.error().message.c_str());
            return 1;
        }
        std::printf("set isolation ok.\n");
        std::printf("preparing: %s\n", sql.c_str());
        auto stmt = ifxsqli::SqliStatement::prepare(*conn, sql);
        if (!stmt) {
            std::fprintf(stderr, "prepare failed: %s\n", stmt.error().message.c_str());
            return 1;
        }

        std::printf("columns:\n");
        for (const auto& col : stmt->columns()) {
            std::printf("  %-20s type=%d%s encodedLength=%u extendedId=%u owner=\"%s\" name=\"%s\"\n",
                        col.name.c_str(), col.ifxType, col.notNull ? " NOT NULL" : "", col.encodedLength,
                        col.extendedId, col.extendedOwnerName.c_str(), col.extendedTypeName.c_str());
        }

        if (auto r = stmt->execute(); !r) {
            std::fprintf(stderr, "execute failed: %s\n", r.error().message.c_str());
            return 1;
        }

        int rowNum = 0;
        for (;;) {
            auto row = stmt->fetchRow();
            if (!row) {
                std::fprintf(stderr, "fetch failed: %s\n", row.error().message.c_str());
                return 1;
            }
            if (!*row) break;
            ++rowNum;
            std::printf("row %d: ", rowNum);
            const auto& cols = stmt->columns();
            for (std::size_t i = 0; i < (*row)->size(); ++i) {
                if (i > 0) std::printf(", ");
                printColumnValue(cols[i], (**row)[i]);
            }
            std::printf("\n");
        }
        std::printf("%d row(s)\n", rowNum);

        if (auto r = stmt->close(); !r) {
            std::fprintf(stderr, "close failed: %s\n", r.error().message.c_str());
            return 1;
        }
        if (auto r = stmt->release(); !r) {
            std::fprintf(stderr, "release failed: %s\n", r.error().message.c_str());
            return 1;
        }
    }

    auto disc = conn->disconnectOrderly();
    if (!disc) {
        std::fprintf(stderr, "disconnect failed: %s\n", disc.error().message.c_str());
        return 1;
    }
    std::printf("disconnected cleanly.\n");
    return 0;
}
