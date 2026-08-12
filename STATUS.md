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
   `SQL_NUMERIC_STRUCT` support. `SQL_TYPE_TIMESTAMP` is scoped to
   whole-second `YEAR TO SECOND` only, hardcoded rather than derived from
   the target column's actual qualifier (the only one confirmed live);
   FRACTION values are rejected outright rather than silently truncated.
   Note VARCHAR was initially mis-traced - a first pass followed
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
   BOP's Informix plugin eventually needs (LOB, INTERVAL, sub-second
   DATETIME all remain unimplemented on both the bind and fetch sides).
   BOOLEAN and FLOAT/SMALLFLOAT are now fully done end to end (bind,
   fetch, and catalog). BOOLEAN turned out to be an extended/opaque type
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
7. **Missing scalar types**: INTERVAL, LOBs (BLOB/CLOB/BYTE/TEXT), and
   DATETIME with fractional (sub-second) qualifiers. BOOLEAN and
   FLOAT/SMALLFLOAT are fully done, including catalog rows (see item 3
   above).
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

1. **Remaining scalar types beyond the MVP-8 set - INTERVAL, sub-second
   DATETIME, then LOBs (both bind and fetch sides). BOOLEAN and
   FLOAT/SMALLFLOAT are now fully done end to end (bind, fetch, and
   catalog rows, sqli-layer plus ODBC-layer), each confirmed live and
   through the real Driver Manager.**
   Blocking: a real Informix database backup/restore tool can't skip
   columns of a type it doesn't support. LOBs in particular are likely to
   need their own multi-message wire handling, not just another entry in
   the existing scalar bind/fetch tables.
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
