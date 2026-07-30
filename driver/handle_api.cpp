// SQLAllocHandle / SQLFreeHandle: the entry points every ODBC session
// starts and ends with, for all three handle types this driver supports so
// far (ENV, DBC, STMT - SQL_HANDLE_DESC is not implemented, per the Phase 4
// slice 1 MVP scope in the plan).

#include "driver/handles.h"

using namespace ifxodbc;

extern "C" {

SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT HandleType, SQLHANDLE InputHandle, SQLHANDLE* OutputHandlePtr) {
    if (!OutputHandlePtr) return SQL_INVALID_HANDLE;

    switch (HandleType) {
        case SQL_HANDLE_ENV: {
            *OutputHandlePtr = new OdbcEnv();
            return SQL_SUCCESS;
        }
        case SQL_HANDLE_DBC: {
            auto* env = static_cast<OdbcEnv*>(InputHandle);
            if (!env) return SQL_INVALID_HANDLE;
            *OutputHandlePtr = new OdbcConnection(env);
            return SQL_SUCCESS;
        }
        case SQL_HANDLE_STMT: {
            auto* conn = static_cast<OdbcConnection*>(InputHandle);
            if (!conn) return SQL_INVALID_HANDLE;
            *OutputHandlePtr = new OdbcStatement(conn);
            return SQL_SUCCESS;
        }
        default:
            *OutputHandlePtr = SQL_NULL_HANDLE;
            return SQL_ERROR; // SQL_HANDLE_DESC and anything else: not yet supported
    }
}

SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT HandleType, SQLHANDLE Handle) {
    if (!Handle) return SQL_INVALID_HANDLE;
    switch (HandleType) {
        case SQL_HANDLE_ENV:
            delete static_cast<OdbcEnv*>(Handle);
            return SQL_SUCCESS;
        case SQL_HANDLE_DBC:
            delete static_cast<OdbcConnection*>(Handle);
            return SQL_SUCCESS;
        case SQL_HANDLE_STMT:
            delete static_cast<OdbcStatement*>(Handle);
            return SQL_SUCCESS;
        default:
            return SQL_ERROR;
    }
}

} // extern "C"
