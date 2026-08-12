#pragma once

// ODBC handle wrappers: Environment -> Connection -> Statement, each owning
// a diagnostic record queue per the ODBC spec (SQLGetDiagRec is called
// against whichever handle the failing call was made on). These are plain
// heap-allocated objects; the ODBC-visible SQLHANDLE is just their address
// cast to SQLHANDLE (a void*) - the Driver Manager treats it as opaque and
// always hands it back verbatim on every subsequent call, so no separate
// handle-table/registry is needed.

#include "sqli/connection.h"
#include "sqli/statement.h"

#ifdef _WIN32
// sql.h/sqlext.h assume windows.h has already provided DWORD/WORD/BYTE/
// BOOL/WCHAR/GUID/etc. - standard practice for ODBC headers on Windows.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

// Marks the small, intentional set of ODBC entry points this driver
// exports. Necessary (not just tidy) on both platforms: on Linux, this
// driver is dlopen'd by a Driver Manager that is itself a shared object
// exporting functions with the exact same names (SQLPrepare, SQLExecute,
// etc.) - without restricting everything else to hidden visibility (set via
// CMAKE_CXX_VISIBILITY_PRESET in driver/CMakeLists.txt), an internal
// cross-call between two of this driver's own functions can be silently
// interposed by the DM's same-named symbol instead of resolving locally
// (confirmed live - see the comment on doPrepare/doExecute in
// driver/statement.cpp for the exact failure this caused). On Windows, a
// DLL exports nothing by default at all unless explicitly marked; without
// this, none of these functions would be reachable via GetProcAddress.
#ifdef _WIN32
#define IFXODBC_EXPORT __declspec(dllexport)
#else
#define IFXODBC_EXPORT __attribute__((visibility("default")))
#endif

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ifxodbc {

struct DiagRecord {
    std::string sqlstate;  // always exactly 5 characters, e.g. "08001"
    std::string message;
    SQLINTEGER nativeError = 0;
};

// One handle's queue of diagnostic records from its most recent call. Per
// the ODBC spec, a new call that returns anything other than SQL_SUCCESS
// replaces the queue; SQL_SUCCESS clears it. SQLGetDiagRec walks the queue
// by 1-based RecNumber.
class DiagQueue {
public:
    void clear() { records_.clear(); }
    void push(std::string sqlstate, std::string message, SQLINTEGER nativeError = 0) {
        records_.push_back(DiagRecord{std::move(sqlstate), std::move(message), nativeError});
    }
    [[nodiscard]] bool empty() const { return records_.empty(); }
    [[nodiscard]] std::size_t count() const { return records_.size(); }

    // Shared implementation of SQLGetDiagRec for any handle type (the
    // record shape and lookup logic don't depend on which handle owns the
    // queue).
    [[nodiscard]] SQLRETURN getDiagRec(SQLSMALLINT recNumber, SQLCHAR* sqlState, SQLINTEGER* nativeErrorPtr,
                                        SQLCHAR* messageText, SQLSMALLINT bufferLength,
                                        SQLSMALLINT* textLengthPtr) const;

private:
    std::vector<DiagRecord> records_;
};

struct OdbcEnv {
    DiagQueue diag;
    SQLINTEGER odbcVersion = SQL_OV_ODBC3;
};

struct OdbcConnection {
    explicit OdbcConnection(OdbcEnv* e) : env(e) {}

    OdbcEnv* env;
    DiagQueue diag;
    std::optional<ifxsqli::SqliConnection> sqli;
    bool connected = false;

    // ODBC connection attributes this driver tracks but doesn't yet act on
    // (SQLSetConnectAttr/SQLGetConnectAttr round-trip them so callers that
    // set-then-get don't see a spurious failure - see Phase 4 slice 1 notes
    // in the plan for why this matters for a clean DM handshake).
    SQLUINTEGER autoCommit = SQL_AUTOCOMMIT_ON;
    SQLUINTEGER loginTimeoutSec = 0;

    // True between a lazily-issued SQ_BEGIN (see driver/statement.cpp's
    // doExecute, which opens one on the first statement after autocommit
    // was turned off, or after the previous SQLEndTran) and the matching
    // SQ_CMMTWORK/SQ_RBWORK. Always false while autoCommit is
    // SQL_AUTOCOMMIT_ON - see driver/transaction.cpp's SQLEndTran and
    // driver/connect.cpp's SQLSetConnectAttr/SQLDisconnect for the other
    // three places this gets resolved.
    bool transactionOpen = false;
};

// A statement's lifecycle, mirroring what the ODBC spec calls "prepared"
// and "executed" cursor states. SQLFetch/SQLGetData are only valid in
// kExecuted; SQLExecute/SQLExecDirect require having reached at least
// kPrepared first.
enum class StmtState { kUnprepared, kPrepared, kExecuted };

// One SQLBindCol() binding: where to write a column's value on each
// subsequent SQLFetch, and how. Phase 4 slice 2 only supports SQL_C_CHAR/
// SQL_C_DEFAULT (see driver/convert.h) - native C types are slice 3.
struct BoundColumn {
    SQLSMALLINT targetType = SQL_C_CHAR;
    SQLPOINTER targetValuePtr = nullptr;
    SQLLEN bufferLength = 0;
    SQLLEN* strLenOrIndPtr = nullptr;
};

// One SQLBindParameter() binding for a "?" input parameter. Unlike
// BoundColumn (an output binding read *from* a fetched row), this is read
// at SQLExecute time from ParameterValuePtr/StrLenOrIndPtr to build a
// sqli::BindParam - see driver/convert_native.h's encode-direction
// helpers.
struct ParamBinding {
    SQLSMALLINT valueType = SQL_C_DEFAULT;    // SQL_C_* - how to interpret ParameterValuePtr
    SQLSMALLINT parameterType = SQL_INTEGER;  // SQL_* - the target Informix wire type
    SQLPOINTER parameterValuePtr = nullptr;
    SQLLEN bufferLength = 0;
    SQLLEN* strLenOrIndPtr = nullptr;
};

struct OdbcStatement {
    explicit OdbcStatement(OdbcConnection* c) : conn(c) {}

    // Releases the server-side statement (SQ_RELEASE) when this handle is
    // torn down (SQLFreeHandle(SQL_HANDLE_STMT, ...) and
    // SQLFreeStmt(..., SQL_DROP) both just `delete` this object - neither
    // called SqliStatement::release() before this destructor existed,
    // silently leaking every prepared statement's server-side resources
    // for the lifetime of the connection). Best-effort: a destructor can't
    // propagate an error, and if the connection has already dropped there
    // is nothing meaningful left to release anyway.
    ~OdbcStatement() {
        if (stmt && conn && conn->connected) {
            (void)stmt->release();
        }
    }

    OdbcConnection* conn;
    DiagQueue diag;
    std::optional<ifxsqli::SqliStatement> stmt;

    StmtState state = StmtState::kUnprepared;
    std::map<SQLUSMALLINT, BoundColumn> boundColumns;
    std::map<SQLUSMALLINT, ParamBinding> paramBindings;
    // The most recently fetched row's raw per-column byte buffers (as
    // returned by SqliStatement::fetchRow), consulted by SQLGetData for
    // columns that weren't bound via SQLBindCol.
    std::optional<std::vector<std::vector<std::byte>>> currentRow;
};

} // namespace ifxodbc
