#include "sqli/types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
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

std::expected<std::optional<double>, Error> decodeFloat(std::span<const std::byte> raw) {
    if (raw.size() != 8) return std::unexpected(Error{ErrorKind::Protocol, "FLOAT: expected 8 bytes"});
    bool allOnes = true;
    for (auto b : raw) {
        if (b != std::byte{0xFF}) { allOnes = false; break; }
    }
    if (allOnes) return std::nullopt;
    auto bits = readBigEndian<std::uint64_t>(raw);
    return std::bit_cast<double>(bits);
}

std::expected<std::optional<float>, Error> decodeSmallFloat(std::span<const std::byte> raw) {
    if (raw.size() != 4) return std::unexpected(Error{ErrorKind::Protocol, "SMALLFLOAT: expected 4 bytes"});
    bool allOnes = true;
    for (auto b : raw) {
        if (b != std::byte{0xFF}) { allOnes = false; break; }
    }
    if (allOnes) return std::nullopt;
    auto bits = readBigEndian<std::uint32_t>(raw);
    return std::bit_cast<float>(bits);
}

std::vector<std::byte> encodeFloat(double v) { return writeBigEndian<std::int64_t>(std::bit_cast<std::int64_t>(v)); }

std::vector<std::byte> encodeSmallFloat(float v) {
    return writeBigEndian<std::int32_t>(std::bit_cast<std::int32_t>(v));
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

std::expected<std::optional<bool>, Error> decodeBoolean(std::span<const std::byte> raw) {
    if (raw.empty()) return std::unexpected(Error{ErrorKind::Protocol, "BOOLEAN: empty buffer"});
    if (raw[0] == std::byte{1}) return std::nullopt;
    if (raw[0] != std::byte{0}) {
        return std::unexpected(Error{ErrorKind::Protocol, "BOOLEAN: unrecognized null-flag byte"});
    }
    if (raw.size() != 6) return std::unexpected(Error{ErrorKind::Protocol, "BOOLEAN: expected 6 bytes for a value"});
    auto len = readBigEndian<std::int32_t>(raw.subspan(1, 4));
    if (len != 1) return std::unexpected(Error{ErrorKind::Protocol, "BOOLEAN: unexpected payload length"});
    return raw[5] != std::byte{0};
}

std::vector<std::byte> encodeBoolean(bool v) {
    auto lengthBytes = writeBigEndian<std::int32_t>(1);
    std::vector<std::byte> out;
    out.reserve(5);
    out.insert(out.end(), lengthBytes.begin(), lengthBytes.end());
    out.push_back(v ? std::byte{1} : std::byte{0});
    return out;
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

namespace {

// Field order matches wire order: YEAR (2 mantissa bytes / 4 decimal
// digits) then MONTH/DAY/HOUR/MINUTE/SECOND (1 mantissa byte each). Shared
// by decodeDatetime and encodeDatetime so both directions can't drift.
struct DatetimeField {
    unsigned tu;
    unsigned mantissaBytes;
};
constexpr std::array<DatetimeField, 6> kDatetimeFields{{{0, 2}, {2, 1}, {4, 1}, {6, 1}, {8, 1}, {10, 1}}};

struct DatetimeQualifier {
    unsigned totalDigits;
    unsigned start;
    unsigned end;
    // end - 10 when end is a FRACTION code (11-15), 0 otherwise.
    unsigned fractionDigits;
};

std::expected<DatetimeQualifier, Error> parseDatetimeQualifier(std::uint32_t encodedLength) {
    unsigned totalDigits = (encodedLength >> 8) & 0xFF;
    unsigned qualifierByte = encodedLength & 0xFF;
    unsigned start = (qualifierByte >> 4) & 0xF;
    unsigned end = qualifierByte & 0xF;

    if (end > 15) {
        return std::unexpected(Error{ErrorKind::Protocol, "DATETIME: malformed qualifier byte"});
    }
    if (end > 10) {
        // FRACTION end code: start must be a whole-field code (FRACTION
        // can't be a start).
        if (start > 10 || start % 2 != 0) {
            return std::unexpected(Error{ErrorKind::Protocol, "DATETIME: malformed qualifier byte"});
        }
        return DatetimeQualifier{totalDigits, start, end, end - 10};
    }
    if (start > end || start % 2 != 0 || end % 2 != 0) {
        return std::unexpected(Error{ErrorKind::Protocol, "DATETIME: malformed qualifier byte"});
    }
    return DatetimeQualifier{totalDigits, start, end, 0};
}

} // namespace

std::expected<std::optional<DatetimeValue>, Error> decodeDatetime(std::span<const std::byte> raw,
                                                                     std::uint32_t encodedLength) {
    auto qualifierR = parseDatetimeQualifier(encodedLength);
    if (!qualifierR) return std::unexpected(qualifierR.error());
    auto [totalDigits, start, end, fractionDigits] = *qualifierR;
    const auto& kFields = kDatetimeFields;

    std::size_t expectedMantissa = 0;
    unsigned expectedDigits = 0;
    for (const auto& f : kFields) {
        if (f.tu >= start && f.tu <= std::min(end, 10u)) {
            expectedMantissa += f.mantissaBytes;
            expectedDigits += f.mantissaBytes * 2;
        }
    }
    std::size_t fractionMantissaBytes = (fractionDigits + 1) / 2;
    expectedMantissa += fractionMantissaBytes;
    expectedDigits += fractionDigits;
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
        if (f.tu < start || f.tu > std::min(end, 10u)) continue;
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
    if (fractionDigits > 0) {
        int fractionValue = 0;
        for (std::size_t i = 0; i < fractionMantissaBytes; ++i) {
            int pair = digits[idx + i];
            bool lastPairIsOdd = (i + 1 == fractionMantissaBytes) && (fractionDigits % 2 != 0);
            // The trailing digit of an odd-length fraction is padded with a
            // zero on the wire (e.g. FRACTION(5) .12345 -> [12][34][50]) -
            // drop that padding digit rather than treating it as real.
            fractionValue = lastPairIsOdd ? (fractionValue * 10 + pair / 10) : (fractionValue * 100 + pair);
        }
        result.fraction = fractionValue;
        result.fractionDigits = static_cast<int>(fractionDigits);
    }
    return result;
}

std::expected<std::vector<std::byte>, Error> encodeDatetime(const DatetimeValue& value,
                                                                std::uint32_t encodedLength) {
    auto qualifierR = parseDatetimeQualifier(encodedLength);
    if (!qualifierR) return std::unexpected(qualifierR.error());
    unsigned start = qualifierR->start;
    unsigned end = qualifierR->end;
    unsigned fractionDigits = qualifierR->fractionDigits;

    std::vector<int> mantissa;
    for (const auto& f : kDatetimeFields) {
        if (f.tu < start || f.tu > std::min(end, 10u)) continue;
        int fieldValue = 0;
        switch (f.tu) {
            case 0: fieldValue = value.year; break;
            case 2: fieldValue = value.month; break;
            case 4: fieldValue = value.day; break;
            case 6: fieldValue = value.hour; break;
            case 8: fieldValue = value.minute; break;
            case 10: fieldValue = value.second; break;
        }
        if (f.mantissaBytes == 2) {
            mantissa.push_back(fieldValue / 100);
            mantissa.push_back(fieldValue % 100);
        } else {
            mantissa.push_back(fieldValue);
        }
    }
    // The exponent byte's biased magnitude is fixed to this whole-field
    // pair count even when FRACTION pairs follow - DATETIME's "decimal
    // point" always sits right after SECOND (confirmed live: a YEAR TO
    // FRACTION(5) value's wire exponent encoded 7, the YEAR..SECOND pair
    // count, not 10, the total mantissa byte count including fraction).
    auto wholeFieldPairs = static_cast<std::uint16_t>(mantissa.size());
    if (fractionDigits > 0) {
        // Zero-pad the fraction value to the qualifier's declared width,
        // then split into digit pairs left to right - confirmed live
        // convention (see decodeDatetime's doc comment): a trailing odd
        // digit is padded with a zero, not left-padded.
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%0*d", static_cast<int>(fractionDigits), value.fraction);
        for (unsigned p = 0; p < fractionDigits; p += 2) {
            int d1 = buf[p] - '0';
            int d2 = (p + 1 < fractionDigits) ? (buf[p + 1] - '0') : 0;
            mantissa.push_back(d1 * 10 + d2);
        }
    }

    // Always positive - no hundred's-complement step, since datetimes
    // aren't signed (unlike DECIMAL's negative case).
    auto exponentByte = static_cast<std::uint8_t>(0x80 | ((64 + wholeFieldPairs) & 0x7F));

    std::vector<std::byte> data;
    auto decNdgts = static_cast<std::uint16_t>(mantissa.size());
    auto lengthField = writeBigEndian<std::uint16_t>(static_cast<std::uint16_t>(decNdgts + 1));
    data.insert(data.end(), lengthField.begin(), lengthField.end());
    data.push_back(std::byte{exponentByte});
    for (int d : mantissa) data.push_back(static_cast<std::byte>(static_cast<unsigned char>(d)));
    return data;
}

namespace {

// Un-normalizes an INTERVAL's packed digits back to a plain, zero-padded
// `2*expectedMantissa`-char digit string, and returns the sign - the
// un-normalization machinery shared by both interval classes (see
// decodeIntervalYearMonth's doc comment in sqli/types.h for the
// live-capture-derived reasoning: INTERVAL reuses DECIMAL's floating
// representation, not DATETIME's fixed-point one, so leading zero-pairs
// may have been stripped and need restoring before the digit string means
// what the qualifier says it should).
//
// Deliberately does NOT trim the result down to the qualifier's actual
// digit count - unlike YEAR-MONTH's single leading field, DAY-FRACTION has
// *two* independently-padded regions (the leading field's own front pad
// when its width is odd, and FRACTION's own back pad when its digit count
// is odd - see decodeIntervalDayFraction's doc comment), so a single
// blanket trim on the whole string is wrong. Callers own trimming each
// field's own raw slice instead.
struct IntervalDigits {
    int sign;
    std::string digits; // 2*expectedMantissa chars, caller trims per field
};

std::expected<std::optional<IntervalDigits>, Error> decodeIntervalDigits(std::span<const std::byte> raw,
                                                                            unsigned totalDigits, unsigned end,
                                                                            std::size_t expectedMantissa) {
    if (raw.size() != 1 + expectedMantissa) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: buffer size doesn't match qualifier"});
    }

    int sign = 0, trueExp = 0;
    auto digitsR = decodePackedDigits(raw, /*allowNegative=*/true, &sign, &trueExp);
    if (!digitsR) return std::unexpected(digitsR.error());
    if (!*digitsR) return std::nullopt;
    const auto& digits = **digitsR;

    int unnormalizedExp = (static_cast<int>(totalDigits) + 10 - static_cast<int>(end) + 1) / 2;
    int shift = unnormalizedExp - trueExp;
    if (shift < 0) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: exponent exceeds qualifier width"});
    }
    std::vector<int> fullPairs(static_cast<std::size_t>(shift), 0);
    fullPairs.insert(fullPairs.end(), digits.begin(), digits.end());
    fullPairs.resize(expectedMantissa, 0);

    std::string digitStr;
    digitStr.reserve(fullPairs.size() * 2);
    for (int d : fullPairs) {
        digitStr += static_cast<char>('0' + d / 10);
        digitStr += static_cast<char>('0' + d % 10);
    }
    return IntervalDigits{sign, std::move(digitStr)};
}

std::int64_t parseIntervalDigits(std::string_view s) {
    std::int64_t v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    return v;
}

// The leading field occupies the first `2*leadingPairBytes` chars of the
// un-normalized digit string; when its declared width is odd, that raw
// slice has one extra digit at the front (confirmed live via `INTERVAL
// MONTH(3) TO MONTH` holding 43: raw slice "0043", correct 3-digit value
// "043", not "004") - drop it there, not trim the whole string.
std::string_view trimLeadingField(std::string_view rawSlice, unsigned leadingWidth) {
    return rawSlice.substr(rawSlice.size() - leadingWidth);
}

} // namespace

std::expected<std::optional<std::int64_t>, Error> decodeIntervalYearMonth(std::span<const std::byte> raw,
                                                                             std::uint32_t encodedLength) {
    unsigned totalDigits = (encodedLength >> 8) & 0xFF;
    unsigned qualifierByte = encodedLength & 0xFF;
    unsigned start = (qualifierByte >> 4) & 0xF;
    unsigned end = qualifierByte & 0xF;

    if (start > end || end > 2) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: not a YEAR-MONTH class qualifier"});
    }
    unsigned leadingWidth = totalDigits - (end - start);
    if (leadingWidth < 1 || leadingWidth > 9) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: malformed qualifier byte"});
    }
    // MONTH is a plain trailing field (like DAY-FRACTION's HOUR/MINUTE/
    // SECOND) only when it's not itself the leading field.
    unsigned leadingPairBytes = (leadingWidth + 1) / 2;
    bool hasMonthField = (end == 2 && start != 2);
    std::size_t expectedMantissa = leadingPairBytes + (hasMonthField ? 1 : 0);

    auto digitsR = decodeIntervalDigits(raw, totalDigits, end, expectedMantissa);
    if (!digitsR) return std::unexpected(digitsR.error());
    if (!*digitsR) return std::nullopt;
    const auto& [sign, digitStr] = **digitsR;

    std::string_view leadingRaw(digitStr.data(), 2 * leadingPairBytes);
    std::string_view leadingStr = trimLeadingField(leadingRaw, leadingWidth);

    std::int64_t months;
    if (start == 2) {
        // MONTH TO MONTH: a single field that's already a total-months
        // value, not years - the decompiled reference's separate branch
        // for this case (no years/12 split).
        months = parseIntervalDigits(leadingStr);
    } else {
        months = parseIntervalDigits(leadingStr) * 12;
        if (hasMonthField) {
            months += parseIntervalDigits(std::string_view(digitStr).substr(2 * leadingPairBytes, 2));
        }
    }
    if (sign < 0) months = -months;
    return months;
}

std::expected<std::optional<IntervalDayFractionValue>, Error> decodeIntervalDayFraction(
    std::span<const std::byte> raw, std::uint32_t encodedLength) {
    unsigned totalDigits = (encodedLength >> 8) & 0xFF;
    unsigned qualifierByte = encodedLength & 0xFF;
    unsigned start = (qualifierByte >> 4) & 0xF;
    unsigned end = qualifierByte & 0xF;

    if (start < 4 || start > 10 || start % 2 != 0 || end < start || end > 15) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: not a DAY-FRACTION class qualifier"});
    }
    unsigned wholeEnd = std::min(end, 10u);
    unsigned fractionDigits = end > 10 ? end - 10 : 0;
    unsigned leadingWidth = totalDigits - (wholeEnd - start) - fractionDigits;
    if (leadingWidth < 1 || leadingWidth > 9) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: malformed qualifier byte"});
    }
    unsigned leadingPairBytes = (leadingWidth + 1) / 2;
    unsigned wholeFieldCount = (wholeEnd - start) / 2;
    unsigned fractionPairBytes = (fractionDigits + 1) / 2;
    std::size_t expectedMantissa = leadingPairBytes + wholeFieldCount + fractionPairBytes;

    auto digitsR = decodeIntervalDigits(raw, totalDigits, end, expectedMantissa);
    if (!digitsR) return std::unexpected(digitsR.error());
    if (!*digitsR) return std::nullopt;
    const auto& [sign, digitStr] = **digitsR;

    IntervalDayFractionValue result;
    std::string_view leadingRaw(digitStr.data(), 2 * leadingPairBytes);
    std::int64_t leadingValue = parseIntervalDigits(trimLeadingField(leadingRaw, leadingWidth));
    switch (start) {
        case 4: result.day = leadingValue; break;
        case 6: result.hour = leadingValue; break;
        case 8: result.minute = leadingValue; break;
        case 10: result.second = leadingValue; break;
    }
    // Every whole field after the leading one is a plain 2-digit 0-99
    // value (HOUR 0-23, MINUTE/SECOND 0-59) - never wide like the leading
    // field can be, so no leading-field-style trimming applies here.
    std::size_t pos = 2 * leadingPairBytes;
    for (unsigned tu = start + 2; tu <= wholeEnd; tu += 2) {
        std::int64_t value = parseIntervalDigits(std::string_view(digitStr).substr(pos, 2));
        pos += 2;
        switch (tu) {
            case 6: result.hour = value; break;
            case 8: result.minute = value; break;
            case 10: result.second = value; break;
        }
    }
    if (fractionDigits > 0) {
        // FRACTION's raw slice is `2*fractionPairBytes` chars; when
        // fractionDigits is odd, the *last* char of that slice is padding
        // (matching decodeDatetime's own FRACTION convention exactly -
        // this is a coincidence of two unrelated mechanisms landing on the
        // same padding side, not shared code: DATETIME never needs
        // un-normalizing in the first place).
        std::string_view fracRaw = std::string_view(digitStr).substr(pos, 2 * fractionPairBytes);
        result.fraction = static_cast<int>(parseIntervalDigits(fracRaw.substr(0, fractionDigits)));
        result.fractionDigits = static_cast<int>(fractionDigits);
    }
    result.negative = sign < 0;
    return result;
}

namespace {

// Appends `value` as `width` zero-padded decimal digits to `mantissa`, one
// pair per slot. When `width` is odd, one extra zero digit is prepended to
// reach an even pair count - the front-padding convention
// decodeIntervalDigits's trimLeadingField undoes on the fetch side (see its
// doc comment), reproduced here directly rather than inverted, since the
// bind direction never goes through un-normalization at all.
void pushLeadingField(std::vector<int>& mantissa, std::int64_t value, unsigned width) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*lld", static_cast<int>(width), static_cast<long long>(value));
    std::string padded;
    if (width % 2 != 0) padded += '0';
    padded.append(buf, width);
    for (std::size_t i = 0; i + 1 < padded.size(); i += 2) {
        mantissa.push_back((padded[i] - '0') * 10 + (padded[i + 1] - '0'));
    }
}

// Appends a FRACTION value's digits, back-padding with a trailing zero when
// `fractionDigits` is odd - matching decodeDatetime's own FRACTION
// convention (encodeDatetime does the identical thing for DATETIME's
// FRACTION suffix).
void pushFractionField(std::vector<int>& mantissa, int fractionValue, unsigned fractionDigits) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%0*d", static_cast<int>(fractionDigits), fractionValue);
    for (unsigned p = 0; p < fractionDigits; p += 2) {
        int d1 = buf[p] - '0';
        int d2 = (p + 1 < fractionDigits) ? (buf[p + 1] - '0') : 0;
        mantissa.push_back(d1 * 10 + d2);
    }
}

// Shared SQ_BIND framing for both interval classes: u16 length prefix, then
// the exponent byte (positive sign bit vs. hundred's-complement negative,
// same convention encodeDecimal/decodePackedDigits use), then the mantissa
// bytes as-is - no stripping, unlike the fetch direction (see
// encodeIntervalYearMonth's doc comment in sqli/types.h for why).
std::vector<std::byte> packIntervalMantissa(std::vector<int> mantissa, int sign, int unnormalizedExp) {
    std::uint8_t exponentByte;
    if (sign < 0) {
        hundredsComplementInPlace(mantissa);
        auto positiveLow7 = static_cast<std::uint8_t>((64 + unnormalizedExp) & 0x7F);
        exponentByte = static_cast<std::uint8_t>(0x7F - positiveLow7);
    } else {
        exponentByte = static_cast<std::uint8_t>(0x80 | ((64 + unnormalizedExp) & 0x7F));
    }

    std::vector<std::byte> data;
    auto decNdgts = static_cast<std::uint16_t>(mantissa.size());
    auto lengthField = writeBigEndian<std::uint16_t>(static_cast<std::uint16_t>(decNdgts + 1));
    data.insert(data.end(), lengthField.begin(), lengthField.end());
    data.push_back(std::byte{exponentByte});
    for (int d : mantissa) data.push_back(static_cast<std::byte>(static_cast<unsigned char>(d)));
    return data;
}

} // namespace

std::expected<std::vector<std::byte>, Error> encodeIntervalYearMonth(std::int64_t months,
                                                                         std::uint32_t encodedLength) {
    unsigned totalDigits = (encodedLength >> 8) & 0xFF;
    unsigned qualifierByte = encodedLength & 0xFF;
    unsigned start = (qualifierByte >> 4) & 0xF;
    unsigned end = qualifierByte & 0xF;
    if (start > end || end > 2) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: not a YEAR-MONTH class qualifier"});
    }
    unsigned leadingWidth = totalDigits - (end - start);
    if (leadingWidth < 1 || leadingWidth > 9) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: malformed qualifier byte"});
    }
    bool hasMonthField = (end == 2 && start != 2);

    int sign = months < 0 ? -1 : 1;
    std::int64_t magnitude = months < 0 ? -months : months;

    std::vector<int> mantissa;
    if (start == 2) {
        // MONTH TO MONTH: a single field that's already a total-months
        // value, no years/12 split (matches decodeIntervalYearMonth's
        // start==2 branch).
        pushLeadingField(mantissa, magnitude, leadingWidth);
    } else {
        pushLeadingField(mantissa, magnitude / 12, leadingWidth);
        if (hasMonthField) mantissa.push_back(static_cast<int>(magnitude % 12));
    }

    int unnormalizedExp = (static_cast<int>(totalDigits) + 10 - static_cast<int>(end) + 1) / 2;
    return packIntervalMantissa(std::move(mantissa), sign, unnormalizedExp);
}

std::expected<std::vector<std::byte>, Error> encodeIntervalDayFraction(const IntervalDayFractionValue& value,
                                                                           std::uint32_t encodedLength) {
    unsigned totalDigits = (encodedLength >> 8) & 0xFF;
    unsigned qualifierByte = encodedLength & 0xFF;
    unsigned start = (qualifierByte >> 4) & 0xF;
    unsigned end = qualifierByte & 0xF;
    if (start < 4 || start > 10 || start % 2 != 0 || end < start || end > 15) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: not a DAY-FRACTION class qualifier"});
    }
    unsigned wholeEnd = std::min(end, 10u);
    unsigned fractionDigits = end > 10 ? end - 10 : 0;
    unsigned leadingWidth = totalDigits - (wholeEnd - start) - fractionDigits;
    if (leadingWidth < 1 || leadingWidth > 9) {
        return std::unexpected(Error{ErrorKind::Protocol, "INTERVAL: malformed qualifier byte"});
    }

    std::int64_t leadingValue = 0;
    switch (start) {
        case 4: leadingValue = value.day; break;
        case 6: leadingValue = value.hour; break;
        case 8: leadingValue = value.minute; break;
        case 10: leadingValue = value.second; break;
    }

    std::vector<int> mantissa;
    pushLeadingField(mantissa, leadingValue, leadingWidth);
    for (unsigned tu = start + 2; tu <= wholeEnd; tu += 2) {
        std::int64_t fieldValue = 0;
        switch (tu) {
            case 6: fieldValue = value.hour; break;
            case 8: fieldValue = value.minute; break;
            case 10: fieldValue = value.second; break;
        }
        mantissa.push_back(static_cast<int>(fieldValue));
    }
    if (fractionDigits > 0) pushFractionField(mantissa, value.fraction, fractionDigits);

    int unnormalizedExp = (static_cast<int>(totalDigits) + 10 - static_cast<int>(end) + 1) / 2;
    return packIntervalMantissa(std::move(mantissa), value.negative ? -1 : 1, unnormalizedExp);
}

} // namespace ifxsqli
