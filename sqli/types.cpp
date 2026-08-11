#include "sqli/types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace ifxsqli {

namespace {

bool allZero(std::span<const std::byte> raw) {
    for (auto b : raw) {
        if (b != std::byte{0}) return false;
    }
    return true;
}

template <class Int>
Int readBigEndian(std::span<const std::byte> raw) {
    using Unsigned = std::make_unsigned_t<Int>;
    Unsigned u = 0;
    for (auto b : raw) {
        u = static_cast<Unsigned>((u << 8) | std::to_integer<unsigned char>(b));
    }
    return static_cast<Int>(u);
}

template <class Int>
std::vector<std::byte> writeBigEndian(Int v) {
    using Unsigned = std::make_unsigned_t<Int>;
    auto u = static_cast<Unsigned>(v);
    std::vector<std::byte> out(sizeof(Int));
    for (std::size_t i = sizeof(Int); i-- > 0;) {
        out[i] = static_cast<std::byte>(u & 0xFF);
        u = static_cast<Unsigned>(u >> 8);
    }
    return out;
}

// The "hundred's complement" transform used for negative DECIMAL mantissas:
// 99-complement every digit, then add 1 to the rightmost digit, propagating
// carry leftwards. This is an involution (applying it twice restores the
// original digits), so it's used for both directions - see decodeDecimal.
void hundredsComplementInPlace(std::vector<int>& digits) {
    for (auto& d : digits) d = 99 - d;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (++*it <= 99) break;
        *it = 0;
    }
}

} // namespace

std::expected<std::optional<std::int16_t>, Error> decodeSmallInt(std::span<const std::byte> raw) {
    if (raw.size() != 2) return std::unexpected(Error{ErrorKind::Protocol, "SMALLINT: expected 2 bytes"});
    auto v = readBigEndian<std::int16_t>(raw);
    if (v == std::numeric_limits<std::int16_t>::min()) return std::nullopt;
    return v;
}

std::expected<std::optional<std::int32_t>, Error> decodeInteger(std::span<const std::byte> raw) {
    if (raw.size() != 4) return std::unexpected(Error{ErrorKind::Protocol, "INTEGER: expected 4 bytes"});
    auto v = readBigEndian<std::int32_t>(raw);
    if (v == std::numeric_limits<std::int32_t>::min()) return std::nullopt;
    return v;
}

std::vector<std::byte> encodeInteger(std::int32_t v) { return writeBigEndian<std::int32_t>(v); }

std::vector<std::byte> encodeSmallInt(std::int16_t v) { return writeBigEndian<std::int16_t>(v); }
std::vector<std::byte> encodeBigInt(std::int64_t v) { return writeBigEndian<std::int64_t>(v); }

std::vector<std::byte> encodeChar(std::string_view s) {
    auto lengthBytes = writeBigEndian<std::uint16_t>(static_cast<std::uint16_t>(s.size()));
    std::vector<std::byte> out;
    out.reserve(lengthBytes.size() + s.size());
    out.insert(out.end(), lengthBytes.begin(), lengthBytes.end());
    for (char c : s) out.push_back(std::byte(c));
    return out;
}

std::expected<std::optional<std::int64_t>, Error> decodeBigInt(std::span<const std::byte> raw) {
    if (raw.size() != 8) return std::unexpected(Error{ErrorKind::Protocol, "BIGINT: expected 8 bytes"});
    auto v = readBigEndian<std::int64_t>(raw);
    if (v == std::numeric_limits<std::int64_t>::min()) return std::nullopt;
    return v;
}

std::expected<std::optional<std::string>, Error> decodeChar(std::span<const std::byte> raw) {
    if (raw.empty()) return std::unexpected(Error{ErrorKind::Protocol, "CHAR: empty buffer"});
    if (raw[0] == std::byte{0}) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
}

std::expected<std::optional<std::string>, Error> decodeVarchar(std::span<const std::byte> raw) {
    if (raw.empty()) return std::unexpected(Error{ErrorKind::Protocol, "VARCHAR: empty buffer"});
    auto len = std::to_integer<unsigned char>(raw[0]);
    if (raw.size() != 1u + len) {
        return std::unexpected(Error{ErrorKind::Protocol, "VARCHAR: length prefix doesn't match buffer size"});
    }
    if (len == 1 && raw[1] == std::byte{0}) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(raw.data() + 1), len);
}

// Confirmed live: DATE's day count is relative to 1899-12-31, which is
// 25568 days before the Unix epoch (1970-01-01).
constexpr std::int32_t kIfxEpochToUnixEpochDays = 25568;

std::expected<std::optional<std::int32_t>, Error> decodeDateUnixDays(std::span<const std::byte> raw) {
    if (raw.size() != 4) return std::unexpected(Error{ErrorKind::Protocol, "DATE: expected 4 bytes"});
    auto v = readBigEndian<std::int32_t>(raw);
    if (v == std::numeric_limits<std::int32_t>::min()) return std::nullopt;
    return v - kIfxEpochToUnixEpochDays;
}

std::vector<std::byte> encodeDateUnixDays(std::int32_t unixDays) {
    return writeBigEndian<std::int32_t>(unixDays + kIfxEpochToUnixEpochDays);
}

namespace {

std::expected<std::optional<std::vector<int>>, Error> decodePackedDigits(std::span<const std::byte> raw,
                                                                            bool allowNegative, int* signOut,
                                                                            int* trueExpOut) {
    if (raw.empty()) return std::unexpected(Error{ErrorKind::Protocol, "packed decimal: empty buffer"});
    if (allZero(raw)) return std::nullopt;

    auto expByte = std::to_integer<unsigned char>(raw[0]);
    bool positive = (expByte & 0x80) != 0;
    if (!allowNegative && !positive) {
        return std::unexpected(Error{ErrorKind::Protocol, "packed decimal: unexpected sign bit for unsigned field"});
    }
    int low7 = positive ? (expByte & 0x7F) : (0x7F - (expByte & 0x7F));
    int trueExp = low7 - 64;

    std::vector<int> digits;
    digits.reserve(raw.size() - 1);
    for (std::size_t i = 1; i < raw.size(); ++i) digits.push_back(std::to_integer<unsigned char>(raw[i]));
    if (!positive) hundredsComplementInPlace(digits);

    *signOut = positive ? 1 : -1;
    *trueExpOut = trueExp;
    return digits;
}

} // namespace

std::expected<std::optional<std::string>, Error> decodeDecimal(std::span<const std::byte> raw, unsigned precision,
                                                                  unsigned scale) {
    std::size_t expectedMantissa = (precision + 1) / 2;
    if (raw.size() != 1 + expectedMantissa) {
        return std::unexpected(Error{ErrorKind::Protocol, "DECIMAL: buffer size doesn't match precision"});
    }

    int sign = 0, trueExp = 0;
    auto digitsR = decodePackedDigits(raw, /*allowNegative=*/true, &sign, &trueExp);
    if (!digitsR) return std::unexpected(digitsR.error());
    if (!*digitsR) return std::nullopt;
    const auto& digits = **digitsR;

    bool isZero = true;
    for (int d : digits) {
        if (d != 0) { isZero = false; break; }
    }
    if (isZero) {
        // Canonical zero: exponent byte 0x80 (sign bit set, magnitude 0),
        // confirmed live. trueExp in this case isn't a real digit-placement
        // exponent, so it's not used.
        std::string s = "0";
        if (scale > 0) s += "." + std::string(scale, '0');
        return s;
    }

    std::string digitStr;
    digitStr.reserve(digits.size() * 2);
    for (int d : digits) {
        digitStr += static_cast<char>('0' + d / 10);
        digitStr += static_cast<char>('0' + d % 10);
    }

    long long pointPos = 2LL * trueExp; // position of the decimal point within digitStr
    std::string intPart, fracPart;
    if (pointPos <= 0) {
        intPart = "0";
        fracPart = std::string(static_cast<std::size_t>(-pointPos), '0') + digitStr;
    } else if (static_cast<std::size_t>(pointPos) >= digitStr.size()) {
        intPart = digitStr + std::string(static_cast<std::size_t>(pointPos) - digitStr.size(), '0');
        fracPart.clear();
    } else {
        intPart = digitStr.substr(0, static_cast<std::size_t>(pointPos));
        fracPart = digitStr.substr(static_cast<std::size_t>(pointPos));
    }

    // Strip leading zeros from the integer part (but keep at least one digit).
    std::size_t firstNonZero = intPart.find_first_not_of('0');
    intPart = (firstNonZero == std::string::npos) ? "0" : intPart.substr(firstNonZero);

    if (fracPart.size() < scale) fracPart += std::string(scale - fracPart.size(), '0');
    fracPart.resize(scale);

    std::string result = (sign < 0 ? "-" : "") + intPart;
    if (scale > 0) result += "." + fracPart;
    return result;
}

std::expected<DecimalBindEncoding, Error> encodeDecimal(std::string_view decimalLiteral) {
    if (decimalLiteral.empty()) return std::unexpected(Error{ErrorKind::Protocol, "DECIMAL: empty literal"});

    bool negative = false;
    std::string_view s = decimalLiteral;
    if (s.front() == '-') {
        negative = true;
        s.remove_prefix(1);
    } else if (s.front() == '+') {
        s.remove_prefix(1);
    }

    auto dot = s.find('.');
    std::string_view intPart = dot == std::string_view::npos ? s : s.substr(0, dot);
    std::string_view fracPart = dot == std::string_view::npos ? std::string_view{} : s.substr(dot + 1);
    if (intPart.empty()) return std::unexpected(Error{ErrorKind::Protocol, "DECIMAL: missing integer part"});
    auto isAllDigits = [](std::string_view part) {
        for (char c : part) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };
    if (!isAllDigits(intPart) || !isAllDigits(fracPart)) {
        return std::unexpected(Error{ErrorKind::Protocol, "DECIMAL: non-digit character in literal"});
    }

    auto scale = static_cast<std::uint16_t>(fracPart.size());

    // BigDecimal's unscaledValue digit string: intPart+fracPart with
    // leading zeros stripped (a plain "0.01" literal has digits "001"
    // before stripping but unscaledValue is "1", not "001" - confirmed
    // against convFromBigDecimal, which derives everything below from
    // this stripped string, not from the literal's own formatting).
    std::string combined;
    combined.reserve(intPart.size() + fracPart.size());
    combined += intPart;
    combined += fracPart;
    std::size_t firstNonZero = combined.find_first_not_of('0');
    bool isZero = firstNonZero == std::string::npos;
    std::string unscaled = isZero ? "0" : combined.substr(firstNonZero);

    // Mirrors convFromBigDecimal's exp-parity/prepend logic exactly (see
    // sqli/types.h's encodeDecimal doc comment): trueExp is derived from
    // buflen-scale, not from where the decimal point falls in the
    // formatted literal, and a leading '0' digit is prepended only when
    // that difference is odd. Also applies uniformly to the zero case
    // (unscaled="0", buflen=1), matching convFromBigDecimal falling
    // through to this same derivation even when dec_exp is separately
    // overridden to the canonical -64 for zero.
    int buflen = static_cast<int>(unscaled.size());
    int exp = buflen - static_cast<int>(scale);
    std::vector<char> unpack(unscaled.begin(), unscaled.end());
    int trueExp;
    if (exp % 2 != 0) {
        unpack.insert(unpack.begin(), '0');
        trueExp = (exp + 1) / 2;
    } else {
        trueExp = exp / 2;
    }
    if (unpack.size() % 2 != 0) unpack.push_back('0');

    std::vector<int> mantissa;
    mantissa.reserve(unpack.size() / 2);
    for (std::size_t i = 0; i + 1 < unpack.size(); i += 2) {
        mantissa.push_back((unpack[i] - '0') * 10 + (unpack[i + 1] - '0'));
    }

    std::uint8_t exponentByte;
    if (isZero) {
        // Canonical zero (matches decodeDecimal's own special case):
        // magnitude bits aren't meaningful, sign is dropped regardless of
        // the literal's own leading '-' (matches BigDecimal.ZERO
        // comparison being sign-blind).
        exponentByte = 0x80;
        std::fill(mantissa.begin(), mantissa.end(), 0);
    } else if (!negative) {
        exponentByte = static_cast<std::uint8_t>(0x80 | ((64 + trueExp) & 0x7F));
    } else {
        hundredsComplementInPlace(mantissa);
        auto positiveLow7 = static_cast<std::uint8_t>((64 + trueExp) & 0x7F);
        exponentByte = static_cast<std::uint8_t>(0x7F - positiveLow7);
    }

    DecimalBindEncoding result;
    result.scale = scale;
    auto decNdgts = static_cast<std::uint16_t>(mantissa.size());
    auto lengthField = writeBigEndian<std::uint16_t>(static_cast<std::uint16_t>(decNdgts + 1));
    result.data.insert(result.data.end(), lengthField.begin(), lengthField.end());
    result.data.push_back(std::byte{exponentByte});
    for (int d : mantissa) result.data.push_back(static_cast<std::byte>(static_cast<unsigned char>(d)));
    return result;
}

std::expected<std::optional<DatetimeValue>, Error> decodeDatetime(std::span<const std::byte> raw,
                                                                     std::uint32_t encodedLength) {
    unsigned totalDigits = (encodedLength >> 8) & 0xFF;
    unsigned qualifierByte = encodedLength & 0xFF;
    unsigned start = (qualifierByte >> 4) & 0xF;
    unsigned end = qualifierByte & 0xF;

    if (end > 10) {
        return std::unexpected(
            Error{ErrorKind::Protocol, "DATETIME: fractional-second qualifiers not yet supported (Phase 3)"});
    }
    if (start > end || start % 2 != 0 || end % 2 != 0) {
        return std::unexpected(Error{ErrorKind::Protocol, "DATETIME: malformed qualifier byte"});
    }

    // Field order matches wire order: YEAR (2 mantissa bytes / 4 decimal
    // digits) then MONTH/DAY/HOUR/MINUTE/SECOND (1 mantissa byte each).
    struct Field { unsigned tu; unsigned mantissaBytes; };
    static constexpr std::array<Field, 6> kFields{{{0, 2}, {2, 1}, {4, 1}, {6, 1}, {8, 1}, {10, 1}}};

    std::size_t expectedMantissa = 0;
    unsigned expectedDigits = 0;
    for (const auto& f : kFields) {
        if (f.tu >= start && f.tu <= end) {
            expectedMantissa += f.mantissaBytes;
            expectedDigits += f.mantissaBytes * 2;
        }
    }
    if (expectedDigits != totalDigits) {
        return std::unexpected(Error{ErrorKind::Protocol, "DATETIME: qualifier digit count mismatch"});
    }
    if (raw.size() != 1 + expectedMantissa) {
        return std::unexpected(Error{ErrorKind::Protocol, "DATETIME: buffer size doesn't match qualifier"});
    }

    int sign = 0, trueExp = 0;
    auto digitsR = decodePackedDigits(raw, /*allowNegative=*/false, &sign, &trueExp);
    if (!digitsR) return std::unexpected(digitsR.error());
    if (!*digitsR) return std::nullopt;
    const auto& digits = **digitsR;

    DatetimeValue result;
    std::size_t idx = 0;
    for (const auto& f : kFields) {
        if (f.tu < start || f.tu > end) continue;
        int value = (f.mantissaBytes == 2) ? (digits[idx] * 100 + digits[idx + 1]) : digits[idx];
        idx += f.mantissaBytes;
        switch (f.tu) {
            case 0: result.year = value; break;
            case 2: result.month = value; break;
            case 4: result.day = value; break;
            case 6: result.hour = value; break;
            case 8: result.minute = value; break;
            case 10: result.second = value; break;
        }
    }
    return result;
}

} // namespace ifxsqli
