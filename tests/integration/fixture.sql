-- Type-probe fixture used by tests/integration/test_type_probe.cpp.
--
-- Kept here as the durable, reproducible source of truth for the table
-- shape and row values that docs/protocol-notes.md's scalar-type section
-- was verified against. The integration test itself recreates this table
-- programmatically (via SqliConnection::executeCommand) rather than
-- shelling out to run this file, so this copy is for humans: to inspect
-- the exact fixture by hand (e.g. via dbaccess) or to recreate it manually
-- against a server the test isn't pointed at.
--
-- Note: the DATE literals below use MM/DD/YYYY, matching dbaccess's default
-- session locale in the Docker image's shell. The C++ integration test uses
-- ISO YYYY-MM-DD instead, since this driver explicitly sends DBDATE=Y4MD-
-- (see docs/protocol-notes.md) rather than inheriting dbaccess's locale -
-- these are two different sessions with two different (and both
-- deliberately fixed) date-parsing conventions, not an inconsistency.

drop table if exists type_probe;

create table type_probe (
    id            serial not null,
    col_smallint  smallint,
    col_integer   integer,
    col_bigint    bigint,
    col_char      char(10),
    col_varchar   varchar(32),
    col_decimal   decimal(10,2),
    col_date      date,
    col_datetime  datetime year to second
);

-- Row "positive": every column a representative non-NULL value.
insert into type_probe (col_smallint, col_integer, col_bigint, col_char, col_varchar,
                         col_decimal, col_date, col_datetime)
values (12345, 1234567890, 123456789012345, 'hello', 'world varchar',
        12345.67, DATE('07/29/2026'), DATETIME(2026-07-29 12:34:56) YEAR TO SECOND);

-- Row "negative": same shape, negative/small-magnitude values, to exercise
-- the DECIMAL sign/complement path and confirm two's-complement integers.
insert into type_probe (col_smallint, col_integer, col_bigint, col_char, col_varchar,
                         col_decimal, col_date, col_datetime)
values (-12345, -2147483000, -123456789012345, 'ab', 'x',
        -99.99, DATE('01/01/2000'), DATETIME(2000-01-01 00:00:00) YEAR TO SECOND);

-- Row "all NULL": every nullable column NULL, to exercise every type's NULL
-- sentinel in one shot.
insert into type_probe (col_smallint, col_integer, col_bigint, col_char, col_varchar,
                         col_decimal, col_date, col_datetime)
values (null, null, null, null, null, null, null, null);
