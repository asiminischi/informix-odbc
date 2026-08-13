#pragma once

// Encodes an application's bound "?" parameter value (SQLBindParameter's
// ValueType/ParameterType + a live pointer into the caller's buffer) into a
// sqli::BindParam ready for SQ_BIND. The mirror image of
// driver/convert_native.h's decode direction, but deliberately its own
// module rather than added there: convert_native.h's existing functions all
// take an already-fetched column's raw bytes and a ColumnDesc; encoding
// starts from an ODBC C value instead.
//
// All 8 MVP scalar types are done: SQL_INTEGER/SQL_SMALLINT/SQL_BIGINT/
// SQL_CHAR/SQL_VARCHAR/SQL_TYPE_DATE/SQL_DECIMAL/SQL_TYPE_TIMESTAMP. None
// of them need SQLBindParameter's ColumnSize/DecimalDigits - see
// docs/protocol-notes.md's SQ_BIND notes for why (DECIMAL's precisionField
// is the value's own scale, not any target column's declared scale).
// SQL_TYPE_TIMESTAMP is scoped to whole-second YEAR TO SECOND only
// (hardcoded, not derived from the target column's actual qualifier - the
// only one confirmed live); FRACTION qualifiers are rejected rather than
// silently truncated.
//
// INTERVAL (SQL_INTERVAL_YEAR .. SQL_INTERVAL_MINUTE_TO_SECOND, both
// classes) is the one type genuinely needing ColumnSize/DecimalDigits:
// unlike every type above, INTERVAL's leading field width is a per-column,
// user-declared value (1-9 digits) that can't be recovered from the bind
// value's magnitude alone - see intervalEncodedLengthFor's doc comment for
// how ParameterType + ColumnSize + DecimalDigits together reconstruct the
// wire qualifier. Value comes via the native SQL_INTERVAL_STRUCT
// (SQL_C_INTERVAL_*), not a text literal like DECIMAL - unlike DECIMAL's
// "12345.67" (which happens to already be the ODBC C-to-SQL text
// representation for that type), this driver's own interval display text
// (formatIntervalYearMonth/DayFraction) is a dbaccess-mimicking *output*
// convention it built itself, not an externally-defined grammar - so
// parsing that same text back as input would only prove self-consistency,
// not interoperability with a real ODBC caller. SQL_INTERVAL_STRUCT's
// layout is externally defined (sqltypes.h), so encoding/decoding it is
// meaningfully verifiable instead.

#include "sqli/statement.h"

#include <cstdint>
#include <optional>

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
// ignore it entirely. `columnSize`/`decimalDigits` are SQLBindParameter's
// own same-named arguments, needed only by INTERVAL (see this header's top
// comment) - every other type ignores them.
[[nodiscard]] BindEncodeResult encodeBindParam(SQLSMALLINT valueType, SQLSMALLINT parameterType,
                                                SQLPOINTER parameterValuePtr, SQLLEN strLenOrInd,
                                                SQLULEN columnSize, SQLSMALLINT decimalDigits);

// Reconstructs the wire qualifier (the same packed `(totalDigits << 8) |
// qualifierByte` shape `sqli::encodeIntervalYearMonth`/
// `encodeIntervalDayFraction` take) from SQLBindParameter's ParameterType/
// ColumnSize/DecimalDigits triple - the ODBC-standard carriers for an
// interval parameter's leading precision and fractional-seconds precision
// respectively (this driver has no target-column metadata to consult at
// bind time, the same limitation as every other qualified bind type).
// Pure, no-server function - independently unit-tested against three
// qualifier values already confirmed live via driver/catalog.cpp's
// SQLColumns work (`tests/driver/test_bind_encode.cpp`), since this is the
// one piece of new logic in the INTERVAL bind path that doesn't reduce to
// an already-live-verified encoder. `parameterType` must be one of the 13
// `SQL_INTERVAL_*` concise type constants (matching
// `driver/catalog.cpp`'s own `IntervalShapeEntry` shapes, kept as an
// independent copy per that file's own stated precedent for small,
// per-file mappings); `columnSize` must be 1-9 (Informix's own leading-
// field digit-width limit). Returns nullopt for either violation.
[[nodiscard]] std::optional<std::uint32_t> intervalEncodedLengthFor(SQLSMALLINT parameterType, SQLULEN columnSize,
                                                                     SQLSMALLINT decimalDigits);

// Maps SQLBindParameter's ParameterType to the Informix wire type SQ_BIND
// needs, independent of any value - needed for NULL binds, which carry no
// value to dispatch encodeBindParam on but still must advertise the real
// target type. The decompiled JDBC reference's sendBind() always sends the
// bound value's actual ifxType even when isNull() (see IfxSqli.java: the
// `ival.isNull()` branch reads `type` from the same `ival.getIfxType()`
// call every other branch uses) - it is never hardcoded to CHAR/type-0 for
// a NULL, so this brings the driver in line with the reference. Not
// independently confirmed live that a type-0 NULL actually misbehaves
// against this server - no integration test drives SQLBindParameter
// through the ODBC surface yet, only sqli::SqliStatement::execute()
// directly with hand-built BindParams. Returns nullopt for a
// ParameterType this driver doesn't support at all, same as
// encodeBindParam's kUnsupported.
[[nodiscard]] std::optional<int> ifxTypeForParameterType(SQLSMALLINT parameterType);

} // namespace ifxodbc
