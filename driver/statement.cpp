// Phase 4 slice 2: SQLPrepare/SQLExecute/SQLExecDirect, SQLNumResultCols,
// SQLDescribeCol, SQLColAttribute, SQLBindCol, SQLFetch, SQLGetData,
// SQLFreeStmt, SQLRowCount - the SQL_C_CHAR-only SELECT round trip. See the
// plan for why native C-type conversions (SQL_C_SLONG, SQL_DATE_STRUCT,
// etc.) are deliberately deferred to slice 3.

#include "driver/bind_encode.h"
#include "driver/convert.h"
#include "driver/convert_native.h"
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
        case kBoolean: return SQL_BIT;
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
        case kBoolean: return 1;
        default: return 0;
    }
}

SQLSMALLINT decimalDigitsFor(const ifxsqli::ColumnDesc& col) {
    return (col.ifxType == ifxsqli::ifxtype::kDecimal) ? static_cast<SQLSMALLINT>(col.encodedLength & 0xFF) : 0;
}

// Shared by SQLFetch's bound-column loop and SQLGetData: writes one cell to
// an application buffer per the requested C type, posting whichever
// diagnostic fits the outcome. SQL_C_CHAR/SQL_C_DEFAULT stays on the
// existing text path (driver/convert.h); every other supported C type
// (slice 3) goes through driver/convert_native.h.
SQLRETURN writeColumnValue(OdbcStatement* s, const ifxsqli::ColumnDesc& col, std::span<const std::byte> raw,
                           SQLSMALLINT targetType, SQLPOINTER targetValuePtr, SQLLEN bufferLength,
                           SQLLEN* strLenOrIndPtr) {
    if (targetType == SQL_C_CHAR || targetType == SQL_C_DEFAULT) {
        auto cell = formatCellAsText(col, raw);
        if (!cell) {
            postSqliError(s->diag, cell.error());
            return SQL_ERROR;
        }
        if (writeCharResult(*cell, targetValuePtr, bufferLength, strLenOrIndPtr)) {
            postDriverError(s->diag, "01004", "string data, right-truncated");
            return SQL_SUCCESS_WITH_INFO;
        }
        return SQL_SUCCESS;
    }
    if (isNativeTargetType(targetType)) {
        auto r = writeNativeCell(col, raw, targetType, targetValuePtr, bufferLength, strLenOrIndPtr);
        if (!r) {
            postSqliError(s->diag, r.error());
            return SQL_ERROR;
        }
        switch (r->status) {
            case NativeConvertStatus::kOk:
            case NativeConvertStatus::kNull:
                return SQL_SUCCESS;
            case NativeConvertStatus::kOverflow:
                postDriverError(s->diag, "22003", "numeric value out of range");
                return SQL_ERROR;
            case NativeConvertStatus::kUnsupported:
                postDriverError(s->diag, "07006", "restricted data type attribute violation");
                return SQL_ERROR;
        }
    }
    postDriverError(s->diag, "HYC00", "unsupported SQLBindCol/SQLGetData C target type");
    return SQL_ERROR;
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

// Reads each bound parameter's current value (from wherever
// SQLBindParameter pointed - the application may have changed the pointed-
// to value between binding and this SQLExecute call, which is expected)
// and encodes it into a sqli::BindParam list ready for SqliStatement::
// execute(). Returns SQL_ERROR (with a diagnostic already posted) on a
// count mismatch or an unsupported ValueType/ParameterType pairing.
std::optional<std::vector<ifxsqli::BindParam>> buildBindParams(OdbcStatement* s) {
    const std::size_t expected = s->stmt->paramCount();
    if (expected == 0) return std::vector<ifxsqli::BindParam>{};
    if (s->paramBindings.size() != expected) {
        postDriverError(s->diag, "07002",
                        "COUNT field incorrect: statement has " + std::to_string(expected) +
                            " parameter marker(s), " + std::to_string(s->paramBindings.size()) + " bound");
        return std::nullopt;
    }

    std::vector<ifxsqli::BindParam> params;
    params.reserve(expected);
    for (std::size_t i = 1; i <= expected; ++i) {
        auto it = s->paramBindings.find(static_cast<SQLUSMALLINT>(i));
        if (it == s->paramBindings.end()) {
            postDriverError(s->diag, "07002", "parameter #" + std::to_string(i) + " was never bound");
            return std::nullopt;
        }
        const auto& binding = it->second;
        bool isNull = binding.strLenOrIndPtr && *binding.strLenOrIndPtr == SQL_NULL_DATA;
        if (isNull) {
            // SQ_BIND's ifxType field should be the target column's real
            // type, not a placeholder, per the decompiled JDBC reference's
            // sendBind() (see ifxTypeForParameterType's doc comment) - it
            // is never hardcoded to CHAR/type-0 for a NULL there. Leaving
            // this defaulted to 0 (kChar/CHAR) regardless of the bound
            // ParameterType matched the reference's shape less closely,
            // though it was never independently shown live to misbehave
            // (see ifxTypeForParameterType's doc comment for why: nothing
            // exercises SQLBindParameter through the ODBC surface yet).
            auto ifxType = ifxTypeForParameterType(binding.parameterType);
            if (!ifxType) {
                postDriverError(s->diag, "HYC00",
                                "SQLBindParameter: unsupported ParameterType for NULL parameter #" +
                                    std::to_string(i));
                return std::nullopt;
            }
            ifxsqli::BindParam p;
            p.ifxType = *ifxType;
            p.isNull = true;
            params.push_back(std::move(p));
            continue;
        }
        SQLLEN strLenOrInd = binding.strLenOrIndPtr ? *binding.strLenOrIndPtr : SQL_NTS;
        auto encoded = encodeBindParam(binding.valueType, binding.parameterType, binding.parameterValuePtr, strLenOrInd);
        if (encoded.status != BindEncodeStatus::kOk) {
            postDriverError(s->diag, "HYC00",
                            "SQLBindParameter: unsupported ValueType/ParameterType combination for parameter #" +
                                std::to_string(i));
            return std::nullopt;
        }
        params.push_back(std::move(encoded.param));
    }
    return params;
}

SQLRETURN doExecute(OdbcStatement* s) {
    if (s->state == StmtState::kUnprepared || !s->stmt) {
        postDriverError(s->diag, "HY010", "function sequence error: statement is not prepared");
        return SQL_ERROR;
    }
    // Lazily opens a transaction on the first statement since the last
    // commit/rollback/connect, matching ODBC's manual-commit contract: the
    // application never calls anything to *start* a transaction, only
    // SQLEndTran to end one - see driver/handles.h's transactionOpen doc
    // comment for the other three places this state gets resolved.
    if (s->conn->autoCommit == SQL_AUTOCOMMIT_OFF && !s->conn->transactionOpen) {
        auto begin = s->conn->sqli->beginTransaction();
        if (!begin) {
            postSqliError(s->diag, begin.error());
            return SQL_ERROR;
        }
        s->conn->transactionOpen = true;
    }
    auto params = buildBindParams(s);
    if (!params) return SQL_ERROR;
    auto result = s->stmt->execute(*params);
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
        case SQL_COLUMN_NAME:
        // SQL_DESC_LABEL is #defined as SQL_COLUMN_LABEL (same numeric value,
        // 18) on this SDK, so only one case label is listed here (a second
        // would be a duplicate-case error, same situation as
        // SQL_DESC_DISPLAY_SIZE/SQL_COLUMN_DISPLAY_SIZE above). This is the
        // display label a client uses for a column header (distinct from
        // SQL_DESC_NAME when a query uses an alias/expression - we don't
        // track aliases separately yet, so the base column name is a
        // correct answer either way). Added because isql's tabular header
        // printer was showing blank headers despite SQLDescribeCol/
        // SQL_DESC_NAME both returning the correct name when called
        // directly - the most likely explanation is isql asks for this
        // field instead, which previously fell through to the unsupported-
        // field default (SQL_ERROR/HYC00).
        case SQL_COLUMN_LABEL: {
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

// Phase 5: SQL_INTEGER/SQL_SMALLINT/SQL_BIGINT/SQL_CHAR "?" parameters are
// supported so far - see driver/bind_encode.h for what's still missing
// (VARCHAR/DATE/DECIMAL/DATETIME). Values aren't read here at all:
// SQLBindParameter only records where to read from later
// (ParameterValuePtr/StrLen_or_IndPtr), matching ODBC's late-binding
// semantics (the application may change the pointed-to value any number of
// times before SQLExecute actually reads it) - the same pattern
// SQLBindCol already uses for output columns. ColumnSize/DecimalDigits are
// still unused - CHAR's SQ_BIND framing is self-describing so this driver
// currently doesn't need them (see sqli/types.h's encodeChar), but VARCHAR
// will (see docs/protocol-notes.md's SQ_BIND notes).
IFXODBC_EXPORT SQLRETURN SQL_API SQLBindParameter(SQLHSTMT StatementHandle, SQLUSMALLINT ParameterNumber,
                                    SQLSMALLINT /*InputOutputType*/, SQLSMALLINT ValueType, SQLSMALLINT ParameterType,
                                    SQLULEN /*ColumnSize*/, SQLSMALLINT /*DecimalDigits*/, SQLPOINTER ParameterValuePtr,
                                    SQLLEN /*BufferLength*/, SQLLEN* StrLen_or_IndPtr) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();

    if (!ParameterValuePtr) {
        s->paramBindings.erase(ParameterNumber);
        return SQL_SUCCESS;
    }
    s->paramBindings[ParameterNumber] =
        ParamBinding{ValueType, ParameterType, ParameterValuePtr, 0, StrLen_or_IndPtr};
    return SQL_SUCCESS;
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
    if (TargetType != SQL_C_CHAR && TargetType != SQL_C_DEFAULT && !isNativeTargetType(TargetType)) {
        postDriverError(s->diag, "HYC00", "SQLBindCol: unsupported C target type");
        return SQL_ERROR;
    }
    s->boundColumns[ColumnNumber] = BoundColumn{TargetType, TargetValuePtr, BufferLength, StrLen_or_IndPtr};
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

    bool anyInfo = false;
    const auto& cols = s->stmt->columns();
    for (auto& [colNumber, binding] : s->boundColumns) {
        if (colNumber < 1 || colNumber > cols.size()) continue;
        SQLRETURN rc = writeColumnValue(s, cols[colNumber - 1], (*s->currentRow)[colNumber - 1], binding.targetType,
                                         binding.targetValuePtr, binding.bufferLength, binding.strLenOrIndPtr);
        if (rc == SQL_ERROR) return SQL_ERROR;
        if (rc == SQL_SUCCESS_WITH_INFO) anyInfo = true;
    }
    return anyInfo ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
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
    if (TargetType != SQL_C_CHAR && TargetType != SQL_C_DEFAULT && !isNativeTargetType(TargetType)) {
        postDriverError(s->diag, "HYC00", "SQLGetData: unsupported C target type");
        return SQL_ERROR;
    }
    const auto& cols = s->stmt->columns();
    if (ColumnNumber < 1 || ColumnNumber > cols.size()) {
        postDriverError(s->diag, "07009", "invalid column number");
        return SQL_ERROR;
    }

    return writeColumnValue(s, cols[ColumnNumber - 1], (*s->currentRow)[ColumnNumber - 1], TargetType, TargetValuePtr,
                             BufferLength, StrLen_or_IndPtr);
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
            s->paramBindings.clear();
            return SQL_SUCCESS;
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
    if (RowCountPtr) {
        // A statement with output columns is SELECT-shaped: SQ_DONE's row
        // count there is the cursor-open acknowledgement, not a real row
        // count, and ODBC leaves SQLRowCount's value implementation-defined
        // for SELECT anyway, so -1 ("unknown") is returned. For INSERT/
        // UPDATE/DELETE (no output columns), sqli/statement.cpp's
        // SqliStatement::rowCount() carries the real SQ_DONE value.
        *RowCountPtr = (s->stmt && s->stmt->columns().empty())
                           ? static_cast<SQLLEN>(s->stmt->rowCount())
                           : -1;
    }
    return SQL_SUCCESS;
}

} // extern "C"
