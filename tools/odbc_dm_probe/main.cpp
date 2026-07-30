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

    rc = SQLDisconnect(dbc);
    std::printf("SQLDisconnect rc=%d\n", static_cast<int>(rc));
    printDiag(SQL_HANDLE_DBC, dbc, "dbc");

    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
    std::printf("done.\n");
    return (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) ? 0 : 1;
}
