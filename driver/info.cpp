// SQLGetInfo / SQLGetFunctions: driver/DBMS introspection the Driver
// Manager (and clients like isql) use to decide how to talk to us. The
// plan's Phase 4 slice 1 explicitly calls out not guessing this list -
// this is a reasonable starting baseline to get a real DM through connect;
// expect to extend it once tested against an actual ODBC trace.

#include "driver/error_map.h"
#include "driver/handles.h"

#include <cstring>

using namespace ifxodbc;

namespace {

SQLRETURN writeString(SQLPOINTER out, SQLSMALLINT bufferLength, SQLSMALLINT* lengthPtr, const char* value) {
    auto len = static_cast<SQLSMALLINT>(std::strlen(value));
    if (lengthPtr) *lengthPtr = len;
    if (out && bufferLength > 0) {
        auto toCopy = std::min<SQLSMALLINT>(len, static_cast<SQLSMALLINT>(bufferLength - 1));
        std::memcpy(out, value, static_cast<std::size_t>(toCopy));
        static_cast<char*>(out)[toCopy] = '\0';
    }
    return SQL_SUCCESS;
}

template <class T>
SQLRETURN writeNumeric(SQLPOINTER out, SQLSMALLINT* lengthPtr, T value) {
    if (out) *static_cast<T*>(out) = value;
    if (lengthPtr) *lengthPtr = sizeof(T);
    return SQL_SUCCESS;
}

// Marks a function as supported in the ODBC 3.0 SQLGetFunctions bitmap
// (see sqlext.h's SQL_FUNC_EXISTS for the read-side counterpart of this
// exact bit layout: one bit per function ID, packed 16 per SQLUSMALLINT).
void markSupported(SQLUSMALLINT* bitmap, SQLUSMALLINT functionId) {
    bitmap[functionId >> 4] |= static_cast<SQLUSMALLINT>(1u << (functionId & 0x0F));
}

} // namespace

extern "C" {

IFXODBC_EXPORT SQLRETURN SQL_API SQLGetInfo(SQLHDBC ConnectionHandle, SQLUSMALLINT InfoType, SQLPOINTER InfoValuePtr,
                              SQLSMALLINT BufferLength, SQLSMALLINT* StringLengthPtr) {
    auto* conn = static_cast<OdbcConnection*>(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;
    conn->diag.clear();

    switch (InfoType) {
        case SQL_DRIVER_NAME:
#ifdef _WIN32
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, "ifxodbc.dll");
#else
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, "libifxodbc.so");
#endif
        case SQL_DRIVER_VER:
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, "00.01.0000");
        case SQL_DRIVER_ODBC_VER:
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, "03.80");
        case SQL_DBMS_NAME:
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, "Informix Dynamic Server");
        case SQL_DBMS_VER:
            if (conn->connected && conn->sqli) {
                return writeString(InfoValuePtr, BufferLength, StringLengthPtr,
                                    conn->sqli->serverInfo().serverVersion.c_str());
            }
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, "unknown");
        case SQL_DATA_SOURCE_NAME:
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, "");
        case SQL_IDENTIFIER_QUOTE_CHAR:
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, "\"");
        case SQL_SEARCH_PATTERN_ESCAPE:
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, "\\");
        case SQL_CATALOG_NAME_SEPARATOR:
            return writeString(InfoValuePtr, BufferLength, StringLengthPtr, ".");
        case SQL_TXN_CAPABLE:
            // SQL_TC_DML, not SQL_TC_ALL/SQL_TC_DDL_COMMIT: SQ_BEGIN/
            // SQ_CMMTWORK/SQ_RBWORK are confirmed live for DML statements
            // (see docs/protocol-notes.md's Transactions section), but
            // whether DDL inside an open transaction behaves the way any
            // of the SQL_TC_DDL_* values promise hasn't been checked.
            // Was SQL_TC_NONE before SQLEndTran existed - left at NONE
            // this long was itself a bug once transactions were wired up:
            // unixODBC's Driver Manager consults this (together with
            // SQLGetFunctions) before it will even attempt to call
            // SQLEndTran, and returns IM001 itself without reaching this
            // driver's code at all if it's NONE.
            return writeNumeric<SQLUSMALLINT>(InfoValuePtr, StringLengthPtr, SQL_TC_DML);
        case SQL_GETDATA_EXTENSIONS:
            return writeNumeric<SQLUINTEGER>(InfoValuePtr, StringLengthPtr, 0);
        case SQL_MAX_COLUMN_NAME_LEN:
            return writeNumeric<SQLUSMALLINT>(InfoValuePtr, StringLengthPtr, 128);
        case SQL_CURSOR_COMMIT_BEHAVIOR:
        case SQL_CURSOR_ROLLBACK_BEHAVIOR:
            return writeNumeric<SQLUSMALLINT>(InfoValuePtr, StringLengthPtr, SQL_CB_PRESERVE);
        case SQL_DEFAULT_TXN_ISOLATION:
            return writeNumeric<SQLUINTEGER>(InfoValuePtr, StringLengthPtr, SQL_TXN_READ_COMMITTED);
        case SQL_ACTIVE_ENVIRONMENTS:
            return writeNumeric<SQLUSMALLINT>(InfoValuePtr, StringLengthPtr, 0);
        default:
            postDriverError(conn->diag, "HY096", "unsupported SQLGetInfo InfoType");
            return SQL_ERROR;
    }
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLGetFunctions(SQLHDBC ConnectionHandle, SQLUSMALLINT FunctionId, SQLUSMALLINT* SupportedPtr) {
    auto* conn = static_cast<OdbcConnection*>(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;
    conn->diag.clear();
    if (!SupportedPtr) return SQL_ERROR;

    // Functions implemented so far (Phase 4 slices 1-2: handle lifetime +
    // connect/disconnect + diagnostics/info + the SQL_C_CHAR SELECT round
    // trip). Extended as later slices land.
    static constexpr SQLUSMALLINT kImplemented[] = {
        SQL_API_SQLALLOCHANDLE,     SQL_API_SQLFREEHANDLE,      SQL_API_SQLSETENVATTR,
        SQL_API_SQLGETENVATTR,      SQL_API_SQLSETCONNECTATTR,  SQL_API_SQLGETCONNECTATTR,
        SQL_API_SQLDRIVERCONNECT,   SQL_API_SQLDISCONNECT,      SQL_API_SQLGETDIAGREC,
        SQL_API_SQLGETDIAGFIELD,    SQL_API_SQLGETINFO,         SQL_API_SQLGETFUNCTIONS,
        SQL_API_SQLPREPARE,         SQL_API_SQLEXECUTE,         SQL_API_SQLEXECDIRECT,
        SQL_API_SQLNUMRESULTCOLS,   SQL_API_SQLDESCRIBECOL,     SQL_API_SQLCOLATTRIBUTE,
        SQL_API_SQLCOLATTRIBUTES,
        SQL_API_SQLBINDCOL,         SQL_API_SQLFETCH,           SQL_API_SQLGETDATA,
        SQL_API_SQLFREESTMT,        SQL_API_SQLROWCOUNT,        SQL_API_SQLSETSTMTATTR,
        SQL_API_SQLGETSTMTATTR,     SQL_API_SQLCANCEL,          SQL_API_SQLBINDPARAMETER,
        SQL_API_SQLENDTRAN,         SQL_API_SQLTABLES,
    };
    auto isImplemented = [](SQLUSMALLINT id) {
        for (auto f : kImplemented) {
            if (f == id) return true;
        }
        return false;
    };

    if (FunctionId == SQL_API_ODBC3_ALL_FUNCTIONS) {
        std::memset(SupportedPtr, 0, sizeof(SQLUSMALLINT) * SQL_API_ODBC3_ALL_FUNCTIONS_SIZE);
        for (auto f : kImplemented) markSupported(SupportedPtr, f);
        return SQL_SUCCESS;
    }
    if (FunctionId == SQL_API_ALL_FUNCTIONS) {
        for (SQLUSMALLINT i = 0; i < 100; ++i) SupportedPtr[i] = SQL_FALSE;
        for (auto f : kImplemented) {
            if (f < 100) SupportedPtr[f] = SQL_TRUE;
        }
        return SQL_SUCCESS;
    }
    *SupportedPtr = isImplemented(FunctionId) ? SQL_TRUE : SQL_FALSE;
    return SQL_SUCCESS;
}

} // extern "C"
