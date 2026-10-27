# @postarodiy/informix-native

Native Node.js client for IBM Informix. It talks Informix's SQLI wire
protocol directly (the `sqli/` layer of
[informix-odbc](../README.md)), so you need no JDBC driver, no ODBC Driver
Manager, no Java and no informix-proxy. It's a Node-API addon, so one
binary works across Node versions.

It ships a drop-in replacement for `@postarodiy/informix-client` (the
informix-proxy gRPC client). Migrating is an import swap.

## Install

```
npm install @postarodiy/informix-native
```

- **Prebuilt binaries** in `prebuilds/` load with no compiler and no install
  script. Currently: linux-x64 on glibc 2.25+ (Debian 10+, Ubuntu 18.04+,
  RHEL 8+) and on musl (Alpine). Tested on Debian 11/12 and Alpine 3.24 with
  Node 18 and 22.
- **Other platforms** build from source on install. That needs a C++26
  compiler and Python (for node-gyp). Tested with g++ 16 and clang++ 23;
  older compilers are untested. Windows/macOS build settings exist in
  `binding.gyp` but haven't been tried yet.
- **npm 12+** blocks dependency install scripts by default, so from-source
  builds need approval:
  `npm install-scripts approve @postarodiy/informix-native`, then
  `npm rebuild @postarodiy/informix-native`.

## Migrating from informix-proxy

```diff
-import { InformixClient } from '@postarodiy/informix-client';
+import { InformixClient } from '@postarodiy/informix-native';

 const client = new InformixClient('localhost', 50051); // proxy address: now ignored
 await client.connect({
   host: 'informix-db',   // now resolved by *this* process, not by the proxy
   port: 9088,
   database: 'testdb',
   username: 'informix',
   password: 'in4mix',
   poolSize: 10,
 });
```

Every method keeps its name and result shape: `connect`, `disconnect`,
`ping`, `query`, `queryStream`, `execute`, `batch`, `beginTransaction`,
`commit`, `rollback`, `prepareStatement`, `executePrepared`,
`closePrepared`, `getMetadata`, `resetConnection`, `isConnected`. Values come
back the way the proxy returned them: BIGINT and DECIMAL as `number`, dates
as strings, BYTE/BLOB as `Buffer`.

Differences to be aware of:

- `host` in `connect()` must be reachable from your app now. It's no longer
  resolved inside the proxy's Docker network.
- `server` (the INFORMIXSERVER name) defaults to `'informix'`. Set it if your
  instance is named differently.
- `properties` is accepted and ignored.
- With no proxy in between, there are no proxy-side Prometheus metrics.

## Direct API

```js
const { connect, createPool } = require('@postarodiy/informix-native');

const conn = await connect({
  host: 'db.example.com', port: 9088, server: 'ol_informix1410',
  database: 'stores', username: 'informix', password: 'secret',
  decimal: 'string', // exact DECIMAL/MONEY values
});

const { rows, columns } = await conn.query('select * from customer where state = ?', ['CA']);
const affected = await conn.execute('update customer set phone = ? where customer_num = ?', ['555-1234', 101]);

for await (const row of conn.iterate('select * from orders', [], { fetchSize: 1000 })) {
  // streamed; the connection is locked until the loop ends
}

await conn.transaction(async (tx) => {
  await tx.execute('insert into log (msg) values (?)', ['hello']);
}, 'REPEATABLE_READ');

const stmt = await conn.prepare('insert into items (id, name) values (?, ?)');
for (const [id, name] of data) await stmt.execute([id, name]);
await stmt.close();

await conn.close();

// A pool for concurrent work
const pool = createPool({ ...config, poolSize: 10 });
await Promise.all(ids.map((id) => pool.query('select * from orders where id = ?', [id])));
await pool.close();
```

A `Connection` runs one call at a time, because the SQLI wire carries one
request at a time. Concurrent calls queue. Use a `Pool` or
`InformixClient` for parallelism.

### Connection options

| option | default | |
|---|---|---|
| `host`, `port` | `localhost`, `9088` | Informix listener (onsoctcp) |
| `server` | `informix` | INFORMIXSERVER name |
| `database`, `username`, `password` | | |
| `connectTimeoutMs` | `10000` | bounds the TCP connect and login handshake (0 = never) |
| `queryTimeoutMs` | `0` (none) | bounds each wait for the server while a statement runs, which effectively caps query run time. A timeout closes the connection (it's not a cancel). |
| `int64` | `'number'` | BIGINT/BIGSERIAL as `'number'`, `'bigint'` or `'string'` |
| `decimal` | `'number'` | DECIMAL/MONEY as `'number'` or exact `'string'` |

### Defaults

All defaults live in one file, [`lib/defaults.js`](lib/defaults.js). The
connection-option table above lists them, along with `fetchSize` (500),
`maxRows` (0), `poolSize` (10) and `isolationLevel` (`READ_COMMITTED`,
used by `InformixClient.beginTransaction()`). Per-call options always win
over defaults. To change defaults for the whole process:

```js
const { setDefaults, getDefaults, resetDefaults } = require('@postarodiy/informix-native');

setDefaults({ server: 'ol_prod', decimal: 'string', queryTimeoutMs: 60000 });
```

`setDefaults` validates every value and throws `InformixError` on an
unknown key or a bad value. Changes apply to connections, pools and
queries created afterwards; already-open connections keep their settings.

### Type mapping

| Informix | JS (result) | JS (parameter) |
|---|---|---|
| SMALLINT, INTEGER, SERIAL, FLOAT, SMALLFLOAT | `number` | `number` |
| BIGINT, BIGSERIAL | `number` (see `int64`) | `number`, `bigint` |
| DECIMAL, MONEY | `number` (see `decimal`) | `number`, `string` (exact) |
| CHAR, VARCHAR, NVARCHAR | `string` (CHAR keeps its padding) | `string` |
| DATE | `'YYYY-MM-DD'` | `'YYYY-MM-DD'`, `Date` (UTC day) |
| DATETIME | `'YYYY-MM-DD HH:MM:SS[.fffff]'` | `string`, `Date` (UTC, YEAR TO FRACTION(5)) |
| INTERVAL | `string` | `string` |
| BOOLEAN | `boolean` | `boolean` |
| BYTE, BLOB | `Buffer` | `Buffer` |
| TEXT, CLOB | `string` | `string`, `Buffer` |
| NULL | `null` | `null`, `undefined` |

### Errors

Everything throws `InformixError`. It has a `kind` and a `sqlcode`:

- **`server`**: the statement failed. The connection is still usable, and
  `sqlcode` holds the Informix code (e.g. -206, table not found).
- **`transport` / `protocol`**: network failure, timeout or unexpected
  bytes. The connection is closed (`isFatal` is `true`). A Pool discards
  such connections automatically.
- **`usage`**: a client-side mistake, e.g. a wrong parameter count.

## Limitations

These are inherited from the protocol layer. See the repo's `STATUS.md`.

- **Unsupported column types.** LVARCHAR, ROW/SET/LIST and other UDT
  columns are rejected when a statement is prepared, not mid-fetch. Cast
  them in SQL instead, e.g. `cast(col as varchar(255))`.
- **BLOB/CLOB parameters** work only in `INSERT INTO <table> ... VALUES (?)`
  statements, where the target column can be resolved.
- **Parameter type inference.** In a SELECT (or anywhere the server doesn't
  describe the parameters), the bind type comes from the JS value. Strings
  go as VARCHAR, at most 32767 bytes.
- **No `?` in the select list.** Informix itself rejects it (sqlcode -201).
- **No TLS** (the SSL listener isn't implemented yet).
- **No generated keys.** Serial values from INSERT aren't returned. Run
  `select dbinfo('sqlca.sqlerrd1') as id from systables where tabid = 1`
  right after the INSERT, on the same `Connection` (with a pool, inside
  `withConnection`/`transaction`).

## Development

From an informix-odbc checkout:

```
cd node
npm install          # copies ../sqli etc. into vendor/ and builds
npm test             # live tests against the docker/ dev server
npm run prebuild:linux  # portable linux-x64 glibc + musl binaries (needs Docker)
npm pack
```

`npm run prebuildify` builds for the current machine only. On Linux that
binary requires the build host's glibc version, so publish the output of
`prebuild:linux` instead.

The tests expect the dev container from `../docker` and a logged database
named `ifxodbc_test`. The BLOB/CLOB tests also need an sbspace.

```
docker exec ifxodbc_dev bash -lc 'echo "create database ifxodbc_test with log;" | dbaccess - -'
docker exec ifxodbc_dev bash -lc 'cd /opt/ibm/data/spaces && touch sbspace.000 && chmod 660 sbspace.000 && onspaces -c -S sbspace -p /opt/ibm/data/spaces/sbspace.000 -o 0 -s 100000'
```

Override the target with `IFX_HOST`, `IFX_PORT`, `IFX_SERVER`, `IFX_USER`,
`IFX_PASSWORD` and `IFX_DATABASE`.
