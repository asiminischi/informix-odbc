#pragma once

// Maps sqli/error.h's (ErrorKind, message) shape onto ODBC SQLSTATEs and
// posts the result to a handle's diagnostic queue. This is deliberately
// coarse for Phase 4 slice 1 (the plan's slice 4 owns a full, correct
// sqlcode -> SQLSTATE table) - the goal here is just that every failure
// path produces a *valid, well-formed* SQLSTATE a caller can branch on via
// SQLGetDiagRec, never a crash or an empty diag queue.

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
