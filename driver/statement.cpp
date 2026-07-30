// Phase 4 slice 2: SQLPrepare/SQLExecute/SQLExecDirect, SQLNumResultCols,
// SQLDescribeCol, SQLColAttribute, SQLBindCol, SQLFetch, SQLGetData,
// SQLFreeStmt, SQLRowCount - the SQL_C_CHAR-only SELECT round trip. See the
// plan for why native C-type conversions (SQL_C_SLONG, SQL_DATE_STRUCT,
// etc.) are deliberately deferred to slice 3.

#include "driver/convert.h"
#include "driver/error_map.h"
#include "driver/handles.h"
#include "sqli/types.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace ifxodbc;

namespace {

std::string toStdString(SQLCHAR* text, SQLINTEGER length) {
    if (!text) return {};
    if (length == SQL_NTS) return reinterpret_cast<const char*>(text);
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(length));
}

// Writes `text` (or a NULL indicator) into an application buffer per the
// SQLGetData/SQLBindCol truncation contract: copies up to BufferLength-1
// bytes plus a NUL, reports the *untruncated* length via
// StrLen_or_IndPtr, and signals truncation to the caller via the returned
// bool (so callers can post 01004 and return SQL_SUCCESS_WITH_INFO).
bool writeCharResult(const CellText& cell, SQLPOINTER targetValuePtr, SQLLEN bufferLength, SQLLEN* strLenOrIndPtr) {
    if (cell.isNull) {
        if (strLenOrIndPtr) *strLenOrIndPtr = SQL_NULL_DATA;
        return false;
    }
    auto fullLen = static_cast<SQLLEN>(cell.text.size());
    if (strLenOrIndPtr) *strLenOrIndPtr = fullLen;
    if (!targetValuePtr || bufferLength <= 0) return false;

    auto toCopy = std::min<SQLLEN>(fullLen, bufferLength - 1);
    std::memcpy(targetValuePtr, cell.text.data(), static_cast<std::size_t>(toCopy));
    static_cast<char*>(targetValuePtr)[toCopy] = '\0';
    return toCopy < fullLen;
}

// Maps an Informix base type code to the nearest standard SQL_* type, for
// SQLDescribeCol/SQLColAttribute. Approximate by design (Phase 4 slice 2 is
// about proving the round trip works, not full catalog-metadata fidelity).
SQLSMALLINT sqlTypeFor(int ifxType) {
    using namespace ifxsqli::ifxtype;
    switch (ifxType) {
        case kSmallInt: return SQL_SMALLINT;
        case kInteger:
        case kSerial: return SQL_INTEGER;
        case kBigInt:
        case kBigSerial: return SQL_BIGINT;
        case kChar: return SQL_CHAR;
        case kVarchar:
        case kNVarchar: return SQL_VARCHAR;
        case kDecimal: return SQL_DECIMAL;
        case kDate: return SQL_TYPE_DATE;
        case kDatetime: return SQL_TYPE_TIMESTAMP;
        default: return SQL_VARCHAR; // unknown types still round-trip as text (driver/convert.h's default case)
    }
}

SQLULEN columnSizeFor(const ifxsqli::ColumnDesc& col) {
    using namespace ifxsqli::ifxtype;
    switch (col.ifxType) {
        case kSmallInt: return 5;
        case kInteger:
        case kSerial: return 10;
        case kBigInt:
        case kBigSerial: return 19;
        case kChar: return col.encodedLength;
        case kVarchar:
        case kNVarchar: return col.encodedLength;
        case kDecimal: return (col.encodedLength >> 8) & 0xFF;
        case kDate: return 10;      // "YYYY-MM-DD"
        case kDatetime: return 19;  // "YYYY-MM-DD HH:MM:SS"
        default: return 0;
    }
}

SQLSMALLINT decimalDigitsFor(const ifxsqli::ColumnDesc& col) {
    return (col.ifxType == ifxsqli::ifxtype::kDecimal) ? static_cast<SQLSMALLINT>(col.encodedLength & 0xFF) : 0;
}

} // namespace

namespace {

// SQLExecDirect is implemented as prepare-then-execute, but it must NOT do
// so by calling the exported SQLPrepare/SQLExecute symbols by name: on ELF
// platforms, a same-named default-visibility symbol in another loaded
// shared object (unixODBC's own libodbc.so, which - being a Driver
// Manager - also exports functions literally named SQLPrepare/SQLExecute/
// etc.) can interpose on the call, silently redirecting it to the DM's
// version instead of this driver's own. Confirmed live: an earlier version
// of this function called SQLPrepare(...)/SQLExecute(...) directly and
// SQLExecDirect returned SQL_INVALID_HANDLE (-2) every time - the DM's
// SQLPrepare doesn't understand this driver's raw OdbcStatement* handles.
// Routing all three entry points through these internal (non-exported)
// helpers instead sidesteps the whole class of bug.
SQLRETURN doPrepare(OdbcStatement* s, const std::string& sql) {
    if (!s->conn->connected) {
        postDriverError(s->diag, "08003", "connection is not open");
        return SQL_ERROR;
    }
    auto result = ifxsqli::SqliStatement::prepare(*s->conn->sqli, sql);
    if (!result) {
        postSqliError(s->diag, result.error());
        return SQL_ERROR;
    }
    s->stmt.emplace(std::move(*result));
    s->state = StmtState::kPrepared;
    s->currentRow.reset();
    return SQL_SUCCESS;
}

SQLRETURN doExecute(OdbcStatement* s) {
    if (s->state == StmtState::kUnprepared || !s->stmt) {
        postDriverError(s->diag, "HY010", "function sequence error: statement is not prepared");
        return SQL_ERROR;
    }
    auto result = s->stmt->execute();
    if (!result) {
        postSqliError(s->diag, result.error());
        return SQL_ERROR;
    }
    s->state = StmtState::kExecuted;
    s->currentRow.reset();
    return SQL_SUCCESS;
}

} // namespace

extern "C" {

IFXODBC_EXPORT SQLRETURN SQL_API SQLPrepare(SQLHSTMT StatementHandle, SQLCHAR* StatementText, SQLINTEGER TextLength) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    return doPrepare(s, toStdString(StatementText, TextLength));
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLExecute(SQLHSTMT StatementHandle) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    return doExecute(s);
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLExecDirect(SQLHSTMT StatementHandle, SQLCHAR* StatementText, SQLINTEGER TextLength) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    SQLRETURN rc = doPrepare(s, toStdString(StatementText, TextLength));
    if (rc != SQL_SUCCESS) return rc;
    return doExecute(s);
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT StatementHandle, SQLSMALLINT* ColumnCountPtr) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    if (!s->stmt) {
        postDriverError(s->diag, "HY010", "function sequence error: statement is not prepared");
        return SQL_ERROR;
    }
    if (ColumnCountPtr) *ColumnCountPtr = static_cast<SQLSMALLINT>(s->stmt->columns().size());
    return SQL_SUCCESS;
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLDescribeCol(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber, SQLCHAR* ColumnName,
                                  SQLSMALLINT BufferLength, SQLSMALLINT* NameLengthPtr, SQLSMALLINT* DataTypePtr,
                                  SQLULEN* ColumnSizePtr, SQLSMALLINT* DecimalDigitsPtr, SQLSMALLINT* NullablePtr) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    if (!s->stmt) {
        postDriverError(s->diag, "HY010", "function sequence error: statement is not prepared");
        return SQL_ERROR;
    }
    const auto& cols = s->stmt->columns();
    if (ColumnNumber < 1 || ColumnNumber > cols.size()) {
        postDriverError(s->diag, "07009", "invalid column number");
        return SQL_ERROR;
    }
    const auto& col = cols[ColumnNumber - 1];

    if (ColumnName) {
        auto len = static_cast<SQLSMALLINT>(col.name.size());
        auto toCopy = std::min<SQLSMALLINT>(len, static_cast<SQLSMALLINT>(BufferLength > 0 ? BufferLength - 1 : 0));
        std::memcpy(ColumnName, col.name.data(), static_cast<std::size_t>(toCopy));
        if (BufferLength > 0) ColumnName[toCopy] = '\0';
        if (NameLengthPtr) *NameLengthPtr = len;
    } else if (NameLengthPtr) {
        *NameLengthPtr = static_cast<SQLSMALLINT>(col.name.size());
    }

    if (DataTypePtr) *DataTypePtr = sqlTypeFor(col.ifxType);
    if (ColumnSizePtr) *ColumnSizePtr = columnSizeFor(col);
    if (DecimalDigitsPtr) *DecimalDigitsPtr = decimalDigitsFor(col);
    if (NullablePtr) *NullablePtr = col.notNull ? SQL_NO_NULLS : SQL_NULLABLE;
    return SQL_SUCCESS;
}

namespace {

// Shared by SQLColAttribute (ODBC3) and SQLColAttributes (the ODBC2 legacy
// name some clients - isql included - still call; same parameter shapes,
// and the field-identifier constants used here are the ones common to
// both). Kept as an internal (non-exported) helper for the same reason
// doPrepare/doExecute are: an exported function calling another exported
// same-named symbol risks ELF interposition from the Driver Manager's own
// same-named export - see that comment for the concrete failure this
// caused elsewhere in this file.
SQLRETURN doColAttribute(OdbcStatement* s, SQLUSMALLINT ColumnNumber, SQLUSMALLINT FieldIdentifier,
                          SQLPOINTER CharacterAttributePtr, SQLSMALLINT BufferLength, SQLSMALLINT* StringLengthPtr,
                          SQLLEN* NumericAttributePtr) {
    if (!s->stmt) {
        postDriverError(s->diag, "HY010", "function sequence error: statement is not prepared");
        return SQL_ERROR;
    }
    const auto& cols = s->stmt->columns();
    if (ColumnNumber < 1 || ColumnNumber > cols.size()) {
        postDriverError(s->diag, "07009", "invalid column number");
        return SQL_ERROR;
    }
    const auto& col = cols[ColumnNumber - 1];

    switch (FieldIdentifier) {
        case SQL_DESC_NAME:
        case SQL_COLUMN_NAME: {
            auto len = static_cast<SQLSMALLINT>(col.name.size());
            auto toCopy =
                std::min<SQLSMALLINT>(len, static_cast<SQLSMALLINT>(BufferLength > 0 ? BufferLength - 1 : 0));
            if (CharacterAttributePtr && BufferLength > 0) {
                std::memcpy(CharacterAttributePtr, col.name.data(), static_cast<std::size_t>(toCopy));
                static_cast<char*>(CharacterAttributePtr)[toCopy] = '\0';
            }
            if (StringLengthPtr) *StringLengthPtr = len;
            return SQL_SUCCESS;
        }
        case SQL_DESC_TYPE:
        case SQL_COLUMN_TYPE:
            if (NumericAttributePtr) *NumericAttributePtr = sqlTypeFor(col.ifxType);
            return SQL_SUCCESS;
        case SQL_DESC_LENGTH:
        case SQL_COLUMN_LENGTH:
        // SQL_DESC_DISPLAY_SIZE is #defined as SQL_COLUMN_DISPLAY_SIZE (same
        // numeric value) on this SDK - listed once to avoid a duplicate
        // case error; the two names are simply the ODBC3/2.x aliases.
        case SQL_COLUMN_DISPLAY_SIZE:
            if (NumericAttributePtr) *NumericAttributePtr = static_cast<SQLLEN>(columnSizeFor(col));
            return SQL_SUCCESS;
        case SQL_DESC_SCALE:
        case SQL_COLUMN_SCALE:
            if (NumericAttributePtr) *NumericAttributePtr = decimalDigitsFor(col);
            return SQL_SUCCESS;
        case SQL_DESC_NULLABLE:
        case SQL_COLUMN_NULLABLE:
            if (NumericAttributePtr) *NumericAttributePtr = col.notNull ? SQL_NO_NULLS : SQL_NULLABLE;
            return SQL_SUCCESS;
        default:
            postDriverError(s->diag, "HYC00", "SQLColAttribute field not yet supported");
            return SQL_ERROR;
    }
}

} // namespace

IFXODBC_EXPORT SQLRETURN SQL_API SQLColAttribute(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber, SQLUSMALLINT FieldIdentifier,
                                   SQLPOINTER CharacterAttributePtr, SQLSMALLINT BufferLength,
                                   SQLSMALLINT* StringLengthPtr, SQLLEN* NumericAttributePtr) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    return doColAttribute(s, ColumnNumber, FieldIdentifier, CharacterAttributePtr, BufferLength, StringLengthPtr,
                          NumericAttributePtr);
}

// ODBC 2.x legacy name for the same function, same parameter shapes except
// pfDesc is a plain SQLLEN* on this SDK (matching NumericAttributePtr
// exactly) - still called by some clients (confirmed: isql prints blank
// column headers without this, even though SQLDescribeCol/SQLColAttribute
// both return the correct name when called directly - see
// tools/odbc_dm_probe, which exercises SQLDescribeCol successfully).
IFXODBC_EXPORT SQLRETURN SQL_API SQLColAttributes(SQLHSTMT hstmt, SQLUSMALLINT icol, SQLUSMALLINT fDescType,
                                    SQLPOINTER rgbDesc, SQLSMALLINT cbDescMax, SQLSMALLINT* pcbDesc,
                                    SQLLEN* pfDesc) {
    auto* s = static_cast<OdbcStatement*>(hstmt);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    return doColAttribute(s, icol, fDescType, rgbDesc, cbDescMax, pcbDesc, pfDesc);
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLBindCol(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
                              SQLPOINTER TargetValuePtr, SQLLEN BufferLength, SQLLEN* StrLen_or_IndPtr) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();

    if (!TargetValuePtr) {
        s->boundColumns.erase(ColumnNumber);
        return SQL_SUCCESS;
    }
    if (TargetType != SQL_C_CHAR && TargetType != SQL_C_DEFAULT) {
        postDriverError(s->diag, "HYC00", "SQLBindCol: only SQL_C_CHAR is supported so far (Phase 4 slice 2)");
        return SQL_ERROR;
    }
    s->boundColumns[ColumnNumber] = BoundColumn{SQL_C_CHAR, TargetValuePtr, BufferLength, StrLen_or_IndPtr};
    return SQL_SUCCESS;
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLFetch(SQLHSTMT StatementHandle) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    if (s->state != StmtState::kExecuted || !s->stmt) {
        postDriverError(s->diag, "HY010", "function sequence error: statement is not executed");
        return SQL_ERROR;
    }

    auto row = s->stmt->fetchRow();
    if (!row) {
        postSqliError(s->diag, row.error());
        return SQL_ERROR;
    }
    if (!*row) {
        s->currentRow.reset();
        return SQL_NO_DATA;
    }
    s->currentRow = std::move(*row);

    bool anyTruncated = false;
    const auto& cols = s->stmt->columns();
    for (auto& [colNumber, binding] : s->boundColumns) {
        if (colNumber < 1 || colNumber > cols.size()) continue;
        auto cell = formatCellAsText(cols[colNumber - 1], (*s->currentRow)[colNumber - 1]);
        if (!cell) {
            postSqliError(s->diag, cell.error());
            return SQL_ERROR;
        }
        if (writeCharResult(*cell, binding.targetValuePtr, binding.bufferLength, binding.strLenOrIndPtr)) {
            anyTruncated = true;
        }
    }
    if (anyTruncated) {
        postDriverError(s->diag, "01004", "string data, right-truncated");
        return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLGetData(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
                              SQLPOINTER TargetValuePtr, SQLLEN BufferLength, SQLLEN* StrLen_or_IndPtr) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    if (!s->currentRow) {
        postDriverError(s->diag, "HY010", "function sequence error: no current row (call SQLFetch first)");
        return SQL_ERROR;
    }
    if (TargetType != SQL_C_CHAR && TargetType != SQL_C_DEFAULT) {
        postDriverError(s->diag, "HYC00", "SQLGetData: only SQL_C_CHAR is supported so far (Phase 4 slice 2)");
        return SQL_ERROR;
    }
    const auto& cols = s->stmt->columns();
    if (ColumnNumber < 1 || ColumnNumber > cols.size()) {
        postDriverError(s->diag, "07009", "invalid column number");
        return SQL_ERROR;
    }

    auto cell = formatCellAsText(cols[ColumnNumber - 1], (*s->currentRow)[ColumnNumber - 1]);
    if (!cell) {
        postSqliError(s->diag, cell.error());
        return SQL_ERROR;
    }
    if (writeCharResult(*cell, TargetValuePtr, BufferLength, StrLen_or_IndPtr)) {
        postDriverError(s->diag, "01004", "string data, right-truncated");
        return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT StatementHandle, SQLUSMALLINT Option) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();

    switch (Option) {
        case SQL_CLOSE:
            if (s->state == StmtState::kExecuted && s->stmt) {
                auto result = s->stmt->close();
                if (!result) {
                    postSqliError(s->diag, result.error());
                    return SQL_ERROR;
                }
                s->state = StmtState::kPrepared;
            }
            s->currentRow.reset();
            return SQL_SUCCESS;
        case SQL_UNBIND:
            s->boundColumns.clear();
            return SQL_SUCCESS;
        case SQL_RESET_PARAMS:
            return SQL_SUCCESS; // no bind parameters yet (Phase 5)
        case SQL_DROP:
            delete s; // equivalent to SQLFreeHandle(SQL_HANDLE_STMT, ...) per spec
            return SQL_SUCCESS;
        default:
            postDriverError(s->diag, "HY092", "invalid SQLFreeStmt option");
            return SQL_ERROR;
    }
}

IFXODBC_EXPORT SQLRETURN SQL_API SQLRowCount(SQLHSTMT StatementHandle, SQLLEN* RowCountPtr) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();
    // SELECT row counts aren't known ahead of a full fetch in this
    // protocol, and DML row counts aren't wired up yet (sqli/statement.cpp
    // reads but discards SQ_DONE's row-count field - see the plan's Phase 4
    // slice 3). -1 ("unknown") is spec-compliant for SELECT statements.
    if (RowCountPtr) *RowCountPtr = -1;
    return SQL_SUCCESS;
}

} // extern "C"
