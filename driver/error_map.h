#pragma once

// Maps sqli/error.h's (ErrorKind, message) shape onto ODBC SQLSTATEs and
// posts the result to a handle's diagnostic queue. Server errors get a
// specific SQLSTATE for the sqlcodes covered by error_map.cpp's mapping
// table (Phase 4 slice 4); anything not in that table still gets a valid,
// well-formed fallback (HY000) rather than a crash or an empty diag queue -
// the table's coverage is necessarily partial (there is no complete public
// sqlcode/SQLSTATE spec to implement against), not a promise every sqlcode
// has a precise answer.

#include "driver/handles.h"
#include "sqli/error.h"

namespace ifxodbc {

// Posts `err` to `diag` as a single diagnostic record, choosing a SQLSTATE
// from its ErrorKind. Server errors keep the raw Informix sqlcode as the
// native error code (parsed back out of Error::message, which
// sqli/connection.cpp and sqli/statement.cpp format as "sqlcode N: ...").
void postSqliError(DiagQueue& diag, const ifxsqli::Error& err);

// Posts a driver-generated (not sqli-layer) diagnostic - state-machine
// errors (HY010), not-yet-implemented functions (HYC00), invalid arguments
// (HY009/HY090), etc.
void postDriverError(DiagQueue& diag, const char* sqlstate, std::string message, SQLINTEGER nativeError = 0);

} // namespace ifxodbc
