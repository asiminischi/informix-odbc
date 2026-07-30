// SQLSetStmtAttr/SQLGetStmtAttr: many ODBC clients (isql included) set a
// handful of routine statement attributes before executing anything (e.g.
// SQL_ATTR_ROW_ARRAY_SIZE, cursor type) even for the simplest single-row
// fetch. This driver doesn't act on any of them yet (no scrollable
// cursors, no row-array batching - forward-only, one-row-at-a-time fetch
// only, per the Phase 4 MVP scope), so they're round-tripped rather than
// rejected, matching SQLSetConnectAttr's rationale in driver/connect.cpp.

#include "driver/handles.h"

using namespace ifxodbc;

extern "C" {

SQLRETURN SQL_API SQLSetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER /*Attribute*/, SQLPOINTER /*ValuePtr*/,
                                  SQLINTEGER /*StringLength*/) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER Attribute, SQLPOINTER ValuePtr,
                                  SQLINTEGER /*BufferLength*/, SQLINTEGER* StringLengthPtr) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;

    // The two attributes worth a real answer even before slice 3's fuller
    // attribute support: cursor type and row array size, both fixed at
    // "forward-only, one row" for this driver's current scope.
    switch (Attribute) {
        case SQL_ATTR_CURSOR_TYPE:
            if (ValuePtr) *static_cast<SQLULEN*>(ValuePtr) = SQL_CURSOR_FORWARD_ONLY;
            if (StringLengthPtr) *StringLengthPtr = sizeof(SQLULEN);
            return SQL_SUCCESS;
        case SQL_ATTR_ROW_ARRAY_SIZE:
            if (ValuePtr) *static_cast<SQLULEN*>(ValuePtr) = 1;
            if (StringLengthPtr) *StringLengthPtr = sizeof(SQLULEN);
            return SQL_SUCCESS;
        default:
            return SQL_SUCCESS; // unrecognized-but-tolerated, see file comment
    }
}

SQLRETURN SQL_API SQLCancel(SQLHSTMT StatementHandle) {
    // No asynchronous execution or separate cancel thread exists yet -
    // accepted as a no-op success, since this driver's calls are already
    // synchronous/blocking by the time SQLCancel could plausibly be called.
    return static_cast<OdbcStatement*>(StatementHandle) ? SQL_SUCCESS : SQL_INVALID_HANDLE;
}

} // extern "C"
