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

// Mirrors the decompiled IfxBoolean.fromString's accepted spellings
// ("t"/"true"/"f"/"false", case-insensitive) - the same convention this
// driver's own fetch side already surfaces BOOLEAN as via formatCellAsText
// ("1"/"0"), extended here to accept what a human or Informix tooling
// would actually type. std::nullopt means unrecognized text.
std::optional<bool> boolFromText(std::string_view s) {
    if (s == "1" || s == "t" || s == "T" || s == "true" || s == "TRUE") return true;
    if (s == "0" || s == "f" || s == "F" || s == "false" || s == "FALSE") return false;
    return std::nullopt;
}

// INTERVAL's SQL_INTERVAL_* concise ParameterType -> qualifier start/end
// codes - an independent copy of driver/catalog.cpp's IntervalShapeEntry
// table, kept separate per that file's own stated precedent for small,
// stable per-file mappings rather than threaded through a shared header.
// `end` here is the whole-field end (0/2/4/6/8/10, never a FRACTION code -
// DecimalDigits supplies the fraction separately, see
// intervalEncodedLengthFor).
struct IntervalParamShape {
    SQLSMALLINT parameterType;
    unsigned start;
    unsigned end;
};

constexpr IntervalParamShape kIntervalParamShapes[] = {
    {SQL_INTERVAL_YEAR, 0, 0},
    {SQL_INTERVAL_YEAR_TO_MONTH, 0, 2},
    {SQL_INTERVAL_MONTH, 2, 2},
    {SQL_INTERVAL_DAY, 4, 4},
    {SQL_INTERVAL_DAY_TO_HOUR, 4, 6},
    {SQL_INTERVAL_DAY_TO_MINUTE, 4, 8},
    {SQL_INTERVAL_DAY_TO_SECOND, 4, 10},
    {SQL_INTERVAL_HOUR, 6, 6},
    {SQL_INTERVAL_HOUR_TO_MINUTE, 6, 8},
    {SQL_INTERVAL_HOUR_TO_SECOND, 6, 10},
    {SQL_INTERVAL_MINUTE, 8, 8},
    {SQL_INTERVAL_MINUTE_TO_SECOND, 8, 10},
    {SQL_INTERVAL_SECOND, 10, 10},
};

std::optional<IntervalParamShape> findIntervalShape(SQLSMALLINT parameterType) {
    for (const auto& e : kIntervalParamShapes) {
        if (e.parameterType == parameterType) return e;
    }
    return std::nullopt;
}

// 10^n, only ever called with n in [0, 9] (nanosecond-to-FRACTION-digit
// scaling below) - a loop is simpler than pulling in <cmath> for this.
std::uint32_t pow10(unsigned n) {
    std::uint32_t r = 1;
    for (unsigned i = 0; i < n; ++i) r *= 10;
    return r;
}

} // namespace

std::optional<std::uint32_t> intervalEncodedLengthFor(SQLSMALLINT parameterType, SQLULEN columnSize,
                                                        SQLSMALLINT decimalDigits) {
    auto shape = findIntervalShape(parameterType);
    if (!shape) return std::nullopt;
    if (columnSize < 1 || columnSize > 9) return std::nullopt;
    auto leadingWidth = static_cast<unsigned>(columnSize);

    // FRACTION only exists for the DAY-FRACTION class's *_TO_SECOND
    // shapes (SQL_INTERVAL_SECOND included - a bare SECOND(p) TO
    // FRACTION(n) qualifier is legal, same as YEAR TO FRACTION(5) is for
    // DATETIME) - DecimalDigits is silently ignored for every other
    // shape, matching this driver's existing lenient-encode conventions
    // elsewhere (e.g. DATE's unvalidated precisionField).
    // Informix caps FRACTION at 5 digits (same limit DATETIME's FRACTION
    // has) - past that, (10 + fractionDigits) would overflow the
    // qualifier byte's low nibble and silently corrupt the wire qualifier,
    // so this is a real boundary check on a real ODBC entry point, not
    // defensive padding for an unreachable case.
    if (shape->end == 10 && decimalDigits > 5) return std::nullopt;
    unsigned fractionDigits = (shape->end == 10 && decimalDigits > 0) ? static_cast<unsigned>(decimalDigits) : 0;
    unsigned qualifierEnd = fractionDigits > 0 ? (10 + fractionDigits) : shape->end;
    unsigned totalDigits = leadingWidth + (shape->end - shape->start) + fractionDigits;
    unsigned qualifierByte = (shape->start << 4) | qualifierEnd;
    return static_cast<std::uint32_t>((totalDigits << 8) | qualifierByte);
}

BindEncodeResult encodeBindParam(SQLSMALLINT valueType, SQLSMALLINT parameterType, SQLPOINTER parameterValuePtr,
                                  SQLLEN strLenOrInd, SQLULEN columnSize, SQLSMALLINT decimalDigits) {
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
    // Text-based like CHAR/VARCHAR/DECIMAL above, not a native SQL_C_BIT
    // path - mirrors this driver's own fetch-side "1"/"0" text convention
    // for BOOLEAN (formatCellAsText) rather than adding yet another type
    // to the shared native-type gate (see the SQL_REAL comment above).
    if (parameterType == SQL_BIT && (valueType == SQL_C_CHAR || valueType == SQL_C_DEFAULT)) {
        std::string_view s = charBindValue(parameterValuePtr, strLenOrInd);
        auto b = boolFromText(s);
        if (!b) return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kBooleanBindType;
        p.isNull = false;
        p.data = ifxsqli::encodeBoolean(*b);
        p.precisionField = ifxsqli::kBooleanBindPrecision;
        p.extendedOwnerName = "informix";
        p.extendedTypeName = "boolean";
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
    // Simple LOB (BYTE/TEXT): unlike CHAR/VARCHAR above, `data` here is a
    // 56-byte SQ_BIND placeholder descriptor (sqli::encodeSimpleLobPlaceholder),
    // not the value - the real content goes in BindParam::simpleLobContent,
    // streamed out-of-band by sqli::writeBindBlock's SQ_BBIND(41) chunk
    // stream (see sqli/statement.cpp). precisionField is 0, matching the
    // sqli-layer finding that IfxBlob never overrides IfxObject's zero
    // default (see sqli/types.h's encodeSimpleLobPlaceholder doc comment).
    // BYTE takes SQL_C_BINARY (raw bytes, length from strLenOrInd - never
    // SQL_NTS, binary data isn't NUL-terminated); TEXT reuses
    // charBindValue's SQL_C_CHAR handling, same as CHAR/VARCHAR above.
    if (parameterType == SQL_LONGVARBINARY && (valueType == SQL_C_BINARY || valueType == SQL_C_DEFAULT)) {
        if (strLenOrInd < 0) return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
        const auto* bytes = static_cast<const std::byte*>(parameterValuePtr);
        std::vector<std::byte> content(bytes, bytes + strLenOrInd);
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kByte;
        p.isNull = false;
        p.data = ifxsqli::encodeSimpleLobPlaceholder(static_cast<std::uint32_t>(content.size()));
        p.precisionField = 0;
        p.simpleLobContent = std::move(content);
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    if (parameterType == SQL_LONGVARCHAR && (valueType == SQL_C_CHAR || valueType == SQL_C_DEFAULT)) {
        std::string_view s = charBindValue(parameterValuePtr, strLenOrInd);
        std::vector<std::byte> content(s.size());
        std::memcpy(content.data(), s.data(), s.size());
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kText;
        p.isNull = false;
        p.data = ifxsqli::encodeSimpleLobPlaceholder(static_cast<std::uint32_t>(content.size()));
        p.precisionField = 0;
        p.simpleLobContent = std::move(content);
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
        ifxsqli::DatetimeValue value{static_cast<int>(ts.year),  static_cast<int>(ts.month), static_cast<int>(ts.day),
                                      static_cast<int>(ts.hour), static_cast<int>(ts.minute), static_cast<int>(ts.second)};
        // The qualifier is hardcoded, not derived from the target column's
        // actual declared qualifier - this driver has no column metadata
        // to consult at bind time regardless (same limitation as
        // DATE/CHAR/VARCHAR). YEAR TO SECOND when there's no sub-second
        // part, otherwise YEAR TO FRACTION(5) - Informix DATETIME's finest
        // precision (ten-microsecond resolution). ts.fraction is
        // nanoseconds (SQL_TIMESTAMP_STRUCT's unit); scaling down by 10^4
        // truncates, rather than rounds, any sub-ten-microsecond precision
        // the wire format has no room for.
        std::uint32_t qualifier;
        if (ts.fraction != 0) {
            value.fraction = static_cast<int>(ts.fraction / 10000);
            value.fractionDigits = 5;
            qualifier = (19u << 8) | 0x0F;
        } else {
            qualifier = (14u << 8) | 0x0A;
        }
        auto encoded = ifxsqli::encodeDatetime(value, qualifier);
        if (!encoded) return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kDatetime;
        p.isNull = false;
        p.data = std::move(*encoded);
        p.precisionField = static_cast<std::uint16_t>(qualifier);
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    // SQL_C_INTERVAL_* and SQL_INTERVAL_* are literally the same macro
    // values (sqlext.h #defines the C type constants directly as the SQL
    // type constants) - an interval C type has no "generic wider type"
    // the way integers do, so ValueType is expected to exactly match
    // ParameterType (or SQL_C_DEFAULT).
    if (auto shape = findIntervalShape(parameterType); shape && (valueType == SQL_C_DEFAULT || valueType == parameterType)) {
        auto encodedLength = intervalEncodedLengthFor(parameterType, columnSize, decimalDigits);
        if (!encodedLength) return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
        const auto& s = *static_cast<SQL_INTERVAL_STRUCT*>(parameterValuePtr);
        bool negative = s.interval_sign != 0;

        if (shape->start <= 2) {
            // YEAR-MONTH class. MONTH TO MONTH (start==2) is a single
            // field, already total months - matches
            // encodeIntervalYearMonth's own start==2 branch, which expects
            // `months` as the raw leading-field value in that case, not
            // years*12 (see sqli/types.h's doc comment).
            std::int64_t months = shape->start == 2
                                       ? static_cast<std::int64_t>(s.intval.year_month.month)
                                       : static_cast<std::int64_t>(s.intval.year_month.year) * 12 +
                                             static_cast<std::int64_t>(s.intval.year_month.month);
            if (negative) months = -months;
            auto encoded = ifxsqli::encodeIntervalYearMonth(months, *encodedLength);
            if (!encoded) return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
            ifxsqli::BindParam p;
            p.ifxType = ifxsqli::ifxtype::kInterval;
            p.isNull = false;
            p.data = std::move(*encoded);
            p.precisionField = static_cast<std::uint16_t>(*encodedLength);
            return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
        }

        // DAY-FRACTION class: only the fields the qualifier actually
        // covers are read out of `day_second` (its day/hour/minute/second
        // members are always present regardless of qualifier, but fields
        // outside [start, wholeEnd] are the caller's problem to leave
        // zeroed, not this driver's to guess at) - same field-by-field
        // walk decodeIntervalDayFraction/formatIntervalDayFraction already
        // use elsewhere in this driver.
        // shape->end is never >10 (see IntervalParamShape's doc comment -
        // it's always a whole-field end code), so this is really just
        // shape->end - written as a bound rather than std::min to avoid
        // colliding with windows.h's own min/max macros in this
        // translation unit.
        unsigned wholeEnd = shape->end < 10u ? shape->end : 10u;
        ifxsqli::IntervalDayFractionValue v;
        v.negative = negative;
        switch (shape->start) {
            case 4: v.day = s.intval.day_second.day; break;
            case 6: v.hour = s.intval.day_second.hour; break;
            case 8: v.minute = s.intval.day_second.minute; break;
            case 10: v.second = s.intval.day_second.second; break;
        }
        for (unsigned tu = shape->start + 2; tu <= wholeEnd; tu += 2) {
            switch (tu) {
                case 6: v.hour = s.intval.day_second.hour; break;
                case 8: v.minute = s.intval.day_second.minute; break;
                case 10: v.second = s.intval.day_second.second; break;
            }
        }
        unsigned qualifierEnd = *encodedLength & 0xF;
        if (qualifierEnd > 10) {
            // FRACTION(n): SQL_DAY_SECOND_STRUCT's own `fraction` field is
            // nanoseconds, the same convention SQL_TIMESTAMP_STRUCT's
            // fraction field uses (see this function's SQL_TYPE_TIMESTAMP
            // branch above) - scale down to Informix's own digit count,
            // truncating rather than rounding any finer precision the wire
            // format has no room for.
            unsigned fractionDigits = qualifierEnd - 10;
            v.fraction = static_cast<int>(s.intval.day_second.fraction / pow10(9 - fractionDigits));
            v.fractionDigits = static_cast<int>(fractionDigits);
        }
        auto encoded = ifxsqli::encodeIntervalDayFraction(v, *encodedLength);
        if (!encoded) return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
        ifxsqli::BindParam p;
        p.ifxType = ifxsqli::ifxtype::kInterval;
        p.isNull = false;
        p.data = std::move(*encoded);
        p.precisionField = static_cast<std::uint16_t>(*encodedLength);
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
        case SQL_BIT:
            return ifxsqli::ifxtype::kBooleanBindType;
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
        case SQL_LONGVARBINARY:
            return ifxsqli::ifxtype::kByte;
        case SQL_LONGVARCHAR:
            return ifxsqli::ifxtype::kText;
        default:
            // INTERVAL isn't a single case label (13 distinct
            // SQL_INTERVAL_* constants, unlike every type above) - reuses
            // findIntervalShape rather than listing all 13 here too.
            if (findIntervalShape(parameterType)) return ifxsqli::ifxtype::kInterval;
            return std::nullopt;
    }
}

} // namespace ifxodbc
