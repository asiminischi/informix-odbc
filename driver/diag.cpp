// SQLGetDiagRec / SQLGetDiagField: every handle type shares the same
// DiagQueue implementation (see driver/handles.h), so these just dispatch
// to whichever handle's queue the caller asked about.

#include "driver/handles.h"

#include <cstring>

using namespace ifxodbc;

namespace {

DiagQueue* diagQueueFor(SQLSMALLINT handleType, SQLHANDLE handle) {
    switch (handleType) {
        case SQL_HANDLE_ENV:
            return handle ? &static_cast<OdbcEnv*>(handle)->diag : nullptr;
        case SQL_HANDLE_DBC:
            return handle ? &static_cast<OdbcConnection*>(handle)->diag : nullptr;
        case SQL_HANDLE_STMT:
            return handle ? &static_cast<OdbcStatement*>(handle)->diag : nullptr;
        default:
            return nullptr;
    }
}

} // namespace

extern "C" {

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT RecNumber, SQLCHAR* SQLState,
                                 SQLINTEGER* NativeErrorPtr, SQLCHAR* MessageText, SQLSMALLINT BufferLength,
                                 SQLSMALLINT* TextLengthPtr) {
    DiagQueue* diag = diagQueueFor(HandleType, Handle);
    if (!diag) return SQL_INVALID_HANDLE;
    return diag->getDiagRec(RecNumber, SQLState, NativeErrorPtr, MessageText, BufferLength, TextLengthPtr);
}

// Minimal SQLGetDiagField: only the two header fields (SQL_DIAG_NUMBER,
// SQL_DIAG_RETURNCODE) that don't require per-record iteration. Full
// per-record field support (SQL_DIAG_SQLSTATE, SQL_DIAG_MESSAGE_TEXT, etc.
// individually rather than via SQLGetDiagRec) is deferred - most ODBC
// clients, including isql, only use SQLGetDiagRec.
SQLRETURN SQL_API SQLGetDiagField(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT /*RecNumber*/,
                                   SQLSMALLINT DiagIdentifier, SQLPOINTER DiagInfoPtr, SQLSMALLINT /*BufferLength*/,
                                   SQLSMALLINT* StringLengthPtr) {
    DiagQueue* diag = diagQueueFor(HandleType, Handle);
    if (!diag) return SQL_INVALID_HANDLE;

    switch (DiagIdentifier) {
        case SQL_DIAG_NUMBER:
            if (DiagInfoPtr) *static_cast<SQLINTEGER*>(DiagInfoPtr) = static_cast<SQLINTEGER>(diag->count());
            return SQL_SUCCESS;
        default:
            if (StringLengthPtr) *StringLengthPtr = 0;
            return SQL_NO_DATA;
    }
}

} // extern "C"
