#pragma once

// Phase 4 slice 3: native ODBC C-type conversions (SQL_C_SSHORT/SQL_C_SLONG/
// SQL_C_SBIGINT/SQL_C_DOUBLE for the numeric types, SQL_DATE_STRUCT for DATE,
// SQL_TIMESTAMP_STRUCT for DATETIME). SQL_C_CHAR stays in driver/convert.h/
// .cpp (formatCellAsText) - every MVP type already round-trips through text,
// and this module only needs to handle the additional native shapes on top.

#include "sqli/error.h"
#include "sqli/statement.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

#include <cstddef>
#include <expected>
#include <span>

namespace ifxodbc {

enum class NativeConvertStatus {
    kOk,
    kNull,
    kOverflow,     // source value doesn't fit in the requested C type - maps to 22003
    kUnsupported,  // this (source Informix type, target C type) pairing isn't implemented - maps to HYC00
};

struct NativeConvertResult {
    NativeConvertStatus status = NativeConvertStatus::kOk;
};

// Returns true if targetType is one of the native (non-SQL_C_CHAR/SQL_C_
// DEFAULT) types this module handles - lets callers route a binding/GetData
// request to this module vs. the SQL_C_CHAR path in driver/convert.cpp
// without duplicating the SQL_C_* constant list at each call site.
[[nodiscard]] bool isNativeTargetType(SQLSMALLINT targetType);

// Decodes `raw` per col.ifxType and writes it as `targetType` into
// targetValuePtr/bufferLength, setting *strLenOrIndPtr to the C type's byte
// size (or SQL_NULL_DATA). The outer expected is a protocol-level decode
// error (malformed bytes); NativeConvertResult::status carries the
// driver-level outcome (null/overflow/unsupported) for the caller to map to
// the right SQLRETURN/SQLSTATE, mirroring how driver/statement.cpp already
// handles formatCellAsText's errors vs. writeCharResult's truncation bool.
[[nodiscard]] std::expected<NativeConvertResult, ifxsqli::Error> writeNativeCell(
    const ifxsqli::ColumnDesc& col, std::span<const std::byte> raw, SQLSMALLINT targetType,
    SQLPOINTER targetValuePtr, SQLLEN bufferLength, SQLLEN* strLenOrIndPtr);

} // namespace ifxodbc
