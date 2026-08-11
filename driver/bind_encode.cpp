#include "driver/bind_encode.h"

#include "sqli/types.h"

#include <cstring>
#include <string_view>

namespace ifxodbc {

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
    if ((parameterType == SQL_CHAR || parameterType == SQL_VARCHAR) &&
        (valueType == SQL_C_CHAR || valueType == SQL_C_DEFAULT)) {
        const auto* text = static_cast<const char*>(parameterValuePtr);
        std::string_view s = strLenOrInd == SQL_NTS ? std::string_view(text)
                                                      : std::string_view(text, static_cast<std::size_t>(strLenOrInd));
        ifxsqli::BindParam p;
        p.ifxType = parameterType == SQL_CHAR ? ifxsqli::ifxtype::kChar : ifxsqli::ifxtype::kVarchar;
        p.isNull = false;
        p.data = ifxsqli::encodeChar(s);
        p.precisionField = static_cast<std::uint16_t>(s.size());
        return BindEncodeResult{BindEncodeStatus::kOk, std::move(p)};
    }
    return BindEncodeResult{BindEncodeStatus::kUnsupported, {}};
}

} // namespace ifxodbc
