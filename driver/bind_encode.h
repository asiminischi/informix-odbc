#pragma once

// Encodes an application's bound "?" parameter value (SQLBindParameter's
// ValueType/ParameterType + a live pointer into the caller's buffer) into a
// sqli::BindParam ready for SQ_BIND. The mirror image of
// driver/convert_native.h's decode direction, but deliberately its own
// module rather than added there: convert_native.h's existing functions all
// take an already-fetched column's raw bytes and a ColumnDesc; encoding
// starts from an ODBC C value instead.
//
// SQL_INTEGER/SQL_SMALLINT/SQL_BIGINT/SQL_CHAR/SQL_VARCHAR/SQL_TYPE_DATE/
// SQL_DECIMAL are done - none of them need SQLBindParameter's ColumnSize/
// DecimalDigits (still discarded), see docs/protocol-notes.md's SQ_BIND
// notes for why (DECIMAL's precisionField is the value's own scale, not
// any target column's declared scale). Still missing: DATETIME.

#include "sqli/statement.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

namespace ifxodbc {

enum class BindEncodeStatus {
    kOk,
    kUnsupported,  // this (ValueType, ParameterType) pairing isn't implemented yet - maps to HY004/HYC00
};

struct BindEncodeResult {
    BindEncodeStatus status = BindEncodeStatus::kOk;
    ifxsqli::BindParam param;
};

// Encodes one non-NULL parameter value. Callers are responsible for the
// NULL case (SQL_NULL_DATA in *StrLen_or_IndPtr) themselves, same division
// of responsibility as driver/convert_native.h's writeNativeCell/caller
// split - encoding a NULL needs no type-specific logic at all (SQ_BIND
// signals it via the indicator field alone).
//
// `strLenOrInd` is SQLBindParameter's StrLen_or_IndPtr value at execute
// time, already confirmed non-NULL-indicator by the caller - only
// meaningful for variable-length ValueTypes (SQL_C_CHAR today): SQL_NTS
// means "read a NUL-terminated string from parameterValuePtr", any other
// non-negative value is the exact byte count to read. Fixed-width types
// ignore it entirely.
[[nodiscard]] BindEncodeResult encodeBindParam(SQLSMALLINT valueType, SQLSMALLINT parameterType,
                                                SQLPOINTER parameterValuePtr, SQLLEN strLenOrInd);

} // namespace ifxodbc
