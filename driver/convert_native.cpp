#include "driver/convert_native.h"

#include "sqli/types.h"

#include <charconv>
#include <chrono>
#include <limits>

namespace ifxodbc {

namespace {

using namespace ifxsqli;

bool isIntegerFamily(int ifxType) {
    switch (ifxType) {
        case ifxtype::kSmallInt:
        case ifxtype::kInteger:
        case ifxtype::kSerial:
        case ifxtype::kBigInt:
        case ifxtype::kBigSerial:
            return true;
        default:
            return false;
    }
}

// Widens any MVP integer-family column to int64_t so the C-type writers
// below share one overflow-checked narrowing path instead of one per
// (source type, target type) pair. NULL is nullopt, matching the
// sqli/types.h decoders' own convention. Caller must check isIntegerFamily
// first - this only decodes, it doesn't classify (an unsupported source
// type is a driver-level "no such conversion" outcome, not a protocol
// decode error, so it's not reported via this function's Error channel).
std::expected<std::optional<std::int64_t>, Error> decodeAsInt64(const ColumnDesc& col, std::span<const std::byte> raw) {
    switch (col.ifxType) {
        case ifxtype::kSmallInt: {
            auto v = decodeSmallInt(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return std::optional<std::int64_t>{};
            return std::optional<std::int64_t>(**v);
        }
        case ifxtype::kInteger:
        case ifxtype::kSerial: {
            auto v = decodeInteger(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return std::optional<std::int64_t>{};
            return std::optional<std::int64_t>(**v);
        }
        default: { // kBigInt / kBigSerial
            auto v = decodeBigInt(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return std::optional<std::int64_t>{};
            return std::optional<std::int64_t>(**v);
        }
    }
}

template <class T>
NativeConvertStatus writeIntegral(std::int64_t v, SQLPOINTER out, SQLLEN* indPtr) {
    // Parenthesized (std::numeric_limits<T>::min)()/max() to defeat
    // windows.h's own min/max macros (this file has no NOMINMAX guard,
    // matching driver/handles.h's existing windows.h include) - without
    // this, "std::numeric_limits<T>::min()" expands as a macro call and
    // fails to compile.
    if (v < static_cast<std::int64_t>((std::numeric_limits<T>::min)()) ||
        v > static_cast<std::int64_t>((std::numeric_limits<T>::max)())) {
        return NativeConvertStatus::kOverflow;
    }
    if (out) *static_cast<T*>(out) = static_cast<T>(v);
    if (indPtr) *indPtr = sizeof(T);
    return NativeConvertStatus::kOk;
}

NativeConvertStatus writeDouble(double v, SQLPOINTER out, SQLLEN* indPtr) {
    if (out) *static_cast<SQLDOUBLE*>(out) = v;
    if (indPtr) *indPtr = sizeof(SQLDOUBLE);
    return NativeConvertStatus::kOk;
}

SQL_DATE_STRUCT dateStructFromUnixDays(std::int32_t unixDays) {
    using namespace std::chrono;
    sys_days days{std::chrono::days{unixDays}};
    year_month_day ymd{days};
    SQL_DATE_STRUCT ds{};
    ds.year = static_cast<SQLSMALLINT>(static_cast<int>(ymd.year()));
    ds.month = static_cast<SQLUSMALLINT>(static_cast<unsigned>(ymd.month()));
    ds.day = static_cast<SQLUSMALLINT>(static_cast<unsigned>(ymd.day()));
    return ds;
}

// FRACTION qualifiers aren't supported yet (see decodeDatetime's doc
// comment), so `fraction` is always 0 here - correct for the confirmed-live
// YEAR TO SECOND case, a known gap for anything finer.
SQL_TIMESTAMP_STRUCT timestampStructFromDatetime(const DatetimeValue& dt) {
    SQL_TIMESTAMP_STRUCT ts{};
    ts.year = static_cast<SQLSMALLINT>(dt.year);
    ts.month = static_cast<SQLUSMALLINT>(dt.month);
    ts.day = static_cast<SQLUSMALLINT>(dt.day);
    ts.hour = static_cast<SQLUSMALLINT>(dt.hour);
    ts.minute = static_cast<SQLUSMALLINT>(dt.minute);
    ts.second = static_cast<SQLUSMALLINT>(dt.second);
    ts.fraction = 0;
    return ts;
}

} // namespace

bool isNativeTargetType(SQLSMALLINT targetType) {
    switch (targetType) {
        case SQL_C_SSHORT:
        case SQL_C_SHORT:
        case SQL_C_SLONG:
        case SQL_C_LONG:
        case SQL_C_SBIGINT:
        case SQL_C_DOUBLE:
        case SQL_C_TYPE_DATE:
        case SQL_C_DATE:
        case SQL_C_TYPE_TIMESTAMP:
        case SQL_C_TIMESTAMP:
            return true;
        default:
            return false;
    }
}

std::expected<NativeConvertResult, Error> writeNativeCell(const ColumnDesc& col, std::span<const std::byte> raw,
                                                           SQLSMALLINT targetType, SQLPOINTER targetValuePtr,
                                                           SQLLEN /*bufferLength*/, SQLLEN* strLenOrIndPtr) {
    switch (targetType) {
        case SQL_C_SSHORT:
        case SQL_C_SHORT:
        case SQL_C_SLONG:
        case SQL_C_LONG:
        case SQL_C_SBIGINT: {
            if (!isIntegerFamily(col.ifxType)) return NativeConvertResult{NativeConvertStatus::kUnsupported};
            auto v = decodeAsInt64(col, raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) {
                if (strLenOrIndPtr) *strLenOrIndPtr = SQL_NULL_DATA;
                return NativeConvertResult{NativeConvertStatus::kNull};
            }
            NativeConvertStatus status;
            switch (targetType) {
                case SQL_C_SSHORT:
                case SQL_C_SHORT:
                    status = writeIntegral<SQLSMALLINT>(**v, targetValuePtr, strLenOrIndPtr);
                    break;
                case SQL_C_SLONG:
                case SQL_C_LONG:
                    status = writeIntegral<SQLINTEGER>(**v, targetValuePtr, strLenOrIndPtr);
                    break;
                default: // SQL_C_SBIGINT
                    status = writeIntegral<SQLBIGINT>(**v, targetValuePtr, strLenOrIndPtr);
                    break;
            }
            return NativeConvertResult{status};
        }
        case SQL_C_DOUBLE: {
            if (col.ifxType == ifxtype::kDecimal) {
                unsigned precision = (col.encodedLength >> 8) & 0xFF;
                unsigned scale = col.encodedLength & 0xFF;
                auto v = decodeDecimal(raw, precision, scale);
                if (!v) return std::unexpected(v.error());
                if (!*v) {
                    if (strLenOrIndPtr) *strLenOrIndPtr = SQL_NULL_DATA;
                    return NativeConvertResult{NativeConvertStatus::kNull};
                }
                // Exact-decimal-string -> double is inherently an
                // approximation (the whole reason decodeDecimal hands back a
                // string instead of a float) - acceptable for SQL_C_DOUBLE
                // specifically, since that's what the caller asked for.
                // std::from_chars (not strtod/atof) deliberately - this
                // driver can be loaded into any host process, and strtod's
                // decimal-point character follows the process-wide
                // LC_NUMERIC locale (a process that set a comma-decimal
                // locale would silently truncate "12345.67" at the dot);
                // from_chars is always locale-independent.
                double parsed = 0.0;
                std::from_chars((*v)->data(), (*v)->data() + (*v)->size(), parsed);
                return NativeConvertResult{writeDouble(parsed, targetValuePtr, strLenOrIndPtr)};
            }
            if (!isIntegerFamily(col.ifxType)) return NativeConvertResult{NativeConvertStatus::kUnsupported};
            auto v = decodeAsInt64(col, raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) {
                if (strLenOrIndPtr) *strLenOrIndPtr = SQL_NULL_DATA;
                return NativeConvertResult{NativeConvertStatus::kNull};
            }
            return NativeConvertResult{writeDouble(static_cast<double>(**v), targetValuePtr, strLenOrIndPtr)};
        }
        case SQL_C_TYPE_DATE:
        case SQL_C_DATE: {
            if (col.ifxType != ifxtype::kDate) return NativeConvertResult{NativeConvertStatus::kUnsupported};
            auto v = decodeDateUnixDays(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) {
                if (strLenOrIndPtr) *strLenOrIndPtr = SQL_NULL_DATA;
                return NativeConvertResult{NativeConvertStatus::kNull};
            }
            if (targetValuePtr) *static_cast<SQL_DATE_STRUCT*>(targetValuePtr) = dateStructFromUnixDays(**v);
            if (strLenOrIndPtr) *strLenOrIndPtr = sizeof(SQL_DATE_STRUCT);
            return NativeConvertResult{NativeConvertStatus::kOk};
        }
        case SQL_C_TYPE_TIMESTAMP:
        case SQL_C_TIMESTAMP: {
            if (col.ifxType != ifxtype::kDatetime) return NativeConvertResult{NativeConvertStatus::kUnsupported};
            auto v = decodeDatetime(raw, col.encodedLength);
            if (!v) return std::unexpected(v.error());
            if (!*v) {
                if (strLenOrIndPtr) *strLenOrIndPtr = SQL_NULL_DATA;
                return NativeConvertResult{NativeConvertStatus::kNull};
            }
            if (targetValuePtr) *static_cast<SQL_TIMESTAMP_STRUCT*>(targetValuePtr) = timestampStructFromDatetime(**v);
            if (strLenOrIndPtr) *strLenOrIndPtr = sizeof(SQL_TIMESTAMP_STRUCT);
            return NativeConvertResult{NativeConvertStatus::kOk};
        }
        default:
            return NativeConvertResult{NativeConvertStatus::kUnsupported};
    }
}

} // namespace ifxodbc
