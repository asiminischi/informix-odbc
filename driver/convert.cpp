#include "driver/convert.h"

#include "sqli/types.h"

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
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                  dt.second);
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
        case ifxtype::kDecimal: {
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
        default:
            return std::unexpected(
                Error{ErrorKind::Protocol,
                      "column type " + std::to_string(col.ifxType) + " has no text conversion yet"});
    }
}

} // namespace ifxodbc
