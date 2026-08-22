// Verifies Phase 4 slice 1 (see the plan) through the *real* platform ODBC
// Driver Manager - not a direct call into our own driver code, which
// wouldn't prove the DM can load/talk to it. Links against the system's
// odbc32.lib (Windows) so this goes through the same SQLAllocHandle/
// SQLDriverConnect path any ODBC application (isql, pyodbc, etc.) uses.
//
// Loads the driver directly via the connection string's DRIVER={path}
// keyword rather than requiring odbcinst.ini/registry registration, so this
// runs with no admin/install step - just point it at the built .dll/.so.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void printDiag(SQLSMALLINT handleType, SQLHANDLE handle, const char* context) {
    SQLCHAR sqlState[6];
    SQLINTEGER nativeError = 0;
    SQLCHAR message[512];
    SQLSMALLINT textLength = 0;
    SQLSMALLINT rec = 1;
    while (SQLGetDiagRec(handleType, handle, rec, sqlState, &nativeError, message, sizeof(message), &textLength) ==
           SQL_SUCCESS) {
        std::printf("  [%s] diag #%d: SQLSTATE=%s native=%d: %s\n", context, rec, sqlState, static_cast<int>(nativeError),
                    message);
        ++rec;
    }
}

bool execDirect(SQLHDBC dbc, const char* sql) {
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) return false;
    SQLRETURN rc = SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        std::printf("execDirect(\"%s\") failed rc=%d\n", sql, static_cast<int>(rc));
        printDiag(SQL_HANDLE_STMT, stmt, "stmt");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return false;
    }
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return true;
}

// Verifies Phase 4 slice 3 (see the plan): native SQL_C_* conversions
// (SQL_C_SSHORT/SQL_C_SLONG/SQL_C_SBIGINT/SQL_C_DOUBLE, SQL_DATE_STRUCT,
// SQL_TIMESTAMP_STRUCT) and real SQLRowCount for an UPDATE, all through the
// real Driver Manager - not just unit-testing driver/convert_native.cpp in
// isolation. Reuses the same fixture values as
// tests/integration/test_type_probe.cpp (positive row, negative row, all-
// NULL row) so the expected values are already independently confirmed live.
bool runNativeTypeTest(SQLHDBC dbc) {
    bool ok = true;
    ok &= execDirect(dbc, "drop table if exists native_probe");
    ok &= execDirect(dbc,
                      "create table native_probe ("
                      "  id serial not null, col_smallint smallint, col_integer integer,"
                      "  col_bigint bigint, col_decimal decimal(10,2), col_date date,"
                      "  col_datetime datetime year to second)");
    ok &= execDirect(dbc,
                      "insert into native_probe (col_smallint, col_integer, col_bigint,"
                      " col_decimal, col_date, col_datetime) values (12345, 1234567890,"
                      " 123456789012345, 12345.67, DATE('2026-07-29'),"
                      " DATETIME(2026-07-29 12:34:56) YEAR TO SECOND)");
    ok &= execDirect(dbc,
                      "insert into native_probe (col_smallint, col_integer, col_bigint,"
                      " col_decimal, col_date, col_datetime) values (-12345, -2147483000,"
                      " -123456789012345, -99.99, DATE('2000-01-01'),"
                      " DATETIME(2000-01-01 00:00:00) YEAR TO SECOND)");
    ok &= execDirect(dbc,
                      "insert into native_probe (col_smallint, col_integer, col_bigint,"
                      " col_decimal, col_date, col_datetime) values (null, null, null,"
                      " null, null, null)");
    if (!ok) {
        std::printf("[native] fixture setup failed, aborting native-type test\n");
        return false;
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) return false;

    const char* sql =
        "select col_smallint, col_integer, col_bigint, col_decimal, col_date, col_datetime"
        " from native_probe order by id";
    SQLRETURN rc = SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        std::printf("[native] SQLExecDirect(select) failed rc=%d\n", static_cast<int>(rc));
        printDiag(SQL_HANDLE_STMT, stmt, "stmt");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return false;
    }

    SQLSMALLINT smallintVal = 0;
    SQLLEN smallintInd = 0;
    SQLINTEGER integerVal = 0;
    SQLLEN integerInd = 0;
    SQLBIGINT bigintVal = 0;
    SQLLEN bigintInd = 0;
    SQLDOUBLE decimalVal = 0;
    SQLLEN decimalInd = 0;
    SQL_DATE_STRUCT dateVal{};
    SQLLEN dateInd = 0;
    SQL_TIMESTAMP_STRUCT tsVal{};
    SQLLEN tsInd = 0;

    SQLBindCol(stmt, 1, SQL_C_SSHORT, &smallintVal, 0, &smallintInd);
    SQLBindCol(stmt, 2, SQL_C_SLONG, &integerVal, 0, &integerInd);
    SQLBindCol(stmt, 3, SQL_C_SBIGINT, &bigintVal, 0, &bigintInd);
    SQLBindCol(stmt, 4, SQL_C_DOUBLE, &decimalVal, 0, &decimalInd);
    SQLBindCol(stmt, 5, SQL_C_TYPE_DATE, &dateVal, 0, &dateInd);
    SQLBindCol(stmt, 6, SQL_C_TYPE_TIMESTAMP, &tsVal, 0, &tsInd);

    auto expectEq = [&ok](const char* label, bool cond) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
        if (!cond) ok = false;
    };

    int rowNum = 0;
    while ((rc = SQLFetch(stmt)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        ++rowNum;
        std::printf("native row %d: smallint=%d integer=%d bigint=%lld decimal=%f date=%04d-%02d-%02d"
                    " ts=%04d-%02d-%02d %02d:%02d:%02d\n",
                    rowNum, static_cast<int>(smallintVal), static_cast<int>(integerVal),
                    static_cast<long long>(bigintVal), decimalVal, dateVal.year, dateVal.month, dateVal.day,
                    tsVal.year, tsVal.month, tsVal.day, tsVal.hour, tsVal.minute, tsVal.second);
        if (rowNum == 1) {
            expectEq("row1 smallint == 12345", smallintInd != SQL_NULL_DATA && smallintVal == 12345);
            expectEq("row1 integer == 1234567890", integerInd != SQL_NULL_DATA && integerVal == 1234567890);
            expectEq("row1 bigint == 123456789012345", bigintInd != SQL_NULL_DATA && bigintVal == 123456789012345LL);
            expectEq("row1 decimal ~= 12345.67",
                      decimalInd != SQL_NULL_DATA && decimalVal > 12345.66 && decimalVal < 12345.68);
            expectEq("row1 date == 2026-07-29",
                      dateInd != SQL_NULL_DATA && dateVal.year == 2026 && dateVal.month == 7 && dateVal.day == 29);
            expectEq("row1 timestamp == 2026-07-29 12:34:56",
                      tsInd != SQL_NULL_DATA && tsVal.year == 2026 && tsVal.month == 7 && tsVal.day == 29 &&
                          tsVal.hour == 12 && tsVal.minute == 34 && tsVal.second == 56);
        } else if (rowNum == 2) {
            expectEq("row2 smallint == -12345", smallintInd != SQL_NULL_DATA && smallintVal == -12345);
            expectEq("row2 integer == -2147483000", integerInd != SQL_NULL_DATA && integerVal == -2147483000);
            expectEq("row2 bigint == -123456789012345",
                      bigintInd != SQL_NULL_DATA && bigintVal == -123456789012345LL);
            expectEq("row2 decimal ~= -99.99",
                      decimalInd != SQL_NULL_DATA && decimalVal > -100.0 && decimalVal < -99.98);
            expectEq("row2 date == 2000-01-01",
                      dateInd != SQL_NULL_DATA && dateVal.year == 2000 && dateVal.month == 1 && dateVal.day == 1);
            expectEq("row2 timestamp == 2000-01-01 00:00:00",
                      tsInd != SQL_NULL_DATA && tsVal.year == 2000 && tsVal.month == 1 && tsVal.day == 1 &&
                          tsVal.hour == 0 && tsVal.minute == 0 && tsVal.second == 0);
        } else if (rowNum == 3) {
            expectEq("row3 smallint is NULL", smallintInd == SQL_NULL_DATA);
            expectEq("row3 integer is NULL", integerInd == SQL_NULL_DATA);
            expectEq("row3 bigint is NULL", bigintInd == SQL_NULL_DATA);
            expectEq("row3 decimal is NULL", decimalInd == SQL_NULL_DATA);
            expectEq("row3 date is NULL", dateInd == SQL_NULL_DATA);
            expectEq("row3 timestamp is NULL", tsInd == SQL_NULL_DATA);
        } else {
            expectEq("no unexpected 4th row", false);
        }
    }
    expectEq("SQLFetch loop ended with SQL_NO_DATA", rc == SQL_NO_DATA);
    expectEq("exactly 3 rows fetched", rowNum == 3);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    // SQLRowCount: rows 1 and 2 have a non-NULL col_integer, row 3 doesn't -
    // exactly 2 rows should be reported as updated.
    SQLHSTMT updStmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &updStmt) == SQL_SUCCESS) {
        const char* upd = "update native_probe set col_integer = col_integer + 1 where col_integer is not null";
        rc = SQLExecDirect(updStmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(upd)), SQL_NTS);
        if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
            SQLLEN rowCount = -99;
            SQLRowCount(updStmt, &rowCount);
            expectEq("SQLRowCount after UPDATE == 2", rowCount == 2);
        } else {
            std::printf("[native] UPDATE failed rc=%d\n", static_cast<int>(rc));
            printDiag(SQL_HANDLE_STMT, updStmt, "stmt");
            ok = false;
        }
        SQLFreeHandle(SQL_HANDLE_STMT, updStmt);
    } else {
        ok = false;
    }

    execDirect(dbc, "drop table if exists native_probe");
    return ok;
}

// Verifies Phase 4 slice 4 (see the plan): a malformed query surfaces a
// sane, specific SQLSTATE (not just a blanket HY000) via SQLGetDiagRec, and
// out-of-sequence calls (SQLFetch/SQLExecute with no prior SQLPrepare/
// SQLExecDirect) return a diagnostic instead of crashing - both through the
// real Driver Manager.
bool runDiagnosticsTest(SQLHDBC dbc) {
    bool ok = true;
    auto expectEq = [&ok](const char* label, bool cond) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
        if (!cond) ok = false;
    };

    // Malformed query #1: syntax error.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        const char* sql = "seelct 1 from systables";
        SQLRETURN rc = SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
        expectEq("syntax error returns SQL_ERROR", rc == SQL_ERROR);
        SQLCHAR sqlState[6] = {};
        SQLINTEGER nativeError = 0;
        SQLCHAR message[512] = {};
        SQLSMALLINT textLength = 0;
        SQLRETURN diagRc =
            SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, sqlState, &nativeError, message, sizeof(message), &textLength);
        expectEq("syntax error has a diag record", diagRc == SQL_SUCCESS);
        std::printf("  syntax error: SQLSTATE=%s native=%d\n", sqlState, static_cast<int>(nativeError));
        expectEq("syntax error SQLSTATE == 42000", std::strcmp(reinterpret_cast<char*>(sqlState), "42000") == 0);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // Malformed query #2: table not found.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        const char* sql = "select 1 from no_such_table_xyz";
        SQLRETURN rc = SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
        expectEq("table-not-found returns SQL_ERROR", rc == SQL_ERROR);
        SQLCHAR sqlState[6] = {};
        SQLINTEGER nativeError = 0;
        SQLCHAR message[512] = {};
        SQLSMALLINT textLength = 0;
        SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, sqlState, &nativeError, message, sizeof(message), &textLength);
        std::printf("  table not found: SQLSTATE=%s native=%d\n", sqlState, static_cast<int>(nativeError));
        expectEq("table-not-found SQLSTATE == 42S02", std::strcmp(reinterpret_cast<char*>(sqlState), "42S02") == 0);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // Out-of-sequence: SQLFetch with no prior SQLPrepare/SQLExecute at all.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLFetch(stmt);
        expectEq("SQLFetch with no prior execute returns SQL_ERROR (not a crash)", rc == SQL_ERROR);
        SQLCHAR sqlState[6] = {};
        SQLINTEGER nativeError = 0;
        SQLCHAR message[512] = {};
        SQLSMALLINT textLength = 0;
        SQLRETURN diagRc =
            SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, sqlState, &nativeError, message, sizeof(message), &textLength);
        expectEq("out-of-sequence SQLFetch has a diag record", diagRc == SQL_SUCCESS);
        std::printf("  out-of-sequence SQLFetch: SQLSTATE=%s: %s\n", sqlState, message);
        expectEq("out-of-sequence SQLFetch SQLSTATE == HY010", std::strcmp(reinterpret_cast<char*>(sqlState), "HY010") == 0);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // Out-of-sequence: SQLExecute with no prior SQLPrepare.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLExecute(stmt);
        expectEq("SQLExecute with no prior prepare returns SQL_ERROR (not a crash)", rc == SQL_ERROR);
        SQLCHAR sqlState[6] = {};
        SQLINTEGER nativeError = 0;
        SQLCHAR message[512] = {};
        SQLSMALLINT textLength = 0;
        SQLRETURN diagRc =
            SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, sqlState, &nativeError, message, sizeof(message), &textLength);
        expectEq("out-of-sequence SQLExecute has a diag record", diagRc == SQL_SUCCESS);
        expectEq("out-of-sequence SQLExecute SQLSTATE == HY010", std::strcmp(reinterpret_cast<char*>(sqlState), "HY010") == 0);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    return ok;
}

// Verifies Phase 5's first slice (see the plan): a single bound SQL_INTEGER
// "?" parameter round-trips through SQ_BIND, for both the DML branch
// (INSERT, no cursor) and the SELECT branch (cursor-based) of
// SqliStatement::execute() - proving the wire plumbing on one type before
// widening to the rest of the MVP scalar types. Also exercises NULL (via
// SQL_NULL_DATA) and rebinding the same parameter to a new value between
// executes (proving SQLBindParameter's late-binding semantics - the value
// is read fresh from ParameterValuePtr at each SQLExecute, not captured at
// bind time).
bool runBindParamTest(SQLHDBC dbc) {
    bool ok = true;
    auto expectEq = [&ok](const char* label, bool cond) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
        if (!cond) ok = false;
    };

    ok &= execDirect(dbc, "drop table if exists bind_probe");
    ok &= execDirect(dbc, "create table bind_probe (id serial not null, col_integer integer)");
    if (!ok) {
        std::printf("[bind] fixture setup failed, aborting bind-parameter test\n");
        return false;
    }

    // INSERT with a bound INTEGER parameter (the DML/no-cursor execute()
    // branch), executed three times with three different bound values -
    // including a rebind between executes and a NULL - via one prepared
    // statement handle.
    SQLHSTMT insStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &insStmt);
    const char* insSql = "insert into bind_probe (col_integer) values (?)";
    SQLRETURN rc =
        SQLPrepare(insStmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(insSql)), SQL_NTS);
    expectEq("SQLPrepare(insert ... values (?)) succeeds", rc == SQL_SUCCESS);

    SQLINTEGER boundValue = 0;
    SQLLEN boundInd = 0;
    rc = SQLBindParameter(insStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &boundValue, 0, &boundInd);
    expectEq("SQLBindParameter(1, SQL_C_SLONG, SQL_INTEGER) succeeds", rc == SQL_SUCCESS);

    boundValue = 42;
    boundInd = 0;
    rc = SQLExecute(insStmt);
    expectEq("SQLExecute #1 (value=42) succeeds", rc == SQL_SUCCESS);
    if (rc != SQL_SUCCESS) printDiag(SQL_HANDLE_STMT, insStmt, "insStmt");

    boundValue = -7;
    boundInd = 0;
    rc = SQLExecute(insStmt);
    expectEq("SQLExecute #2 (rebind to -7) succeeds", rc == SQL_SUCCESS);

    boundInd = SQL_NULL_DATA;
    rc = SQLExecute(insStmt);
    expectEq("SQLExecute #3 (NULL) succeeds", rc == SQL_SUCCESS);

    SQLFreeHandle(SQL_HANDLE_STMT, insStmt);

    // SELECT with a bound INTEGER parameter in a WHERE clause (the cursor-
    // based execute() branch), verifying the three rows above round-tripped
    // correctly.
    SQLHSTMT selStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &selStmt);
    const char* selSql = "select col_integer from bind_probe where col_integer = ?";
    rc = SQLPrepare(selStmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(selSql)), SQL_NTS);
    expectEq("SQLPrepare(select ... where col_integer = ?) succeeds", rc == SQL_SUCCESS);

    SQLINTEGER whereValue = 42;
    SQLLEN whereInd = 0;
    SQLBindParameter(selStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &whereValue, 0, &whereInd);

    SQLINTEGER fetchedValue = 0;
    SQLLEN fetchedInd = 0;
    SQLBindCol(selStmt, 1, SQL_C_SLONG, &fetchedValue, 0, &fetchedInd);

    rc = SQLExecute(selStmt);
    expectEq("SQLExecute(select where col_integer = 42) succeeds", rc == SQL_SUCCESS);
    rc = SQLFetch(selStmt);
    expectEq("row found for col_integer = 42", rc == SQL_SUCCESS);
    expectEq("fetched value == 42", fetchedInd != SQL_NULL_DATA && fetchedValue == 42);
    rc = SQLFetch(selStmt);
    expectEq("exactly one row for col_integer = 42", rc == SQL_NO_DATA);

    // Rebind the same parameter to a different value and re-execute the
    // same prepared statement - proves SQLBindParameter's late-binding
    // semantics (a fresh SQ_BIND is sent from the pointer's *current*
    // value, not whatever it held at bind time).
    whereValue = -7;
    rc = SQLExecute(selStmt);
    expectEq("SQLExecute(select where col_integer = -7) succeeds after rebind", rc == SQL_SUCCESS);
    rc = SQLFetch(selStmt);
    expectEq("row found for col_integer = -7", rc == SQL_SUCCESS);
    expectEq("fetched value == -7", fetchedInd != SQL_NULL_DATA && fetchedValue == -7);

    SQLFreeHandle(SQL_HANDLE_STMT, selStmt);

    // NULL bound ahead of another parameter, through the real ODBC surface
    // via SQLBindParameter/SQL_NULL_DATA rather than a hand-built
    // sqli::BindParam - the two driver-layer pieces added alongside the
    // sqli::writeBindBlock framing fix (buildBindParams' isNull branch,
    // ifxTypeForParameterType) had no coverage through SQLBindParameter
    // itself yet; this closes that gap. Also re-proves the framing fix
    // live at this layer, not just sqli's own integration test.
    ok &= execDirect(dbc, "drop table if exists null_order_probe");
    ok &= execDirect(dbc, "create table null_order_probe (col_smallint smallint, col_integer integer)");
    SQLHSTMT nullOrderStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &nullOrderStmt);
    const char* nullOrderSql = "insert into null_order_probe (col_smallint, col_integer) values (?, ?)";
    rc = SQLPrepare(nullOrderStmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(nullOrderSql)), SQL_NTS);
    expectEq("SQLPrepare(null_order_probe insert) succeeds", rc == SQL_SUCCESS);

    SQLSMALLINT nullParamPlaceholder = 0;
    SQLLEN nullInd = SQL_NULL_DATA;
    rc = SQLBindParameter(nullOrderStmt, 1, SQL_PARAM_INPUT, SQL_C_SSHORT, SQL_SMALLINT, 0, 0, &nullParamPlaceholder,
                           0, &nullInd);
    expectEq("SQLBindParameter(1, NULL smallint) succeeds", rc == SQL_SUCCESS);
    SQLINTEGER nullOrderIntVal = 99;
    SQLLEN nullOrderIntInd = 0;
    rc = SQLBindParameter(nullOrderStmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &nullOrderIntVal, 0,
                           &nullOrderIntInd);
    expectEq("SQLBindParameter(2, integer 99) succeeds", rc == SQL_SUCCESS);

    rc = SQLExecute(nullOrderStmt);
    expectEq("SQLExecute(NULL-then-value insert) succeeds, not a hang", rc == SQL_SUCCESS);
    if (rc != SQL_SUCCESS) printDiag(SQL_HANDLE_STMT, nullOrderStmt, "nullOrderStmt");
    SQLFreeHandle(SQL_HANDLE_STMT, nullOrderStmt);

    SQLHSTMT nullOrderSelStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &nullOrderSelStmt);
    const char* nullOrderSelSql = "select col_smallint, col_integer from null_order_probe";
    rc = SQLExecDirect(nullOrderSelStmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(nullOrderSelSql)), SQL_NTS);
    expectEq("select-back after NULL-then-value insert succeeds", rc == SQL_SUCCESS);
    SQLSMALLINT roSmallint = 0;
    SQLLEN roSmallintInd = 0;
    SQLINTEGER roInteger = 0;
    SQLLEN roIntegerInd = 0;
    SQLBindCol(nullOrderSelStmt, 1, SQL_C_SSHORT, &roSmallint, 0, &roSmallintInd);
    SQLBindCol(nullOrderSelStmt, 2, SQL_C_SLONG, &roInteger, 0, &roIntegerInd);
    rc = SQLFetch(nullOrderSelStmt);
    expectEq("row found after NULL-then-value insert", rc == SQL_SUCCESS);
    expectEq("col_smallint round-tripped as NULL", roSmallintInd == SQL_NULL_DATA);
    expectEq("col_integer round-tripped as 99", roIntegerInd != SQL_NULL_DATA && roInteger == 99);
    SQLFreeHandle(SQL_HANDLE_STMT, nullOrderSelStmt);

    execDirect(dbc, "drop table if exists null_order_probe");
    execDirect(dbc, "drop table if exists bind_probe");
    return ok;
}

// Verifies the smart LOB (BLOB/CLOB) SQLBindParameter surface through the
// real Driver Manager: SQLNumParams/SQLDescribeParam report the right
// shape for a "?" targeting a smart-LOB column, and SQLBindParameter with
// SQL_LONGVARBINARY/SQL_LONGVARCHAR - the same ParameterType a simple LOB
// (BYTE/TEXT) bind also uses - is correctly routed to
// SqliConnection::createSmartLob rather than the simple-LOB bind path
// (driver/statement.cpp's buildBindParams, dispatching on the target
// column's real paramDescs() type since ODBC itself can't tell the two
// apart at bind time). Also covers a NULL smart-LOB bind and a
// same-prepared-statement rebind, mirroring runBindParamTest's own
// coverage shape for the INTEGER case.
bool runSmartLobBindTest(SQLHDBC dbc) {
    bool ok = true;
    auto expectEq = [&ok](const char* label, bool cond) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
        if (!cond) ok = false;
    };

    ok &= execDirect(dbc, "drop table if exists smart_lob_bind_probe");
    ok &= execDirect(dbc, "create table smart_lob_bind_probe (id integer, b blob, c clob)"
                          " put b in (sbspace1), c in (sbspace1)");
    if (!ok) {
        std::printf("[smart-lob-bind] fixture setup failed, aborting smart LOB bind test\n");
        return false;
    }

    SQLHSTMT insStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &insStmt);
    const char* insSql = "insert into smart_lob_bind_probe (id, b) values (?, ?)";
    SQLRETURN rc = SQLPrepare(insStmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(insSql)), SQL_NTS);
    expectEq("SQLPrepare(insert ... b values (?, ?)) succeeds", rc == SQL_SUCCESS);
    if (rc != SQL_SUCCESS) printDiag(SQL_HANDLE_STMT, insStmt, "insStmt");

    SQLSMALLINT numParams = 0;
    rc = SQLNumParams(insStmt, &numParams);
    expectEq("SQLNumParams succeeds", rc == SQL_SUCCESS);
    expectEq("SQLNumParams reports 2", numParams == 2);

    SQLSMALLINT paramDataType = 0;
    SQLULEN paramSize = 0;
    SQLSMALLINT paramDecimalDigits = 0;
    SQLSMALLINT paramNullable = 0;
    rc = SQLDescribeParam(insStmt, 2, &paramDataType, &paramSize, &paramDecimalDigits, &paramNullable);
    expectEq("SQLDescribeParam(2) succeeds", rc == SQL_SUCCESS);
    if (rc != SQL_SUCCESS) printDiag(SQL_HANDLE_STMT, insStmt, "insStmt");
    expectEq("SQLDescribeParam(2) reports SQL_LONGVARBINARY for the blob column",
             paramDataType == SQL_LONGVARBINARY);

    SQLINTEGER idVal = 1;
    SQLLEN idInd = 0;
    rc = SQLBindParameter(insStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &idVal, 0, &idInd);
    expectEq("SQLBindParameter(1, id) succeeds", rc == SQL_SUCCESS);

    std::string content1 = "smart lob bind through the real driver manager";
    SQLLEN contentInd = static_cast<SQLLEN>(content1.size());
    rc = SQLBindParameter(insStmt, 2, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_LONGVARBINARY, 0, 0,
                           const_cast<char*>(content1.data()), 0, &contentInd);
    expectEq("SQLBindParameter(2, blob content, SQL_LONGVARBINARY) succeeds", rc == SQL_SUCCESS);

    rc = SQLExecute(insStmt);
    expectEq("SQLExecute(insert with smart-LOB bind, id=1) succeeds", rc == SQL_SUCCESS);
    if (rc != SQL_SUCCESS) printDiag(SQL_HANDLE_STMT, insStmt, "insStmt");

    // Rebind to a NULL value and a different id, same prepared statement -
    // proves the NULL smart-LOB bind path (buildBindParams' isSmartLobTarget
    // NULL branch) doesn't hang the connection the way an extended-type
    // bind missing its owner/name fields is known to elsewhere in this
    // driver (see BindParam's doc comment in sqli/statement.h).
    idVal = 2;
    contentInd = SQL_NULL_DATA;
    rc = SQLExecute(insStmt);
    expectEq("SQLExecute(insert with NULL smart-LOB bind, id=2) succeeds", rc == SQL_SUCCESS);
    if (rc != SQL_SUCCESS) printDiag(SQL_HANDLE_STMT, insStmt, "insStmt");

    // Rebind to a third value and a payload well over writeLoData's 30000-
    // byte LO_WRITE chunk size (sqli/connection.cpp's kMaxChunk) - every
    // other smart-LOB write this driver has ever run (sqli's own
    // integration test, ifx_probe, this test's own id=1/id=2 above) fit in
    // one chunk, so the multi-chunk request-side framing (several
    // length-prefixed chunks under one declared total, one trailing EOT)
    // had never actually executed before this. A non-repeating byte
    // pattern (not e.g. all 'x') so a chunk-boundary reordering/truncation
    // bug would show up as a content mismatch rather than being masked by
    // uniform bytes.
    idVal = 3;
    std::string content3(70000, '\0');
    for (std::size_t k = 0; k < content3.size(); ++k) {
        content3[k] = static_cast<char>(static_cast<unsigned char>(k % 251));
    }
    contentInd = static_cast<SQLLEN>(content3.size());
    rc = SQLBindParameter(insStmt, 2, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_LONGVARBINARY, 0, 0,
                           const_cast<char*>(content3.data()), 0, &contentInd);
    rc = SQLExecute(insStmt);
    expectEq("SQLExecute(insert with 70000-byte (multi-chunk LO_WRITE) smart-LOB bind, id=3) succeeds",
             rc == SQL_SUCCESS);
    if (rc != SQL_SUCCESS) printDiag(SQL_HANDLE_STMT, insStmt, "insStmt");

    SQLFreeHandle(SQL_HANDLE_STMT, insStmt);

    // Read every row back through an ordinary SQLGetData fetch (the
    // already-confirmed-live smart-LOB fetch surface, driver/statement.cpp's
    // writeSmartLobColumnValue) to confirm what buildBindParams actually
    // wrote is byte-exact, not just "SQLExecute didn't fail".
    SQLHSTMT selStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &selStmt);
    const char* selSql = "select id, b from smart_lob_bind_probe order by id";
    rc = SQLExecDirect(selStmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(selSql)), SQL_NTS);
    expectEq("select-back after smart-LOB bind inserts succeeds", rc == SQL_SUCCESS);

    static char buf[80000]; // big enough for content3's 70000 bytes in one SQLGetData call
    for (int expectedId = 1; expectedId <= 3; ++expectedId) {
        rc = SQLFetch(selStmt);
        expectEq(("row found for id=" + std::to_string(expectedId)).c_str(), rc == SQL_SUCCESS);
        if (rc != SQL_SUCCESS) break;

        SQLINTEGER fetchedId = 0;
        SQLLEN fetchedIdInd = 0;
        SQLGetData(selStmt, 1, SQL_C_SLONG, &fetchedId, 0, &fetchedIdInd);
        expectEq(("fetched id == " + std::to_string(expectedId)).c_str(),
                 fetchedIdInd != SQL_NULL_DATA && fetchedId == expectedId);

        SQLLEN bInd = 0;
        SQLRETURN getRc = SQLGetData(selStmt, 2, SQL_C_BINARY, buf, sizeof(buf), &bInd);
        if (expectedId == 2) {
            expectEq("id=2 blob round-trips as NULL", bInd == SQL_NULL_DATA);
        } else {
            const std::string& expected = expectedId == 1 ? content1 : content3;
            bool matches = getRc == SQL_SUCCESS && bInd == static_cast<SQLLEN>(expected.size()) &&
                            std::memcmp(buf, expected.data(), expected.size()) == 0;
            expectEq(("id=" + std::to_string(expectedId) + " blob content round-trips byte-exact").c_str(), matches);
        }
    }
    rc = SQLFetch(selStmt);
    expectEq("no unexpected 4th row", rc == SQL_NO_DATA);
    SQLFreeHandle(SQL_HANDLE_STMT, selStmt);

    // CLOB column: SQLDescribeParam should report SQL_LONGVARCHAR (not
    // SQL_LONGVARBINARY) - proves the dispatch consults the *described*
    // column's own extendedTypeName ("blob" vs "clob"), not just "is this
    // a smart LOB at all".
    SQLHSTMT clobStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &clobStmt);
    const char* clobInsSql = "insert into smart_lob_bind_probe (id, c) values (?, ?)";
    rc = SQLPrepare(clobStmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(clobInsSql)), SQL_NTS);
    expectEq("SQLPrepare(insert ... c values (?, ?)) succeeds", rc == SQL_SUCCESS);

    SQLSMALLINT clobParamDataType = 0;
    rc = SQLDescribeParam(clobStmt, 2, &clobParamDataType, &paramSize, &paramDecimalDigits, &paramNullable);
    expectEq("SQLDescribeParam(2) succeeds for clob column", rc == SQL_SUCCESS);
    expectEq("SQLDescribeParam(2) reports SQL_LONGVARCHAR for the clob column",
             clobParamDataType == SQL_LONGVARCHAR);

    SQLINTEGER clobId = 10;
    SQLLEN clobIdInd = 0;
    SQLBindParameter(clobStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &clobId, 0, &clobIdInd);
    std::string clobContent = "smart lob bind, clob column, through the real driver manager";
    SQLLEN clobContentInd = static_cast<SQLLEN>(clobContent.size());
    rc = SQLBindParameter(clobStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_LONGVARCHAR, 0, 0,
                           const_cast<char*>(clobContent.data()), 0, &clobContentInd);
    rc = SQLExecute(clobStmt);
    expectEq("SQLExecute(insert with clob smart-LOB bind) succeeds", rc == SQL_SUCCESS);
    if (rc != SQL_SUCCESS) printDiag(SQL_HANDLE_STMT, clobStmt, "clobStmt");
    SQLFreeHandle(SQL_HANDLE_STMT, clobStmt);

    SQLHSTMT clobSelStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &clobSelStmt);
    const char* clobSelSql = "select c from smart_lob_bind_probe where id = 10";
    rc = SQLExecDirect(clobSelStmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(clobSelSql)), SQL_NTS);
    expectEq("select-back for clob row succeeds", rc == SQL_SUCCESS);
    rc = SQLFetch(clobSelStmt);
    expectEq("clob row found", rc == SQL_SUCCESS);
    SQLLEN clobFetchInd = 0;
    SQLRETURN clobGetRc = SQLGetData(clobSelStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &clobFetchInd);
    bool clobMatches = clobGetRc == SQL_SUCCESS && clobFetchInd == static_cast<SQLLEN>(clobContent.size()) &&
                        std::memcmp(buf, clobContent.data(), clobContent.size()) == 0;
    expectEq("clob content round-trips byte-exact", clobMatches);
    SQLFreeHandle(SQL_HANDLE_STMT, clobSelStmt);

    execDirect(dbc, "drop table if exists smart_lob_bind_probe");
    return ok;
}

// Verifies the driver-layer transaction wiring added on top of the
// already-confirmed-live SqliConnection::begin/commit/rollbackTransaction
// primitives: SQL_ATTR_AUTOCOMMIT actually changing behavior (not just
// round-tripping, which is all it did before), the lazy SQ_BEGIN
// driver/statement.cpp's doExecute now issues before the first statement
// of a manual-commit transaction, and SQLEndTran itself. Also specifically
// checks what a commit does to an already-open SELECT cursor - Informix
// SQLI cursors can be closed server-side by a COMMIT unless declared WITH
// HOLD, which this driver never does, so a stale client-side cursorOpen_
// flag after a commit would desync the next fetch/close exactly the way
// this project's other framing bugs have (see docs/protocol-notes.md's
// Transactions section) - worth knowing rather than assuming either way.
bool runTransactionTest(SQLHENV env, SQLHDBC dbc, const std::string& connStr) {
    bool ok = true;
    auto expectEq = [&ok](const char* label, bool cond) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
        if (!cond) ok = false;
    };

    ok &= execDirect(dbc, "drop table if exists txn_probe");
    ok &= execDirect(dbc, "create table txn_probe (id serial not null, tag varchar(32))");
    if (!ok) {
        std::printf("[txn] fixture setup failed, aborting transaction test\n");
        return false;
    }

    auto countTag = [&](SQLHDBC handle, const char* tag) -> long long {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, handle, &stmt);
        std::string sql = std::string("select count(*) from txn_probe where tag = '") + tag + "'";
        SQLRETURN rc = SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS);
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
            printDiag(SQL_HANDLE_STMT, stmt, "count");
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return -1;
        }
        // SQL_C_CHAR, not SQL_C_SBIGINT: count(*) comes back as DECIMAL on
        // this server (same finding as tests/integration/test_transactions.cpp's
        // countRows), and DECIMAL -> SQL_C_SBIGINT isn't a conversion this
        // driver implements (only DECIMAL -> SQL_C_DOUBLE/SQL_C_CHAR are -
        // see driver/convert_native.cpp's isIntegerFamily gate). SQL_C_CHAR
        // is the one target type every column type supports.
        char countBuf[32] = {};
        SQLLEN ind = 0;
        SQLBindCol(stmt, 1, SQL_C_CHAR, countBuf, sizeof(countBuf), &ind);
        SQLRETURN fetchRc = SQLFetch(stmt);
        if (fetchRc != SQL_SUCCESS && fetchRc != SQL_SUCCESS_WITH_INFO) {
            printDiag(SQL_HANDLE_STMT, stmt, "count-fetch");
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return -1;
        }
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return ind == SQL_NULL_DATA ? -1 : std::strtoll(countBuf, nullptr, 10);
    };

    SQLRETURN rc = SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);
    expectEq("SQLSetConnectAttr(AUTOCOMMIT_OFF) succeeds", rc == SQL_SUCCESS);

    // Rollback: insert, roll back, confirm absent on this same connection.
    ok &= execDirect(dbc, "insert into txn_probe (tag) values ('rollback-me')");
    rc = SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
    expectEq("SQLEndTran(ROLLBACK) succeeds", rc == SQL_SUCCESS);
    expectEq("rolled-back row is absent on the same connection", countTag(dbc, "rollback-me") == 0);

    // Commit: insert two statements in the same transaction, commit,
    // confirm visible from a *separate* connection (proves it actually
    // reached the server, not just this connection's own read-your-writes).
    ok &= execDirect(dbc, "insert into txn_probe (tag) values ('commit-me')");
    ok &= execDirect(dbc, "insert into txn_probe (tag) values ('commit-me')");
    rc = SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    expectEq("SQLEndTran(COMMIT) succeeds", rc == SQL_SUCCESS);

    SQLHDBC dbc2 = SQL_NULL_HDBC;
    SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc2);
    SQLCHAR outConnStr[1024];
    SQLSMALLINT outConnStrLen = 0;
    rc = SQLDriverConnect(dbc2, nullptr, reinterpret_cast<SQLCHAR*>(const_cast<char*>(connStr.c_str())),
                           static_cast<SQLSMALLINT>(connStr.size()), outConnStr, sizeof(outConnStr), &outConnStrLen,
                           SQL_DRIVER_NOPROMPT);
    if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        expectEq("committed rows are visible from a separate connection", countTag(dbc2, "commit-me") == 2);
        SQLDisconnect(dbc2);
    } else {
        printDiag(SQL_HANDLE_DBC, dbc2, "dbc2");
        expectEq("second connection for commit-visibility check succeeded", false);
    }
    SQLFreeHandle(SQL_HANDLE_DBC, dbc2);

    // A commit's effect on an already-open SELECT cursor: prepare+execute
    // a query that opens a server-side cursor, fetch one row, commit, then
    // try to fetch again. Whatever happens here (clean SQL_NO_DATA, a
    // clean error, or a hang under this process's own timeout wrapper)
    // is worth knowing rather than assumed - this driver never declares
    // cursors WITH HOLD, so a commit closing them server-side is plausible.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        const char* sql = "select tag from txn_probe where tag = 'commit-me'";
        rc = SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
        expectEq("cursor-probe SQLExecDirect succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        rc = SQLFetch(stmt);
        expectEq("cursor-probe first SQLFetch succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);

        rc = SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        std::printf("  [info] SQLEndTran(COMMIT) with an open cursor: rc=%d\n", static_cast<int>(rc));
        printDiag(SQL_HANDLE_DBC, dbc, "dbc (post-commit-with-open-cursor)");

        rc = SQLFetch(stmt);
        std::printf("  [info] SQLFetch after commit-with-open-cursor: rc=%d\n", static_cast<int>(rc));
        printDiag(SQL_HANDLE_STMT, stmt, "stmt (post-commit-with-open-cursor fetch)");

        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // Autocommit OFF -> ON transition: commits whatever's currently open.
    ok &= execDirect(dbc, "insert into txn_probe (tag) values ('autocommit-flip')");
    rc = SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);
    expectEq("SQLSetConnectAttr(AUTOCOMMIT_ON) with an open transaction succeeds", rc == SQL_SUCCESS);

    SQLHDBC dbc3 = SQL_NULL_HDBC;
    SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc3);
    rc = SQLDriverConnect(dbc3, nullptr, reinterpret_cast<SQLCHAR*>(const_cast<char*>(connStr.c_str())),
                           static_cast<SQLSMALLINT>(connStr.size()), outConnStr, sizeof(outConnStr), &outConnStrLen,
                           SQL_DRIVER_NOPROMPT);
    if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        expectEq("autocommit ON->OFF->ON flip's row is visible from a separate connection",
                  countTag(dbc3, "autocommit-flip") == 1);
        SQLDisconnect(dbc3);
    } else {
        expectEq("third connection for autocommit-flip check succeeded", false);
    }
    SQLFreeHandle(SQL_HANDLE_DBC, dbc3);

    // Back under autocommit ON: statements need no SQLEndTran at all.
    ok &= execDirect(dbc, "insert into txn_probe (tag) values ('plain-autocommit')");
    expectEq("plain autocommit insert is visible on the same connection with no SQLEndTran",
              countTag(dbc, "plain-autocommit") == 1);

    // SQLDisconnect with an unresolved transaction: on a *dedicated*
    // connection (dbc's own eventual SQLDisconnect in main() always runs
    // under autocommit ON by this point, so it would never exercise this
    // branch) - autocommit off, insert, disconnect with no SQLEndTran at
    // all, then check from a fresh connection that the row never
    // committed. This is the one driver/connect.cpp SQLDisconnect branch
    // added alongside SQLEndTran that nothing above exercises, and it's
    // exactly the guarantee a BOP restore that dies mid-transaction and
    // disconnects would depend on: better an aborted restore leaves
    // nothing applied than half of it silently committed.
    {
        SQLHDBC dbc4 = SQL_NULL_HDBC;
        SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc4);
        rc = SQLDriverConnect(dbc4, nullptr, reinterpret_cast<SQLCHAR*>(const_cast<char*>(connStr.c_str())),
                               static_cast<SQLSMALLINT>(connStr.size()), outConnStr, sizeof(outConnStr),
                               &outConnStrLen, SQL_DRIVER_NOPROMPT);
        if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
            rc = SQLSetConnectAttr(dbc4, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);
            expectEq("dbc4 SQLSetConnectAttr(AUTOCOMMIT_OFF) succeeds", rc == SQL_SUCCESS);
            ok &= execDirect(dbc4, "insert into txn_probe (tag) values ('disconnect-me')");
            rc = SQLDisconnect(dbc4);
            expectEq("SQLDisconnect with an open, unresolved transaction succeeds", rc == SQL_SUCCESS);
        } else {
            printDiag(SQL_HANDLE_DBC, dbc4, "dbc4");
            expectEq("fourth connection for disconnect-rollback check succeeded", false);
        }
        SQLFreeHandle(SQL_HANDLE_DBC, dbc4);

        SQLHDBC dbc5 = SQL_NULL_HDBC;
        SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc5);
        rc = SQLDriverConnect(dbc5, nullptr, reinterpret_cast<SQLCHAR*>(const_cast<char*>(connStr.c_str())),
                               static_cast<SQLSMALLINT>(connStr.size()), outConnStr, sizeof(outConnStr),
                               &outConnStrLen, SQL_DRIVER_NOPROMPT);
        if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
            expectEq("row from a transaction never SQLEndTran'd before SQLDisconnect was rolled back",
                      countTag(dbc5, "disconnect-me") == 0);
            SQLDisconnect(dbc5);
        } else {
            expectEq("fifth connection for disconnect-rollback check succeeded", false);
        }
        SQLFreeHandle(SQL_HANDLE_DBC, dbc5);
    }

    execDirect(dbc, "drop table if exists txn_probe");
    return ok;
}

// Verifies SQLTables (driver/catalog.cpp): a synthesized query against
// systables run through the same prepare/execute/fetch pipeline every
// other statement uses, not a separate in-memory result-set mechanism -
// see that file's top comment for the exact scope (plain filtering, not
// the ODBC catalog/schema "list everything" enumeration modes).
bool runCatalogTest(SQLHDBC dbc, const std::string& database) {
    bool ok = true;
    auto expectEq = [&ok](const char* label, bool cond) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
        if (!cond) ok = false;
    };

    ok &= execDirect(dbc, "drop view if exists catalog_probe_v");
    ok &= execDirect(dbc, "drop table if exists catalog_probe");
    ok &= execDirect(dbc, "create table catalog_probe (id integer)");
    ok &= execDirect(dbc, "create view catalog_probe_v as select id from catalog_probe");
    if (!ok) {
        std::printf("[catalog] fixture setup failed, aborting catalog test\n");
        return false;
    }

    // One row's shape, in full: TABLE_CAT/TABLE_SCHEM/TABLE_TYPE content,
    // not just that catalog_probe shows up at all.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLTables(stmt, nullptr, 0, nullptr, 0,
                                  reinterpret_cast<SQLCHAR*>(const_cast<char*>("catalog_probe")), SQL_NTS, nullptr, 0);
        expectEq("SQLTables(TableName=catalog_probe) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) printDiag(SQL_HANDLE_STMT, stmt, "SQLTables");

        char cat[128] = {}, schem[128] = {}, name[128] = {}, type[128] = {};
        SQLLEN catInd = 0, schemInd = 0, nameInd = 0, typeInd = 0;
        SQLBindCol(stmt, 1, SQL_C_CHAR, cat, sizeof(cat), &catInd);
        SQLBindCol(stmt, 2, SQL_C_CHAR, schem, sizeof(schem), &schemInd);
        SQLBindCol(stmt, 3, SQL_C_CHAR, name, sizeof(name), &nameInd);
        SQLBindCol(stmt, 4, SQL_C_CHAR, type, sizeof(type), &typeInd);

        rc = SQLFetch(stmt);
        expectEq("row found for TableName=catalog_probe", rc == SQL_SUCCESS);
        expectEq("TABLE_CAT == connected database", catInd != SQL_NULL_DATA && database == cat);
        expectEq("TABLE_SCHEM == informix (owner)", schemInd != SQL_NULL_DATA && std::string("informix") == schem);
        expectEq("TABLE_NAME == catalog_probe", nameInd != SQL_NULL_DATA && std::string("catalog_probe") == name);
        expectEq("TABLE_TYPE == TABLE", typeInd != SQL_NULL_DATA && std::string("TABLE") == type);
        rc = SQLFetch(stmt);
        expectEq("exactly one row for TableName=catalog_probe", rc == SQL_NO_DATA);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // Wildcard pattern (LIKE, not exact match).
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLTables(stmt, nullptr, 0, nullptr, 0,
                                  reinterpret_cast<SQLCHAR*>(const_cast<char*>("catalog\\_probe%")), SQL_NTS, nullptr, 0);
        expectEq("SQLTables(TableName=catalog\\_probe%) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        char name[128] = {};
        SQLLEN nameInd = 0;
        SQLBindCol(stmt, 3, SQL_C_CHAR, name, sizeof(name), &nameInd);
        int rowCount = 0;
        while ((rc = SQLFetch(stmt)) == SQL_SUCCESS) ++rowCount;
        expectEq("wildcard pattern matches both catalog_probe and catalog_probe_v", rowCount == 2);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // TableType filter: VIEW only should return catalog_probe_v, not
    // catalog_probe.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLTables(stmt, nullptr, 0, nullptr, 0,
                                  reinterpret_cast<SQLCHAR*>(const_cast<char*>("catalog%")), SQL_NTS,
                                  reinterpret_cast<SQLCHAR*>(const_cast<char*>("VIEW")), SQL_NTS);
        expectEq("SQLTables(TableType=VIEW) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        char name[128] = {};
        SQLLEN nameInd = 0;
        SQLBindCol(stmt, 3, SQL_C_CHAR, name, sizeof(name), &nameInd);
        rc = SQLFetch(stmt);
        expectEq("TableType=VIEW filter finds exactly catalog_probe_v",
                  rc == SQL_SUCCESS && nameInd != SQL_NULL_DATA && std::string("catalog_probe_v") == name);
        rc = SQLFetch(stmt);
        expectEq("TableType=VIEW filter finds only that one row", rc == SQL_NO_DATA);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // A system table (owner=informix, tabid<=99) is labeled SYSTEM TABLE,
    // not TABLE.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLTables(stmt, nullptr, 0, nullptr, 0,
                                  reinterpret_cast<SQLCHAR*>(const_cast<char*>("systables")), SQL_NTS, nullptr, 0);
        char type[128] = {};
        SQLLEN typeInd = 0;
        SQLBindCol(stmt, 4, SQL_C_CHAR, type, sizeof(type), &typeInd);
        rc = SQLFetch(stmt);
        expectEq("systables is reported as SYSTEM TABLE",
                  rc == SQL_SUCCESS && typeInd != SQL_NULL_DATA && std::string("SYSTEM TABLE") == type);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // A CatalogName that doesn't match the connected database returns zero
    // rows rather than erroring or ignoring the filter.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc =
            SQLTables(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("no_such_database")), SQL_NTS, nullptr, 0,
                      reinterpret_cast<SQLCHAR*>(const_cast<char*>("catalog_probe")), SQL_NTS, nullptr, 0);
        expectEq("SQLTables(CatalogName=no_such_database) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        rc = SQLFetch(stmt);
        expectEq("mismatched CatalogName returns zero rows", rc == SQL_NO_DATA);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // Catalog reads work the same under manual-commit mode as under
    // autocommit - they deliberately don't open a transaction of their own
    // (see driver/catalog.cpp's runCatalogQuery), a state this pipeline
    // didn't have to account for before SQLEndTran existed.
    {
        SQLRETURN rc = SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);
        expectEq("SQLSetConnectAttr(AUTOCOMMIT_OFF) for catalog-under-transaction check succeeds", rc == SQL_SUCCESS);
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        rc = SQLTables(stmt, nullptr, 0, nullptr, 0,
                        reinterpret_cast<SQLCHAR*>(const_cast<char*>("catalog_probe")), SQL_NTS, nullptr, 0);
        expectEq("SQLTables succeeds under SQL_ATTR_AUTOCOMMIT_OFF", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        rc = SQLFetch(stmt);
        expectEq("SQLTables still finds catalog_probe under autocommit off", rc == SQL_SUCCESS);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        rc = SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        expectEq("SQLEndTran after the autocommit-off catalog check succeeds", rc == SQL_SUCCESS);
        SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);
    }

    execDirect(dbc, "drop view if exists catalog_probe_v");
    execDirect(dbc, "drop table if exists catalog_probe");
    return ok;
}

// Verifies SQLColumns (driver/catalog.cpp): DATA_TYPE/TYPE_NAME/
// COLUMN_SIZE/DECIMAL_DIGITS/NULLABLE/IS_NULLABLE per column, derived from
// syscolumns.coltype/collength - see that file's buildColumnsSql doc
// comment for the packed-field decoding this relies on (confirmed live
// separately via isql before this test existed).
bool runColumnsTest(SQLHDBC dbc, const std::string& database) {
    bool ok = true;
    auto expectEq = [&ok](const char* label, bool cond) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
        if (!cond) ok = false;
    };

    ok &= execDirect(dbc, "drop table if exists columns_probe");
    ok &= execDirect(dbc,
                      "create table columns_probe (c_smallint smallint, c_integer integer not null,"
                      " c_char char(20), c_decimal decimal(10,2), c_date date,"
                      " c_datetime datetime year to second)");
    if (!ok) {
        std::printf("[columns] fixture setup failed, aborting columns test\n");
        return false;
    }

    struct ColRow {
        char name[128] = {};
        SQLLEN nameInd = 0;
        SQLSMALLINT dataType = 0;
        SQLLEN dataTypeInd = 0;
        char typeName[64] = {};
        SQLLEN typeNameInd = 0;
        SQLINTEGER columnSize = 0;
        SQLLEN columnSizeInd = 0;
        SQLSMALLINT decimalDigits = 0;
        SQLLEN decimalDigitsInd = 0;
        SQLSMALLINT nullable = 0;
        SQLLEN nullableInd = 0;
        char isNullable[8] = {};
        SQLLEN isNullableInd = 0;
        SQLINTEGER ordinalPosition = 0;
        SQLLEN ordinalPositionInd = 0;
        SQLSMALLINT sqlDataType = 0;
        SQLLEN sqlDataTypeInd = 0;
        SQLSMALLINT sqlDatetimeSub = 0;
        SQLLEN sqlDatetimeSubInd = 0;
    };

    // All four columns, in declared order, checking every field this
    // driver derives from syscolumns.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLColumns(stmt, nullptr, 0, nullptr, 0,
                                   reinterpret_cast<SQLCHAR*>(const_cast<char*>("columns_probe")), SQL_NTS, nullptr, 0);
        expectEq("SQLColumns(TableName=columns_probe) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) printDiag(SQL_HANDLE_STMT, stmt, "SQLColumns");

        ColRow row;
        SQLBindCol(stmt, 4, SQL_C_CHAR, row.name, sizeof(row.name), &row.nameInd);
        SQLBindCol(stmt, 5, SQL_C_SSHORT, &row.dataType, 0, &row.dataTypeInd);
        SQLBindCol(stmt, 6, SQL_C_CHAR, row.typeName, sizeof(row.typeName), &row.typeNameInd);
        SQLBindCol(stmt, 7, SQL_C_SLONG, &row.columnSize, 0, &row.columnSizeInd);
        SQLBindCol(stmt, 9, SQL_C_SSHORT, &row.decimalDigits, 0, &row.decimalDigitsInd);
        SQLBindCol(stmt, 11, SQL_C_SSHORT, &row.nullable, 0, &row.nullableInd);
        SQLBindCol(stmt, 17, SQL_C_SLONG, &row.ordinalPosition, 0, &row.ordinalPositionInd);
        SQLBindCol(stmt, 18, SQL_C_CHAR, row.isNullable, sizeof(row.isNullable), &row.isNullableInd);
        SQLBindCol(stmt, 14, SQL_C_SSHORT, &row.sqlDataType, 0, &row.sqlDataTypeInd);
        SQLBindCol(stmt, 15, SQL_C_SSHORT, &row.sqlDatetimeSub, 0, &row.sqlDatetimeSubInd);

        rc = SQLFetch(stmt);
        if (rc != SQL_SUCCESS) printDiag(SQL_HANDLE_STMT, stmt, "columns-row1");
        expectEq("row 1 (c_smallint) found", rc == SQL_SUCCESS);
        expectEq("c_smallint: COLUMN_NAME", std::string("c_smallint") == row.name);
        expectEq("c_smallint: DATA_TYPE == SQL_SMALLINT", row.dataType == SQL_SMALLINT);
        expectEq("c_smallint: TYPE_NAME == smallint", std::string("smallint") == row.typeName);
        expectEq("c_smallint: COLUMN_SIZE == 5", row.columnSize == 5);
        expectEq("c_smallint: DECIMAL_DIGITS is NULL", row.decimalDigitsInd == SQL_NULL_DATA);
        expectEq("c_smallint: NULLABLE == SQL_NULLABLE", row.nullable == SQL_NULLABLE);
        expectEq("c_smallint: IS_NULLABLE == YES", std::string("YES") == row.isNullable);
        expectEq("c_smallint: ORDINAL_POSITION == 1", row.ordinalPosition == 1);
        expectEq("c_smallint: SQL_DATA_TYPE == SQL_SMALLINT", row.sqlDataType == SQL_SMALLINT);
        expectEq("c_smallint: SQL_DATETIME_SUB is NULL", row.sqlDatetimeSubInd == SQL_NULL_DATA);

        rc = SQLFetch(stmt);
        expectEq("row 2 (c_integer) found", rc == SQL_SUCCESS);
        expectEq("c_integer: DATA_TYPE == SQL_INTEGER", row.dataType == SQL_INTEGER);
        expectEq("c_integer: NULLABLE == SQL_NO_NULLS", row.nullable == SQL_NO_NULLS);
        expectEq("c_integer: IS_NULLABLE == NO", std::string("NO") == row.isNullable);
        expectEq("c_integer: ORDINAL_POSITION == 2", row.ordinalPosition == 2);

        rc = SQLFetch(stmt);
        expectEq("row 3 (c_char) found", rc == SQL_SUCCESS);
        expectEq("c_char: DATA_TYPE == SQL_CHAR", row.dataType == SQL_CHAR);
        expectEq("c_char: COLUMN_SIZE == 20", row.columnSize == 20);

        rc = SQLFetch(stmt);
        expectEq("row 4 (c_decimal) found", rc == SQL_SUCCESS);
        expectEq("c_decimal: DATA_TYPE == SQL_DECIMAL", row.dataType == SQL_DECIMAL);
        expectEq("c_decimal: COLUMN_SIZE == 10 (precision)", row.columnSize == 10);
        expectEq("c_decimal: DECIMAL_DIGITS == 2 (scale)",
                  row.decimalDigitsInd != SQL_NULL_DATA && row.decimalDigits == 2);

        rc = SQLFetch(stmt);
        expectEq("row 5 (c_date) found", rc == SQL_SUCCESS);
        expectEq("c_date: DATA_TYPE == SQL_TYPE_DATE", row.dataType == SQL_TYPE_DATE);
        expectEq("c_date: TYPE_NAME == date", std::string("date") == row.typeName);
        expectEq("c_date: COLUMN_SIZE == 10", row.columnSize == 10);
        expectEq("c_date: SQL_DATA_TYPE == SQL_DATETIME", row.sqlDataType == SQL_DATETIME);
        expectEq("c_date: SQL_DATETIME_SUB == SQL_CODE_DATE",
                  row.sqlDatetimeSubInd != SQL_NULL_DATA && row.sqlDatetimeSub == SQL_CODE_DATE);

        rc = SQLFetch(stmt);
        expectEq("row 6 (c_datetime) found", rc == SQL_SUCCESS);
        expectEq("c_datetime: DATA_TYPE == SQL_TYPE_TIMESTAMP", row.dataType == SQL_TYPE_TIMESTAMP);
        expectEq("c_datetime: TYPE_NAME == datetime", std::string("datetime") == row.typeName);
        expectEq("c_datetime: COLUMN_SIZE == 19", row.columnSize == 19);
        expectEq("c_datetime: SQL_DATA_TYPE == SQL_DATETIME", row.sqlDataType == SQL_DATETIME);
        expectEq("c_datetime: SQL_DATETIME_SUB == SQL_CODE_TIMESTAMP",
                  row.sqlDatetimeSubInd != SQL_NULL_DATA && row.sqlDatetimeSub == SQL_CODE_TIMESTAMP);

        rc = SQLFetch(stmt);
        expectEq("exactly 6 columns for columns_probe", rc == SQL_NO_DATA);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // ColumnName filter.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc =
            SQLColumns(stmt, nullptr, 0, nullptr, 0,
                       reinterpret_cast<SQLCHAR*>(const_cast<char*>("columns_probe")), SQL_NTS,
                       reinterpret_cast<SQLCHAR*>(const_cast<char*>("c_int%")), SQL_NTS);
        expectEq("SQLColumns(ColumnName=c_int%) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        char name[128] = {};
        SQLLEN nameInd = 0;
        SQLBindCol(stmt, 4, SQL_C_CHAR, name, sizeof(name), &nameInd);
        rc = SQLFetch(stmt);
        expectEq("ColumnName=c_int% finds exactly c_integer",
                  rc == SQL_SUCCESS && nameInd != SQL_NULL_DATA && std::string("c_integer") == name);
        rc = SQLFetch(stmt);
        expectEq("ColumnName=c_int% finds only that one column", rc == SQL_NO_DATA);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // TABLE_CAT/TABLE_SCHEM sanity, and a mismatched CatalogName returning
    // zero rows, same convention as SQLTables.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLColumns(stmt, nullptr, 0, nullptr, 0,
                                   reinterpret_cast<SQLCHAR*>(const_cast<char*>("columns_probe")), SQL_NTS,
                                   reinterpret_cast<SQLCHAR*>(const_cast<char*>("c_smallint")), SQL_NTS);
        char cat[128] = {}, schem[128] = {};
        SQLLEN catInd = 0, schemInd = 0;
        SQLBindCol(stmt, 1, SQL_C_CHAR, cat, sizeof(cat), &catInd);
        SQLBindCol(stmt, 2, SQL_C_CHAR, schem, sizeof(schem), &schemInd);
        rc = SQLFetch(stmt);
        expectEq("TABLE_CAT == connected database", rc == SQL_SUCCESS && catInd != SQL_NULL_DATA && database == cat);
        expectEq("TABLE_SCHEM == informix", schemInd != SQL_NULL_DATA && std::string("informix") == schem);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);

        SQLHSTMT stmt2 = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt2);
        rc = SQLColumns(stmt2, reinterpret_cast<SQLCHAR*>(const_cast<char*>("no_such_database")), SQL_NTS, nullptr, 0,
                         reinterpret_cast<SQLCHAR*>(const_cast<char*>("columns_probe")), SQL_NTS, nullptr, 0);
        expectEq("SQLColumns(CatalogName=no_such_database) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        rc = SQLFetch(stmt2);
        expectEq("mismatched CatalogName returns zero rows", rc == SQL_NO_DATA);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt2);
    }

    execDirect(dbc, "drop table if exists columns_probe");
    return ok;
}

// Verifies SQLGetTypeInfo (driver/catalog.cpp): SQL_ALL_TYPES returns a row
// per kTypeMap entry with sane field values, a specific DataType returns
// exactly the matching rows (including the DATA_TYPE-collision case -
// INTEGER and SERIAL both report SQL_INTEGER), and a DataType nothing maps
// to returns zero rows rather than an error.
bool runTypeInfoTest(SQLHDBC dbc) {
    bool ok = true;
    auto expectEq = [&ok](const char* label, bool cond) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
        if (!cond) ok = false;
    };

    struct TypeRow {
        char typeName[64] = {};
        SQLLEN typeNameInd = 0;
        SQLSMALLINT dataType = 0;
        SQLLEN dataTypeInd = 0;
        SQLINTEGER columnSize = 0;
        SQLLEN columnSizeInd = 0;
        char literalPrefix[4] = {};
        SQLLEN literalPrefixInd = 0;
        char createParams[32] = {};
        SQLLEN createParamsInd = 0;
        SQLSMALLINT nullable = 0;
        SQLLEN nullableInd = 0;
        SQLSMALLINT caseSensitive = 0;
        SQLLEN caseSensitiveInd = 0;
        SQLSMALLINT unsignedAttr = 0;
        SQLLEN unsignedAttrInd = 0;
        SQLSMALLINT autoUnique = 0;
        SQLLEN autoUniqueInd = 0;
        SQLSMALLINT sqlDataType = 0;
        SQLLEN sqlDataTypeInd = 0;
        SQLSMALLINT sqlDatetimeSub = 0;
        SQLLEN sqlDatetimeSubInd = 0;
    };

    // SQL_ALL_TYPES: find "integer" among the rows and check its fields.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLGetTypeInfo(stmt, SQL_ALL_TYPES);
        expectEq("SQLGetTypeInfo(SQL_ALL_TYPES) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) printDiag(SQL_HANDLE_STMT, stmt, "SQLGetTypeInfo");

        TypeRow row;
        SQLBindCol(stmt, 1, SQL_C_CHAR, row.typeName, sizeof(row.typeName), &row.typeNameInd);
        SQLBindCol(stmt, 2, SQL_C_SSHORT, &row.dataType, 0, &row.dataTypeInd);
        SQLBindCol(stmt, 3, SQL_C_SLONG, &row.columnSize, 0, &row.columnSizeInd);
        SQLBindCol(stmt, 4, SQL_C_CHAR, row.literalPrefix, sizeof(row.literalPrefix), &row.literalPrefixInd);
        SQLBindCol(stmt, 6, SQL_C_CHAR, row.createParams, sizeof(row.createParams), &row.createParamsInd);
        SQLBindCol(stmt, 7, SQL_C_SSHORT, &row.nullable, 0, &row.nullableInd);
        SQLBindCol(stmt, 8, SQL_C_SSHORT, &row.caseSensitive, 0, &row.caseSensitiveInd);
        SQLBindCol(stmt, 10, SQL_C_SSHORT, &row.unsignedAttr, 0, &row.unsignedAttrInd);
        SQLBindCol(stmt, 12, SQL_C_SSHORT, &row.autoUnique, 0, &row.autoUniqueInd);
        // Columns 16/17, not 14/15 (that's SQLColumns' layout) -
        // SQLGetTypeInfo's result set has MINIMUM_SCALE/MAXIMUM_SCALE at
        // 14/15, ahead of SQL_DATA_TYPE/SQL_DATETIME_SUB.
        SQLBindCol(stmt, 16, SQL_C_SSHORT, &row.sqlDataType, 0, &row.sqlDataTypeInd);
        SQLBindCol(stmt, 17, SQL_C_SSHORT, &row.sqlDatetimeSub, 0, &row.sqlDatetimeSubInd);

        bool foundInteger = false;
        bool foundChar = false;
        bool foundDate = false;
        bool foundDatetime = false;
        int rowCount = 0;
        while (SQLFetch(stmt) == SQL_SUCCESS) {
            ++rowCount;
            std::string name = row.typeNameInd != SQL_NULL_DATA ? row.typeName : "";
            if (name == "integer") {
                foundInteger = true;
                expectEq("integer: DATA_TYPE == SQL_INTEGER", row.dataType == SQL_INTEGER);
                expectEq("integer: COLUMN_SIZE == 10", row.columnSize == 10);
                expectEq("integer: LITERAL_PREFIX is NULL", row.literalPrefixInd == SQL_NULL_DATA);
                expectEq("integer: NULLABLE == SQL_NULLABLE", row.nullable == SQL_NULLABLE);
                expectEq("integer: CASE_SENSITIVE == 0", row.caseSensitive == 0);
                expectEq("integer: UNSIGNED_ATTRIBUTE == 0",
                          row.unsignedAttrInd != SQL_NULL_DATA && row.unsignedAttr == 0);
                expectEq("integer: AUTO_UNIQUE_VALUE is NULL", row.autoUniqueInd == SQL_NULL_DATA);
            } else if (name == "serial") {
                expectEq("serial: DATA_TYPE == SQL_INTEGER (collides with integer)", row.dataType == SQL_INTEGER);
                expectEq("serial: AUTO_UNIQUE_VALUE == 1",
                          row.autoUniqueInd != SQL_NULL_DATA && row.autoUnique == 1);
            } else if (name == "varchar") {
                foundChar = true;
                expectEq("varchar: LITERAL_PREFIX == '", row.literalPrefixInd != SQL_NULL_DATA &&
                                                              std::string(row.literalPrefix) == "'");
                expectEq("varchar: CASE_SENSITIVE == 1", row.caseSensitive == 1);
                expectEq("varchar: CREATE_PARAMS == max length",
                          row.createParamsInd != SQL_NULL_DATA && std::string(row.createParams) == "max length");
            } else if (name == "date") {
                foundDate = true;
                expectEq("date: SQL_DATA_TYPE == SQL_DATETIME",
                          row.sqlDataTypeInd != SQL_NULL_DATA && row.sqlDataType == SQL_DATETIME);
                expectEq("date: SQL_DATETIME_SUB == SQL_CODE_DATE",
                          row.sqlDatetimeSubInd != SQL_NULL_DATA && row.sqlDatetimeSub == SQL_CODE_DATE);
            } else if (name == "datetime") {
                foundDatetime = true;
                expectEq("datetime: SQL_DATA_TYPE == SQL_DATETIME",
                          row.sqlDataTypeInd != SQL_NULL_DATA && row.sqlDataType == SQL_DATETIME);
                expectEq("datetime: SQL_DATETIME_SUB == SQL_CODE_TIMESTAMP",
                          row.sqlDatetimeSubInd != SQL_NULL_DATA && row.sqlDatetimeSub == SQL_CODE_TIMESTAMP);
                expectEq("datetime: SQL_DATETIME_SUB is not SQL_CODE_DATE (date/datetime are distinguishable)",
                          row.sqlDatetimeSub != SQL_CODE_DATE);
            } else {
                expectEq((name + ": SQL_DATETIME_SUB is NULL (not a datetime type)").c_str(),
                          row.sqlDatetimeSubInd == SQL_NULL_DATA);
            }
        }
        expectEq("SQL_ALL_TYPES found an 'integer' row", foundInteger);
        expectEq("SQL_ALL_TYPES found a 'varchar' row", foundChar);
        expectEq("SQL_ALL_TYPES found a 'date' row", foundDate);
        expectEq("SQL_ALL_TYPES found a 'datetime' row", foundDatetime);
        // 16, not the 14 this assertion originally hardcoded: byte/text
        // (simple LOB) rows were added to kTypeMap in a later commit
        // without updating this count - a pre-existing staleness bug
        // unrelated to the smart LOB bind work, caught incidentally while
        // verifying that work through this same tool.
        expectEq("SQL_ALL_TYPES returned all 16 kTypeMap rows", rowCount == 16);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // DataType=SQL_INTEGER: exactly the two rows that collide on DATA_TYPE
    // (integer, serial), ordered by data_type (both are SQL_INTEGER, so no
    // further ordering guarantee - just check both are present).
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLGetTypeInfo(stmt, SQL_INTEGER);
        expectEq("SQLGetTypeInfo(SQL_INTEGER) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);

        char name[64] = {};
        SQLLEN nameInd = 0;
        SQLBindCol(stmt, 1, SQL_C_CHAR, name, sizeof(name), &nameInd);
        int rowCount = 0;
        bool sawInteger = false, sawSerial = false;
        while (SQLFetch(stmt) == SQL_SUCCESS) {
            ++rowCount;
            std::string n = nameInd != SQL_NULL_DATA ? name : "";
            if (n == "integer") sawInteger = true;
            if (n == "serial") sawSerial = true;
        }
        expectEq("DataType=SQL_INTEGER returns exactly 2 rows", rowCount == 2);
        expectEq("DataType=SQL_INTEGER includes integer and serial", sawInteger && sawSerial);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // A DataType nothing maps to (SQL_WCHAR - no Unicode support yet)
    // returns zero rows, not an error.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
        SQLRETURN rc = SQLGetTypeInfo(stmt, SQL_WCHAR);
        expectEq("SQLGetTypeInfo(SQL_WCHAR) succeeds", rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
        rc = SQLFetch(stmt);
        expectEq("DataType with no match returns zero rows", rc == SQL_NO_DATA);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::printf("usage: odbc_dm_probe <driver_path> <host> <port> <user> <password> [server] [database]\n");
        return 1;
    }
    std::string driverPath = argv[1];
    std::string host = argv[2];
    std::string port = argv[3];
    std::string user = argv[4];
    std::string password = argv[5];
    std::string server = argc > 6 ? argv[6] : "informix";
    std::string database = argc > 7 ? argv[7] : "";

    SQLHENV env = SQL_NULL_HENV;
    SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    if (rc != SQL_SUCCESS) {
        std::printf("SQLAllocHandle(ENV) failed: rc=%d\n", static_cast<int>(rc));
        return 1;
    }
    std::printf("SQLAllocHandle(ENV) ok\n");

    rc = SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    std::printf("SQLSetEnvAttr(ODBC3) rc=%d\n", static_cast<int>(rc));

    SQLHDBC dbc = SQL_NULL_HDBC;
    rc = SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
    if (rc != SQL_SUCCESS) {
        std::printf("SQLAllocHandle(DBC) failed: rc=%d\n", static_cast<int>(rc));
        printDiag(SQL_HANDLE_ENV, env, "env");
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return 1;
    }
    std::printf("SQLAllocHandle(DBC) ok\n");

    std::string connStr = "DRIVER={" + driverPath + "};HOST=" + host + ";SERVICE=" + port + ";SERVER=" + server +
                          ";UID=" + user + ";PWD=" + password;
    if (!database.empty()) connStr += ";DATABASE=" + database;

    SQLCHAR outConnStr[1024];
    SQLSMALLINT outConnStrLen = 0;
    std::printf("SQLDriverConnect: %s\n", connStr.c_str());
    rc = SQLDriverConnect(dbc, nullptr, reinterpret_cast<SQLCHAR*>(connStr.data()),
                           static_cast<SQLSMALLINT>(connStr.size()), outConnStr, sizeof(outConnStr), &outConnStrLen,
                           SQL_DRIVER_NOPROMPT);
    std::printf("SQLDriverConnect rc=%d\n", static_cast<int>(rc));
    printDiag(SQL_HANDLE_DBC, dbc, "dbc");
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return 1;
    }

    SQLCHAR dbmsName[128];
    SQLSMALLINT dbmsNameLen = 0;
    if (SQLGetInfo(dbc, SQL_DBMS_VER, dbmsName, sizeof(dbmsName), &dbmsNameLen) == SQL_SUCCESS) {
        std::printf("SQLGetInfo(SQL_DBMS_VER) = %s\n", dbmsName);
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    rc = SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
    std::printf("SQLAllocHandle(STMT) rc=%d\n", static_cast<int>(rc));
    if (rc == SQL_SUCCESS) {
        const char* sql = "select tabid, tabname from systables where tabid <= 3";
        rc = SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
        std::printf("SQLExecDirect rc=%d\n", static_cast<int>(rc));
        printDiag(SQL_HANDLE_STMT, stmt, "stmt");

        if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
            SQLSMALLINT numCols = 0;
            SQLNumResultCols(stmt, &numCols);
            std::printf("SQLNumResultCols = %d\n", static_cast<int>(numCols));

            for (SQLSMALLINT i = 1; i <= numCols; ++i) {
                SQLCHAR colName[128];
                SQLSMALLINT colNameLen = 0, dataType = 0, decimalDigits = 0, nullable = 0;
                SQLULEN colSize = 0;
                rc = SQLDescribeCol(stmt, static_cast<SQLUSMALLINT>(i), colName, sizeof(colName), &colNameLen,
                                     &dataType, &colSize, &decimalDigits, &nullable);
                std::printf("SQLDescribeCol(%d) rc=%d name=\"%s\" nameLen=%d type=%d size=%lu\n", i,
                            static_cast<int>(rc), colName, static_cast<int>(colNameLen), static_cast<int>(dataType),
                            static_cast<unsigned long>(colSize));
            }

            int rowNum = 0;
            while ((rc = SQLFetch(stmt)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
                ++rowNum;
                std::printf("row %d:", rowNum);
                for (SQLSMALLINT i = 1; i <= numCols; ++i) {
                    SQLCHAR buf[256];
                    SQLLEN ind = 0;
                    SQLGetData(stmt, static_cast<SQLUSMALLINT>(i), SQL_C_CHAR, buf, sizeof(buf), &ind);
                    std::printf(" [%s]", ind == SQL_NULL_DATA ? "NULL" : reinterpret_cast<char*>(buf));
                }
                std::printf("\n");
            }
            std::printf("SQLFetch loop ended rc=%d\n", static_cast<int>(rc));
        }
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    std::printf("--- Phase 4 slice 3: native C-type conversions ---\n");
    bool nativeOk = runNativeTypeTest(dbc);
    std::printf("native-type test: %s\n", nativeOk ? "PASS" : "FAIL");

    std::printf("--- Phase 4 slice 4: diagnostics/SQLSTATE hardening ---\n");
    bool diagOk = runDiagnosticsTest(dbc);
    std::printf("diagnostics test: %s\n", diagOk ? "PASS" : "FAIL");

    std::printf("--- Phase 5 slice 1: bind parameters (SQL_INTEGER) ---\n");
    bool bindOk = runBindParamTest(dbc);
    std::printf("bind-parameter test: %s\n", bindOk ? "PASS" : "FAIL");

    std::printf("--- Smart LOB (BLOB/CLOB) bind parameters ---\n");
    bool smartLobBindOk = runSmartLobBindTest(dbc);
    std::printf("smart-LOB bind-parameter test: %s\n", smartLobBindOk ? "PASS" : "FAIL");

    std::printf("--- SQLEndTran + SQL_ATTR_AUTOCOMMIT ---\n");
    bool txnOk = runTransactionTest(env, dbc, connStr);
    std::printf("transaction test: %s\n", txnOk ? "PASS" : "FAIL");

    std::printf("--- Phase 7: SQLTables ---\n");
    bool catalogOk = runCatalogTest(dbc, database);
    std::printf("catalog test: %s\n", catalogOk ? "PASS" : "FAIL");

    std::printf("--- Phase 7: SQLColumns ---\n");
    bool columnsOk = runColumnsTest(dbc, database);
    std::printf("columns test: %s\n", columnsOk ? "PASS" : "FAIL");

    std::printf("--- Phase 7: SQLGetTypeInfo ---\n");
    bool typeInfoOk = runTypeInfoTest(dbc);
    std::printf("type-info test: %s\n", typeInfoOk ? "PASS" : "FAIL");

    rc = SQLDisconnect(dbc);
    std::printf("SQLDisconnect rc=%d\n", static_cast<int>(rc));
    printDiag(SQL_HANDLE_DBC, dbc, "dbc");

    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
    std::printf("done.\n");
    bool disconnectOk = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
    return (disconnectOk && nativeOk && diagOk && bindOk && smartLobBindOk && txnOk && catalogOk && columnsOk &&
            typeInfoOk)
               ? 0
               : 1;
}
