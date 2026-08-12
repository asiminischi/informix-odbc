// SQLEndTran: commits or rolls back a connection's current transaction,
// wired to the already-confirmed-live SqliConnection::commitTransaction/
// rollbackTransaction (see docs/protocol-notes.md's Transactions section).
// The other three places a transaction's lifecycle gets resolved -
// lazily opening one on the first statement after autocommit is off
// (driver/statement.cpp's doExecute), auto-committing one when autocommit
// is turned back on (driver/connect.cpp's SQLSetConnectAttr), and rolling
// one back on disconnect (driver/connect.cpp's SQLDisconnect) - live next
// to the state they touch rather than here.

#include "driver/error_map.h"
#include "driver/handles.h"

using namespace ifxodbc;

extern "C" {

IFXODBC_EXPORT SQLRETURN SQL_API SQLEndTran(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT CompletionType) {
    if (!Handle) return SQL_INVALID_HANDLE;

    if (HandleType == SQL_HANDLE_ENV) {
        // Ending every connection under an environment in one call isn't
        // implemented - this driver has no env-to-connections index to
        // walk (OdbcEnv doesn't track its child OdbcConnections). Same
        // "diagnostic, not a crash or silent no-op" treatment as
        // SQLConnect's DSN-based path in driver/connect.cpp.
        auto* env = static_cast<OdbcEnv*>(Handle);
        env->diag.clear();
        postDriverError(env->diag, "HYC00", "SQLEndTran(SQL_HANDLE_ENV) is not yet implemented; call it per-connection");
        return SQL_ERROR;
    }
    if (HandleType != SQL_HANDLE_DBC) return SQL_INVALID_HANDLE;

    auto* conn = static_cast<OdbcConnection*>(Handle);
    conn->diag.clear();
    if (!conn->connected || !conn->sqli) {
        postDriverError(conn->diag, "08003", "connection not open");
        return SQL_ERROR;
    }
    if (!conn->transactionOpen) {
        // Matches typical driver behavior: SQLEndTran on a connection with
        // no open transaction (e.g. still under autocommit, or already
        // resolved) is a no-op success, not an error.
        return SQL_SUCCESS;
    }

    auto result =
        (CompletionType == SQL_COMMIT) ? conn->sqli->commitTransaction() : conn->sqli->rollbackTransaction();
    conn->transactionOpen = false;
    if (!result) {
        postSqliError(conn->diag, result.error());
        return SQL_ERROR;
    }
    return SQL_SUCCESS;
}

} // extern "C"
