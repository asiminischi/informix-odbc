// SQLSetEnvAttr/SQLGetEnvAttr, SQLSetConnectAttr/SQLGetConnectAttr,
// SQLDriverConnect/SQLConnect/SQLDisconnect: the Phase 4 slice 1 core -
// getting a real Driver Manager through a full connect/disconnect cycle.
// See the plan (docs/protocol-notes.md's sibling plan file) for why this
// slice is sequenced first: a DM that can't complete this handshake never
// calls any of the SQL execution functions at all.

#include "driver/connstr.h"
#include "driver/error_map.h"
#include "driver/handles.h"

#include <cstdlib>
#include <cstring>

using namespace ifxodbc;

namespace {

// Shared by SQLDriverConnect and (eventually) SQLConnect: parses the
// driver-keyword attribute set (HOST/SERVICE/SERVER/DATABASE/UID/PWD, per
// the plan) and attempts the underlying sqli connect.
SQLRETURN connectWithAttrs(OdbcConnection& conn, const std::map<std::string, std::string>& attrs) {
    if (conn.connected) {
        postDriverError(conn.diag, "08002", "connection already established");
        return SQL_ERROR;
    }

    std::string host = lookupAny(attrs, {"HOST"});
    std::string serviceStr = lookupAny(attrs, {"SERVICE", "PORT"});
    if (host.empty() || serviceStr.empty()) {
        postDriverError(conn.diag, "HY000", "connection string must specify HOST and SERVICE (port)");
        return SQL_ERROR;
    }
    int port = std::atoi(serviceStr.c_str());

    ifxsqli::ConnectParams params;
    params.serverName = lookupAny(attrs, {"SERVER"}, "informix");
    params.database = lookupAny(attrs, {"DATABASE", "DB"});
    params.username = lookupAny(attrs, {"UID", "USER", "USERNAME"});
    params.password = lookupAny(attrs, {"PWD", "PASSWORD"});

    auto result = ifxsqli::SqliConnection::connect(host, port, params);
    if (!result) {
        postSqliError(conn.diag, result.error());
        return SQL_ERROR;
    }
    conn.sqli.emplace(std::move(*result));
    conn.connected = true;
    return SQL_SUCCESS;
}

} // namespace

extern "C" {

IFXODBC_EXPORT SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute, SQLPOINTER ValuePtr,
                                 SQLINTEGER /*StringLength*/) {
    auto* env = static_cast<OdbcEnv*>(EnvironmentHandle);
    if (!env) return SQL_INVALID_HANDLE;
    env->diag.clear();

    if (Attribute == SQL_ATTR_ODBC_VERSION) {
        env->odbcVersion = static_cast<SQLINTEGER>(reinterpret_cast<std::intptr_t>(ValuePtr));
        return SQL_SUCCESS;
    }
    // Anything else (e.g. SQL_ATTR_CONNECTION_POOLING, a process-global
    // attribute this driver doesn't participate in) is accepted as a no-op
    // rather than rejected, matching how many drivers treat env attributes
    // they don't specifically need to act on.
    return SQL_SUCCESS;
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute, SQLPOINTER ValuePtr,
                                 SQLINTEGER /*BufferLength*/, SQLINTEGER* StringLengthPtr) {
    auto* env = static_cast<OdbcEnv*>(EnvironmentHandle);
    if (!env) return SQL_INVALID_HANDLE;
    env->diag.clear();

    if (Attribute == SQL_ATTR_ODBC_VERSION) {
        if (ValuePtr) *static_cast<SQLINTEGER*>(ValuePtr) = env->odbcVersion;
        if (StringLengthPtr) *StringLengthPtr = sizeof(SQLINTEGER);
        return SQL_SUCCESS;
    }
    return SQL_ERROR;
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLSetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute, SQLPOINTER ValuePtr,
                                     SQLINTEGER /*StringLength*/) {
    auto* conn = static_cast<OdbcConnection*>(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;
    conn->diag.clear();

    switch (Attribute) {
        case SQL_ATTR_AUTOCOMMIT: {
            auto newValue = static_cast<SQLUINTEGER>(reinterpret_cast<std::intptr_t>(ValuePtr));
            // Per the ODBC spec: turning autocommit back on commits
            // whatever transaction is currently open, rather than leaving
            // it dangling with no way left to end it (SQLEndTran would
            // still work, but a caller that just asked for autocommit
            // semantics shouldn't have to know that). No-op if nothing is
            // open, which covers both "was already autocommit" and "was
            // manual-commit but hadn't executed a statement yet."
            if (newValue == SQL_AUTOCOMMIT_ON && conn->transactionOpen) {
                if (!conn->connected || !conn->sqli) {
                    postDriverError(conn->diag, "08003", "connection not open");
                    return SQL_ERROR;
                }
                auto r = conn->sqli->commitTransaction();
                conn->transactionOpen = false;
                if (!r) {
                    postSqliError(conn->diag, r.error());
                    return SQL_ERROR;
                }
            }
            conn->autoCommit = newValue;
            return SQL_SUCCESS;
        }
        case SQL_ATTR_LOGIN_TIMEOUT:
            conn->loginTimeoutSec = static_cast<SQLUINTEGER>(reinterpret_cast<std::intptr_t>(ValuePtr));
            return SQL_SUCCESS;
        default:
            // Round-tripped-but-unenforced for anything else, same
            // rationale as SQLSetEnvAttr above - a DM that sets an
            // attribute it considers routine (e.g. SQL_ATTR_TXN_ISOLATION)
            // shouldn't get SQL_ERROR just because this driver doesn't act
            // on it yet.
            return SQL_SUCCESS;
    }
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute, SQLPOINTER ValuePtr,
                                     SQLINTEGER /*BufferLength*/, SQLINTEGER* StringLengthPtr) {
    auto* conn = static_cast<OdbcConnection*>(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;
    conn->diag.clear();

    switch (Attribute) {
        case SQL_ATTR_AUTOCOMMIT:
            if (ValuePtr) *static_cast<SQLUINTEGER*>(ValuePtr) = conn->autoCommit;
            if (StringLengthPtr) *StringLengthPtr = sizeof(SQLUINTEGER);
            return SQL_SUCCESS;
        case SQL_ATTR_LOGIN_TIMEOUT:
            if (ValuePtr) *static_cast<SQLUINTEGER*>(ValuePtr) = conn->loginTimeoutSec;
            if (StringLengthPtr) *StringLengthPtr = sizeof(SQLUINTEGER);
            return SQL_SUCCESS;
        default:
            return SQL_ERROR;
    }
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLDriverConnect(SQLHDBC ConnectionHandle, SQLHWND /*WindowHandle*/, SQLCHAR* InConnectionString,
                                    SQLSMALLINT StringLength1, SQLCHAR* OutConnectionString,
                                    SQLSMALLINT BufferLength, SQLSMALLINT* StringLength2Ptr,
                                    SQLUSMALLINT /*DriverCompletion*/) {
    auto* conn = static_cast<OdbcConnection*>(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;
    conn->diag.clear();
    if (!InConnectionString) {
        postDriverError(conn->diag, "HY009", "connection string is null");
        return SQL_ERROR;
    }

    std::string_view connStr =
        (StringLength1 == SQL_NTS)
            ? std::string_view(reinterpret_cast<const char*>(InConnectionString))
            : std::string_view(reinterpret_cast<const char*>(InConnectionString), static_cast<std::size_t>(StringLength1));

    auto attrs = parseConnectionString(connStr);
    SQLRETURN rc = connectWithAttrs(*conn, attrs);
    if (rc != SQL_SUCCESS) return rc;

    // No GUI prompting (SQL_DRIVER_PROMPT/COMPLETE) is implemented -
    // DriverCompletion is accepted but ignored, matching SQL_DRIVER_NOPROMPT
    // behavior, since this driver requires a fully-specified connection
    // string rather than an interactive dialog (out of scope: no GUI
    // toolkit dependency for a CLI-oriented MVP driver).
    if (OutConnectionString && BufferLength > 0) {
        auto toCopy = std::min<std::size_t>(connStr.size(), static_cast<std::size_t>(BufferLength - 1));
        std::memcpy(OutConnectionString, connStr.data(), toCopy);
        OutConnectionString[toCopy] = '\0';
    }
    if (StringLength2Ptr) *StringLength2Ptr = static_cast<SQLSMALLINT>(connStr.size());
    return SQL_SUCCESS;
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLConnect(SQLHDBC ConnectionHandle, SQLCHAR* ServerName, SQLSMALLINT /*NameLength1*/,
                              SQLCHAR* UserName, SQLSMALLINT /*NameLength2*/, SQLCHAR* Authentication,
                              SQLSMALLINT /*NameLength3*/) {
    auto* conn = static_cast<OdbcConnection*>(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;
    conn->diag.clear();

    // DSN-based connect (odbc.ini lookup via SQLGetPrivateProfileString) is
    // not implemented yet - this driver's MVP path is SQLDriverConnect's
    // explicit-keyword form. ServerName is accepted as the INFORMIXSERVER
    // name directly (not a DSN) so a caller that already knows HOST/SERVICE
    // out-of-band could still succeed, but that's not the intended path.
    postDriverError(conn->diag, "HYC00",
                     "SQLConnect (DSN-based connect) is not yet implemented; use SQLDriverConnect");
    (void)ServerName;
    (void)UserName;
    (void)Authentication;
    return SQL_ERROR;
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLDisconnect(SQLHDBC ConnectionHandle) {
    auto* conn = static_cast<OdbcConnection*>(ConnectionHandle);
    if (!conn) return SQL_INVALID_HANDLE;
    conn->diag.clear();
    if (!conn->connected || !conn->sqli) {
        postDriverError(conn->diag, "08003", "connection not open");
        return SQL_ERROR;
    }

    // An unresolved manual-commit transaction shouldn't be left for the
    // server to decide about on its own - roll it back explicitly so
    // disconnecting has the same "nothing half-applied" guarantee
    // regardless of autocommit mode. Best-effort: a failed rollback here
    // doesn't block the disconnect itself, since there would be no way to
    // retry it afterward anyway.
    if (conn->transactionOpen) {
        (void)conn->sqli->rollbackTransaction();
        conn->transactionOpen = false;
    }

    auto result = conn->sqli->disconnectOrderly();
    conn->sqli.reset();
    conn->connected = false;
    if (!result) {
        postSqliError(conn->diag, result.error());
        return SQL_ERROR;
    }
    return SQL_SUCCESS;
}

} // extern "C"
