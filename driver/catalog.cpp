// SQLTables/SQLColumns: catalog introspection, implemented by synthesizing
// a real SQL query against Informix's system catalog (systables/
// syscolumns) and running it through the same prepare/execute/fetch
// pipeline every other statement uses (driver/statement.cpp's doPrepare/
// doExecute/SQLFetch) - not a separate in-memory result-set mechanism.
// SQLDescribeCol, SQLBindCol, SQLGetData, SQLNumResultCols all work
// unchanged once the query is executed, since as far as they're concerned
// this is just a SELECT like any other.
//
// Scope (matches STATUS.md's "SQLTables/SQLColumns first" priority):
// covers the plain "list what matches these filters" case that isql's
// `help` and ordinary schema browsing use - catalog/schema/
// table-name filtering via LIKE patterns, and table-type filtering via an
// IN list. NOT implemented: the ODBC "special value" enumeration modes
// (CatalogName="%" alone to list catalogs, SchemaName="%" alone to list
// schemas) - Informix's model (one connected database acting as the sole
// catalog, "owner" acting as schema) makes these low-value here, and nothing
// in this project's verification bar (isql, a generic schema browser doing
// ordinary browsing) exercises them. A caller relying on that specific
// enumeration trick will get an empty result instead, not a crash.
//
// SQLTables is implemented and confirmed live (tools/odbc_dm_probe's
// runCatalogTest through the real Driver Manager, plus isql's `help`) -
// see docs/protocol-notes.md's Catalog functions section. SQLColumns is
// not implemented yet - `help <table>` calls SQLColumns, not SQLTables,
// and fails accordingly ("Could not SQLColumns") until it lands.

#include "driver/error_map.h"
#include "driver/handles.h"
#include "sqli/types.h"

#include <algorithm>
#include <cctype>

using namespace ifxodbc;

namespace {

std::string toStdString(SQLCHAR* text, SQLSMALLINT length) {
    if (!text) return {};
    if (length == SQL_NTS) return reinterpret_cast<const char*>(text);
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(length));
}

// ODBC catalog-function convention: a null pointer or empty string for a
// filter argument means "don't filter on this," which this driver
// implements as a match-everything LIKE pattern rather than special-casing
// SQL generation per argument.
std::string toPattern(SQLCHAR* text, SQLSMALLINT length) {
    std::string s = toStdString(text, length);
    return s.empty() ? "%" : s;
}

ifxsqli::BindParam charParam(const std::string& s) {
    return ifxsqli::BindParam{ifxsqli::ifxtype::kVarchar, false, ifxsqli::encodeChar(s),
                               static_cast<std::uint16_t>(s.size())};
}

// Splits TableType ("TABLE,VIEW" or "'TABLE','VIEW'") into normalized,
// deduplication-free tokens (trimmed, unquoted, uppercased) for an IN-list
// filter against the TABLE_TYPE this driver computes (see buildTablesSql).
// Empty input or a bare "%" means "no filter" - returns an empty vector,
// which callers treat as "don't add a WHERE clause" rather than "match
// nothing."
std::vector<std::string> parseTableTypes(SQLCHAR* text, SQLSMALLINT length) {
    std::string raw = toStdString(text, length);
    if (raw.empty() || raw == "%") return {};

    std::vector<std::string> result;
    std::size_t pos = 0;
    while (pos <= raw.size()) {
        std::size_t comma = raw.find(',', pos);
        std::string token = raw.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? raw.size() + 1 : comma + 1;

        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        auto begin = std::find_if(token.begin(), token.end(), notSpace);
        auto end = std::find_if(token.rbegin(), token.rend(), notSpace).base();
        if (begin >= end) continue;
        token = std::string(begin, end);
        if (token.size() >= 2 && token.front() == '\'' && token.back() == '\'') {
            token = token.substr(1, token.size() - 2);
        }
        for (char& c : token) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (!token.empty()) result.push_back(std::move(token));
    }
    return result;
}

// Runs a synthesized catalog query the same way doPrepare+the cursor branch
// of doExecute (driver/statement.cpp) would, but bypassing the app-facing
// SQLBindParameter/paramBindings path entirely - `params` is built directly
// by the caller, not read from `s->paramBindings` (which holds whatever the
// app itself bound on this handle, if anything, and is irrelevant here).
// Deliberately does not participate in a manual-commit transaction the way
// doExecute's DML/SELECT path does (no lazy SQ_BEGIN): catalog reads are
// metadata, not data the caller would ever need to roll back, so this
// stays out of whatever transaction the app has open - confirmed live to
// work the same under SQL_ATTR_AUTOCOMMIT_OFF as under ON.
SQLRETURN runCatalogQuery(OdbcStatement* s, const std::string& sql, const std::vector<ifxsqli::BindParam>& params) {
    if (!s->conn->connected) {
        postDriverError(s->diag, "08003", "connection is not open");
        return SQL_ERROR;
    }
    auto prepared = ifxsqli::SqliStatement::prepare(*s->conn->sqli, sql);
    if (!prepared) {
        postSqliError(s->diag, prepared.error());
        return SQL_ERROR;
    }
    s->stmt.emplace(std::move(*prepared));
    s->boundColumns.clear();
    s->paramBindings.clear();
    s->currentRow.reset();
    s->state = StmtState::kPrepared;

    auto result = s->stmt->execute(params);
    if (!result) {
        postSqliError(s->diag, result.error());
        return SQL_ERROR;
    }
    s->state = StmtState::kExecuted;
    return SQL_SUCCESS;
}

// TABLE_CAT is a fixed literal (the connected database) rather than a bound
// "?": whether a "?" is even legal as a bare SELECT-list expression (with
// no surrounding operator to infer a type from) is untested and plausibly
// unsupported, and there's no need to risk it for a value that's already
// known and trusted (it came from this connection's own DriverConnect
// call, not from the SQLTables caller). Single quotes are escaped in case
// a database name ever contains one, defense in depth rather than an
// expected case.
std::string catalogLiteral(const std::string& database) {
    if (database.empty()) return "cast(null as varchar(128))";
    std::string escaped;
    escaped.reserve(database.size() + 2);
    escaped += '\'';
    for (char c : database) {
        if (c == '\'') escaped += '\'';
        escaped += c;
    }
    escaped += '\'';
    return escaped;
}

// Builds the SQLTables query text and its WHERE-clause bind params.
// TABLE_TYPE is computed in an inner derived table (systables.tabtype/
// tabid alone) so the outer query can filter on it by name - a bare SQL
// SELECT can't reference its own alias in the same level's WHERE clause.
// tabid <= 99 -> SYSTEM TABLE is the standard Informix convention (every
// table with tabid in that range is one of Informix's own system catalog
// tables, confirmed live - see docs/protocol-notes.md); tabtype 'T' above
// that range is an ordinary user table, 'V' is a view. Rows with any other
// tabtype (confirmed live: a handful of internal pseudo-rows like
// " GL_COLLATE"/" VERSION" carry a blank tabtype) are excluded by the
// `tabtype in ('T', 'V')` filter, not surfaced as an unknown table type.
//
// Both `owner` and the CASE expression are wrapped in trim(): confirmed
// live that systables.owner is a space-padded fixed-width CHAR column (not
// VARCHAR), and separately that Informix infers the CASE expression's
// result type as a fixed CHAR sized to its widest branch ('SYSTEM TABLE',
// 12 characters) - a 'TABLE'/'VIEW' row comes back padded to that same
// width. Left untrimmed, TABLE_SCHEM/TABLE_TYPE would carry trailing
// spaces no ODBC caller expects (a straightforward, non-wildcard
// TableType='TABLE' filter would silently match nothing). tabname itself
// is confirmed live to need no such trim (already VARCHAR).
std::pair<std::string, std::vector<ifxsqli::BindParam>> buildTablesSql(const std::string& database,
                                                                        const std::string& schemaPattern,
                                                                        const std::string& tablePattern,
                                                                        const std::vector<std::string>& tableTypes,
                                                                        bool catalogMatches) {
    std::string sql = "select " + catalogLiteral(database) +
                       " as table_cat, table_schem, table_name, table_type,"
                       " cast(null as varchar(254)) as remarks"
                       " from (select trim(owner) as table_schem, tabname as table_name,"
                       " trim(case when tabtype = 'V' then 'VIEW'"
                       " when tabid <= 99 then 'SYSTEM TABLE'"
                       " else 'TABLE' end) as table_type"
                       " from systables where tabtype in ('T', 'V')) t"
                       " where table_name like ? escape '\\'"
                       " and table_schem like ? escape '\\'";

    std::vector<ifxsqli::BindParam> params;
    params.push_back(charParam(tablePattern));
    params.push_back(charParam(schemaPattern));

    if (!tableTypes.empty()) {
        sql += " and table_type in (";
        for (std::size_t i = 0; i < tableTypes.size(); ++i) {
            sql += (i == 0) ? "?" : ", ?";
            params.push_back(charParam(tableTypes[i]));
        }
        sql += ")";
    }
    // Single-catalog driver: a CatalogName that doesn't match the connected
    // database can't match anything, so short-circuit to an always-false
    // filter rather than pretending to search a catalog this connection
    // was never opened against. Still runs the real query (same column set
    // gets described either way) rather than fabricating a separate
    // empty-result-set path.
    if (!catalogMatches) sql += " and 1 = 0";
    sql += " order by table_type, table_schem, table_name";
    return {sql, params};
}

} // namespace

extern "C" {

IFXODBC_EXPORT SQLRETURN SQL_API SQLTables(SQLHSTMT StatementHandle, SQLCHAR* CatalogName, SQLSMALLINT NameLength1,
                                            SQLCHAR* SchemaName, SQLSMALLINT NameLength2, SQLCHAR* TableName,
                                            SQLSMALLINT NameLength3, SQLCHAR* TableType, SQLSMALLINT NameLength4) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();

    std::string catalog = toStdString(CatalogName, NameLength1);
    std::string schemaPattern = toPattern(SchemaName, NameLength2);
    std::string tablePattern = toPattern(TableName, NameLength3);
    std::vector<std::string> tableTypes = parseTableTypes(TableType, NameLength4);

    bool catalogMatches = catalog.empty() || catalog == s->conn->database;
    auto [sql, params] = buildTablesSql(s->conn->database, schemaPattern, tablePattern, tableTypes, catalogMatches);

    return runCatalogQuery(s, sql, params);
}

} // extern "C"
