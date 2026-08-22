#include <doctest/doctest.h>

#include "driver/insert_table_name.h"

using namespace ifxodbc;

// The one new logic-heavy pure function driver/statement.cpp's smart-LOB
// bind dispatch depends on - factored out specifically so it gets the same
// kind of independent unit coverage this project already gives
// intervalEncodedLengthFor (driver/bind_encode.h), rather than relying on
// the live integration/DM-probe tests' single happy-path shape
// ("insert into <table> (id, col) values (?, ?)") to exercise it.
TEST_CASE("parseInsertTableName: plain table name") {
    CHECK(parseInsertTableName("insert into foo (a, b) values (?, ?)") == "foo");
    CHECK(parseInsertTableName("insert into foo values (?, ?)") == "foo");
}

TEST_CASE("parseInsertTableName: whitespace tolerance") {
    CHECK(parseInsertTableName("  insert   into\tfoo (a) values (?)") == "foo");
    CHECK(parseInsertTableName("insert\ninto\nfoo\n(a) values (?)") == "foo");
}

TEST_CASE("parseInsertTableName: case insensitivity") {
    CHECK(parseInsertTableName("INSERT INTO foo (a) values (?)") == "foo");
    CHECK(parseInsertTableName("Insert Into foo (a) values (?)") == "foo");
    CHECK(parseInsertTableName("InSeRt InTo foo (a) values (?)") == "foo");
}

// Owner-qualified and Informix database-qualified table names - the case
// an earlier version of this function got wrong by allow-listing
// identifier characters (alnum/'_'/'.') instead of stopping at the next
// metacharacter: a colon-qualified name like "db:owner.table" silently
// truncated at the colon, which would have fed a wrong "table.column" to
// SqliConnection::createSmartLob's informix.ifx_lo_col_sbinfo call.
TEST_CASE("parseInsertTableName: qualified table names") {
    CHECK(parseInsertTableName("insert into owner.foo (a) values (?)") == "owner.foo");
    CHECK(parseInsertTableName("insert into mydb:owner.foo (a) values (?)") == "mydb:owner.foo");
    CHECK(parseInsertTableName("insert into mydb@server:owner.foo (a) values (?)") == "mydb@server:owner.foo");
}

TEST_CASE("parseInsertTableName: rejects non-INSERT statements") {
    CHECK(parseInsertTableName("select * from foo") == std::nullopt);
    CHECK(parseInsertTableName("update foo set a = ?") == std::nullopt);
    CHECK(parseInsertTableName("delete from foo") == std::nullopt);
    CHECK(parseInsertTableName("") == std::nullopt);
}

TEST_CASE("parseInsertTableName: rejects malformed or incomplete INSERT text") {
    CHECK(parseInsertTableName("insert foo (a) values (?)") == std::nullopt);          // missing "into"
    CHECK(parseInsertTableName("insert into") == std::nullopt);                        // no table name at all
    CHECK(parseInsertTableName("insert into ") == std::nullopt);                       // trailing whitespace only
    CHECK(parseInsertTableName("insertinto foo (a) values (?)") == std::nullopt);      // "insert"/"into" must be
                                                                                         // separate words
}

TEST_CASE("parseInsertTableName: table name immediately followed by '('") {
    // No space between the table name and its column list - still a valid
    // stopping point (the scan halts at '(' the same as at whitespace).
    CHECK(parseInsertTableName("insert into foo(a) values (?)") == "foo");
}
