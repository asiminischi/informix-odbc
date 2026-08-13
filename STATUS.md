# Status report

Last verified: 2026-08-10 (re-built and re-tested end-to-end on both platforms
as part of writing this report; see "How this was verified" below).

## TL;DR

This is a working prototype, not a production-grade driver yet. The protocol
layer and a real ODBC surface both round-trip correctly against a live
Informix server today, confirmed via `isql`/`tools/odbc_dm_probe` through
the actual unixODBC Driver Manager, not just unit tests. Transactions
(including `SQLEndTran`/`SQL_ATTR_AUTOCOMMIT` now actually driving
begin/commit/rollback, not just the underlying primitives) and all 8 MVP
bind-parameter types are done and confirmed live, including NULL binds
(fixed two framing/type bugs this pass - see gap 3 below). `SQLTables`,
`SQLColumns`, and `SQLGetTypeInfo` are all done and confirmed live via
`isql help`/`help <table>` and `tools/odbc_dm_probe` against a real
multi-table database. But large, expected pieces of a production ODBC
driver are still missing outright: `SQLPrimaryKeys`/`SQLStatistics` and
the rest of catalog introspection, Unicode, Windows driver registration,
LOBs, and more. See "Gaps to close before production-grade" below for the
concrete list.

## What's done (verified live, not just "compiles")

**Protocol layer (`sqli/`)**: connect handshake, statement protocol
(prepare/describe/execute/fetch/close/release), and all 8 MVP scalar type
codecs (SMALLINT, INTEGER/SERIAL, BIGINT/BIGSERIAL, CHAR, VARCHAR, DECIMAL,
DATE, DATETIME) are implemented and confirmed against a live IBM Informix
Dynamic Server (Docker, IDS 14.10.FC7W1DE), including every NULL sentinel and
DECIMAL's negative-value "hundred's complement" encoding. Full detail and the
exact wire byte layouts are in `docs/protocol-notes.md`.

**ODBC surface (`driver/`)**: 29 exported `SQL*` entry points, built as
`ifxodbc.dll` (Windows) / `libifxodbc.so` (Linux). Most do real work; a few
are accepted-but-inert stubs kept only so a Driver Manager doesn't get an
error for calling something routine, called out explicitly below rather
than folded into the count as if they were full implementations:

- Handle lifetime: `SQLAllocHandle`, `SQLFreeHandle`
- Connect: `SQLDriverConnect`, `SQLDisconnect`, `SQLGetEnvAttr`,
  `SQLGetConnectAttr`. `SQLSetEnvAttr`/`SQLSetConnectAttr` only actually act
  on `SQL_ATTR_ODBC_VERSION`/`SQL_ATTR_AUTOCOMMIT`/`SQL_ATTR_LOGIN_TIMEOUT`;
  every other attribute is accepted and silently ignored, not rejected.
  `SQLConnect` (the DSN-based path) is a full stub: it always returns a
  clean "not implemented" diagnostic rather than connecting.
- Statements: `SQLPrepare`, `SQLExecute`, `SQLExecDirect`, `SQLBindParameter`
  (SQL_INTEGER only, see below), `SQLBindCol`, `SQLFetch`, `SQLGetData`,
  `SQLFreeStmt`. **Inert stubs**: `SQLCancel` (always returns success, no
  asynchronous execution exists to cancel) and `SQLSetStmtAttr` (accepts and
  ignores every attribute; no scrollable cursors or row-array batching are
  implemented). `SQLGetStmtAttr` gives real answers for
  `SQL_ATTR_CURSOR_TYPE`/`SQL_ATTR_ROW_ARRAY_SIZE` (both fixed at
  forward-only/one-row) and a tolerant default otherwise.
- Result metadata: `SQLNumResultCols`, `SQLDescribeCol`, `SQLColAttribute` (+
  the ODBC2 legacy `SQLColAttributes`), `SQLRowCount`
- Diagnostics: `SQLGetDiagRec`, `SQLGetDiagField`, a real per-handle
  diagnostic queue, cleared correctly at the start of each call (not
  appended-forever)
- Info: `SQLGetInfo`, `SQLGetFunctions` (starter set, sufficient for `isql`)

**Type conversion coverage**: every MVP scalar type fetches correctly as
`SQL_C_CHAR` (text) and as native C types: `SQL_C_SSHORT/SLONG/SBIGINT`
(with `22003` overflow detection), `SQL_C_DOUBLE` (locale-independent via
`std::from_chars`), `SQL_C_TYPE_DATE`, `SQL_C_TYPE_TIMESTAMP`. Truncation into
an undersized buffer correctly returns `SQL_SUCCESS_WITH_INFO`/`01004`.

**Bind parameters**: `SQLBindParameter` exists and works end-to-end, but only
for `SQL_INTEGER` (`SQL_C_SLONG`/`SQL_C_LONG`/`SQL_C_DEFAULT`). Late-binding
semantics confirmed (rebind + re-execute on the same prepared statement picks
up the new value each time, including binding to NULL).

**Diagnostics**: a ~70-entry sqlcode to SQLSTATE lookup table
(`driver/error_map.cpp`); anything not in the table still gets a
well-formed `HY000` rather than a blank/malformed state. State-machine
misuse (fetch before execute, etc.) returns a diagnostic, never a crash.

**Cross-platform**: builds clean on Windows (clang++ 22, MSVC STL) and Linux
(WSL2 Ubuntu, clang++ 21 against libstdc++-15). A real Linux-specific bug
(ELF symbol interposition making the driver's internal cross-calls get
silently redirected into the Driver Manager's same-named functions) was
found and fixed; see `docs/protocol-notes.md`/plan history for detail.

**Test coverage**: 12 unit tests (doctest: handshake encoding, all 8 type
codecs) run via plain `ctest`, no server needed. A separate live-server
integration test (`tests/integration/test_type_probe.cpp`, gated behind
`-DIFX_ENABLE_INTEGRATION_TESTS=ON`) creates its own fixture table, inserts
positive/negative/all-NULL rows, and fetches them back through the real
prepare-execute-fetch-decode path. Two manual probe CLIs
(`tools/ifx_probe` for the protocol layer directly, `tools/odbc_dm_probe` for
the ODBC surface through the real platform Driver Manager) exist for ad hoc
testing.

## Gaps to close before "production-grade"

Roughly in the order they'd bite a real application:

1. **~~`SQLEndTran` not wired up yet~~ - done.** `SQLEndTran(SQL_HANDLE_DBC, ...)`
   is implemented (`driver/transaction.cpp`), and `SQL_ATTR_AUTOCOMMIT`
   actually changes behavior instead of just being round-tripped:
   `driver/statement.cpp`'s `doExecute` lazily opens a transaction
   (`SQ_BEGIN`) on the first statement after autocommit goes off, turning
   autocommit back on commits whatever's currently open
   (`driver/connect.cpp`'s `SQLSetConnectAttr`), and `SQLDisconnect` rolls
   back an unresolved transaction rather than leaving the server to decide.
   Confirmed live through the real Driver Manager (`tools/odbc_dm_probe`,
   run via WSL2/unixODBC - see that tool's `runTransactionTest`), not just
   the sqli layer: rollback via `SQLEndTran` leaves the row invisible on
   the same connection, commit makes it visible from a separate connection,
   and the autocommit OFF -> ON transition commits an in-flight insert.
   Also checked live what a commit does to an already-open SELECT cursor
   (this driver never declares cursors `WITH HOLD`, so a server-side close
   was plausible) - it survives: fetching more rows after
   `SQLEndTran(SQL_COMMIT)` on a statement with an open cursor keeps
   working, no explicit cursor-state reset needed in the driver.
   `SQLEndTran(SQL_HANDLE_ENV, ...)` (commit/rollback every connection
   under an environment) isn't implemented - this driver has no
   env-to-connections index to walk - and returns HYC00 rather than
   silently no-opping.

   Getting this working through unixODBC's Driver Manager surfaced two
   bugs unrelated to the transaction logic itself, both now fixed:
   `driver/info.cpp`'s `SQLGetFunctions` never listed `SQL_API_SQLENDTRAN`
   as implemented, and `SQLGetInfo(SQL_TXN_CAPABLE)` still reported
   `SQL_TC_NONE` (a leftover from before any transaction support existed) -
   unixODBC checks both before it will even attempt to call `SQLEndTran`,
   and returned `IM001` ("driver does not support this function") itself
   without ever reaching this driver's code. `SQL_TXN_CAPABLE` is now
   `SQL_TC_DML` (DML confirmed transactional; DDL-inside-a-transaction
   behavior hasn't been checked, so not claiming `SQL_TC_ALL`/
   `SQL_TC_DDL_COMMIT` yet).
2. **Catalog functions: `SQLTables`/`SQLColumns`/`SQLGetTypeInfo`
   ~~missing~~ - done; `SQLPrimaryKeys`/`SQLStatistics` still missing.**
   All three (`driver/catalog.cpp`) are implemented by synthesizing a SQL
   query and running it through the same prepare/execute/fetch pipeline
   every other statement uses - confirmed live through the real Driver
   Manager (`tools/odbc_dm_probe`'s `runCatalogTest`/`runColumnsTest`/
   `runTypeInfoTest`) and, for the first two, `isql`'s `help`/
   `help <table>` against a live multi-table database (including a
   `SERIAL NOT NULL` column correctly reported as `integer`/NOT NULL).
   `SQLGetTypeInfo` has no isql-native invocation path, so `odbc_dm_probe`
   is its verification bar. See `docs/protocol-notes.md`'s Catalog
   functions section for what the live testing surfaced:
   `systables.owner` and several `CASE` expressions' inferred types turn
   out to be space-padded fixed-width `CHAR` on this server (worked
   around with `trim()`), `syscolumns.coltype`/`collength` pack fields the
   same way the wire protocol already does, Informix's `trunc()`/`mod()`
   return `DECIMAL` even for integer arguments - contaminating an entire
   `CASE`'s inferred type, not just the branch that uses them, worked
   around with explicit `cast(... as integer)` - and, hit building
   `SQLGetTypeInfo`: a `UNION ALL`-specific variant of the `CASE`-padding
   quirk where `trim()` does *not* undo the padding, and where casting to
   `lvarchar` to dodge it instead desyncs this driver's wire decoder and
   hangs the connection (this driver has never implemented `lvarchar`
   fetch decoding) - fixed by rewriting `SQLGetTypeInfo` to use the same
   derived-table-plus-outer-`CASE` shape as `SQLTables`/`SQLColumns`
   instead of `UNION ALL`. Scoped to the plain filtering case those tools
   actually use (catalog/schema/table-name/column-name `LIKE` patterns,
   table-type `IN`-list filtering) - the ODBC catalog/schema-listing
   "special value" enumeration modes and `COLUMN_DEF` (default-value
   introspection) are documented, deliberate gaps, not oversights (see
   that file's top comment). `SQLPrimaryKeys`/`SQLStatistics` are next;
   most real ODBC clients (BI tools, admin GUIs, ORM schema-introspection
   code) can already do useful schema browsing with just `SQLTables`/
   `SQLColumns`/`SQLGetTypeInfo`.
3. **~~Bind parameters cover N of 8 MVP types~~ - done.** All 8 MVP types
   (`SQL_INTEGER`/`SQL_SMALLINT`/`SQL_BIGINT`/`SQL_CHAR`/`SQL_VARCHAR`/
   `SQL_TYPE_DATE`/`SQL_DECIMAL`/`SQL_TYPE_TIMESTAMP`) are confirmed live
   (`tests/integration/test_bind.cpp`), none needing `SQLBindParameter`'s
   `ColumnSize`/`DecimalDigits` (still discarded by
   `driver/bind_encode.cpp`'s `encodeBindParam`) - the integer types and
   DATE use a hardcoded SQ_BIND precision field, CHAR/VARCHAR's value
   bytes are self-describing (a length prefix inside the data itself),
   and DECIMAL/DATETIME's precision field is intrinsic to the value
   itself (scale, or qualifier) rather than the target column's declared
   metadata - all confirmed live to need no column metadata, including
   that a minimally-sized DECIMAL mantissa (not padded to the target
   column's declared precision) round-trips fine. `SQL_DECIMAL` binds go
   through an exact decimal-literal string (`SQL_C_CHAR`), matching this
   driver's existing exact-DECIMAL-fetch convention, since there's no
   `SQL_NUMERIC_STRUCT` support. `SQL_TYPE_TIMESTAMP` hardcodes `YEAR TO
   SECOND` when there's no sub-second part and `YEAR TO FRACTION(5)`
   otherwise (Informix DATETIME's finest precision, ten-microsecond
   resolution - `ts.fraction` nanoseconds are truncated, not rounded, down
   to that), rather than deriving either from the target column's actual
   qualifier (the only two confirmed live). Note VARCHAR was initially
   mis-traced - a first pass followed
   `IfxVarChar.toIfxTuple()` (a bulk-load path, not `sendBind`'s) and
   concluded the encoded buffer was fixed at the declared column width;
   the real bind path (`toIfx()`) turned out to be identical to CHAR's.

   NULL parameter binding got two changes this pass, with different
   verification strength:
   - **Confirmed live, and a real bug**: `sqli/statement.cpp`'s
     `writeBindBlock` omitted a NULL parameter's `precisionField` entirely
     instead of sending `0`, which desynced the wire (and hung the
     connection) whenever a NULL wasn't the bind block's last parameter -
     invisible in every round trip tried before now because the only NULLs
     bound so far were either the sole parameter or the last one. See
     `docs/protocol-notes.md`'s SQ_BIND section and
     `tests/integration/test_bind.cpp`'s `checkNullBind`, which
     specifically binds a NULL ahead of other parameters to exercise it.
   - **Brought into line with the decompiled reference, not independently
     shown to be a live bug**: `driver/statement.cpp`'s `buildBindParams`
     used to leave a NULL bind's `ifxType` defaulted to 0/CHAR regardless
     of the target column's real type; it now resolves the real type via
     `ifxTypeForParameterType`. This path lives in the ODBC driver DLL,
     which no integration test drives yet (`checkNullBind` calls
     `SqliStatement::execute` directly with hand-built `BindParam`s,
     bypassing `buildBindParams` entirely) - it was never demonstrated
     that a type-0 NULL into a typed column actually misbehaves live, only
     that the reference always sends the real type. Treat this as
     unverified-but-reference-consistent until an ODBC-surface
     (`SQLBindParameter`) test harness exists to exercise it end-to-end.

   This is still mostly just the 8-type MVP set, not the full type system
   BOP's Informix plugin eventually needs (LOB and INTERVAL remain
   unimplemented on both the bind and fetch sides; sub-second DATETIME is
   now done - see below). BOOLEAN and FLOAT/SMALLFLOAT are now fully done
   end to end (bind, fetch, and catalog). BOOLEAN turned out to be an
   extended/opaque type
   (nonzero `extendedId` in DESCRIBE, owner="informix"/name="boolean")
   rather than a plain fixed-width scalar, on the wire protocol layer
   only: fetch needed SQ_RETURN_TYPE's extended-column encoding plus a
   self-describing 5/6-byte TUPLE decoder (`decodeBoolean`), and bind
   needed SQ_BIND's own, separately-traced extended-type framing (the
   owner/name fields written as two `ifxChar2`s right after the type
   code, for every bind of that type including NULL ones - confirmed
   live that omitting them hangs the connection) plus the JDBC-matching
   remapped type code 45 (`ifxtype::kBooleanBindType`, not 41/
   `kBoolean`) and `encodeBoolean`. The catalog layer needed none of
   that - confirmed live that `syscolumns.coltype` uses the same base
   type code (41) as DESCRIBE's wire ifxType, so `SQLColumns`/
   `SQLGetTypeInfo` support was just three ordinary `kTypeMap` rows.
   FLOAT/SMALLFLOAT needed no extended-type handling anywhere - plain
   fixed-width IEEE 754 values (wire ifxType matches the decompiled JDBC
   driver's codes exactly); see `decodeFloat`/`decodeSmallFloat`/
   `encodeFloat`/`encodeSmallFloat`.

   DATETIME FRACTION (sub-second precision) is now fully done end to end
   (bind, fetch-as-text, and fetch-as-`SQL_TIMESTAMP_STRUCT`). Confirmed
   live via `IFX_SQLI_TRACE` wire capture of a `YEAR TO FRACTION(5)` column
   holding `2024-06-15 13:45:52.12345`: FRACTION digits continue the same
   packed base-100-digit-pair mantissa stream DECIMAL and the rest of
   DATETIME already use, immediately after SECOND, left-justified with the
   trailing digit of an odd digit count zero-padded on the wire. The one
   real bug this surfaced: the exponent byte's biased magnitude has to stay
   pinned to the *whole-field* (YEAR..SECOND) mantissa byte count even once
   FRACTION pairs are appended - using the total mantissa count instead
   (an easy mistake, since it happens to be correct whenever there's no
   FRACTION) self-round-trips fine through `decodeDatetime` but the server
   rejects it on bind with sqlcode -1266. `driver/bind_encode.cpp` hardcodes
   `YEAR TO FRACTION(5)` (Informix DATETIME's finest precision) whenever
   `SQL_TIMESTAMP_STRUCT.fraction != 0`, scaling nanoseconds down by `10^4`
   (truncating, not rounding). See `sqli/types.h`'s decodeDatetime/
   encodeDatetime doc comments and `docs/protocol-notes.md`'s
   DATETIME(qualifier) section for the full byte-level writeup.

   INTERVAL is now done on the fetch side for both classes - YEAR-MONTH
   (`YEAR TO YEAR`, `YEAR TO MONTH`, `MONTH TO MONTH`) and DAY-FRACTION
   (`DAY TO SECOND`, `HOUR TO FRACTION(n)`, and similar) - sqli layer plus
   `driver/convert.cpp`'s `SQL_C_CHAR` text surface (`sqli::
   decodeIntervalYearMonth`/`decodeIntervalDayFraction`), confirmed live
   across ten total wire captures covering every qualifier shape, both
   signs, and both leading-field-anchor cases (DAY vs HOUR) -
   `tests/sqli/test_types.cpp`. INTERVAL turned out to share
   DECIMAL's exact packed-digit byte format but *not* DATETIME's
   fixed-point exponent convention - it reuses DECIMAL's floating
   representation outright (leading zero digit-pairs stripped, exponent
   shrinks, trailing zero-pairs pad back to the fixed width), which a
   first pass got wrong before a live capture with a non-full-width
   leading field (`INTERVAL YEAR(4) TO MONTH` holding `3-07`) caught it.
   A second gotcha at the odd/even digit-count boundary (`INTERVAL
   MONTH(3) TO MONTH` holding `43`) needed the padding slot dropped from
   the front of the leading field specifically, not the back. Extending
   to DAY-FRACTION then surfaced a *third* gotcha that generalized the
   second one: padding isn't one global correction on the whole digit
   string - the leading field pads its own front, and FRACTION
   independently pads its own back (matching `decodeDatetime`'s own
   FRACTION convention) - conflating them into a single blanket
   "totalDigits" truncation corrupted the leading field of `INTERVAL
   DAY(2) TO FRACTION(5)` (decoded day=20, hour=30 - an impossible hour -
   instead of day=12, hour=3) until per-field byte accounting replaced the
   blanket approach. Each of these three was caught only because the
   specific qualifier shape that exposed it got live-tested rather than
   assumed from a prior, superficially-similar case. See
   `docs/protocol-notes.md`'s INTERVAL(qualifier) section for the full
   byte-level writeup.

   INTERVAL bind encoding is now done at the sqli layer for both classes
   (`sqli::encodeIntervalYearMonth`/`encodeIntervalDayFraction`), and it's
   genuinely *not* the fetch side's mathematical inverse: SQ_BIND sends the
   full, un-stripped fixed-width mantissa (no floating-point stripping),
   traced via the decompiled JDBC reference's `Decimal(sign, byte[], int,
   short)` constructor. Confirmed live for both classes and both signs
   (`tests/integration/test_bind.cpp`) using values that specifically need
   fetch-side stripping, to prove the server accepts the client's
   un-stripped bind form rather than just self-round-tripping through this
   driver's own decoder - this project's INTERVAL work has hit three bugs
   already caught only by testing the specific shape, so the sign +
   un-stripped-exponent combination got its own live check too rather than
   being assumed safe from DECIMAL's separately-confirmed negative-bind
   mechanism. **Now wired to the ODBC `SQLBindParameter` surface too**:
   unlike every other bind type so far, INTERVAL's leading field width is a
   genuine per-column, user-declared value that can't be recovered from the
   bind value's magnitude alone, so `ColumnSize`/`DecimalDigits` (previously
   received and discarded outright) are now plumbed through `ParamBinding`
   into `encodeBindParam`, which reconstructs the wire qualifier from the
   `SQL_INTERVAL_*` `ParameterType` + `ColumnSize` + `DecimalDigits` triple
   (`intervalEncodedLengthFor`, unit-tested against three live-captured
   qualifier values with no server needed - `tests/driver/test_bind_encode.cpp`,
   a new test target). The value itself comes through the native
   `SQL_INTERVAL_STRUCT` (`SQL_C_INTERVAL_*`), not a `SQL_C_CHAR` text
   literal like DECIMAL's - deliberately not this driver's own display-text
   convention, since parsing that back would only prove self-consistency,
   not real ODBC interoperability (see `docs/protocol-notes.md`'s INTERVAL
   SQ_BIND encoding section for the full reasoning). Confirmed live
   end-to-end through this driver's own exported ODBC entry points, called
   directly against the built `ifxodbc` library (bypassing the Driver
   Manager, which the IM002 registration gap blocks only for DM-mediated
   loading, not direct calls) - both classes, both signs, `FRACTION`, and a
   NULL bind ahead of another parameter all round-tripped correctly.
   Remaining open question: whether a real Driver Manager passes this
   triple through unmolested - needs the Linux/WSL2 path not set up here.

   `SQLColumns` now reports INTERVAL columns correctly for both classes -
   `driver/catalog.cpp` decodes each column's qualifier from
   `syscolumns.collength` (the same packed `(totalDigits << 8) |
   qualifierByte` shape the wire protocol uses, confirmed live via a probe
   table) and dispatches `DATA_TYPE`/`TYPE_NAME`/`SQL_DATA_TYPE`/
   `SQL_DATETIME_SUB`/`COLUMN_SIZE`/`DECIMAL_DIGITS` on the decoded
   qualifier shape instead of the flat `basetype`-keyed lookup every other
   type uses (INTERVAL's wire type code is shared by all 13 qualifier
   shapes across both classes, unlike every other type). `DATA_TYPE` uses
   `sqlext.h`'s positive `SQL_INTERVAL_*` values (101-113), confirmed by
   compiling a probe against this project's toolchain rather than assuming
   from memory (this SDK's default `ODBCVER` selects that branch, not the
   negative ODBC-2.x-era one sharing the same macro names). `COLUMN_SIZE`
   is a character display-size bound (matching `DATE`/`DATETIME`'s
   existing convention), not the leading field's own declared width alone
   - confirmed via `sqlext.h` that leading precision has its own separate
   ODBC descriptor field, so reporting it as `COLUMN_SIZE` would risk
   truncating a `SQL_C_CHAR` fetch. Verified live by running the generated
   SQL directly against the server (four distinct qualifier shapes, plus a
   regression check that non-INTERVAL columns are unaffected), not yet
   through the real Driver Manager (same Linux/WSL2 gap as item 3's bind
   work). `SQLGetTypeInfo` is deliberately *not* extended for INTERVAL -
   its `kTypeMap` design emits one row per type, and INTERVAL's 13
   qualifier shapes don't fit that without a larger restructure nothing in
   this project's verification bar currently needs. See
   `docs/protocol-notes.md`'s "INTERVAL in SQLColumns" section for detail.

   **A bug found while deriving the `COLUMN_SIZE` formula is now fixed**:
   `driver/convert.cpp`'s `formatIntervalDayFraction` (the `SQL_C_CHAR`
   text surface, shipped in the DAY-FRACTION decode commit) originally
   only received `hasDayField`, not the qualifier's actual start/end
   reach - its `!hasDayField` branch always printed `iv.hour` as the first
   field regardless of which field is actually the qualifier's leader, so
   a MINUTE-anchored or SECOND-anchored qualifier (`MINUTE TO SECOND`,
   `SECOND TO SECOND`) printed the true leading value stranded in the
   wrong slot, and it always printed the full `HH:MM:SS` shape even when
   the qualifier didn't reach that far (e.g. `HOUR TO MINUTE` showed a
   spurious `:00` seconds field). Fixed by passing the qualifier's
   start/effective-end into the formatter, which now walks the same
   field-by-field sequence `sqli::decodeIntervalDayFraction` itself uses
   (`tools/ifx_probe`'s own separate, more-broken copy of the same logic -
   it unconditionally used `iv.day`, always 0 - got the identical fix).
   Confirmed live via six new wire captures covering every previously-
   untested leading-field/qualifier-reach combination (`HOUR TO HOUR`,
   `HOUR TO MINUTE`, `MINUTE TO MINUTE`, `MINUTE TO SECOND`, `SECOND TO
   SECOND`, `SECOND TO FRACTION(3)`) plus a negative case, which also
   became new `sqli::decodeIntervalDayFraction` unit tests, closing the
   "MINUTE/SECOND-anchored leading fields... untested" decode-side gap
   this class had carried since the original commit (only `HOUR TO
   SECOND` was previously confirmed, and it happens to reach all the way
   to SECOND, so the formatter bug didn't manifest for that specific
   shape). See `docs/protocol-notes.md`'s INTERVAL DAY-FRACTION class
   section for the full writeup.
4. **No DSN-based connect (`SQLConnect`).** Only `SQLDriverConnect` with an
   explicit keyword string works today. Fine for `isql -k`, but many
   GUI/ODBC-Admin tools default to the DSN path.
5. **Windows driver registration doesn't exist.** The Windows DLL builds and
   its export table is correct, but nothing registers it with the Windows
   ODBC Driver Manager (that needs an HKLM registry entry plus admin
   elevation). Confirmed live during this report: the native Windows DM
   refuses `DRIVER={path}` connection strings the way unixODBC accepts them;
   `SQLDriverConnect` fails with `IM002` before ever reaching this
   driver's code. The only verified end-to-end ODBC path today is
   Linux/WSL2 + unixODBC + `isql`.
6. **No Unicode.** ANSI (`SQL_C_CHAR`) only; no `SQLWCHAR`/`W`-suffixed
   functions.
7. **Missing scalar types**: LOBs (BLOB/CLOB/BYTE/TEXT). INTERVAL is now
   fully done for both classes: fetch, sqli-layer bind, ODBC
   `SQLBindParameter` wiring, and `SQLColumns` (see item 3 above) - only
   `SQLGetTypeInfo` remains a deliberately deferred gap for INTERVAL
   specifically (its `kTypeMap` design doesn't fit INTERVAL's 13 qualifier
   shapes, and nothing in this project's verification bar exercises it).
   BOOLEAN, FLOAT/SMALLFLOAT, and DATETIME (including sub-second FRACTION)
   are fully done too, including catalog rows. **LOBs turned out to be two
   unrelated mechanisms, not one type family**: simple large objects
   (BYTE/TEXT) and smart large objects (BLOB/CLOB, sbspace-backed), which
   share nothing at the wire level (see `docs/protocol-notes.md`'s two
   separate sections). Simple-LOB **fetch is now done and confirmed live**:
   `SqliConnection::fetchSimpleLob` sends `SQ_FETCHBLOB`(38) with the
   column's 56-byte in-row descriptor (the TUPLE row never carries the
   value inline - just this fixed-size mostly-opaque descriptor, confirmed
   live down to which exact byte signals NULL) and accumulates `SQ_BLOB`(39)
   response chunks; confirmed live via `tools/ifx_probe` for both BYTE and
   TEXT, including NULL and a 200KB value spanning multiple chunks
   (byte-for-byte exact). Simple-LOB **bind is now done too**:
   `SQ_BIND` carries a 56-byte placeholder descriptor
   (`sqli::encodeSimpleLobPlaceholder`, only `tb_size` patched in), and the
   real content streams separately under a dedicated `SQ_BBIND`(41)+count
   header followed by `SQ_BLOB`(39) chunks - a header this project's own
   first research pass on the decompiled source missed entirely, causing a
   live tag-56/`SQ_EXIT` connection drop on the first attempt; found by
   re-reading `IfxSqli.java`'s `sendBlob()` directly after bisecting which
   part of the request triggered the failure. Confirmed live and now a
   permanent `ctest` (`tests/integration/test_bind.cpp`'s
   `checkSimpleLobBind`, part of `bind_integration`): a small odd-length
   value, a NULL value bound ahead of another parameter (no desync), and a
   5000-byte multi-chunk upload all round-trip correctly. Still missing:
   the ODBC `SQLGetData`/`SQLBindParameter` surface for either simple-LOB
   type, and all of smart LOBs (BLOB/CLOB share ifxType 41 with BOOLEAN
   and currently misdecode; getting their content needs a whole
   fastpath-RPC subsystem this driver doesn't have yet, reverse-engineered
   but not implemented - see `docs/protocol-notes.md`).
8. **No real generated-key retrieval.** The `SQ_INSERTDONE` wire field is
   decoded, but the driver never sends the extra "SGK" request the server
   needs to actually populate it, so every INSERT currently reports a
   generated key of `0`. Getting a real identity value requires a
   follow-up `SELECT` today.
9. **No SSL, no PAM auth.** Plain username/password only.
10. **No connection pooling.**
11. **No packaged installer** for either platform (no `.msi`, no
    `odbcinst`/`odbc.ini` template automation beyond the manual `docker/`
    dev setup).
12. **Single-threaded handle assumption, no locking.** Reasonable for the
    current phase, but a real gap for any multi-threaded host application.
13. **DECIMAL/DATETIME beyond the tested precision/qualifier range** are
    handled by a formula consistent with the confirmed cases, but aren't
    independently verified against a live capture for every possible
    precision/qualifier combination.

## How this was verified (today, while writing this report)

Re-ran the full verification loop from scratch rather than trusting prior
notes:

- **Windows build**: clean `cmake -G Ninja -B build -DCMAKE_CXX_COMPILER=clang++`
  plus `cmake --build build`, succeeds, 0 errors (only benign warnings from
  the vendored doctest header). `ctest --test-dir build`: **12/12 unit
  tests pass**.
- **Linux build (WSL2)**: clean configure and build with the documented
  gcc-15 workaround flags, succeeds, 0 errors.
- **Live server**: started the `docker/docker-compose.yml` dev container,
  confirmed `onstat -` reports `-- On-Line --`.
- **Protocol layer, live**: `tools/ifx_probe` against the running container:
  connected, ran `select tabid, tabname from systables where tabid <= 3`,
  got the correct 3 rows, disconnected cleanly.
- **ODBC surface, live, through the real Driver Manager**: `isql -k
  "DRIVER=.../libifxodbc.so;HOST=127.0.0.1;SERVICE=9088;SERVER=informix;
  UID=informix;PWD=in4mix;DATABASE=sysmaster"` (WSL2/unixODBC): connected,
  ran the same query, got correct rows **and** correct column headers.
- **Integration test, live**: built with `-DIFX_ENABLE_INTEGRATION_TESTS=ON`
  and ran `type_probe_integration_test` against the same container: **all
  assertions passed** (every MVP type, including the all-NULL row).
- **Windows native ODBC DM**: confirmed (not just per old notes) that it
  currently rejects this driver's `DRIVER={path}` connection string with
  `IM002`, a real, currently-open gap, not a stale claim.

## Suggested next steps, in priority order

Reordered to the lens that actually matters for this driver's purpose -
being usable as the eventual BOP Informix plugin's data path - rather than
"most ODBC-spec-complete first." A backup/restore tool has to read and
write every column type a real database has, and has to actually be
callable from BOP's own code; catalog-completeness and driver-manager
polish don't block that.

1. **Remaining scalar types beyond the MVP-8 set - LOBs are the only
   scalar family left with real gaps. INTERVAL is now fully done for both
   classes end to end, including the ODBC `SQLBindParameter` surface
   (`ColumnSize`/`DecimalDigits` now plumbed through - see item 7) - only
   `SQLGetTypeInfo` remains a deliberately deferred INTERVAL gap.
   BOOLEAN, FLOAT/SMALLFLOAT, and DATETIME (including sub-second FRACTION)
   are fully done end to end too (bind, fetch, and catalog rows,
   sqli-layer plus ODBC-layer). All of this confirmed live - DATETIME/
   FRACTION and INTERVAL (both classes, both signs) through the sqli layer
   and driver-layer conversions directly (`driver/convert.cpp`/
   `convert_native.cpp`/`bind_encode.cpp`), INTERVAL's `SQLColumns` rows
   via generated SQL run directly against the server, and INTERVAL's
   `SQLBindParameter` wiring through this driver's own exported ODBC entry
   points called directly (bypassing the Driver Manager) - none of this
   yet through a *real* Driver Manager, which currently needs the
   Linux/WSL2 path (see item 5's Windows DM gap) not set up in this
   environment. LOBs split into two unrelated mechanisms once
   investigated (simple BYTE/TEXT vs. smart BLOB/CLOB, sharing nothing at
   the wire level - see `docs/protocol-notes.md`); simple-LOB **fetch and
   bind are now both done and confirmed live at the sqli layer**
   (`SqliConnection::fetchSimpleLob`/`writeBindBlock`'s `SQ_BBIND`(41)
   chunk-stream tail, the latter now a permanent `ctest` via
   `tests/integration/test_bind.cpp`'s `checkSimpleLobBind`) - the ODBC
   `SQLGetData`/`SQLBindParameter` surface for either type, and all of
   smart LOBs (needs a fastpath-RPC subsystem, reverse-engineered but not
   implemented) remain.**
   Blocking: a real Informix database backup/restore tool can't skip
   columns of a type it doesn't support. Smart LOBs in particular need
   their own multi-message wire handling (confirmed, not just suspected,
   now that the fastpath/SQ_LODATA mechanism has been reverse-engineered),
   not just another entry in the existing scalar bind/fetch tables.
2. **Decide and prototype the BOP integration mechanism** (CGO, subprocess
   + a small IPC protocol, or something else) - deliberately left
   undecided when this project started. Nothing else here matters to BOP
   until there's a concrete way to call into this driver from BOP's own
   code.
3. SSL and PAM auth - a backup tool touching production data over
   plaintext auth is a hard sell for real deployment.
4. Windows driver registration (registry helper plus install doc) so the
   native Windows ODBC DM path is actually usable, not just the DLL -
   only blocking if BOP's Informix plugin runs on native Windows rather
   than Linux; revisit once the deployment target is known.
5. `SQLPrimaryKeys`/`SQLStatistics` (`SQLTables`/`SQLColumns`/
   `SQLGetTypeInfo` are done) - useful for a general-purpose driver, not
   required for BOP's read-everything/write-everything-transactionally
   use case.
6. Unicode (`SQLWCHAR`) support.
7. Thread-safety pass plus connection pooling.
8. Installer packaging for both platforms.
9. Broaden `SQLGetInfo`/`SQLGetFunctions` and the SQLSTATE table against a
   real ODBC conformance trace rather than "whatever `isql` happened to
   need."

See [`docs/production-plan.md`](docs/production-plan.md) for a phased plan
to close these gaps.
