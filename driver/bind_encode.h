#pragma once

// Phase 5, first slice: encodes an application's bound "?" parameter value
// (SQLBindParameter's ValueType/ParameterType + a live pointer into the
// caller's buffer) into a sqli::BindParam ready for SQ_BIND. The mirror
// image of driver/convert_native.h's decode direction, but deliberately its
// own module rather than added there: convert_native.h existing functions
// all take an already-fetched column's raw bytes and a ColumnDesc; encoding
// starts from an ODBC C value and has no column metadata to consult at all
// (a "?" marker's Informix-side type comes from SQLBindParameter's
// ParameterType argument, not from any DESCRIBE response).
//
// Scope for this first slice: SQL_C_SLONG/SQL_C_DEFAULT -> SQL_INTEGER only
// (see the plan) - proving the SQ_BIND wire plumbing end-to-end before
// widening to the rest of the MVP scalar types.

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
[[nodiscard]] BindEncodeResult encodeBindParam(SQLSMALLINT valueType, SQLSMALLINT parameterType,
                                                SQLPOINTER parameterValuePtr);

} // namespace ifxodbc
