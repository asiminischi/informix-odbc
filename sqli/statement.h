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

    [[nodiscard]] const std::vector<ColumnDesc>& columns() const { return columns_; }
    [[nodiscard]] int statementType() const { return statementType_; }

    // See SqliConnection::lastSerialInserted's doc comment for the same
    // caveat (confirmed live to read back as 0 for a plain INSERT; only
    // meaningful with the SGK mechanism this driver doesn't send yet).
    [[nodiscard]] std::optional<std::int64_t> lastSerialInserted() const { return lastSerialInserted_; }
    [[nodiscard]] std::optional<std::int64_t> lastBigSerialInserted() const { return lastBigSerialInserted_; }

    // Opens the cursor for a SELECT-shaped prepared statement.
    [[nodiscard]] std::expected<void, Error> execute();

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
    std::vector<ColumnDesc> columns_;
    std::string cursorName_;
    bool cursorOpen_ = false;
    bool typeSent_ = false;
    bool exhausted_ = false;
    std::vector<std::vector<std::vector<std::byte>>> pendingRows_;
    std::optional<std::int64_t> lastSerialInserted_;
    std::optional<std::int64_t> lastBigSerialInserted_;
};

} // namespace ifxsqli
