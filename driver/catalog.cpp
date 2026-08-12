// SQLTables/SQLColumns/SQLGetTypeInfo: catalog introspection, implemented by synthesizing
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
// `help`/`help <table>` and ordinary schema browsing use - catalog/schema/
// table-name/column-name filtering via LIKE patterns, and table-type
// filtering via an IN list. NOT implemented: the ODBC "special value"
// enumeration modes (CatalogName="%" alone to list catalogs, SchemaName="%"
// alone to list schemas) - Informix's model (one connected database acting
// as the sole catalog, "owner" acting as schema) makes these low-value
// here, and nothing in this project's verification bar (isql, a generic
// schema browser doing ordinary browsing) exercises them. A caller relying
// on that specific enumeration trick will get an empty result instead of a
// crash. Also not implemented: COLUMN_DEF (default-value introspection) -
// always NULL, regardless of whether the column actually has a default.
// Simplification: BUFFER_LENGTH reuses COLUMN_SIZE's value verbatim (see
// buildColumnsSql) rather than computing an actual transfer-size-in-bytes
// figure (e.g. 4 for INTEGER, not 10). This is an advisory field most
// callers don't consult; fine for MVP scope but not independently verified.
//
// SQLTables, SQLColumns, and SQLGetTypeInfo are implemented and confirmed
// live through the real Driver Manager (tools/odbc_dm_probe's
// runCatalogTest/runColumnsTest/runTypeInfoTest) - for the first two, also
// via isql's `help`/`help <table>`, which SQLGetTypeInfo has no isql-native
// invocation path to exercise. See docs/protocol-notes.md's Catalog
// functions section.

#include "driver/error_map.h"
#include "driver/handles.h"
#include "sqli/types.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <optional>

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

// Per-base-type metadata for SQLColumns' DATA_TYPE/TYPE_NAME/NUM_PREC_RADIX/
// SQL_DATA_TYPE/SQL_DATETIME_SUB columns, generating one SQL CASE expression
// per field from a single table rather than hand-writing five parallel CASE
// blocks that could drift against each other. Deliberately a separate table
// from driver/statement.cpp's sqlTypeFor (same shape, not shared) - see that
// function's neighboring precedent (driver/bind_encode.cpp's
// unixDaysFromDateStruct) for why a small, stable mapping like this is kept
// as its own copy rather than threaded through a new shared header.
// radix=0 and sqlDatetimeSub=0 both mean "NULL in the generated SQL," not
// literal 0 - see caseInt/caseRadix's `v == 0 ? "null" : ...` checks.
struct TypeMapEntry {
    int ifxBaseType;
    SQLSMALLINT dataType;
    SQLSMALLINT sqlDataType;
    SQLSMALLINT sqlDatetimeSub;
    const char* typeName;
    int radix;
};

constexpr TypeMapEntry kTypeMap[] = {
    {ifxsqli::ifxtype::kSmallInt, SQL_SMALLINT, SQL_SMALLINT, 0, "smallint", 10},
    {ifxsqli::ifxtype::kInteger, SQL_INTEGER, SQL_INTEGER, 0, "integer", 10},
    {ifxsqli::ifxtype::kSerial, SQL_INTEGER, SQL_INTEGER, 0, "serial", 10},
    {ifxsqli::ifxtype::kBigInt, SQL_BIGINT, SQL_BIGINT, 0, "bigint", 10},
    {ifxsqli::ifxtype::kBigSerial, SQL_BIGINT, SQL_BIGINT, 0, "bigserial", 10},
    {ifxsqli::ifxtype::kChar, SQL_CHAR, SQL_CHAR, 0, "char", 0},
    {ifxsqli::ifxtype::kVarchar, SQL_VARCHAR, SQL_VARCHAR, 0, "varchar", 0},
    {ifxsqli::ifxtype::kNVarchar, SQL_VARCHAR, SQL_VARCHAR, 0, "nvarchar", 0},
    {ifxsqli::ifxtype::kDecimal, SQL_DECIMAL, SQL_DECIMAL, 0, "decimal", 10},
    {ifxsqli::ifxtype::kDate, SQL_TYPE_DATE, SQL_DATETIME, SQL_CODE_DATE, "date", 0},
    {ifxsqli::ifxtype::kDatetime, SQL_TYPE_TIMESTAMP, SQL_DATETIME, SQL_CODE_TIMESTAMP, "datetime", 0},
};

// `NULL` is not a valid substitute for 0 in the two fields (radix,
// sqlDatetimeSub) that use 0 as their sentinel: SQL_NO_NULLS is 0
// elsewhere in this file's generated SQL, so an integer CASE that means
// "NULL for this branch" has to say so explicitly rather than emitting a
// bare 0.
// Every generated integer CASE is wrapped in `cast(... as integer)`:
// confirmed live that Informix infers a CASE expression's result type from
// *all* its branches together, and a single branch built from trunc()/
// mod() (see caseColumnSize's DECIMAL branch and buildColumnsSql's
// decimal_digits expression) gets typed DECIMAL rather than INTEGER -
// which then contaminates the whole CASE, not just that one branch,
// producing a DECIMAL column this driver's SQL_C_SSHORT/SQL_C_SLONG
// fetch path doesn't accept (07006 "restricted data type attribute
// violation" - the same DECIMAL-vs-integer-family gap
// tests/integration/test_transactions.cpp's countRows and this project's
// own tools/odbc_dm_probe hit earlier for count(*), which is DECIMAL on
// this server for the same reason). The explicit CAST forces the driver-
// visible type regardless of what any individual branch would otherwise
// infer to.
std::string caseInt(std::string_view label, SQLSMALLINT TypeMapEntry::*field, std::string_view elseSql) {
    std::string sql = "cast(case basetype";
    for (const auto& e : kTypeMap) {
        SQLSMALLINT v = e.*field;
        sql += " when " + std::to_string(e.ifxBaseType) + " then " + (v == 0 ? "null" : std::to_string(v));
    }
    sql += " else " + std::string(elseSql) + " end as integer) as " + std::string(label);
    return sql;
}

std::string caseRadix() {
    std::string sql = "cast(case basetype";
    for (const auto& e : kTypeMap) {
        sql += " when " + std::to_string(e.ifxBaseType) + " then " + (e.radix == 0 ? "null" : std::to_string(e.radix));
    }
    sql += " else null end as integer) as num_prec_radix";
    return sql;
}

// Wrapped in trim(): confirmed live (same finding as buildTablesSql's
// TABLE_TYPE) that Informix infers a CASE-of-string-literals' result type
// as CHAR sized to the widest branch ('bigserial', 9 characters) and pads
// shorter results ('smallint') to that width.
std::string caseTypeName() {
    std::string sql = "trim(case basetype";
    for (const auto& e : kTypeMap) {
        sql += " when " + std::to_string(e.ifxBaseType) + " then '" + e.typeName + "'";
    }
    // Matches driver/statement.cpp's sqlTypeFor fallback: an Informix type
    // this driver doesn't otherwise model (FLOAT/SMALLFLOAT/BOOLEAN/
    // INTERVAL/LOBs/opaque types, all outside the MVP-8 set) still shows
    // up as a column rather than being silently dropped from the result.
    sql += " else 'unknown' end) as type_name";
    return sql;
}

// COLUMN_SIZE's meaning is type-specific (see driver/statement.cpp's
// columnSizeFor, which this mirrors exactly for the constant-width types):
// declared digit count for the integer family, the declared width for
// CHAR/VARCHAR (collength itself), precision for DECIMAL (collength's high
// byte - confirmed live that syscolumns.collength packs
// (precision << 8) | scale for DECIMAL, the same convention this driver's
// wire-level DESCRIBE decoding already relies on), and a fixed display
// width for DATE/DATETIME. DATETIME's 19 assumes YEAR TO SECOND, the only
// qualifier this driver's bind/fetch paths support elsewhere - a column
// with a different qualifier gets an approximate value here rather than
// decoding collength's packed qualifier bits for a display-width formula,
// same MVP-scope choice already made for bind/fetch.
// Wrapped in cast(... as integer) for the same reason caseInt/caseRadix
// are: the DECIMAL branch (trunc(collength / 256)) would otherwise type
// the whole expression as DECIMAL, not INTEGER - see caseInt's comment.
std::string caseColumnSize() {
    return "cast(case basetype"
           " when " +
           std::to_string(ifxsqli::ifxtype::kSmallInt) +
           " then 5"
           " when " +
           std::to_string(ifxsqli::ifxtype::kInteger) +
           " then 10"
           " when " +
           std::to_string(ifxsqli::ifxtype::kSerial) +
           " then 10"
           " when " +
           std::to_string(ifxsqli::ifxtype::kBigInt) +
           " then 19"
           " when " +
           std::to_string(ifxsqli::ifxtype::kBigSerial) +
           " then 19"
           " when " +
           std::to_string(ifxsqli::ifxtype::kChar) +
           " then collength"
           " when " +
           std::to_string(ifxsqli::ifxtype::kVarchar) +
           " then collength"
           " when " +
           std::to_string(ifxsqli::ifxtype::kNVarchar) +
           " then collength"
           " when " +
           std::to_string(ifxsqli::ifxtype::kDecimal) +
           " then trunc(collength / 256)"
           " when " +
           std::to_string(ifxsqli::ifxtype::kDate) +
           " then 10"
           " when " +
           std::to_string(ifxsqli::ifxtype::kDatetime) +
           " then 19"
           " else null end as integer)";
}

// Builds the SQLColumns query text and its WHERE-clause bind params.
// syscolumns.coltype packs the NOT NULL flag as bit 0x100 over the base
// type (confirmed live - see docs/protocol-notes.md's SQ_BIND notes,
// which rely on the same encoding at the wire level), so `basetype` and
// `nullable_flag` are split out in the same inner-derived-table shape
// buildTablesSql uses, for the same reason: an outer CASE can't reference
// another outer CASE's own alias, only a derived table's.
std::pair<std::string, std::vector<ifxsqli::BindParam>> buildColumnsSql(const std::string& database,
                                                                         const std::string& schemaPattern,
                                                                         const std::string& tablePattern,
                                                                         const std::string& columnPattern,
                                                                         bool catalogMatches) {
    std::string columnSize = caseColumnSize();
    std::string sql =
        "select " + catalogLiteral(database) +
        " as table_cat, table_schem, table_name, column_name, " +
        caseInt("data_type", &TypeMapEntry::dataType, std::to_string(SQL_VARCHAR)) +
        ", " + caseTypeName() + ", (" + columnSize + ") as column_size, (" + columnSize +
        ") as buffer_length,"
        " cast(case when basetype = " +
        std::to_string(ifxsqli::ifxtype::kDecimal) +
        " then mod(collength, 256) else null end as integer) as decimal_digits,"
        " " +
        caseRadix() +
        ","
        " nullable_flag as nullable,"
        " cast(null as varchar(254)) as remarks,"
        " cast(null as varchar(254)) as column_def,"
        " " +
        caseInt("sql_data_type", &TypeMapEntry::sqlDataType, std::to_string(SQL_VARCHAR)) + ", " +
        caseInt("sql_datetime_sub", &TypeMapEntry::sqlDatetimeSub, "null") +
        ","
        " case when basetype in (" +
        std::to_string(ifxsqli::ifxtype::kChar) + ", " + std::to_string(ifxsqli::ifxtype::kVarchar) + ", " +
        std::to_string(ifxsqli::ifxtype::kNVarchar) +
        ") then collength else null end as char_octet_length,"
        " colno as ordinal_position,"
        " trim(case when nullable_flag = 1 then 'YES' else 'NO' end) as is_nullable"
        " from ("
        " select trim(t.owner) as table_schem, t.tabname as table_name, c.colname as column_name,"
        " case when c.coltype >= 256 then c.coltype - 256 else c.coltype end as basetype,"
        " c.collength as collength, c.colno as colno,"
        " case when c.coltype >= 256 then 0 else 1 end as nullable_flag"
        " from systables t join syscolumns c on t.tabid = c.tabid"
        " where t.tabtype in ('T', 'V')"
        " ) x"
        " where table_name like ? escape '\\'"
        " and table_schem like ? escape '\\'"
        " and column_name like ? escape '\\'";

    std::vector<ifxsqli::BindParam> params{charParam(tablePattern), charParam(schemaPattern), charParam(columnPattern)};

    if (!catalogMatches) sql += " and 1 = 0";
    sql += " order by table_schem, table_name, ordinal_position";
    return {sql, params};
}

// SQLGetTypeInfo: one row per entry in kTypeMap (the same table SQLColumns'
// CASE expressions read from), dispatched via CASE over a `rank` column
// (0-based index into kTypeMap) the same way buildTablesSql/buildColumnsSql
// dispatch over `basetype` - not a UNION ALL of literal rows.
//
// A UNION-specific variant of the CHAR-padding quirk documented above was
// hit and rejected here: confirmed live that Informix infers a UNION ALL's
// string column as a fixed CHAR sized to the widest literal branch and
// blank-pads every row to that width, and unlike the CASE-expression
// version of this quirk, wrapping the result in trim() does *not* strip it
// back out - the value is genuinely stored/returned padded, not just
// displayed that way. Casting each branch to lvarchar instead of varchar
// does avoid the padding, but lvarchar is a wire type this driver's fetch
// path has no decoder for at all, which desyncs the byte stream and hangs
// the connection exactly as docs/protocol-notes.md warns a wrong field
// width will - confirmed live (a standalone SQLGetTypeInfo call, no other
// statement involved, hung forever inside its own SQLFetch loop the moment
// a returned column was lvarchar-typed). Do not reintroduce a UNION ALL of
// string literals here, and do not cast to lvarchar anywhere in this
// driver until fetch-side decoding for it exists.
//
// `rank` is computed as a correlated-subquery row count (how many other
// systables rows with tabid <= 99 have a smaller tabid), not a literal
// tabid value or a window function (ROW_NUMBER, unused elsewhere in this
// project) - gives a stable, gapless 0..N-1 sequence over any Informix
// instance's system catalog without assuming specific tabid values are
// present, only that at least 11 rows with tabid <= 99 exist (already
// relied on elsewhere in this file as "the system table range").
//
// Scope/simplifications, none independently verified against a real ODBC
// conformance trace (nothing in this project's verification bar - isql,
// ordinary schema browsing - exercises SQLGetTypeInfo specifically):
// NULLABLE is always SQL_NULLABLE (describes what the *type* permits, not
// any particular column's declared nullability) and SEARCHABLE is always
// SQL_SEARCHABLE for every type. DECIMAL's MINIMUM_SCALE/MAXIMUM_SCALE
// (0/32) approximate Informix's documented max precision rather than being
// read from the server. INTERVAL_PRECISION is always NULL (no INTERVAL
// support - out of MVP-8 scope, same gap noted elsewhere in this file).
std::string sqlSmallInt(int v) { return "cast(" + std::to_string(v) + " as smallint)"; }
std::string sqlNullSmallInt() { return "cast(null as smallint)"; }
std::string sqlQuotedLiteral(const std::string& s) {
    std::string escaped = "'";
    for (char c : s) {
        if (c == '\'') escaped += '\'';
        escaped += c;
    }
    escaped += '\'';
    return escaped;
}

// COLUMN_SIZE per type mirrors caseColumnSize()'s values for the constant-
// width types; CHAR/VARCHAR/NVARCHAR use Informix's documented maximum
// declared width (32767 for CHAR, 255 for VARCHAR/NVARCHAR) rather than
// collength, since there's no specific column here to read collength from.
int typeInfoColumnSize(int ifxBaseType) {
    switch (ifxBaseType) {
        case ifxsqli::ifxtype::kSmallInt: return 5;
        case ifxsqli::ifxtype::kInteger:
        case ifxsqli::ifxtype::kSerial: return 10;
        case ifxsqli::ifxtype::kBigInt:
        case ifxsqli::ifxtype::kBigSerial: return 19;
        case ifxsqli::ifxtype::kChar: return 32767;
        case ifxsqli::ifxtype::kVarchar:
        case ifxsqli::ifxtype::kNVarchar: return 255;
        case ifxsqli::ifxtype::kDecimal: return 32;
        case ifxsqli::ifxtype::kDate: return 10;
        case ifxsqli::ifxtype::kDatetime: return 19;
        default: return 0;
    }
}

// Builds a "cast(case rank when 0 then ... when 1 then ... else <elseSql>
// end as integer) as <label>" expression, one branch per kTypeMap entry -
// the same shape as caseInt(), but keyed on 0-based array index (`rank`)
// rather than an ifxBaseType value read from a live column. `field` returns
// nullopt for "this branch is NULL" - unlike caseInt's bare-0-means-null
// convention, several columns generated here (UNSIGNED_ATTRIBUTE,
// MINIMUM_SCALE, CASE_SENSITIVE) have 0 as a genuine, non-null value, so
// the sentinel has to be out-of-band, not a magic int.
std::string typeInfoCaseInt(std::string_view label, std::string_view elseSql,
                             const std::function<std::optional<int>(const TypeMapEntry&)>& field) {
    std::string sql = "cast(case rank";
    for (std::size_t i = 0; i < std::size(kTypeMap); ++i) {
        std::optional<int> v = field(kTypeMap[i]);
        sql += " when " + std::to_string(i) + " then " + (v ? std::to_string(*v) : "null");
    }
    sql += " else " + std::string(elseSql) + " end as integer) as " + std::string(label);
    return sql;
}

// `field` returns nullopt for "this branch is NULL" - LITERAL_PREFIX/
// LITERAL_SUFFIX/CREATE_PARAMS are genuinely absent (not just empty) for
// most types, and a bare empty-string literal would read back as a
// zero-length string, not SQL_NULL_DATA.
std::string typeInfoCaseString(std::string_view label,
                                const std::function<std::optional<std::string>(const TypeMapEntry&)>& field) {
    std::string sql = "trim(case rank";
    for (std::size_t i = 0; i < std::size(kTypeMap); ++i) {
        std::optional<std::string> v = field(kTypeMap[i]);
        sql += " when " + std::to_string(i) + " then " + (v ? sqlQuotedLiteral(*v) : "null");
    }
    sql += " else null end) as " + std::string(label);
    return sql;
}

std::pair<std::string, std::vector<ifxsqli::BindParam>> buildTypeInfoSql(SQLSMALLINT dataType) {
    auto isString = [](const TypeMapEntry& e) {
        return e.ifxBaseType == ifxsqli::ifxtype::kChar || e.ifxBaseType == ifxsqli::ifxtype::kVarchar ||
               e.ifxBaseType == ifxsqli::ifxtype::kNVarchar;
    };
    auto isDecimal = [](const TypeMapEntry& e) { return e.ifxBaseType == ifxsqli::ifxtype::kDecimal; };
    auto isDateOrTime = [](const TypeMapEntry& e) {
        return e.ifxBaseType == ifxsqli::ifxtype::kDate || e.ifxBaseType == ifxsqli::ifxtype::kDatetime;
    };
    auto isAutoIncrement = [](const TypeMapEntry& e) {
        return e.ifxBaseType == ifxsqli::ifxtype::kSerial || e.ifxBaseType == ifxsqli::ifxtype::kBigSerial;
    };
    auto isIntegerFamily = [&](const TypeMapEntry& e) {
        return !isString(e) && !isDecimal(e) && !isDateOrTime(e);
    };

    std::vector<int> matchingRanks;
    for (std::size_t i = 0; i < std::size(kTypeMap); ++i) {
        if (dataType == SQL_ALL_TYPES || kTypeMap[i].dataType == dataType) matchingRanks.push_back(static_cast<int>(i));
    }

    std::string sql =
        "select " + typeInfoCaseString("type_name", [](const TypeMapEntry& e) { return e.typeName; }) + ", " +
        typeInfoCaseInt("data_type", "null", [](const TypeMapEntry& e) { return std::optional<int>(e.dataType); }) +
        ", " +
        typeInfoCaseInt("column_size", "null",
                         [](const TypeMapEntry& e) { return std::optional<int>(typeInfoColumnSize(e.ifxBaseType)); }) +
        ", " +
        typeInfoCaseString("literal_prefix",
                            [&](const TypeMapEntry& e) -> std::optional<std::string> {
                                return (isString(e) || isDateOrTime(e)) ? std::optional<std::string>("'") : std::nullopt;
                            }) +
        ", " +
        typeInfoCaseString("literal_suffix",
                            [&](const TypeMapEntry& e) -> std::optional<std::string> {
                                return (isString(e) || isDateOrTime(e)) ? std::optional<std::string>("'") : std::nullopt;
                            }) +
        ", " +
        typeInfoCaseString("create_params",
                            [&](const TypeMapEntry& e) -> std::optional<std::string> {
                                if (isString(e)) return "max length";
                                if (isDecimal(e)) return "precision,scale";
                                return std::nullopt;
                            }) +
        ", " + sqlSmallInt(SQL_NULLABLE) + " as nullable, " +
        typeInfoCaseInt("case_sensitive", "null",
                         [&](const TypeMapEntry& e) { return std::optional<int>(isString(e) ? 1 : 0); }) +
        ", " + sqlSmallInt(SQL_SEARCHABLE) + " as searchable, " +
        typeInfoCaseInt("unsigned_attribute", "null",
                         [&](const TypeMapEntry& e) -> std::optional<int> {
                             if (!isIntegerFamily(e) && !isDecimal(e)) return std::nullopt;
                             return 0;
                         }) +
        ", " +
        typeInfoCaseInt("fixed_prec_scale", "null",
                         [&](const TypeMapEntry& e) { return std::optional<int>(isDecimal(e) ? 1 : 0); }) +
        ", " +
        typeInfoCaseInt("auto_unique_value", "null",
                         [&](const TypeMapEntry& e) -> std::optional<int> {
                             return isAutoIncrement(e) ? std::optional<int>(1) : std::nullopt;
                         }) +
        ", " + typeInfoCaseString("local_type_name", [](const TypeMapEntry& e) { return e.typeName; }) + ", " +
        typeInfoCaseInt("minimum_scale", "null",
                         [&](const TypeMapEntry& e) -> std::optional<int> {
                             return isDecimal(e) ? std::optional<int>(0) : std::nullopt;
                         }) +
        ", " +
        typeInfoCaseInt("maximum_scale", "null",
                         [&](const TypeMapEntry& e) -> std::optional<int> {
                             return isDecimal(e) ? std::optional<int>(32) : std::nullopt;
                         }) +
        ", " +
        typeInfoCaseInt("sql_data_type", "null",
                         [](const TypeMapEntry& e) { return std::optional<int>(e.sqlDataType); }) +
        ", " +
        typeInfoCaseInt("sql_datetime_sub", "null",
                         [](const TypeMapEntry& e) -> std::optional<int> {
                             return e.sqlDatetimeSub == 0 ? std::nullopt : std::optional<int>(e.sqlDatetimeSub);
                         }) +
        ", " +
        typeInfoCaseInt("num_prec_radix", "null",
                         [](const TypeMapEntry& e) -> std::optional<int> {
                             return e.radix == 0 ? std::nullopt : std::optional<int>(e.radix);
                         }) +
        ", " + sqlNullSmallInt() + " as interval_precision" +
        " from (select (select count(*) from systables s2 where s2.tabid <= 99 and s2.tabid < s1.tabid) as rank"
        " from systables s1 where s1.tabid <= 99) t"
        " where rank < " +
        std::to_string(std::size(kTypeMap));

    if (matchingRanks.empty()) {
        sql += " and 1 = 0";
    } else if (matchingRanks.size() != std::size(kTypeMap)) {
        sql += " and rank in (";
        for (std::size_t i = 0; i < matchingRanks.size(); ++i) {
            sql += (i == 0 ? "" : ", ") + std::to_string(matchingRanks[i]);
        }
        sql += ")";
    }
    sql += " order by data_type";
    return {sql, {}};
}

} // namespace

extern "C" {

IFXODBC_EXPORT SQLRETURN SQL_API SQLGetTypeInfo(SQLHSTMT StatementHandle, SQLSMALLINT DataType) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();

    auto [sql, params] = buildTypeInfoSql(DataType);
    return runCatalogQuery(s, sql, params);
}

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

IFXODBC_EXPORT SQLRETURN SQL_API SQLColumns(SQLHSTMT StatementHandle, SQLCHAR* CatalogName, SQLSMALLINT NameLength1,
                                             SQLCHAR* SchemaName, SQLSMALLINT NameLength2, SQLCHAR* TableName,
                                             SQLSMALLINT NameLength3, SQLCHAR* ColumnName, SQLSMALLINT NameLength4) {
    auto* s = static_cast<OdbcStatement*>(StatementHandle);
    if (!s) return SQL_INVALID_HANDLE;
    s->diag.clear();

    std::string catalog = toStdString(CatalogName, NameLength1);
    std::string schemaPattern = toPattern(SchemaName, NameLength2);
    std::string tablePattern = toPattern(TableName, NameLength3);
    std::string columnPattern = toPattern(ColumnName, NameLength4);

    bool catalogMatches = catalog.empty() || catalog == s->conn->database;
    auto [sql, params] =
        buildColumnsSql(s->conn->database, schemaPattern, tablePattern, columnPattern, catalogMatches);

    return runCatalogQuery(s, sql, params);
}

} // extern "C"
