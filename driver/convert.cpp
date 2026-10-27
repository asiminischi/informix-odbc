#include "driver/convert.h"

#include "sqli/types.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ifxodbc {

namespace {

std::string formatDate(std::int32_t unixDays) {
    using namespace std::chrono;
    sys_days days{std::chrono::days{unixDays}};
    year_month_day ymd{days};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return buf;
}

std::string formatDatetime(const ifxsqli::DatetimeValue& dt) {
    char buf[48];
    int len = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour,
                             dt.minute, dt.second);
    if (dt.fractionDigits > 0 && len > 0 && static_cast<std::size_t>(len) < sizeof(buf)) {
        std::snprintf(buf + len, sizeof(buf) - static_cast<std::size_t>(len), ".%0*d", dt.fractionDigits,
                      dt.fraction);
    }
    return buf;
}

// %.17g/%.9g round-trip double/float exactly (the minimum digit counts that
// guarantee it per IEEE 754), unlike std::to_string's fixed 6-decimal-place
// default which would silently truncate.
std::string formatDouble(double d) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    return buf;
}

std::string formatFloat(float f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", f);
    return buf;
}

// Matches Informix's own text rendering (dbaccess's display of the same
// value, e.g. "3-07" or "-3-07"), not ISO 8601 - months is always shown
// zero-padded to 2 digits regardless of whether the qualifier includes a
// MONTH field (YEAR TO YEAR intervals decode with months%12 == 0, which
// still prints as "-00").
std::string formatIntervalYearMonth(std::int64_t months) {
    bool negative = months < 0;
    if (negative) months = -months;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%lld-%02lld", negative ? "-" : "", static_cast<long long>(months / 12),
                  static_cast<long long>(months % 12));
    return buf;
}

// Same convention as formatIntervalYearMonth: matches Informix's own
// display, not ISO 8601. `start`/`wholeEnd` are the qualifier's start code
// and effective end code (FRACTION already collapsed to SECOND=10 by the
// caller, same convention decodeIntervalDayFraction's own `wholeEnd` uses)
// - needed, not just "does DAY appear", because which field is the
// *leading* one (printed unpadded, with the sign) and how many trailing
// fields to print at all both depend on the full qualifier shape, not just
// whether DAY is present. A prior version took only a `hasDayField` bool
// and always printed the full "HH:MM:SS" shape regardless of `wholeEnd`,
// and always used `iv.hour` as the leading value regardless of `start` -
// wrong for any qualifier narrower than *_TO_SECOND (a spurious trailing
// ":00" field) and any MINUTE- or SECOND-anchored qualifier (the true
// leading value printed in the wrong slot, with `iv.hour`, always 0 for
// those qualifiers, printed first instead). Fixed by iterating the same
// start..wholeEnd field walk decodeIntervalDayFraction itself uses.
std::string formatIntervalDayFraction(const ifxsqli::IntervalDayFractionValue& iv, unsigned start,
                                       unsigned wholeEnd) {
    char buf[64];
    long long leadingValue = 0;
    switch (start) {
        case 4: leadingValue = iv.day; break;
        case 6: leadingValue = iv.hour; break;
        case 8: leadingValue = iv.minute; break;
        case 10: leadingValue = iv.second; break;
    }
    int len = std::snprintf(buf, sizeof(buf), "%s%lld", iv.negative ? "-" : "", leadingValue);
    // The separator right after the leading field is a space only when DAY
    // is the leading field (Informix's own "D HH:MM:SS" display); every
    // other field-to-field separator, including DAY's own subsequent ones,
    // is a colon.
    const char* sepAfterLeading = (start == 4) ? " " : ":";
    bool firstAfterLeading = true;
    for (unsigned tu = start + 2; tu <= wholeEnd && len > 0 && static_cast<std::size_t>(len) < sizeof(buf); tu += 2) {
        long long value = 0;
        switch (tu) {
            case 6: value = iv.hour; break;
            case 8: value = iv.minute; break;
            case 10: value = iv.second; break;
        }
        const char* sep = firstAfterLeading ? sepAfterLeading : ":";
        firstAfterLeading = false;
        len += std::snprintf(buf + len, sizeof(buf) - static_cast<std::size_t>(len), "%s%02lld", sep, value);
    }
    if (iv.fractionDigits > 0 && len > 0 && static_cast<std::size_t>(len) < sizeof(buf)) {
        std::snprintf(buf + len, sizeof(buf) - static_cast<std::size_t>(len), ".%0*d", iv.fractionDigits,
                      iv.fraction);
    }
    return buf;
}

} // namespace

std::expected<CellText, ifxsqli::Error> formatCellAsText(const ifxsqli::ColumnDesc& col,
                                                          std::span<const std::byte> raw) {
    using namespace ifxsqli;
    switch (col.ifxType) {
        case ifxtype::kSmallInt: {
            auto v = decodeSmallInt(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, std::to_string(**v)};
        }
        case ifxtype::kInteger:
        case ifxtype::kSerial: {
            auto v = decodeInteger(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, std::to_string(**v)};
        }
        case ifxtype::kBigInt:
        case ifxtype::kBigSerial: {
            auto v = decodeBigInt(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, std::to_string(**v)};
        }
        case ifxtype::kChar: {
            auto v = decodeChar(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, **v};
        }
        case ifxtype::kVarchar:
        case ifxtype::kNVarchar: {
            auto v = decodeVarchar(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, **v};
        }
        case ifxtype::kDate: {
            auto v = decodeDateUnixDays(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, formatDate(**v)};
        }
        case ifxtype::kDecimal:
        case ifxtype::kMoney: {
            unsigned precision = (col.encodedLength >> 8) & 0xFF;
            unsigned scale = col.encodedLength & 0xFF;
            auto v = decodeDecimal(raw, precision, scale);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, **v};
        }
        case ifxtype::kDatetime: {
            auto v = decodeDatetime(raw, col.encodedLength);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, formatDatetime(**v)};
        }
        case ifxtype::kBoolean: {
            auto v = decodeBoolean(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, **v ? "1" : "0"};
        }
        case ifxtype::kFloat: {
            auto v = decodeFloat(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, formatDouble(**v)};
        }
        case ifxtype::kSmallFloat: {
            auto v = decodeSmallFloat(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            return CellText{false, formatFloat(**v)};
        }
        case ifxtype::kInterval: {
            unsigned startCode = (col.encodedLength >> 4) & 0xF;
            if (startCode <= 2) {
                auto v = decodeIntervalYearMonth(raw, col.encodedLength);
                if (!v) return std::unexpected(v.error());
                if (!*v) return CellText{true, {}};
                return CellText{false, formatIntervalYearMonth(**v)};
            }
            auto v = decodeIntervalDayFraction(raw, col.encodedLength);
            if (!v) return std::unexpected(v.error());
            if (!*v) return CellText{true, {}};
            unsigned endCode = col.encodedLength & 0xF;
            unsigned wholeEnd = std::min(endCode, 10u);
            return CellText{false, formatIntervalDayFraction(**v, startCode, wholeEnd)};
        }
        default:
            return std::unexpected(
                Error{ErrorKind::Protocol,
                      "column type " + std::to_string(col.ifxType) + " has no text conversion yet"});
    }
}

} // namespace ifxodbc
