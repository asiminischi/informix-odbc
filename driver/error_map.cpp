#include "driver/error_map.h"

#include <cstdlib>

namespace ifxodbc {

namespace {

// sqli/connection.cpp and sqli/statement.cpp format Server errors as
// "sqlcode N: message" (see readErrorRecord in sqli/connection.cpp) -
// parsed back out here so SQLGetDiagRec's native-error-code field carries
// the original Informix sqlcode rather than 0.
SQLINTEGER extractSqlcode(const std::string& message) {
    if (message.rfind("sqlcode ", 0) != 0) return 0;
    return static_cast<SQLINTEGER>(std::strtol(message.c_str() + 8, nullptr, 10));
}

} // namespace

void postSqliError(DiagQueue& diag, const ifxsqli::Error& err) {
    switch (err.kind) {
        case ifxsqli::ErrorKind::Transport:
            diag.push("08001", err.message); // unable to connect
            break;
        case ifxsqli::ErrorKind::Server:
            diag.push("HY000", err.message, extractSqlcode(err.message));
            break;
        case ifxsqli::ErrorKind::Protocol:
            diag.push("HY000", err.message);
            break;
    }
}

void postDriverError(DiagQueue& diag, const char* sqlstate, std::string message, SQLINTEGER nativeError) {
    diag.push(sqlstate, std::move(message), nativeError);
}

} // namespace ifxodbc
