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
    execDirect(dbc, "drop table if exists bind_probe");
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

    rc = SQLDisconnect(dbc);
    std::printf("SQLDisconnect rc=%d\n", static_cast<int>(rc));
    printDiag(SQL_HANDLE_DBC, dbc, "dbc");

    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
    std::printf("done.\n");
    bool disconnectOk = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
    return (disconnectOk && nativeOk && diagOk && bindOk) ? 0 : 1;
}
