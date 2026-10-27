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
// MONEY(p,s): same packed-decimal wire encoding and DESCRIBE encodedLength
// packing as DECIMAL(p,s) (confirmed live: splitRow/decodeDecimal handle it
// unchanged); only the declared type differs.
inline constexpr int kMoney = 8;
inline constexpr int kDatetime = 10;
inline constexpr int kVarchar = 13;
inline constexpr int kNVarchar = 16;
inline constexpr int kBigInt = 52;
inline constexpr int kBigSerial = 53;
// Simple large objects (the older LO mechanism, distinct from smart
// BLOB/CLOB below) - confirmed live via DESCRIBE against real BYTE/TEXT
// columns. Both report extendedId=0 (plain scalars, not the extended-type
// framing BOOLEAN/BLOB/CLOB use).
inline constexpr int kByte = 11;
inline constexpr int kText = 12;
// Confirmed live via DESCRIBE against an INTERVAL YEAR TO MONTH column -
// matches the decompiled JDBC driver's IfxIntervalYM/IfxIntervalDF exactly
// (both interval classes share this one wire type code; which class a
// given column is comes from the qualifier's start/end codes, not the
// type code). extendedId=0 (a plain scalar, not extended like BOOLEAN).
inline constexpr int kInterval = 14;
// Confirmed live via DESCRIBE against a BOOLEAN column - notably not 45,
// the ifxType the decompiled JDBC driver's IfxBoolean constructor sets
// (that 45 is a JDBC-level/extended-type rendering code, not the SQLI wire
// base type - see docs/protocol-notes.md's BOOLEAN section). BOOLEAN is an
// *extended* type (nonzero extendedId in DESCRIBE) even though its wire
// value is a single byte, which is why fetching it needs the SQ_RETURN_TYPE
// extended-column encoding (SqliStatement::fetchNextBatch) rather than just
// a plain scalar entry.
inline constexpr int kBoolean = 41;
// The *remapped* extended-type code SQ_RETURN_TYPE and SQ_BIND expect for a
// BOOLEAN column/parameter (matches the decompiled JDBC driver's own
// ifxType for IfxBoolean) - confirmed live for SQ_RETURN_TYPE; distinct
// from kBoolean (41), the plain DESCRIBE/TUPLE base type code. Use this,
// not kBoolean, when building a BindParam for a BOOLEAN "?" parameter.
inline constexpr int kBooleanBindType = 45;
// The generic opaque-UDT wire type code (matches the decompiled JDBC
// driver's IfxUDT, which every fastpath RPC parameter/return value not
// covered by a plain scalar type - e.g. the 72-byte smart-LOB locator
// bind_encode.cpp's SqliConnection::fetchSmartLob passes to
// informix.ifx_lo_open - travels as, confirmed live via a JDBC golden-
// capture trace). Also confirmed live for an *ordinary* column bind: the
// smart-LOB locator SqliConnection::createSmartLob returns binds into a
// real INSERT/UPDATE with ifxType=kUdt directly, ExtendedOwner="informix"
// (matching BOOLEAN's own ExtendedOwner convention for its ordinary bind,
// even though BOOLEAN itself remaps to kBooleanBindType rather than using
// kUdt) - unlike BOOLEAN, a smart LOB has no separate "bind type" remap.
inline constexpr int kUdt = 44;
// LVARCHAR's own base type code (long varchar) - confirmed live as
// informix.ifx_lo_col_sbinfo's own first-parameter type (the target
// column's "table.column" identifier), a fastpath call
// SqliConnection::createSmartLob makes. Unlike kUdt, this isn't a generic
// opaque-UDT wrapper - lvarchar is its own scalar-ish type - but on the
// wire it's still extended-framed (type > 18) with the exact same
// [ExtendedOwner][ExtendedTypeName="lvarchar"][indicator][precision]
// [4-byte length][bytes] shape kUdt parameters use (confirmed live -
// see appendFastPathLvarcharParam in sqli/connection.cpp). Not used
// anywhere else in this driver yet (no LVARCHAR column/bind support).
inline constexpr int kLvarchar = 43;
} // namespace ifxtype

// A decoded calendar DATETIME value. Any contiguous qualifier span within
// YEAR..FRACTION(5) is supported - see decodeDatetime.
struct DatetimeValue {
    int year = 0;    // 0 if the qualifier doesn't include YEAR
    int month = 0;   // 1-12, 0 if not included
    int day = 0;     // 1-31, 0 if not included
    int hour = 0;
    int minute = 0;
    int second = 0;
    // Sub-second digits as a plain zero-padded integer, e.g. FRACTION(3)
    // value 5 means .005, not .500 - width is fractionDigits, not fixed.
    // 0 if the qualifier doesn't include FRACTION.
    int fraction = 0;
    // Number of FRACTION digits the qualifier declares (1-5), 0 if the
    // qualifier doesn't include FRACTION. Needed alongside `fraction`
    // because the same integer value means different things at different
    // widths (fraction=5 is .5 at FRACTION(1) but .00005 at FRACTION(5)).
    int fractionDigits = 0;
};

// A decoded INTERVAL value, DAY-FRACTION class (qualifier start in {DAY,
// HOUR, MINUTE, SECOND}) - see decodeIntervalDayFraction. Sign is tracked
// separately, not folded into negative field values (matching ODBC's own
// SQL_INTERVAL_STRUCT convention: `interval_sign` is a separate flag, and
// the numeric fields are always non-negative magnitudes).
struct IntervalDayFractionValue {
    bool negative = false;
    // Exactly one of day/hour/minute/second holds the qualifier's
    // (possibly wide, arbitrary-precision) leading field value - which one
    // depends on the qualifier's start code. The others hold their own
    // field's plain value (HOUR 0-23, MINUTE/SECOND 0-59) if the qualifier
    // includes them, 0 otherwise.
    std::int64_t day = 0;
    std::int64_t hour = 0;
    std::int64_t minute = 0;
    std::int64_t second = 0;
    int fraction = 0;       // see DatetimeValue::fraction's doc comment
    int fractionDigits = 0; // 0 if the qualifier doesn't include FRACTION
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
// NOT the MIN_VALUE-bit-pattern convention the integer types use).
[[nodiscard]] std::expected<std::optional<double>, Error> decodeFloat(std::span<const std::byte> raw);
[[nodiscard]] std::expected<std::optional<float>, Error> decodeSmallFloat(std::span<const std::byte> raw);

// SQ_BIND encoders: same plain big-endian IEEE 754 bytes as the TUPLE
// decoders above (IfxFloat/IfxSmallFloat.toIfx() -> JavaToIfxDouble/
// JavaToIfxReal, byte-identical to the fetch-direction encoding - unlike
// DECIMAL/DATETIME). Unlike the hardcoded-constant precision fields for
// INTEGER/SMALLINT/BIGINT, IfxFloat/IfxSmallFloat don't override
// getEncodedLength(), so there's no equivalent traced constant - the byte
// width (8/4) is a first hypothesis pending a live check, same approach
// that worked for DATE's precisionField.
[[nodiscard]] std::vector<std::byte> encodeFloat(double v);
[[nodiscard]] std::vector<std::byte> encodeSmallFloat(float v);
inline constexpr std::uint16_t kFloatBindPrecision = 8;
inline constexpr std::uint16_t kSmallFloatBindPrecision = 4;

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

// SQ_BIND encoding for a non-NULL BOOLEAN value: a 4-byte big-endian length
// prefix (always 1, the payload's own byte count) followed by the 1-byte
// value (0=false, 1=true) - the decompiled IfxBoolean.toIfx() ->
// IfxBaseType.toIfx()'s shape (distinct from decodeBoolean's TUPLE-format
// 5/6-byte shape, which additionally has its own leading null-flag byte
// that SQ_BIND signals via the indicator field instead - see BindParam's
// doc comment). Use with ifxtype::kBooleanBindType (45, not kBoolean) and
// set extendedOwnerName="informix"/extendedTypeName="boolean" on the
// BindParam (confirmed live via DESCRIBE - see ColumnDesc::extendedOwnerName).
// precisionField is an unconfirmed hypothesis (1, mirroring the payload
// byte count) - IfxBoolean doesn't override getEncodedLength() the way the
// hardcoded-constant integer types do, so there's no traced reference value.
[[nodiscard]] std::vector<std::byte> encodeBoolean(bool v);
inline constexpr std::uint16_t kBooleanBindPrecision = 1;

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

// Informix packed-decimal DECIMAL(precision,scale): 1 exponent byte +
// mantissa bytes (see decimalWireWidth for the exact count), each mantissa
// byte a base-100 digit pair. Confirmed live for both positive and negative values (four
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
// `scale` 0xFF means floating DECIMAL(p): the result then carries
// however many fraction digits the value has (trailing zeros stripped)
// instead of a fixed count.
[[nodiscard]] std::expected<std::optional<std::string>, Error> decodeDecimal(std::span<const std::byte> raw,
                                                                               unsigned precision, unsigned scale);

// Total wire width (exponent byte included) of a DECIMAL/MONEY(precision,
// scale) value in a TUPLE row. The integer and fraction digits pair up
// separately around the decimal point, so an odd scale costs one extra
// mantissa byte - ESQL/C's DECLEN(p, s) = (p + (s & 1) + 3) / 2. Floating
// DECIMAL(p) reports scale 0xFF (odd), and the same formula holds for it.
// Confirmed live for DECIMAL(12,3)/(11,3)/(12,2)/(11,2)/(16)/(15)/(32,15)
// followed by further columns (node/test/live.test.js).
[[nodiscard]] constexpr std::size_t decimalWireWidth(unsigned precision, unsigned scale) {
    return (precision + (scale & 1u) + 3u) / 2u;
}

inline constexpr unsigned kFloatingDecimalScale = 0xFF;

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
// codes (Year=0, Month=2, Day=4, Hour=6, Minute=8, Second=10, Fraction(n)=
// 10+n for n in 1..5). Confirmed live for YEAR TO SECOND (0x0A, encodedLength
// 0x0E0A) and YEAR TO FRACTION(5) (0x0F, encodedLength 0x130F); other
// sub-ranges follow the same confirmed mechanism but aren't independently
// live-tested. FRACTION digits continue the same digit-pair mantissa
// stream immediately after SECOND - e.g. FRACTION(5) value .12345 packs as
// three more mantissa bytes [12][34][50], the odd trailing digit
// right-padded with a zero rather than left-padded (confirmed live via wire
// capture: the 5-digit fraction ".12345" produced pair bytes 0x0c/0x22/0x32
// = 12/34/50 decimal, not 12/34/05). NULL is all-zero bytes, matching
// DECIMAL's convention.
[[nodiscard]] std::expected<std::optional<DatetimeValue>, Error> decodeDatetime(std::span<const std::byte> raw,
                                                                                  std::uint32_t encodedLength);

// DATETIME bind encoding, same qualifier scope as decodeDatetime (including
// FRACTION). `encodedLength` is the same packed (totalDigits << 8) |
// qualifierByte value decodeDatetime takes; `value`'s fields outside
// [start, end] are ignored, and `value.fraction`/`fractionDigits` are only
// consulted when the qualifier's end code is a FRACTION code. Unlike DATE,
// this genuinely differs from decodeDatetime's TUPLE-format shape: it
// prepends a u16 length (dec_ndgts + 1), the same bind-only framing DECIMAL
// uses - confirmed via the decompiled JDBC reference's IfxDateTime.toIfx()
// routing through Decimal.javaToIfx() (the exact same encoder
// IfxDecimal.toIfx() uses, since Informix DATETIME reuses DECIMAL's
// packed-digit wire machinery end to end, not just its TUPLE-format decode
// shape). The exponent byte's biased magnitude equals the total mantissa
// byte count (matching decodeDatetime's own confirmed-live observation that
// this holds for YEAR TO SECOND, generalized here rather than re-derived
// per qualifier), always positive - no hundred's-complement step, since
// datetimes aren't signed. Confirmed live for YEAR TO SECOND
// (tests/integration/test_bind.cpp), including a hand-derived exact-byte
// unit test predicted before ever touching the network - the output
// matched on the first live attempt.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> encodeDatetime(const DatetimeValue& value,
                                                                            std::uint32_t encodedLength);

// Informix INTERVAL(qualifier), YEAR-MONTH class only (qualifier start and
// end both in {YEAR, MONTH} - i.e. `YEAR TO YEAR`, `YEAR TO MONTH`, or
// `MONTH TO MONTH`; DAY-FRACTION class, e.g. `DAY TO SECOND`, isn't
// supported yet). `encodedLength` is packed as (totalDigits << 8) |
// qualifierByte same as DATETIME's, except totalDigits ("length" in
// Informix's own terminology) is *not* a fixed per-field digit count -
// INTERVAL's leading field has a user-declared width of 1-9 digits
// (`INTERVAL YEAR(4) TO MONTH`), so totalDigits = leadingWidth +
// (end - start), and leadingWidth has to be recovered by subtracting
// (end - start) back out.
//
// Genuinely different from decodeDatetime despite sharing DECIMAL's packed
// base-100-digit-pair mantissa byte format: DATETIME is fixed-point (the
// exponent always marks the same place value, no matter the value), but
// INTERVAL reuses DECIMAL's *floating* representation outright - leading
// zero digit-pairs are stripped and the exponent shrinks to match,
// trailing zero-pairs then pad back out to the qualifier's fixed mantissa
// width. Confirmed live via two YEAR TO MONTH captures sharing no
// coincidence: `INTERVAL YEAR(2) TO MONTH` holding 47-07 (leading field
// already fills its 2-digit width, so no stripping occurs) decoded with
// exponent 0xC7's biased magnitude matching the *unnormalized* formula
// `(totalDigits + 10 - end + 1) / 2` directly (mantissa `[47, 7]`); but
// `INTERVAL YEAR(4) TO MONTH` holding 3-07 (leading field's high digit
// pair is zero) decoded with a *smaller* exponent than that formula
// predicts - the leading zero pair was stripped, exponent dropped by
// exactly 1, and mantissa `[3, 7, 0]` has the stripped pair's slot
// backfilled with a trailing zero pair rather than left as-is. Un-shifting
// this (prepending `unnormalizedExponent - actualExponent` zero-pairs then
// truncating back to the fixed mantissa width) recovers the intended
// digit string in both cases. Also confirmed for the negative case (same
// hundred's-complement DECIMAL's negative mantissas use - `decodePackedDigits`
// with `allowNegative=true`), though only via a plain-text INSERT/SELECT
// round trip through dbaccess, not yet through this driver's own bind path.
// Leading-field widths that make totalDigits odd need one more thing:
// expectedMantissa's ceil(totalDigits/2) rounds up one digit slot too many,
// and that extra slot is padding at the *front* (most significant), not
// the back - opposite of decodeDatetime's FRACTION padding, which sits at
// the very end of the byte stream. Confirmed live via `INTERVAL MONTH(3)
// TO MONTH` holding 43 (totalDigits=3, so 2 mantissa bytes/4 digit slots):
// the correct reconstructed field is "043", not "004" - dropping the front
// slot is what gives the right answer; the initial implementation dropped
// the back slot and silently produced 4 instead of 43, caught only because
// this specific case was live-tested rather than assumed from the FRACTION
// precedent. Also confirmed for the YEAR-anchored, no-month-remainder case
// (`INTERVAL YEAR(3) TO YEAR` holding 43, i.e. 43 years == 516 months).
// NULL is all-zero bytes, matching DECIMAL/DATETIME's convention - a
// genuine zero value is NOT all-zero bytes (confirmed live via `INTERVAL
// MONTH(3) TO MONTH` holding 0): like DECIMAL's own canonical zero, it's
// exponent 0x80 (sign bit set, magnitude 0) with an all-zero mantissa. The
// returned value is total months (negative for a negative interval);
// MONTH TO MONTH intervals are a single field with no years/months split
// (confirmed via the decompiled JDBC reference's separate `start == MONTH`
// branch), YEAR-anchored qualifiers combine years*12 plus any trailing
// month field.
[[nodiscard]] std::expected<std::optional<std::int64_t>, Error> decodeIntervalYearMonth(
    std::span<const std::byte> raw, std::uint32_t encodedLength);

// Informix INTERVAL(qualifier), DAY-FRACTION class (qualifier start and
// end both in {DAY, HOUR, MINUTE, SECOND, FRACTION(1..5)} - e.g. `DAY TO
// SECOND`, `HOUR TO FRACTION(3)`; YEAR-MONTH class isn't accepted here,
// see decodeIntervalYearMonth for that). Shares decodeIntervalYearMonth's
// un-normalization machinery (same DECIMAL-style floating representation,
// same front-padding-slot convention for odd totalDigits) - only the
// field layout differs: instead of a years/month split, each whole field
// from the qualifier's start to min(end, SECOND) contributes exactly 2
// digits (the leading field still gets its variable, qualifier-declared
// width; every field after it is a plain 0-99 value), and if `end` is a
// FRACTION code, the remaining digits continue immediately after SECOND
// exactly the way decodeDatetime's FRACTION support works (including the
// same odd-digit-count trailing zero-pad convention - this is a genuine
// coincidence of two different un-normalization schemes, not the same
// code path: DATETIME's FRACTION never needs un-normalizing in the first
// place, since DATETIME has no floating representation to begin with).
//
// Confirmed live via `INTERVAL DAY(2) TO FRACTION(5)` holding
// `12 03:04:05.67890` (encodedLength `0x0D4F`: totalDigits=13, start=DAY,
// end=FRACTION(5)), a maximally diagnostic value with a distinct digit in
// every field and no un-normalization needed (the leading DAY(2) field's
// value, 12, already fills its 2-digit width) - bytes
// `c4 0c 03 04 05 43 59 00`. HOUR/MINUTE/SECOND-anchored leading fields
// (e.g. `INTERVAL HOUR(5) TO SECOND`) and values needing un-normalization
// (a leading field with a zero high digit) aren't independently
// live-tested for this class specifically, only reasoned to follow
// decodeIntervalYearMonth's already-confirmed mechanism.
[[nodiscard]] std::expected<std::optional<IntervalDayFractionValue>, Error> decodeIntervalDayFraction(
    std::span<const std::byte> raw, std::uint32_t encodedLength);

// INTERVAL SQ_BIND encoding, YEAR-MONTH class - genuinely asymmetric with
// decodeIntervalYearMonth, not its mathematical inverse applied blindly.
// Traced via the decompiled JDBC reference's
// `Decimal(boolean sign, byte[] b, int blen, short inPrec)` constructor
// (what `IfxIntervalYM.toIfx()` -> `JavaToIfxType.JavaToIfxInterval` ->
// `convertIntervalToDecimal(...).javaToIfx()` routes through): unlike the
// fetch/TUPLE direction, SQ_BIND does *not* strip leading zero digit-pairs
// down to DECIMAL's floating form - the mantissa is built directly at the
// qualifier's full, fixed field widths (the same un-normalized digit
// layout `decodeIntervalDigits` reconstructs as an intermediate step
// before the fetch side's own trimming), and the exponent byte's biased
// magnitude is unconditionally the qualifier's own unnormalizedExp formula
// `(totalDigits + 10 - end + 1) / 2` - never shifted down for a
// leading-zero leading field the way a live fetch of the same value would
// be. The server is expected to do its own normalizing when it later
// re-serves the value (exactly as it already does for plain DECIMAL
// binds, which also send a non-canonical/non-stripped encoding), not
// require the client to pre-strip on the way in - confirmed live for a
// case that specifically needs this (`INTERVAL YEAR(4) TO MONTH` bound
// with a zero leading digit-pair) round-tripping correctly through a
// plain SELECT afterward (tests/integration/test_bind.cpp), not merely
// self-consistent.
//
// `months` is total months (negative for a negative interval), same
// convention as decodeIntervalYearMonth's return value; `encodedLength` is
// the same packed qualifier value both decode functions take. Like every
// other bind encoder in this file, NULL isn't represented here - it's
// signaled via the bind record's own indicator field (see BindParam),
// never by encoding a value.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> encodeIntervalYearMonth(std::int64_t months,
                                                                                       std::uint32_t encodedLength);

// INTERVAL SQ_BIND encoding, DAY-FRACTION class - see
// encodeIntervalYearMonth's doc comment for the shared no-stripping
// mechanism (same source, `convertIntervalToDecimal(IntervalDF)`). Field
// layout mirrors decodeIntervalDayFraction: `value`'s leading field
// (whichever of day/hour/minute/second matches the qualifier's start
// code) gets the qualifier's declared width, every subsequent whole field
// is a plain 2-digit 0-99 value, and FRACTION digits (if the qualifier's
// end code calls for them) continue the same mantissa stream after SECOND
// with the same odd-digit-count trailing-zero-pad convention
// decodeDatetime's own FRACTION support uses. `value.fractionDigits` is
// not consulted (same reasoning as encodeDatetime: the wire qualifier
// alone declares FRACTION's width, not the value struct) - only
// `value.fraction` and `encodedLength`'s own end code matter.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> encodeIntervalDayFraction(
    const IntervalDayFractionValue& value, std::uint32_t encodedLength);

// Simple large objects (BYTE=kByte, TEXT=kText): confirmed live that the
// TUPLE row does NOT carry the value's bytes inline - it carries this
// fixed-size, mostly-opaque 56-byte descriptor instead (DESCRIBE's
// encodedLength is 56 for both types, confirmed live; matches this
// project's decompiled JDBC reference's IfxBlob.java, which allocates the
// same 56-byte blobDesc buffer). Field layout within the 56 bytes, per the
// decompiled reference: offset 8 (4 bytes) = tb_start, offset 12 (4
// bytes) = tb_end, offset 16 (4 bytes) = tb_size (the value's total byte
// length), offset 38-39 (2 bytes) = tb_flags. tb_start/tb_end/tb_size are
// confirmed live for exactly one sample (an 11-byte value: tb_start=1,
// tb_end=11, tb_size=11) - plausible but not stress-tested across more
// values/qualifiers. tb_flags's NULL sentinel IS independently confirmed
// live and precise: a NULL value's tb_flags is exactly 1 (bytes 00 01)
// with every other byte in the 56-byte descriptor zero, distinct from a
// real value's tb_flags (confirmed live as 0x0208 for one inserted value -
// some other bit pattern, not exactly 1); the rest of tb_flags's bits
// aren't decoded. The remaining bytes (0-7, 20-37, 40-55) are opaque
// server-side bookkeeping this driver never needs to interpret - only
// echo verbatim back to the server (see SqliConnection::fetchSimpleLob),
// the same way the JDBC reference treats them. Getting the actual value
// requires a second round trip using this descriptor as an opaque token
// (SqliConnection::fetchSimpleLob, sqli/connection.h) - there is no
// decoder function here the way every other scalar type has, since the
// descriptor alone doesn't carry the value.
inline constexpr std::size_t kSimpleLobDescriptorSize = 56;

// Builds a SQ_BIND placeholder descriptor for a non-NULL simple LOB
// parameter - the decompiled reference (IfxBlob.toIfx()) describes this as
// "the same 56-byte descriptor shape, with tb_size patched in": all-zero
// except tb_size (offset 16, 4 bytes big-endian) set to `contentLength`.
// Deliberately does NOT set tb_start/tb_end (unlike a real fetched
// descriptor, which has them) - the decompiled source only mentions
// tb_size being patched, and this is genuinely a placeholder the server is
// expected to replace, not a real descriptor being round-tripped, so
// there's no live-fetched reference value to match beyond what's actually
// claimed. NOT yet confirmed live.
[[nodiscard]] std::vector<std::byte> encodeSimpleLobPlaceholder(std::uint32_t contentLength);

} // namespace ifxsqli
