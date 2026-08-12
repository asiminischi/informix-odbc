#pragma once

#include "sqli/connection.h"
#include "sqli/error.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace ifxsqli {

struct ColumnDesc {
    std::string name;
    int ifxType = 0;       // base Informix type code (flag bits already masked off)
    bool notNull = false;
    std::uint32_t encodedLength = 0;
    // Extended/opaque-type identity (BOOLEAN, blobs, UDTs, ...), only
    // populated on wide (4-byte-offset) connections - DESCRIBE's wide branch
    // is the only place the wire sends these. extendedId == 0 means "not an
    // extended type" (ordinary scalar). Needed to echo the same identity
    // back in SQ_RETURN_TYPE - see SqliStatement::fetchNextBatch.
    std::uint32_t extendedId = 0;
    std::string extendedOwnerName;
    std::string extendedTypeName;
};

// One "?" parameter's already-encoded value, ready for SQ_BIND. Building
// this (choosing ifxType, encoding `data`, picking `precisionField`) is the
// driver layer's job (driver/convert_native.h's mirror-image encoder,
// analogous to formatCellAsText's decode direction) - SqliStatement::execute
// just serializes whatever it's given. `data` is empty and unused when
// `isNull` (SQ_BIND signals NULL via the indicator field alone - see
// encodeInteger's doc comment in sqli/types.h).
//
// extendedOwnerName/extendedTypeName: only needed for extended/opaque types
// (BOOLEAN and friends) - confirmed live that omitting them for a BOOLEAN
// bind hangs the connection (the server waits for bytes that never come,
// the same "still-missing request field" failure mode documented in
// docs/protocol-notes.md). When set, SqliStatement::execute writes them as
// two ifxChar2 fields right after the type code, before the indicator -
// see the decompiled IfxSqli.sendBind's `isUSVER()`-gated branch. `ifxType`
// for an extended-type bind is the *remapped* code (e.g. 45 for BOOLEAN,
// matching what SQ_RETURN_TYPE needs - see ifxtype::kBooleanBindType),
// not the plain DESCRIBE/TUPLE base type code.
struct BindParam {
    int ifxType = 0;
    bool isNull = false;
    std::vector<std::byte> data;
    std::uint16_t precisionField = 0;
    std::optional<std::string> extendedOwnerName;
    std::optional<std::string> extendedTypeName;
};

// One prepared statement's cursor lifecycle: prepare -> execute (opens a
// cursor) -> repeated fetchRow() -> close -> release. Phase 2 scope: no bind
// parameters, forward-only cursor, and only the scalar types needed to prove
// the round trip (full type support is Phase 3). See
// docs/protocol-notes.md for the exact wire sequence this implements,
// confirmed against a live server capture.
class SqliStatement {
public:
    [[nodiscard]] static std::expected<SqliStatement, Error> prepare(SqliConnection& conn, const std::string& sql);

    // Real output columns, for a SELECT-shaped statement only. Confirmed
    // live (Phase 5, first bind-parameter slice): DESCRIBE's "nfields"/
    // per-field records describe *input parameter* placeholders instead of
    // output columns whenever the statement has "?" markers and isn't a
    // SELECT (e.g. a 1-parameter INSERT reports nfields=1) - so
    // columns_.empty() alone is not a reliable way to detect "no result
    // set" once bind parameters are in play (that was true only by
    // coincidence before this slice, when no non-SELECT statement ever had
    // parameters to report). This accessor hides that internal storage
    // reuse from every caller (SQLNumResultCols/SQLDescribeCol/
    // SQLColAttribute/SQLRowCount) by returning empty unless
    // hasResultSet().
    [[nodiscard]] const std::vector<ColumnDesc>& columns() const {
        static const std::vector<ColumnDesc> kEmpty;
        return hasResultSet() ? columns_ : kEmpty;
    }
    [[nodiscard]] int statementType() const { return statementType_; }

    // True for a SELECT (SQ_SELECT, confirmed via the decompiled JDBC
    // driver's IfxStatementTypes constants against a live DESCRIBE
    // response) - the only statement shape this driver treats as having a
    // real result set. See columns()'s doc comment for why this can't be
    // inferred from columns_.empty() once parameters are involved.
    [[nodiscard]] bool hasResultSet() const { return statementType_ == 2; }

    // Count of "?" markers found in the SQL text at prepare() time (outside
    // quoted string literals) - sent as SQ_PREPARE's numqmarks field, and
    // what the driver layer validates SQLBindParameter/SQLExecute calls
    // against.
    [[nodiscard]] std::size_t paramCount() const { return paramCount_; }

    // See SqliConnection::lastSerialInserted's doc comment for the same
    // caveat (confirmed live to read back as 0 for a plain INSERT; only
    // meaningful with the SGK mechanism this driver doesn't send yet).
    [[nodiscard]] std::optional<std::int64_t> lastSerialInserted() const { return lastSerialInserted_; }
    [[nodiscard]] std::optional<std::int64_t> lastBigSerialInserted() const { return lastBigSerialInserted_; }

    // SQ_DONE's row-count field, refreshed by execute(). Only meaningful for
    // statements with no output columns (INSERT/UPDATE/DELETE) - for a
    // SELECT-shaped statement (columns() non-empty), the server's DONE here
    // is the cursor-open acknowledgement, not a real affected-row count
    // (mirrors the decompiled JDBC driver's own statementType-based
    // distinction between "has a result set" and "has an update count").
    [[nodiscard]] std::uint32_t rowCount() const { return rowCount_; }

    // Binds `params` (must match paramCount() in size - the caller, not
    // this function, is responsible for validating that) via SQ_BIND, then
    // opens the cursor (SELECT-shaped statements) or runs SQ_EXECUTE
    // directly (everything else - see the columns_.empty() branch below).
    [[nodiscard]] std::expected<void, Error> execute(const std::vector<BindParam>& params = {});

    // Returns the next row (one raw byte buffer per column, in column
    // order), or nullopt once the cursor is exhausted. Fetches a fresh
    // server-side batch transparently when the local buffer runs dry.
    [[nodiscard]] std::expected<std::optional<std::vector<std::vector<std::byte>>>, Error> fetchRow();

    [[nodiscard]] std::expected<void, Error> close();
    [[nodiscard]] std::expected<void, Error> release();

private:
    explicit SqliStatement(SqliConnection& conn) : conn_(&conn) {}

    [[nodiscard]] std::expected<void, Error> fetchNextBatch();
    [[nodiscard]] std::expected<bool, Error> processResponseBatch();

    SqliConnection* conn_;
    int statementType_ = 0;
    std::uint16_t statementId_ = 0;
    std::size_t paramCount_ = 0;
    std::vector<ColumnDesc> columns_;
    std::string cursorName_;
    bool cursorOpen_ = false;
    bool typeSent_ = false;
    bool exhausted_ = false;
    std::vector<std::vector<std::vector<std::byte>>> pendingRows_;
    std::optional<std::int64_t> lastSerialInserted_;
    std::optional<std::int64_t> lastBigSerialInserted_;
    std::uint32_t rowCount_ = 0;
};

} // namespace ifxsqli
