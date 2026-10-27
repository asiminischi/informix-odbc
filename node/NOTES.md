# Working notes: Node package for informix-odbc

Reading notes collected while designing the npm package, so the source
does not need to be re-read. ASCII only.

## Goal

An npm package that connects Node directly to Informix over the SQLI wire
protocol (this repo's `sqli/` layer), replacing `~/Work/informix-proxy`
(Java gRPC proxy + `@postarodiy/informix-client` TS client).

## Environment facts (this machine, 2026-10-27)

- node v26.11.1, npm 12.2.1, global node-gyp 13.1.0
- clang++ 23.1.3, g++ 16.2.1, cmake, ninja present
- unixODBC is NOT installed (no /usr/include/sql.h). The full CMake build
  fails at configure (ODBC_LIBRARY NOTFOUND for tools/odbc_dm_probe).
  This is a strong reason to bypass ODBC entirely.
- Compiled standalone OK with `clang++ -std=c++26 -I. -fPIC -c`:
  all of `sqli/*.cpp`, `driver/convert.cpp`, `driver/insert_table_name.cpp`.
  These need no ODBC headers.
- `driver/convert_native.*` and `driver/bind_encode.*` include sql.h /
  sqlext.h -> not usable without unixODBC. Re-implement bind encoding
  from JS values using the `sqli/types.h` encoders instead.
- Dev Informix: `docker compose up -d` in `docker/` -> container
  `ifxodbc_dev`, host 127.0.0.1, port 9088, server `informix`,
  user `informix`, password `in4mix`, db e.g. `sysmaster`.
  Wait for `docker exec ifxodbc_dev bash -lc 'onstat -'` to say On-Line.
- sqli/CMakeLists.txt uses `${CMAKE_SOURCE_DIR}` as include dir, so it
  cannot be add_subdirectory'd from another CMake project as-is. Compile
  the sources directly with include dir = repo root instead.

## sqli API (namespace ifxsqli)

`sqli/error.h`: `struct Error { ErrorKind kind; std::string message; }`,
ErrorKind = Transport | Protocol | Server. No numeric sqlcode field.

`sqli/handshake.h`:
- `ConnectParams { username, password, serverName, database, appName }`
- `ConnectResult { serverVersion, serialNumber, applicationId, cap1..3 }`

`sqli/connection.h` - `SqliConnection` (movable, not thread-safe):
- `static expected<SqliConnection, Error> connect(host, port, ConnectParams)`
- `disconnectOrderly()` (once; dtor closes socket as fallback)
- `serverInfo()` -> ConnectResult
- `executeCommand(sql)` - resultless SQL, no params
- `beginTransaction()`, `commitTransaction()`, `rollbackTransaction()`
- `lastSerialInserted()` - reads back 0 for plain INSERT (no SGK), unreliable
- `fetchSimpleLob(span descriptor56)` -> bytes (BYTE/TEXT)
- `fetchSmartLob(span locatorPayload)` -> bytes (BLOB/CLOB); payload is the
  TUPLE column slice minus the leading null-flag byte
- `createSmartLob("table.column", content)` -> 72-byte locator

`sqli/statement.h` - `SqliStatement`:
- `static prepare(SqliConnection&, sql)`
- `columns()` (only when hasResultSet), `paramDescs()` (non-SELECT with ?),
  `hasResultSet()` (statementType == 2), `paramCount()`, `rowCount()`
- `execute(vector<BindParam>)`, `fetchRow()` -> optional<vector<vector<byte>>>
- `close()`, `release()`
- `ColumnDesc`: name, ifxType, extendedId, extendedOwnerName,
  extendedTypeName (+ length/qualifier fields; see header)
- `BindParam`: ifxType, isNull, data, precisionField, extendedOwnerName,
  extendedTypeName, optional simple-LOB content

`sqli/types.h` - `ifxtype` codes: kChar 0, kSmallInt 1, kInteger 2,
kFloat 3, kSmallFloat 4, kDecimal 5, kSerial 6, kDate 7, kDatetime 10,
kByte 11, kText 12, kVarchar 13, kInterval 14, kNVarchar 16,
kBoolean 41 (also BLOB/CLOB: check extendedTypeName "blob"/"clob"),
kLvarchar 43 (fetch decoding NOT implemented - can hang), kUdt 44,
kBooleanBindType 45, kBigInt 52, kBigSerial 53.
Encoders + bind precision constants: encodeInteger/kIntegerBindPrecision
(0x0A00), encodeSmallInt (0x0500), encodeBigInt (0x1300), encodeFloat (8),
encodeSmallFloat (4), encodeChar, encodeBoolean (1), encodeDateUnixDays,
encodeDecimal(literal) -> DecimalBindEncoding, encodeDatetime(value, ...),
encodeInterval*, encodeSimpleLobPlaceholder(len). Decoders likewise.

`driver/convert.h`: `formatCellAsText(ColumnDesc, raw)` -> CellText
{isNull, text}. Pure, no ODBC. Handles every scalar type as text.

## How the ODBC driver does the tricky parts (driver/statement.cpp)

- Smart LOB column: `ifxType == kBoolean && extendedTypeName in {blob,clob}`.
  raw[0] == 1 means NULL; else `fetchSmartLob(raw.subspan(1))`.
- Simple LOB column (kByte/kText): raw is 56 bytes; NULL when
  raw[38]==0 && raw[39]==1; else `fetchSimpleLob(raw)`.
- Smart LOB bind: target detected via paramDescs()[i]; table name from
  `parseInsertTableName(sql)` (driver/insert_table_name.h), then
  `createSmartLob(table + "." + col.name, content)` and a kUdt BindParam
  (see `buildSmartLobBindParam`, ~line 384). NULL smart LOB: ifxType
  kBoolean, owner "informix", typeName = paramDesc's.
- NULL bind must carry the real target ifxType; NULL BOOLEAN must carry
  owner "informix" / type "boolean" or the connection hangs.
- Autocommit off: lazily `beginTransaction()` before first statement.

## Proxy client API to mirror (`@postarodiy/informix-client`)

`new InformixClient(host, port, opts)`, then:
`connect({host, port, database, username, password, server?, properties?,
poolSize?, connectTimeoutMs?})` -> `{connectionId, serverVersion}`,
`disconnect()`, `ping()` -> `{alive, latencyMs}`,
`query(sql, params)` -> `{rows, columns, rowCount}`,
`queryStream(sql, params, onRow, {fetchSize, maxRows})` -> `{columns, rowCount}`,
`execute(sql, params)` -> rows affected, `batch(sqls)` -> number[],
`beginTransaction(isolation)`, `commit()`, `rollback()`,
`prepareStatement(sql)` -> `{statementId, parameterCount}`,
`executePrepared(id, params)`, `closePrepared(id)`,
`getMetadata(table?)` -> TableInfo[] `{name, schema, type, columns}`,
`isConnected`. ColumnInfo = `{name, type, precision, scale, nullable}`.
Param types: string | number | boolean | Buffer | null | undefined.

Proxy value mapping (JDBC ResultSetConverter): CHAR/VARCHAR/CLOB -> string,
INT/SMALLINT -> number, BIGINT -> parsed number, DECIMAL/FLOAT -> double,
BOOLEAN -> boolean, BYTE/BLOB -> Buffer, DATE/DATETIME -> string.

## Design decisions (so far)

- N-API addon (node-addon-api) linking sqli + driver/convert.cpp +
  driver/insert_table_name.cpp directly. No ODBC, no unixODBC.
- One AsyncWorker per operation; a per-connection JS promise queue
  serializes access (sqli is not thread-safe).
- JS-side pool for `poolSize`; a transaction pins one connection.
- Bind type chosen from paramDescs() when available, else inferred from
  the JS value (int -> INTEGER/BIGINT, float -> FLOAT, string -> CHAR,
  boolean -> BOOLEAN, Buffer -> BYTE or smart LOB, Date -> DATETIME,
  bigint -> BIGINT, null -> paramDesc type or CHAR).

## Bind encodings used by the ODBC driver (driver/bind_encode.cpp)

| JS-side intent | ifxType | data | precisionField |
|---|---|---|---|
| int32 | kInteger | encodeInteger | kIntegerBindPrecision |
| int16 | kSmallInt | encodeSmallInt | kSmallIntBindPrecision |
| int64 | kBigInt | encodeBigInt | kBigIntBindPrecision |
| double | kFloat | encodeFloat | kFloatBindPrecision |
| boolean | kBooleanBindType (45) | encodeBoolean | kBooleanBindPrecision; owner "informix", type "boolean" |
| string | kChar or kVarchar | encodeChar(s) | s.size() |
| Buffer (BYTE) | kByte | encodeSimpleLobPlaceholder(len) | 0; simpleLobContent = bytes |
| text LOB (TEXT) | kText | encodeSimpleLobPlaceholder(len) | 0; simpleLobContent = bytes |
| date | kDate | encodeDateUnixDays(days) | 4 |
| decimal string | kDecimal | encodeDecimal(s)->data | ->scale |
| datetime | kDatetime | encodeDatetime(v, q) | q; q = (14<<8)/0x0A (YEAR TO SECOND) or (19<<8)/0x0F (YEAR TO FRACTION(5), fraction = ns/10000) |

NULL: ifxType = real target type. SQL type -> ifx map is ifxTypeForParameterType
(kSerial binds as kInteger etc.).

## Socket timeouts (added for the Node work)

`ConnectParams::timeoutMs` (default 0) bounds connect + handshake ONLY
(SO_RCVTIMEO/SO_SNDTIMEO via `TcpSocket::connect(host, port, timeoutMs)`),
then SqliConnection::connect clears it. `TcpSocket::setTimeouts(ms)` sets a
bound later. It is per-recv, and the server sends nothing while a query
runs, so it caps query run time: a 300 ms bound killed a 3.4 s query
(confirmed live). A timed-out read desyncs the stream: connection is dead.
Error text: "recv() failed: timed out waiting for the server".
JS options: connectTimeoutMs (default 10000, matches the proxy) and
queryTimeoutMs (default 0 = none). ODBC driver behaviour unchanged.

## Build notes

- `sqli/byte_io.h` throws `ProtocolError`; handshake.cpp catches it. So the
  addon must build WITH C++ exceptions (node-gyp disables them by default).
- g++ 16 also compiles all needed files with -std=c++26.

## Fetch coverage (driver/convert.cpp formatCellAsText)

Text conversion exists for: smallint, integer, serial, bigint, bigserial,
char, varchar, nvarchar, date (YYYY-MM-DD), decimal, datetime, boolean
("1"/"0"), float, smallfloat, interval. LOBs (byte, text, blob, clob) need
the separate connection round trips. Everything else -> error; the addon
rejects such columns at prepare time (lvarchar can desync/hang the wire).

Helpers: `parseInsertTableName(const std::string&) -> optional<string>`.
Smart LOB bind param (statement.cpp:384): ifxType kUdt, owner "informix",
typeName "blob"/"clob", precisionField = 72, data = [u32 BE len][locator].

## Catalog SQL quirks (driver/catalog.cpp)

- systables.owner is padded CHAR -> trim(owner). CASE results are padded
  CHAR -> trim(case ...). tabid <= 99 = system table; tabtype 'T'/'V'.
- syscolumns.coltype: base type = coltype & 0xFF, NOT NULL = bit 0x100.
  DECIMAL collength = precision*256 + scale. Avoid UNION ALL + trim and
  casts to lvarchar (hangs). mod()/trunc() return DECIMAL.
- Addon plan: fetch raw coltype/collength/extended_id and decode in JS.

## Verified live (2026-10-27, spike against ifxodbc_dev)

- (Superseded: the package now lives in node/ - see the layout note below.)
  `npm install` runs node-gyp-build and builds
  build/Release/informix_native.node (g++, -std=c++26, -fexceptions
  confirmed in build/informix_native.target.mk).
- connect/prepare/execute/fetch/closeStatement/disconnect all work.
  Integer bind in a SELECT WHERE (no paramDescs hint) works.
- Server reports serverVersion "Informix Dynamic Server Version 9.59.FC7W1"
  (handshake string, not the 14.10 onstat shows).
- Server error: kind "server", message "sqlcode -217: nosuchcol".
- Note: node-gyp's build/ shares the dir name with the CMake build/ dir
  (both gitignored).

## JS layer status (2026-10-27)

Files: node/lib/{native,errors,connection,pool,client,index}.js.
Smoke-tested against proxy's testdb (loaded via informix-proxy's
scripts/init-db.sh copied into ifxodbc_dev, DB_NAME=testdb): connect, query
with ? in WHERE, queryStream fetchSize 3, INSERT with 3 varchar binds,
beginTransaction/UPDATE/rollback, getMetadata list + describe (the
sysxtdtypes left outer join works), prepareStatement/executePrepared, ping,
6 parallel queries over a 4-connection pool, server error -> sqlcode.
Informix rejects `?` in the select list (sqlcode -201) - server rule.
Test db for writes: ifxodbc_test (create with log; vanishes when the
container reinitializes).

## sqli bugs found + fixed via the Node tests (2026-10-27)

1. DECIMAL/MONEY row width: was 1 + ceil(p/2); correct is ESQL DECLEN
   (p + (s & 1) + 3) / 2 (odd scale = +1 byte; floating DECIMAL(p) has
   scale 0xFF). Wrong for even p + odd s, e.g. DECIMAL(12,3): misaligned
   every later column (silently for fixed-width ones). Now
   `sqli::decimalWireWidth`, used by splitRow and decodeDecimal. Floating
   DECIMAL(p) (also count(*)'s type!) now prints natural digits instead of
   255 zeros. MONEY (type 8) added: same wire format as DECIMAL.
2. Fastpath/handshake single-reply readers returned on SQ_ERROR without
   reading the trailing SQ_EOT -> next request read the stale EOT as its
   reply; every later SELECT silently returned 0 rows. Repro: BLOB insert
   with no sbspace (sqlcode -9814). Fix: `readErrorReply` in connection.cpp.
Both affect the ODBC driver too (shared sqli code).

Layout change: package lives in node/ (own package.json + binding.gyp);
`scripts/vendor.js` copies sqli/ + driver/convert.* + insert_table_name.*
into node/vendor/ (gitignored, shipped in tarball). Root build/ stays
CMake's - node-gyp rebuild wipes ./build, so it must not run at root.
sqli unit tests without unixODBC: cmake -S . -B <scratch> -DODBC_LIBRARY=/dev/null,
then build only target sqli_tests, run from tests/sqli.

## Connect-time errors (fixed 2026-10-27)

- selectDatabase used to skim every u16 until 0x000C, ignoring SQ_ERROR:
  a nonexistent database "connected" and every query then failed -349.
  Reply for a missing db is SQ_ERROR(sqlcode -329, isam -111, no message)
  + SQ_EOT; now parsed with readErrorReply.
- Handshake rejection svcError is the sqlcode as int16: 64585 = -951 (bad
  user/password), 64775 = -761 (INFORMIXSERVER mismatch). Message now
  starts "sqlcode N:" so the JS layer and driver's extractSqlcode see it.
- errors.js prepends short descriptions for common sqlcodes (the server
  usually sends only an object name, or nothing).

## Prebuilds (verified 2026-10-27)

- Host-built (CachyOS, g++ 16) binary needed GLIBCXX_3.4.35 + GLIBC_2.38:
  would not load on Debian/Ubuntu LTS. With -static-libstdc++ it still
  needed glibc 2.36+ (static libstdc++ built against host glibc).
- Fix: `npm run prebuild:linux` (scripts/prebuild-linux.sh): glibc build in
  quay.io/pypa/manylinux_2_28_x86_64 (GCC 14 compiles the codebase; result
  needs GLIBC_2.25 max, no libstdc++), musl build in node:22-alpine (g++ 15).
  prebuildify --tag-libc names them *.glibc.node / *.musl.node.
- Avoid std::stoi/strtol in addon code: glibc 2.38 headers redirect to
  __isoc23_strtol (new symbol version).
- One tarball verified with no compiler on node:22-bookworm-slim,
  node:18-bullseye-slim, node:22-alpine (live query against ifxodbc_dev).
- npm 12 blocks dependency install scripts unless approved; prebuilds load
  at runtime regardless, so they are the primary install path.

## Repo integration tests (sqli-only, no unixODBC needed)

cmake -S . -B <dir> -DIFX_ENABLE_INTEGRATION_TESTS=ON -DODBC_LIBRARY=/dev/null
then build type_probe/transactions/bind/smart_lob _integration_test and run
from <dir>/tests/integration. test_smart_lob needs sbspace `sbspace1`; the
Node LOB tests need the default `sbspace`. All four passed 2026-10-27.

## Defaults (2026-10-27)

All defaults live in lib/defaults.js (BUILTIN frozen object + current copy):
host, port, server, database, username, password, appName,
connectTimeoutMs, queryTimeoutMs, int64, decimal, fetchSize, maxRows,
poolSize, isolationLevel. Exposed as getDefaults/setDefaults/resetDefaults/
BUILTIN_DEFAULTS. Read at call time via resolveConnectionConfig /
resolveQueryOptions / defaults.current(). addon.cpp fallbacks are unused by
the JS layer (it always passes every field).
