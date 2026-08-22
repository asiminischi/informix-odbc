#pragma once

#include <optional>
#include <string>

namespace ifxodbc {

// Narrow "insert into <table>" scan of a prepared SQL statement's text -
// the one piece of information a smart-LOB "?" bind needs that
// SqliStatement::paramDescs() can't supply: DESCRIBE's per-parameter reply
// carries the target column's real name, but not its table (see
// paramDescs's doc comment in sqli/statement.h) - and sqli::SqliStatement
// doesn't retain the SQL text itself once prepared, so this driver has to
// parse it out of the text it already has on hand
// (driver/handles.h's OdbcStatement::preparedSql).
//
// Deliberately narrow: recognizes only a leading (whitespace-tolerant,
// case-insensitive) "insert into <identifier>", where <identifier> is
// whatever non-whitespace, non-'(' text follows - which naturally covers
// a plain table name, an owner-qualified name ("owner.table"), and an
// Informix database-qualified name ("database:owner.table" or
// "database@server:owner.table") without needing to enumerate every
// separator character Informix's own identifier grammar allows, since
// none of them are whitespace or '('. Returns nullopt for anything else
// (UPDATE, multi-table statements, a quoted identifier containing a
// space, no INSERT INTO at all, ...) rather than guessing - callers must
// treat that as "can't determine the target table", not "no smart LOB
// involved".
[[nodiscard]] std::optional<std::string> parseInsertTableName(const std::string& sql);

} // namespace ifxodbc
