#pragma once

#include "sqli/error.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ifxsqli {

// Named Informix base type codes for the MVP scalar set, confirmed against a
// live server (DESCRIBE's ifxType field, flag bits already masked off by the
// caller). Other codes exist in the wire protocol but aren't part of this
// set and aren't handled by the decoders below.
namespace ifxtype {
inline constexpr int kChar = 0;
inline constexpr int kSmallInt = 1;
inline constexpr int kInteger = 2;
// Confirmed live via DESCRIBE against FLOAT/SMALLFLOAT columns - matches
// the decompiled JDBC driver's ifxType exactly (unlike BOOLEAN). Plain
// fixed-width IEEE 754 values (IfxFloat/IfxSmallFloat extend IfxObject,
// not IfxBaseType - no extended-type framing, no SQ_RETURN_TYPE surprise).
inline constexpr int kFloat = 3;       // 8-byte IEEE 754 double
inline constexpr int kSmallFloat = 4;  // 4-byte IEEE 754 single
inline constexpr int kDecimal = 5;
inline constexpr int kSerial = 6;
inline constexpr int kDate = 7;
inline constexpr int kDatetime = 10;
inline constexpr int kVarchar = 13;
inline constexpr int kNVarchar = 16;
inline constexpr int kBigInt = 52;
inline constexpr int kBigSerial = 53;
// Confirmed live via DESCRIBE against a BOOLEAN column - notably not 45,
// the ifxType the decompiled JDBC driver's IfxBoolean constructor sets
// (that 45 is a JDBC-level/extended-type rendering code, not the SQLI wire
// base type - see docs/protocol-notes.md's BOOLEAN section). BOOLEAN is an
// *extended* type (nonzero extendedId in DESCRIBE) even though its wire
// value is a single byte, which is why fetching it needs the SQ_RETURN_TYPE
// extended-column encoding (SqliStatement::fetchNextBatch) rather than just
// a plain scalar entry.
inline constexpr int kBoolean = 41;
} // namespace ifxtype

// A decoded calendar DATETIME value. Only whole-second qualifiers (some
// contiguous span within YEAR..SECOND) are supported - see decodeDatetime.
struct DatetimeValue {
    int year = 0;    // 0 if the qualifier doesn't include YEAR
    int month = 0;   // 1-12, 0 if not included
    int day = 0;     // 1-31, 0 if not included
    int hour = 0;
    int minute = 0;
    int second = 0;
};

// Decodes a fixed-width big-endian two's-complement integer. NULL is the
// type's MIN_VALUE bit pattern (confirmed live: 0x8000/0x80000000/INT64_MIN).
[[nodiscard]] std::expected<std::optional<std::int16_t>, Error> decodeSmallInt(std::span<const std::byte> raw);
[[nodiscard]] std::expected<std::optional<std::int32_t>, Error> decodeInteger(std::span<const std::byte> raw);
[[nodiscard]] std::expected<std::optional<std::int64_t>, Error> decodeBigInt(std::span<const std::byte> raw);

// FLOAT/SMALLFLOAT: plain big-endian IEEE 754 bit pattern (confirmed via the
// decompiled JavaToIfxType.JavaToIfxDouble/JavaToIfxReal - manual big-endian
// byte packing of Double.doubleToLongBits/Float.floatToIntBits, the same
// convention as the integer types' plain big-endian encoding above). NULL is
// all-0xFF bytes (confirmed via IfxFloat/IfxSmallFloat's isIfxNull - notably
// NOT the MIN_VALUE-bit-pattern convention the integer types use). Bind-side
// (SQ_BIND/encodeFloat) is not implemented yet - fetch/decode only.
[[nodiscard]] std::expected<std::optional<double>, Error> decodeFloat(std::span<const std::byte> raw);
[[nodiscard]] std::expected<std::optional<float>, Error> decodeSmallFloat(std::span<const std::byte> raw);

// Encodes a value for SQ_BIND (a prepared statement's "?" parameters) -
// confirmed via the decompiled JDBC driver's IfxInteger.toIfx()/
// JavaToIfxInt: the same plain big-endian two's-complement bytes as the
// TUPLE-format decoder above (INTEGER is fixed-width either way, unlike
// DECIMAL/DATETIME - see the SQ_BIND wire notes in docs/protocol-notes.md
// once added). Unlike decodeInteger's NULL sentinel, SQ_BIND signals NULL
// via the bind record's own indicator field, never by encoding a value at
// all - so there's no NULL case here for the caller to handle.
[[nodiscard]] std::vector<std::byte> encodeInteger(std::int32_t v);

// SQ_BIND's per-parameter "precision" field for INTEGER (confirmed via the
// decompiled JDBC driver's IfxInteger.getEncodedLength(), a hardcoded
// constant, not a byte count - see docs/protocol-notes.md's SQ_BIND notes).
// 0x0A00 = (10 << 8) | 0, i.e. INTEGER's max decimal digit count in the
// high byte, matching the same (precision << 8 | scale)-shaped metadata
// DESCRIBE uses elsewhere in this protocol.
inline constexpr std::uint16_t kIntegerBindPrecision = 0x0A00;

// SMALLINT/BIGINT bind encoders and their SQ_BIND precision fields, same
// plain big-endian two's-complement shape as the TUPLE decoders above
// (confirmed via IfxShort/IfxBigInt.toIfx() -> JavaToIfxSmallInt/
// JavaToIfxLongBigInt, both byte-identical to the fetch-direction encoding -
// unlike DECIMAL/DATETIME, whose bind and fetch formats differ). The
// precision fields are hardcoded per-type constants exactly like
// kIntegerBindPrecision, confirmed via IfxShort/IfxBigInt.getEncodedLength()
// (1280 = 0x0500, 4864 = 0x1300) - NOT derived from any DESCRIBE-reported
// placeholder metadata, unlike the variable-width types (see
// docs/protocol-notes.md's SQ_BIND notes once the rest of the MVP set is
// wired up).
[[nodiscard]] std::vector<std::byte> encodeSmallInt(std::int16_t v);
[[nodiscard]] std::vector<std::byte> encodeBigInt(std::int64_t v);
inline constexpr std::uint16_t kSmallIntBindPrecision = 0x0500;
inline constexpr std::uint16_t kBigIntBindPrecision = 0x1300;

// CHAR/VARCHAR bind encoding: unlike the fixed-width integer types above,
// this is NOT the same bytes as decodeChar/decodeVarchar's TUPLE-format
// input - traced via the decompiled JDBC reference's IfxChar.toIfx()/
// IfxVarChar.toIfx(), which both call the same JavaToIfxChar: a u16 length
// prefix (not including a NUL) followed by the raw bytes, the same
// self-describing shape as this protocol's own ifxChar2 (see byte_io.h),
// unpadded (writeBindBlock's own even-alignment padding applies afterward,
// same as every other bind value - do not pad here too). Confirmed live
// for both CHAR and VARCHAR bind targets that precisionField can simply be
// the string's own length - the server does not require it to match the
// target column's declared width.
[[nodiscard]] std::vector<std::byte> encodeChar(std::string_view s);

// Fixed-width space-padded CHAR(n). NULL is a leading 0x00 byte (confirmed
// live); the rest of the buffer is not meaningful in that case. Non-NULL
// values are returned exactly as stored on the wire (space-padded to the
// declared width, not trimmed).
[[nodiscard]] std::expected<std::optional<std::string>, Error> decodeChar(std::span<const std::byte> raw);

// VARCHAR/NVARCHAR: raw must be the 1-byte length prefix followed by that
// many data bytes (i.e. exactly the slice splitRow already extracts). NULL
// is the sentinel length=1/data=0x00 (confirmed live; distinct from a real
// empty string, which is length=0).
[[nodiscard]] std::expected<std::optional<std::string>, Error> decodeVarchar(std::span<const std::byte> raw);

// BOOLEAN (an extended type, ifxType 41 - see ifxtype::kBoolean): confirmed
// live against a 3-row fixture (true/false/NULL) that the TUPLE encoding
// matches the decompiled IfxBoolean.toIfxTuple() shape exactly, byte for
// byte - NOT a plain 1-byte scalar despite DESCRIBE reporting
// encodedLength=1. Self-describing, variable width:
//   NULL:     5 bytes, `01 00 00 00 00` (byte 0 = 1, rest unused padding)
//   non-NULL: 6 bytes, `00` + 4-byte big-endian length prefix (always 1,
//             the payload's own byte count) + 1-byte value (0=false,
//             1=true)
// `raw` must be the whole 5- or 6-byte slice (splitRow determines which by
// peeking byte 0, same as this function does).
[[nodiscard]] std::expected<std::optional<bool>, Error> decodeBoolean(std::span<const std::byte> raw);

// 4-byte big-endian signed day count from epoch 1899-12-31 (confirmed
// live). NULL is INT32_MIN, same sentinel as INTEGER. Returns the day count
// relative to the Unix epoch (1970-01-01) for the caller to format however
// it likes (e.g. via std::chrono::sys_days).
[[nodiscard]] std::expected<std::optional<std::int32_t>, Error> decodeDateUnixDays(std::span<const std::byte> raw);

// DATE bind encoding: the same plain 4-byte big-endian day count as
// decodeDateUnixDays's TUPLE-format input, confirmed via the decompiled
// JDBC reference's IfxDate.toIfx() -> JavaToIfxDate -> JavaToIfxInt (unlike
// DECIMAL/DATETIME, whose bind and fetch formats differ). Takes a
// Unix-epoch-relative day count, same convention as decodeDateUnixDays's
// return value, and applies the same epoch shift in reverse. SQ_BIND's
// precisionField is confirmed live to work as 4 (the byte width) - no
// live evidence yet on whether the server actually inspects this value
// for DATE at all, only that 4 doesn't cause an error or desync.
[[nodiscard]] std::vector<std::byte> encodeDateUnixDays(std::int32_t unixDays);

// Informix packed-decimal DECIMAL(precision,scale): 1 exponent byte + N
// mantissa bytes (N = ceil(precision/2)), each mantissa byte a base-100
// digit pair. Confirmed live for both positive and negative values (four
// independent negative test points): the exponent's sign bit (0x80) plus a
// biased 7-bit magnitude for positive values; for negative values the low 7
// bits of the exponent byte are bitwise-complemented (0x7F - x) and the
// mantissa is the "hundred's complement" (base-100 analog of two's
// complement: 99-complement each digit, then +1 with carry from the right)
// of what the positive encoding would have been - an involution, so
// decoding re-applies the same transform. NULL is all-zero bytes (including
// the exponent byte, which is otherwise never 0 for a real value); the
// literal value 0 is distinguished by exponent byte 0x80 with an all-zero
// mantissa (confirmed live), handled as a special case since its magnitude
// exponent isn't a meaningful digit-placement value. Returns an exact
// decimal string (no floating-point rounding).
[[nodiscard]] std::expected<std::optional<std::string>, Error> decodeDecimal(std::span<const std::byte> raw,
                                                                               unsigned precision, unsigned scale);

// One DECIMAL SQ_BIND value: the encoded bytes (ready to hand to
// BindParam::data) and the scale to use as precisionField. Bundled
// together since, unlike every other bind type so far, DECIMAL's
// precisionField is not self-evident from the byte count or a hardcoded
// constant - it is the value's own scale (confirmed via the decompiled
// JDBC reference's dispatch in sendBind: for DECIMAL specifically it sends
// ((IfxDecimal)ival).getScale(), i.e. BigDecimal.scale() - intrinsic to
// the value itself, not derived from any target column's declared scale,
// so no column metadata is needed here either).
struct DecimalBindEncoding {
    std::vector<std::byte> data;
    std::uint16_t scale = 0;
};

// Encodes a decimal literal (e.g. "12345.67", "-0.01", "0") for SQ_BIND.
// Unlike decodeDecimal's TUPLE-format input, this prepends a u16 length
// (the byte count of the exponent+mantissa that follows, i.e. dec_ndgts+1)
// before the same exponent-byte-plus-packed-mantissa shape decodeDecimal
// reads - confirmed via the decompiled JDBC reference's
// IfxDecimal.toIfx() -> convIfx(true) (its convIfx(false), used by
// toIfxTuple, is what decodeDecimal's shape matches instead - see
// VARCHAR's toIfx()/toIfxTuple() mixup in docs/protocol-notes.md for why
// this distinction matters). Uses the minimal mantissa width needed to
// represent the literal's own digits exactly, not padded to any declared
// column precision (convFromBigDecimal pads to a target precision when one
// is known, but this driver has none to consult at bind time - same
// reasoning as CHAR/VARCHAR/DATE). Confirmed live
// (tests/integration/test_bind.cpp) that the server accepts this minimal
// mantissa width without requiring it to match the target column's
// declared precision. The exponent/
// mantissa math is independently derived as decodeDecimal's mathematical
// inverse (reconstructed, not copied), reusing the same hundred's-
// complement transform for negative values.
[[nodiscard]] std::expected<DecimalBindEncoding, Error> encodeDecimal(std::string_view decimalLiteral);

// Informix DATETIME(qualifier): same packed base-100-digit-pair mantissa
// machinery as DECIMAL (always positive/no complement - datetimes aren't
// signed), where the digit sequence is just the calendar fields
// concatenated (YEAR as 4 decimal digits/2 mantissa bytes if present, each
// other field as 2 decimal digits/1 mantissa byte). `encodedLength` is the
// DESCRIBE-reported value, packed as (totalDigits << 8) | qualifierByte
// where qualifierByte's nibbles are the standard Informix TU_* start/end
// codes (Year=0, Month=2, Day=4, Hour=6, Minute=8, Second=10). Confirmed
// live only for the YEAR TO SECOND qualifier (0x0A, encodedLength 0x0E0A);
// other whole-second sub-ranges within YEAR..SECOND follow the same
// confirmed mechanism but aren't independently live-tested. FRACTION
// qualifiers (end nibble 11-15) are not yet supported - the sub-second
// byte layout hasn't been observed against a live server. NULL is all-zero
// bytes, matching DECIMAL's convention.
[[nodiscard]] std::expected<std::optional<DatetimeValue>, Error> decodeDatetime(std::span<const std::byte> raw,
                                                                                  std::uint32_t encodedLength);

// DATETIME bind encoding for whole-second qualifiers only, same scope
// restriction as decodeDatetime (FRACTION isn't supported - see its doc
// comment). `encodedLength` is the same packed (totalDigits << 8) |
// qualifierByte value decodeDatetime takes; `value`'s fields outside
// [start, end] are ignored. Unlike DATE, this genuinely differs from
// decodeDatetime's TUPLE-format shape: it prepends a u16 length
// (dec_ndgts + 1), the same bind-only framing DECIMAL uses - confirmed via
// the decompiled JDBC reference's IfxDateTime.toIfx() routing through
// Decimal.javaToIfx() (the exact same encoder IfxDecimal.toIfx() uses,
// since Informix DATETIME reuses DECIMAL's packed-digit wire machinery
// end to end, not just its TUPLE-format decode shape). The exponent
// byte's biased magnitude equals the total mantissa byte count (matching
// decodeDatetime's own confirmed-live observation that this holds for
// YEAR TO SECOND, generalized here rather than re-derived per qualifier),
// always positive - no hundred's-complement step, since datetimes aren't
// signed. Confirmed live for YEAR TO SECOND
// (tests/integration/test_bind.cpp), including a hand-derived exact-byte
// unit test predicted before ever touching the network - the output
// matched on the first live attempt.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> encodeDatetime(const DatetimeValue& value,
                                                                            std::uint32_t encodedLength);

} // namespace ifxsqli
