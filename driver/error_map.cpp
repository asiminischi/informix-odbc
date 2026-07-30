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

// Informix sqlcode -> ODBC 3.x SQLSTATE, for the sqlcodes common enough to
// be worth a specific answer instead of the HY000 ("general error")
// fallback every unmapped code still gets. Cross-referenced against the
// standard Informix sqlcode/SQLSTATE correspondence (the same X/Open-
// derived mapping IBM's own JDBC driver ships, exposed there as a resource
// bundle of {sqlcode: SQLSTATE} pairs) purely as a research aid for which
// *standard, publicly-documented* SQLSTATE class each sqlcode belongs to -
// this table is our own from-scratch code, not a copy of that resource
// file, matching this project's existing rule for the JDBC decompile
// (protocol facts, not implementation, are what gets reused). A few
// entries there use the legacy pre-ODBC3 "SxxNN"-style class codes (e.g.
// S0002); those are translated here to their documented ODBC 3.x
// successors (S0002 -> 42S02, S1001 -> HY001, etc. - a well-known,
// published correspondence, not a guess) since this driver only ever
// negotiates SQL_OV_ODBC3.
const char* sqlstateForSqlcode(SQLINTEGER sqlcode) {
    switch (sqlcode) {
        case 100: return "02000";     // no data
        case -201: return "42000";    // syntax error
        case -206: return "42S02";    // table/view not found
        case -208: return "HY001";    // memory allocation failure
        case -236: return "21S01";    // insert value list does not match column list
        case -239: return "23000";    // unique constraint (duplicate value for a column with a unique constraint)
        case -254: return "07001";    // wrong number of host variables/parameters
        case -259: return "24000";    // invalid cursor state
        case -260: return "07005";    // prepared statement not a cursor specification
        case -262:
        case -266: return "24000";    // invalid cursor state
        case -268: return "23000";    // unique constraint violation (duplicate key)
        case -272:
        case -273:
        case -274:
        case -275: return "42000";    // syntax/access rule violation
        case -276: return "24000";    // invalid cursor state
        case -284: return "21000";    // cardinality violation (subquery returned >1 row)
        case -285: return "24000";    // invalid cursor state
        case -306: return "22003";    // numeric value out of range
        case -310: return "42S01";    // base table already exists
        case -316:
        case -350: return "42S11";    // index already exists
        case -319: return "42S12";    // index not found
        case -328: return "42S21";    // column already exists
        case -352: return "42S02";    // table not found
        case -371:
        case -391:
        case -525:
        case -530:
        case -536:
        case -551:
        case -676:
        case -691:
        case -692: return "23000";    // integrity constraint violation
        case -385: return "44000";    // WITH CHECK OPTION violation
        case -400: return "24000";    // invalid cursor state
        case -404: return "07003";    // cursor specification cannot be executed
        case -406: return "HY001";    // memory allocation failure
        case -410:
        case -481:
        case -483: return "37000";    // dynamic SQL syntax error/access violation
        case -467: return "22002";    // indicator variable required but not supplied
        case -471: return "33000";    // invalid SQL descriptor name
        case -472: return "22003";    // numeric value out of range
        case -543: return "22019";    // invalid escape character
        case -586: return "24000";    // invalid cursor state
        case -598: return "34000";    // invalid cursor name
        case -699: return "40000";    // transaction rollback
        case -766: return "22024";    // string data, length mismatch
        // -839/-840/-841 (legacy "S0000") intentionally omitted: no
        // confidently-known ODBC3 successor class, and guessing one wrong
        // is worse than the honest HY000 fallback every unmapped code
        // already gets.
        case -879: return "22027";    // trim error
        case -886: return "42000";    // syntax/access rule violation
        case -887: return "2B000";    // dependent privilege descriptors still exist
        case -901:
        case -902: return "28000";    // invalid authorization
        case -908:
        case -930: return "08004";    // server rejected connection
        case -934: return "08006";    // connection failure
        case -942: return "42000";    // syntax/access rule violation
        case -1202: return "22012";   // division by zero
        case -1214:
        case -1215:
        case -1222:
        case -1226:
        case -22283: return "22003"; // numeric value out of range
        case -1278: return "22025";   // invalid escape sequence
        case -1279: return "22001";   // string data, right truncation
        case -1800: return "25000";   // invalid transaction state
        case -1801: return "0A001";   // multiple server transactions not supported
        case -1802: return "08002";   // connection already in use
        case -1803: return "08003";   // connection does not exist
        case -1805: return "2E000";   // invalid connection name
        case -1806:
        case -1809: return "08006";   // connection failure
        case -1810: return "01002";   // disconnect error (warning)
        default: return nullptr;      // no specific mapping - caller falls back to HY000
    }
}

} // namespace

void postSqliError(DiagQueue& diag, const ifxsqli::Error& err) {
    switch (err.kind) {
        case ifxsqli::ErrorKind::Transport:
            diag.push("08001", err.message); // unable to connect
            break;
        case ifxsqli::ErrorKind::Server: {
            SQLINTEGER sqlcode = extractSqlcode(err.message);
            const char* mapped = sqlstateForSqlcode(sqlcode);
            diag.push(mapped ? mapped : "HY000", err.message, sqlcode);
            break;
        }
        case ifxsqli::ErrorKind::Protocol:
            diag.push("HY000", err.message);
            break;
    }
}

void postDriverError(DiagQueue& diag, const char* sqlstate, std::string message, SQLINTEGER nativeError) {
    diag.push(sqlstate, std::move(message), nativeError);
}

} // namespace ifxodbc
