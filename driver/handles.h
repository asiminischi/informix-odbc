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

#include <cstddef>
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

    // The DATABASE= connection-string keyword this connection was opened
    // with (empty if none was given). SqliConnection itself doesn't retain
    // it - only driver/connect.cpp's connectWithAttrs sees the parsed
    // ConnectParams - so it's captured here for driver/catalog.cpp's
    // SQLTables/SQLColumns, which report it as TABLE_CAT and use it to
    // recognize a caller asking about a different (unsupported) catalog.
    std::string database;

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
    // Unused by every MVP-8 bind type (each is either self-describing or
    // carries its own precision intrinsically - see driver/bind_encode.h's
    // top comment) - INTERVAL is the first type that genuinely needs
    // these: its leading field width has no other source at bind time
    // (see encodeBindParam's SQL_INTERVAL_* branch).
    SQLULEN columnSize = 0;
    SQLSMALLINT decimalDigits = 0;
};

// SQLGetData's resumable-fetch state for one simple LOB (BYTE/TEXT)
// column in the current row - genuinely needed only here: every other
// type's SQLGetData call is single-shot (decode raw bytes -> write once),
// but a LOB's actual content needs its own network round trip
// (SqliConnection::fetchSimpleLob) and can be arbitrarily large, so the
// ODBC-standard "call SQLGetData repeatedly with a small buffer" idiom
// needs somewhere to remember how much has already been handed back.
// Keyed per column in OdbcStatement::lobFetchState, cleared on every
// SQLFetch (a fresh row means a fresh LOB, not a continuation of the
// previous row's). NOT used for a SQLBindCol-driven fetch (SQLFetch's
// bound-column loop) - that path stays single-shot-per-row, matching
// every other bound column type's existing truncate-if-too-small
// contract, by simply never persisting a LobFetchState across calls.
struct LobFetchState {
    std::vector<std::byte> content;
    std::size_t offset = 0;
    bool loaded = false;
    // True once every byte has been handed to the caller via a
    // non-truncated SQLGetData call - the actual "no more data" signal
    // (SQL_NO_DATA), distinct from `offset == content.size()` alone,
    // which a genuinely empty (0-byte, non-NULL) LOB value would satisfy
    // on its very first call too.
    bool exhausted = false;
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

    // The exact text last passed to SQLPrepare/SQLExecDirect - sqli::
    // SqliStatement doesn't retain it (see sqli/statement.h), but a smart
    // LOB "?" bind (driver/statement.cpp's buildBindParams) needs the
    // target table name to call SqliConnection::createSmartLob, and
    // DESCRIBE's own paramDescs() only carries the target *column* name,
    // not its table - so this driver has to keep the SQL text around and
    // narrowly parse "insert into <table>" out of it itself.
    std::string preparedSql;
    // The most recently fetched row's raw per-column byte buffers (as
    // returned by SqliStatement::fetchRow), consulted by SQLGetData for
    // columns that weren't bound via SQLBindCol.
    std::optional<std::vector<std::vector<std::byte>>> currentRow;

    // Simple LOB (BYTE/TEXT) SQLGetData resumable-fetch state, keyed by
    // column number - see LobFetchState's doc comment. Cleared on every
    // SQLFetch.
    std::map<SQLUSMALLINT, LobFetchState> lobFetchState;
};

} // namespace ifxodbc
