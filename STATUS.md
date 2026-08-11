# Status report

Last verified: 2026-08-10 (re-built and re-tested end-to-end on both platforms
as part of writing this report; see "How this was verified" below).

## TL;DR

This is a working prototype, not a production-grade driver yet. The protocol
layer and a real ODBC surface both round-trip correctly against a live
Informix server today, confirmed via `isql` through the actual unixODBC
Driver Manager, not just unit tests. But large, expected pieces of a
production ODBC driver are still missing outright: transactions, catalog
functions (`SQLTables`/`SQLColumns`/...), most bind-parameter types, Unicode,
Windows driver registration, LOBs, and more. See "Gaps to close before
production-grade" below for the concrete list.

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

1. **`SQLEndTran` not wired up yet.** The protocol-layer primitive is done:
   `SqliConnection::beginTransaction`/`commitTransaction`/`rollbackTransaction`
   (`SQ_BEGIN`/`SQ_CMMTWORK`/`SQ_RBWORK`) are implemented and confirmed live
   (see `docs/protocol-notes.md`'s Transactions section) - a row inserted
   inside a transaction and rolled back is invisible even to the same
   connection, and a committed row is visible from a separate connection.
   What's still missing is the ODBC entry point itself: `SQLEndTran` doesn't
   call any of this yet, and there's no driver-managed transaction boundary
   (autocommit state, `SQL_ATTR_AUTOCOMMIT`) tying connection lifecycle to
   these calls. Every statement is still effectively auto-committed by the
   server's own default behavior until that wiring exists.
2. **No catalog functions.** `SQLTables`, `SQLColumns`, `SQLGetTypeInfo`,
   `SQLPrimaryKeys`, `SQLStatistics`, etc. don't exist. Most real ODBC
   clients (BI tools, admin GUIs, even some ORM schema-introspection code)
   need at least `SQLTables`/`SQLColumns` to do anything beyond a
   hand-typed query.
3. **Bind parameters cover 6 of 8 MVP types.** `SQL_INTEGER`/`SQL_SMALLINT`/
   `SQL_BIGINT`/`SQL_CHAR`/`SQL_VARCHAR`/`SQL_TYPE_DATE` are done and
   confirmed live (`tests/integration/test_bind.cpp`), none needing
   `SQLBindParameter`'s `ColumnSize`/`DecimalDigits` (currently discarded by
   `driver/bind_encode.cpp`'s `encodeBindParam`) - the integer types and
   DATE use a hardcoded SQ_BIND precision field, CHAR/VARCHAR's value bytes
   are self-describing (a length prefix inside the data itself, confirmed
   live to need no column metadata: a value shorter than the target
   column's declared width round-trips fine with `precisionField` set to
   just the value's own length). Note this was initially mis-traced for
   VARCHAR - a first pass followed `IfxVarChar.toIfxTuple()` (a bulk-load
   path, not `sendBind`'s) and concluded the encoded buffer was fixed at
   the declared column width; the real bind path (`toIfx()`) turned out to
   be identical to CHAR's. Remaining: DECIMAL/DATETIME, which need their
   own encoders since their BIND wire format differs from their fetch
   format (documented in `docs/protocol-notes.md`) - the last two of the
   MVP set.
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
7. **Missing scalar types**: FLOAT/SMALLFLOAT, BOOLEAN, INTERVAL, LOBs
   (BLOB/CLOB/BYTE/TEXT), and DATETIME with fractional (sub-second)
   qualifiers.
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

1. Finish bind-parameter coverage for the remaining 7 MVP types.
2. Wire `SQLEndTran` to the now-confirmed `SqliConnection::beginTransaction`/
   `commitTransaction`/`rollbackTransaction`, plus `SQL_ATTR_AUTOCOMMIT`
   handling.
3. Catalog functions: `SQLTables`/`SQLColumns` first (unblocks the most
   client tooling), then `SQLGetTypeInfo`/`SQLPrimaryKeys`/`SQLStatistics`.
4. Windows driver registration (registry helper plus install doc) so the
   native Windows ODBC DM path is actually usable, not just the DLL.
5. Remaining scalar types (FLOAT/SMALLFLOAT/BOOLEAN/INTERVAL), then LOBs.
6. Unicode (`SQLWCHAR`) support.
7. SSL and PAM auth.
8. Thread-safety pass plus connection pooling.
9. Installer packaging for both platforms.
10. Broaden `SQLGetInfo`/`SQLGetFunctions` and the SQLSTATE table against a
    real ODBC conformance trace rather than "whatever `isql` happened to
    need."

See [`docs/production-plan.md`](docs/production-plan.md) for a phased plan
to close these gaps.
