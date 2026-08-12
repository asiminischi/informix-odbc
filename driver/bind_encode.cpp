#include "driver/bind_encode.h"

#include "sqli/types.h"

#include <chrono>
#include <cstring>
#include <string_view>

namespace ifxodbc {

namespace {

// Inverse of driver/convert_native.cpp's dateStructFromUnixDays - kept as
// its own copy rather than shared, since that one lives in the decode
// direction's translation unit and this module deliberately doesn't
// depend on it (see bind_encode.h's top comment).
std::int32_t unixDaysFromDateStruct(const SQL_DATE_STRUCT& ds) {
    using namespace std::chrono;
    year_month_day ymd{year{ds.year}, month{static_cast<unsigned>(ds.month)}, day{static_cast<unsigned>(ds.day)}};
    return static_cast<std::int32_t>(sys_days{ymd}.time_since_epoch().count());
}

// Shared by every SQL_C_CHAR-based bind (CHAR/VARCHAR/DECIMAL): resolves
// the actual string length per ODBC's StrLen_or_IndPtr convention (SQL_NTS
// means NUL-terminated, otherwise it's the exact byte count).
std::string_view charBindValue(SQLPOINTER parameterValuePtr, SQLLEN strLenOrInd) {
    const auto* text = static_cast<const char*>(parameterValuePtr);
    return strLenOrInd == SQL_NTS ? std::string_view(text) : std::string_view(text, static_cast<std::size_t>(strLenOrInd));
}

} // namespace

BindEncodeResult encodeBindParam(SQLSMALLINT valueType, SQLSMALLINT parameterType, SQLPOINTER parameterValuePtr,
                                  SQLLEN strLenOrInd) {
    if (parameterType == SQL_INTEGER && (valueType == SQL_C_SLONG || valueType == SQL_C_LONG ||
                                          valueType == SQL_C_DEFAULT)) {
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kInteger;
        p.isNull = false;
        p.data = ifxsqli::encodeInteger(*static_cast<SQLINTEGER*>(parameterValuePtr));
        p.precisionField = ifxsqli::kIntegerBindPrecision;
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    if (parameterType == SQL_SMALLINT && (valueType == SQL_C_SSHORT || valueType == SQL_C_SHORT)) {
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kSmallInt;
        p.isNull = false;
        p.data = ifxsqli::encodeSmallInt(*static_cast<SQLSMALLINT*>(parameterValuePtr));
        p.precisionField = ifxsqli::kSmallIntBindPrecision;
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    if (parameterType == SQL_BIGINT && (valueType == SQL_C_SBIGINT || valueType == SQL_C_DEFAULT)) {
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kBigInt;
        p.isNull = false;
        p.data = ifxsqli::encodeBigInt(*static_cast<SQLBIGINT*>(parameterValuePtr));
        p.precisionField = ifxsqli::kBigIntBindPrecision;
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    if (parameterType == SQL_DOUBLE && (valueType == SQL_C_DOUBLE || valueType == SQL_C_DEFAULT)) {
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kFloat;
        p.isNull = false;
        p.data = ifxsqli::encodeFloat(*static_cast<SQLDOUBLE*>(parameterValuePtr));
        p.precisionField = ifxsqli::kFloatBindPrecision;
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    // SQL_C_FLOAT isn't in isNativeTargetType's gate (statement.cpp's
    // SQLBindParameter never reaches here with it), so SMALLFLOAT binds
    // via the already-permitted wider SQL_C_DOUBLE, narrowed here - a
    // standard ODBC pattern (drivers commonly accept a wider C type bound
    // to a narrower SQL type) that avoids touching the shared native-type
    // gate and its fetch-direction implications.
    if (parameterType == SQL_REAL && (valueType == SQL_C_DOUBLE || valueType == SQL_C_DEFAULT)) {
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kSmallFloat;
        p.isNull = false;
        p.data = ifxsqli::encodeSmallFloat(static_cast<float>(*static_cast<SQLDOUBLE*>(parameterValuePtr)));
        p.precisionField = ifxsqli::kSmallFloatBindPrecision;
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    if ((parameterType == SQL_CHAR || parameterType == SQL_VARCHAR) &&
        (valueType == SQL_C_CHAR || valueType == SQL_C_DEFAULT)) {
        std::string_view s = charBindValue(parameterValuePtr, strLenOrInd);
        ifxsqli::BindParam p;
        p.ifxType = parameterType == SQL_CHAR ? ifxsqli::ifxtype::kChar : ifxsqli::ifxtype::kVarchar;
        p.isNull = false;
        p.data = ifxsqli::encodeChar(s);
        p.precisionField = static_cast<std::uint16_t>(s.size());
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    if (parameterType == SQL_TYPE_DATE && (valueType == SQL_C_TYPE_DATE || valueType == SQL_C_DATE)) {
        auto unixDays = unixDaysFromDateStruct(*static_cast<SQL_DATE_STRUCT*>(parameterValuePtr));
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kDate;
        p.isNull = false;
        p.data = ifxsqli::encodeDateUnixDays(unixDays);
        // precisionField's correct value for DATE is untraced (unlike
        // INTEGER/SMALLINT/BIGINT's confirmed hardcoded constants) - 4 (the
        // byte width) is a first hypothesis pending a live check, same
        // approach that worked for CHAR/VARCHAR.
        p.precisionField = 4;
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    if ((parameterType == SQL_DECIMAL || parameterType == SQL_NUMERIC) &&
        (valueType == SQL_C_CHAR || valueType == SQL_C_DEFAULT)) {
        // No SQL_NUMERIC_STRUCT support - DECIMAL binds go through an
        // exact decimal-literal string instead, same convention this
        // driver's fetch direction already uses for exact DECIMAL access
        // (see convert_native.cpp's SQL_C_DOUBLE/decodeDecimal handling
        // and sqli/types.h's encodeDecimal doc comment for why a string is
        // the only representation that doesn't lose precision).
        std::string_view s = charBindValue(parameterValuePtr, strLenOrInd);
        auto encoded = ifxsqli::encodeDecimal(s);
        if (!encoded) return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kDecimal;
        p.isNull = false;
        p.data = std::move(encoded->data);
        p.precisionField = encoded->scale;
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    if (parameterType == SQL_TYPE_TIMESTAMP && (valueType == SQL_C_TYPE_TIMESTAMP || valueType == SQL_C_TIMESTAMP)) {
        const auto& ts = *static_cast<SQL_TIMESTAMP_STRUCT*>(parameterValuePtr);
        if (ts.fraction != 0) {
            // FRACTION qualifiers aren't supported yet - see
            // sqli/types.h's decodeDatetime/encodeDatetime doc comments.
            // Fail loudly rather than silently truncating a value the
            // caller explicitly set.
            return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
        }
        ifxsqli::DatetimeValue value{static_cast<int>(ts.year),  static_cast<int>(ts.month), static_cast<int>(ts.day),
                                      static_cast<int>(ts.hour), static_cast<int>(ts.minute), static_cast<int>(ts.second)};
        // YEAR TO SECOND is hardcoded, not derived from the target
        // column's actual declared qualifier - the only qualifier
        // confirmed live so far (see docs/protocol-notes.md's SQ_BIND
        // notes), and this driver has no column metadata to consult at
        // bind time regardless (same limitation as DATE/CHAR/VARCHAR).
        constexpr std::uint32_t kYearToSecond = (14u << 8) | 0x0A;
        auto encoded = ifxsqli::encodeDatetime(value, kYearToSecond);
        if (!encoded) return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kDatetime;
        p.isNull = false;
        p.data = std::move(*encoded);
        p.precisionField = static_cast<std::uint16_t>(kYearToSecond);
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
}

std::optional<int> ifxTypeForParameterType(SQLSMALLINT parameterType) {
    switch (parameterType) {
        case SQL_INTEGER:
            return ifxsqli::ifxtype::kInteger;
        case SQL_SMALLINT:
            return ifxsqli::ifxtype::kSmallInt;
        case SQL_BIGINT:
            return ifxsqli::ifxtype::kBigInt;
        case SQL_DOUBLE:
            return ifxsqli::ifxtype::kFloat;
        case SQL_REAL:
            return ifxsqli::ifxtype::kSmallFloat;
        case SQL_CHAR:
            return ifxsqli::ifxtype::kChar;
        case SQL_VARCHAR:
            return ifxsqli::ifxtype::kVarchar;
        case SQL_TYPE_DATE:
            return ifxsqli::ifxtype::kDate;
        case SQL_DECIMAL:
        case SQL_NUMERIC:
            return ifxsqli::ifxtype::kDecimal;
        case SQL_TYPE_TIMESTAMP:
            return ifxsqli::ifxtype::kDatetime;
        default:
            return std::nullopt;
    }
}

} // namespace ifxodbc
