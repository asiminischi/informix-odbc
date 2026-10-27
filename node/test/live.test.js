'use strict';

// Live-server tests. Needs a reachable Informix with a logged database
// (default: the docker/ dev container and `ifxodbc_test`):
//
//   docker exec ifxodbc_dev bash -lc 'echo "create database ifxodbc_test with log;" | dbaccess - -'
//   npm test
//
// Override with IFX_HOST / IFX_PORT / IFX_SERVER / IFX_USER / IFX_PASSWORD /
// IFX_DATABASE. Every test is skipped if the server can't be reached.

const test = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const { connect, createPool, InformixClient, InformixError, getDefaults, setDefaults, resetDefaults, BUILTIN_DEFAULTS } =
  require('../lib');

const config = {
  host: process.env.IFX_HOST ?? '127.0.0.1',
  port: Number(process.env.IFX_PORT ?? 9088),
  server: process.env.IFX_SERVER ?? 'informix',
  username: process.env.IFX_USER ?? 'informix',
  password: process.env.IFX_PASSWORD ?? 'in4mix',
  database: process.env.IFX_DATABASE ?? 'ifxodbc_test',
};

let skip = false;
let conn;

test.before(async () => {
  try {
    conn = await connect(config);
  } catch (err) {
    skip = `Informix not reachable at ${config.host}:${config.port}/${config.database}: ${err.message}`;
  }
});

test.after(async () => {
  if (conn) await conn.close();
});

function live(name, fn) {
  test(name, async (t) => {
    if (skip) return t.skip(skip);
    // A fatal (transport/protocol) failure closes the shared connection;
    // reopen it so one broken test doesn't cascade into the rest.
    if (!conn.isConnected) conn = await connect(config);
    await fn(t);
  });
}

async function recreate(table, ddl) {
  await conn.execute(`drop table if exists ${table}`);
  await conn.execute(`create table ${table} (${ddl})`);
}

live('scalar types round-trip through binds', async () => {
  await recreate(
    'njs_types',
    'id serial, si smallint, i integer, bi bigint, f float, sf smallfloat, d decimal(12,3),' +
      ' c char(5), vc varchar(40), dt date, ts datetime year to second, tf datetime year to fraction(5), b boolean',
  );
  const rowCount = await conn.execute(
    'insert into njs_types (si, i, bi, f, sf, d, c, vc, dt, ts, tf, b) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
    [
      -12,
      123456,
      9007199254740991,
      3.25,
      1.5,
      '-1234.567',
      'ab',
      'hello world',
      '2024-02-29',
      new Date(Date.UTC(2024, 1, 29, 13, 14, 15)),
      new Date(Date.UTC(2024, 1, 29, 13, 14, 15, 678)),
      true,
    ],
  );
  assert.equal(rowCount, 1);
  const { rows, columns } = await conn.query('select * from njs_types');
  assert.deepEqual(
    columns.map((c) => c.type),
    ['serial', 'smallint', 'integer', 'bigint', 'float', 'smallfloat', 'decimal', 'char', 'varchar', 'date',
      'datetime', 'datetime', 'boolean'],
  );
  assert.deepEqual(columns[6], { name: 'd', type: 'decimal', precision: 12, scale: 3, nullable: true });
  const r = rows[0];
  assert.equal(r.si, -12);
  assert.equal(r.i, 123456);
  assert.equal(r.bi, 9007199254740991);
  assert.equal(r.f, 3.25);
  assert.equal(r.sf, 1.5);
  assert.equal(r.d, -1234.567);
  assert.equal(r.c.trimEnd(), 'ab');
  assert.equal(r.vc, 'hello world');
  assert.equal(r.dt, '2024-02-29');
  assert.equal(r.ts, '2024-02-29 13:14:15');
  assert.match(r.tf, /^2024-02-29 13:14:15\.678/);
  assert.equal(r.b, true);
});

live('NULLs of every type, with and without a target-column hint', async () => {
  await recreate('njs_nulls', 'si smallint, i integer, bi bigint, f float, d decimal(8,2), vc varchar(10), dt date, b boolean');
  await conn.execute('insert into njs_nulls values (?, ?, ?, ?, ?, ?, ?, ?)', [null, null, null, null, null, null, null, null]);
  await conn.execute('insert into njs_nulls (i, b) values (?, ?)', [7, false]);
  // Informix sorts NULLs first.
  const { rows } = await conn.query('select * from njs_nulls order by i');
  assert.deepEqual(Object.values(rows[0]), [null, null, null, null, null, null, null, null]);
  assert.deepEqual(rows[1], { si: null, i: 7, bi: null, f: null, d: null, vc: null, dt: null, b: false });
  // NULL in a WHERE (no hint): CHAR fallback.
  const r = await conn.query('select count(*) as n from njs_nulls where vc = ? or i = ?', [null, 7]);
  assert.equal(r.rows[0].n, 1);
});

live('int64 and decimal output modes', async () => {
  const c = await connect({ ...config, int64: 'bigint', decimal: 'string' });
  try {
    await c.execute('drop table if exists njs_modes');
    await c.execute('create table njs_modes (bi bigint, d decimal(30,10))');
    await c.execute('insert into njs_modes values (?, ?)', [9223372036854775807n, '12345678901234567890.0123456789']);
    const { rows } = await c.query('select * from njs_modes');
    assert.equal(rows[0].bi, 9223372036854775807n);
    assert.equal(rows[0].d, '12345678901234567890.0123456789');
  } finally {
    await c.close();
  }
  const s = await connect({ ...config, int64: 'string' });
  try {
    const { rows } = await s.query('select bi from njs_modes');
    assert.equal(rows[0].bi, '9223372036854775807');
  } finally {
    await s.close();
  }
});

live('BYTE/TEXT simple LOBs and BLOB/CLOB smart LOBs', async () => {
  await recreate('njs_lobs', 'id integer, by byte, tx text, bl blob, cl clob');
  const big = Buffer.alloc(200 * 1024);
  for (let i = 0; i < big.length; i++) big[i] = i % 251;
  await conn.execute('insert into njs_lobs values (?, ?, ?, ?, ?)', [1, big, 'some text', Buffer.from([0, 1, 2, 255]), 'clob text']);
  await conn.execute('insert into njs_lobs values (?, ?, ?, ?, ?)', [2, null, null, null, null]);
  const { rows } = await conn.query('select * from njs_lobs order by id');
  assert.ok(Buffer.isBuffer(rows[0].by));
  assert.ok(rows[0].by.equals(big));
  assert.equal(rows[0].tx, 'some text');
  assert.deepEqual([...rows[0].bl], [0, 1, 2, 255]);
  assert.equal(rows[0].cl, 'clob text');
  assert.deepEqual(rows[1], { id: 2, by: null, tx: null, bl: null, cl: null });
});

live('fetch batching, maxRows, queryStream and iterate', async () => {
  await recreate('njs_many', 'n integer');
  const ins = await conn.prepare('insert into njs_many values (?)');
  for (let i = 0; i < 250; i++) await ins.execute([i]);
  await ins.close();

  const all = await conn.query('select n from njs_many order by n', [], { fetchSize: 7 });
  assert.equal(all.rowCount, 250);
  assert.equal(all.rows[249].n, 249);

  const capped = await conn.query('select n from njs_many order by n', [], { maxRows: 10, fetchSize: 3 });
  assert.equal(capped.rows.length, 10);

  let sum = 0;
  const s = await conn.queryStream('select n from njs_many', [], (r) => (sum += r.n), { fetchSize: 64 });
  assert.equal(s.rowCount, 250);
  assert.equal(sum, (249 * 250) / 2);

  let seen = 0;
  for await (const row of conn.iterate('select n from njs_many order by n', [], { fetchSize: 16 })) {
    seen++;
    if (row.n === 20) break;
  }
  assert.equal(seen, 21);
  // break released the connection's lock:
  assert.equal((await conn.query('select count(*) as c from njs_many')).rows[0].c, 250);
});

live('prepared statements re-execute with new params, including an open cursor', async () => {
  const st = await conn.prepare('select tabid from systables where tabid <= ? order by tabid');
  assert.equal(st.parameterCount, 1);
  assert.equal((await st.query([2], { maxRows: 1 })).rows.length, 1); // cursor left open
  assert.equal((await st.query([3])).rows.length, 3);
  await st.close();
  await assert.rejects(st.query([1]), /closed/);
});

live('transactions: rollback hides, commit persists, callback form', async () => {
  await recreate('njs_tx', 'n integer');
  await conn.beginTransaction();
  await conn.execute('insert into njs_tx values (1)');
  await conn.rollback();
  assert.equal((await conn.query('select count(*) as c from njs_tx')).rows[0].c, 0);

  await conn.transaction(async (c) => {
    await c.execute('insert into njs_tx values (?)', [2]);
  }, 'READ_COMMITTED');
  const other = await connect(config);
  try {
    assert.equal((await other.query('select count(*) as c from njs_tx')).rows[0].c, 1);
  } finally {
    await other.close();
  }

  await assert.rejects(
    conn.transaction(async (c) => {
      await c.execute('insert into njs_tx values (3)');
      throw new Error('boom');
    }),
    /boom/,
  );
  assert.equal((await conn.query('select count(*) as c from njs_tx')).rows[0].c, 1);
  assert.equal(conn.inTransaction, false);
});

live('server errors carry sqlcode and leave the connection usable', async () => {
  const err = await conn.query('select * from njs_no_such_table').catch((e) => e);
  assert.ok(err instanceof InformixError);
  assert.equal(err.kind, 'server');
  assert.equal(err.sqlcode, -206);
  assert.equal(conn.isConnected, true);
  assert.equal((await conn.query('select tabid from systables where tabid = 1')).rows[0].tabid, 1);
});

live('usage errors: wrong parameter count, unsupported value type', async () => {
  await assert.rejects(conn.query('select tabid from systables where tabid = ?', []), /1 parameter marker/);
  await assert.rejects(conn.query('select tabid from systables where tabid = ?', [{}]), /unsupported parameter type/);
  assert.equal(conn.isConnected, true);
});

live('unsupported column types are rejected at prepare, not mid-fetch', async () => {
  await recreate('njs_lvc', 'v lvarchar(100)');
  const err = await conn.query('select v from njs_lvc').catch((e) => e);
  assert.ok(err instanceof InformixError);
  assert.match(err.message, /lvarchar/);
  assert.equal(conn.isConnected, true);
  assert.equal((await conn.query('select cast(v as varchar(100)) as v from njs_lvc')).rowCount, 0);
});

live('pool runs queries concurrently and recycles connections', async () => {
  const pool = createPool({ ...config, poolSize: 3 });
  try {
    const results = await Promise.all(
      Array.from({ length: 12 }, (_, i) => pool.query('select tabid from systables where tabid = ?', [i + 1])),
    );
    assert.deepEqual(results.map((r) => r.rows[0].tabid), Array.from({ length: 12 }, (_, i) => i + 1));
    assert.ok(pool.size <= 3);
    const n = await pool.transaction(async (c) => (await c.query('select count(*) as c from systables')).rows[0].c);
    assert.ok(n > 0);
  } finally {
    await pool.close();
  }
});

live('InformixClient (proxy-compatible API) end to end', async () => {
  const client = new InformixClient('localhost', 50051);
  const info = await client.connect({ ...config, poolSize: 2 });
  try {
    assert.match(info.serverVersion, /Informix/);
    assert.ok(client.isConnected);
    await client.execute('drop table if exists njs_client');
    await client.execute('create table njs_client (id serial, name varchar(20))');
    await client.beginTransaction();
    await client.execute('insert into njs_client (name) values (?)', ['a']);
    assert.equal((await client.query('select count(*) as c from njs_client')).rows[0].c, 1);
    await client.rollback();
    assert.equal((await client.query('select count(*) as c from njs_client')).rows[0].c, 0);
    const ps = await client.prepareStatement('insert into njs_client (name) values (?)');
    assert.equal(ps.parameterCount, 1);
    await client.executePrepared(ps.statementId, ['b']);
    await client.closePrepared(ps.statementId);
    assert.deepEqual(await client.batch(['update njs_client set name = name', 'delete from njs_client']), [1, 1]);
    const [meta] = await client.getMetadata('njs_client');
    assert.deepEqual(meta.columns.map((c) => [c.name, c.type]), [['id', 'serial'], ['name', 'varchar']]);
    assert.ok((await client.getMetadata()).some((t) => t.name === 'njs_client'));
    assert.equal((await client.ping()).alive, true);
  } finally {
    await client.disconnect();
  }
  assert.equal(client.isConnected, false);
});

const SLOW_SQL =
  'select count(*) as n from syscolumns a, syscolumns b, syscolumns c where a.tabid = 1 and a.colno + b.colno + c.colno > 0';

live('connectTimeoutMs bounds only the login, not long-running queries', async () => {
  const c = await connect({ ...config, connectTimeoutMs: 300 });
  try {
    const started = Date.now();
    const { rows } = await c.query(SLOW_SQL);
    assert.ok(rows[0].n > 0);
    assert.ok(Date.now() - started > 300, 'test query should outlast the connect timeout');
  } finally {
    await c.close();
  }
});

live('queryTimeoutMs cuts off a slow query and closes the connection', async () => {
  const c = await connect({ ...config, queryTimeoutMs: 300 });
  const err = await c.query(SLOW_SQL).catch((e) => e);
  assert.ok(err instanceof InformixError);
  assert.equal(err.kind, 'transport');
  assert.match(err.message, /timed out/);
  assert.equal(c.isConnected, false);
  await c.close();
});

live('login and database errors are server errors with sqlcodes', async () => {
  const cases = [
    [{ password: 'definitely-wrong' }, -951],
    [{ server: 'njs_not_this_server' }, -761],
    [{ database: 'njs_no_such_database' }, -329],
  ];
  for (const [override, sqlcode] of cases) {
    const err = await connect({ ...config, ...override }).catch((e) => e);
    assert.ok(err instanceof InformixError, JSON.stringify(override));
    assert.equal(err.kind, 'server');
    assert.equal(err.sqlcode, sqlcode);
  }
  const err = await connect({ ...config, database: 'njs_no_such_database' }).catch((e) => e);
  assert.match(err.message, /database not found/);
});

test('defaults: validated, overridable, resettable', () => {
  try {
    assert.deepEqual(getDefaults(), { ...BUILTIN_DEFAULTS });
    assert.equal(getDefaults().fetchSize, 500);
    const d = setDefaults({ fetchSize: 64, decimal: 'string' });
    assert.equal(d.fetchSize, 64);
    assert.equal(getDefaults().decimal, 'string');
    assert.throws(() => setDefaults({ nope: 1 }), /unknown default/);
    assert.throws(() => setDefaults({ int64: 'float' }), /int64 must be one of/);
    assert.throws(() => setDefaults({ poolSize: 0 }), /positive integer/);
    assert.equal(getDefaults().fetchSize, 64, 'a rejected call changes nothing');
  } finally {
    resetDefaults();
  }
  assert.deepEqual(getDefaults(), { ...BUILTIN_DEFAULTS });
});

live('setDefaults changes what new connections use', async () => {
  try {
    setDefaults({ decimal: 'string', database: config.database });
    const { database, ...rest } = config;
    const c = await connect(rest);
    try {
      const { rows } = await c.query('select cast(1.50 as decimal(5,2)) as d from systables where tabid = 1');
      assert.equal(rows[0].d, '1.50');
    } finally {
      await c.close();
    }
  } finally {
    resetDefaults();
  }
});

test('a server that never answers times out as a transport error', async () => {
  const sockets = [];
  const server = net.createServer((s) => sockets.push(s));
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  try {
    const started = Date.now();
    const err = await connect({ ...config, host: '127.0.0.1', port: server.address().port, connectTimeoutMs: 500 }).catch((e) => e);
    assert.ok(err instanceof InformixError);
    assert.equal(err.kind, 'transport');
    assert.ok(Date.now() - started < 5000);
  } finally {
    for (const s of sockets) s.destroy();
    server.close();
  }
});

test('connection refused is a transport error', async () => {
  const err = await connect({ ...config, host: '127.0.0.1', port: 1, connectTimeoutMs: 2000 }).catch((e) => e);
  assert.ok(err instanceof InformixError);
  assert.equal(err.kind, 'transport');
});
