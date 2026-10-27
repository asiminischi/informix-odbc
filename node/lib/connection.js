'use strict';

const { NativeConnection } = require('./native');
const { InformixError, wrapNativeError } = require('./errors');
const { resolveConnectionConfig, resolveQueryOptions } = require('./defaults');

const ISOLATION_SQL = {
  READ_UNCOMMITTED: 'set isolation to dirty read',
  READ_COMMITTED: 'set isolation to committed read',
  CURSOR_STABILITY: 'set isolation to cursor stability',
  REPEATABLE_READ: 'set isolation to repeatable read',
  // Informix's REPEATABLE READ already gives serializable behaviour.
  SERIALIZABLE: 'set isolation to repeatable read',
};

// syscolumns.coltype base codes (coltype & 0xFF) -> type names.
const COLTYPE_NAMES = {
  0: 'char', 1: 'smallint', 2: 'integer', 3: 'float', 4: 'smallfloat', 5: 'decimal',
  6: 'serial', 7: 'date', 8: 'money', 10: 'datetime', 11: 'byte', 12: 'text',
  13: 'varchar', 14: 'interval', 15: 'nchar', 16: 'nvarchar', 17: 'int8', 18: 'serial8',
  40: 'lvarchar', 41: 'boolean', 43: 'lvarchar', 45: 'boolean', 52: 'bigint', 53: 'bigserial',
};

function toObjects(columns, rows) {
  const names = columns.map((c) => c.name);
  return rows.map((row) => {
    const obj = {};
    for (let i = 0; i < names.length; i++) obj[names[i]] = row[i];
    return obj;
  });
}

function publicColumns(columns) {
  return columns.map(({ name, type, precision, scale, nullable }) => ({ name, type, precision, scale, nullable }));
}

// One physical Informix session. Operations are serialized: each call waits
// for the previous one to finish, since the SQLI wire carries exactly one
// request at a time. Use a Pool (or InformixClient) for concurrency.
class Connection {
  #native = new NativeConnection();
  #queue = Promise.resolve();
  #config;
  #info = null;
  #inTransaction = false;

  constructor(config) {
    if (!config || typeof config !== 'object') throw new InformixError('connection config is required');
    this.#config = config;
  }

  /**
   * Opens a new connection.
   * config: { host, port, database, username, password, server, appName,
   *           connectTimeoutMs, queryTimeoutMs,
   *           int64: 'number'|'bigint'|'string', decimal: 'number'|'string' }
   * Missing fields come from lib/defaults.js (see setDefaults()).
   */
  static async connect(config) {
    const conn = new Connection(config);
    await conn.open();
    return conn;
  }

  async open() {
    return this.#run(async () => {
      this.#info = await this.#call('connect', resolveConnectionConfig(this.#config));
      return this.#info;
    });
  }

  get isConnected() {
    return this.#native.connected;
  }

  get inTransaction() {
    return this.#inTransaction;
  }

  get serverVersion() {
    return this.#info ? this.#info.serverVersion : null;
  }

  // --- serialization -------------------------------------------------------

  // Resolves to a release function once every earlier operation is done.
  #lock() {
    let release;
    const slot = new Promise((resolve) => {
      release = resolve;
    });
    const prev = this.#queue;
    this.#queue = prev.then(() => slot);
    return prev.then(() => release);
  }

  async #run(fn) {
    const release = await this.#lock();
    try {
      return await fn();
    } finally {
      release();
    }
  }

  async #call(method, ...args) {
    try {
      return await this.#native[method](...args);
    } catch (err) {
      const wrapped = wrapNativeError(err);
      if (wrapped.isFatal) this.#inTransaction = false;
      throw wrapped;
    }
  }

  async #closeQuietly(id) {
    try {
      await this.#native.closeStatement(id);
    } catch {
      // The statement is gone either way; the original error matters more.
    }
  }

  async #withStatement(sql, fn) {
    const st = await this.#call('prepare', sql);
    try {
      return await fn(st);
    } finally {
      await this.#closeQuietly(st.id);
    }
  }

  async #executeOn(st, params) {
    if (!Array.isArray(params)) throw new InformixError('params must be an array');
    return this.#call('execute', st.id, params);
  }

  async #drain(st, options, onBatch) {
    const { fetchSize, maxRows } = resolveQueryOptions(options);
    let count = 0;
    for (;;) {
      const want = maxRows > 0 ? Math.min(fetchSize, maxRows - count) : fetchSize;
      const batch = await this.#call('fetch', st.id, want);
      let rows = batch.rows;
      if (maxRows > 0 && count + rows.length > maxRows) rows = rows.slice(0, maxRows - count);
      count += rows.length;
      if (rows.length) await onBatch(rows);
      if (batch.done || (maxRows > 0 && count >= maxRows)) return count;
    }
  }

  // --- queries -------------------------------------------------------------

  /** Runs a statement and returns every row: { rows, columns, rowCount }. */
  async query(sql, params = [], options = {}) {
    return this.#run(() =>
      this.#withStatement(sql, async (st) => {
        const exec = await this.#executeOn(st, params);
        if (!st.hasResultSet) return { rows: [], columns: null, rowCount: exec.rowCount };
        const rows = [];
        await this.#drain(st, options, async (batch) => {
          for (const r of toObjects(st.columns, batch)) rows.push(r);
        });
        return { rows, columns: publicColumns(st.columns), rowCount: rows.length };
      }),
    );
  }

  /** Calls onRow(row) for each row, fetching fetchSize rows per round trip. */
  async queryStream(sql, params, onRow, options = {}) {
    return this.#run(() =>
      this.#withStatement(sql, async (st) => {
        await this.#executeOn(st, params ?? []);
        if (!st.hasResultSet) return { columns: null, rowCount: 0 };
        const rowCount = await this.#drain(st, options, async (batch) => {
          for (const r of toObjects(st.columns, batch)) await onRow(r);
        });
        return { columns: publicColumns(st.columns), rowCount };
      }),
    );
  }

  /**
   * Async iterator over a result set:
   *   for await (const row of conn.iterate('select ...')) { ... }
   * The connection stays locked until the loop ends (break/return/throw
   * release it), so don't run other queries on the same connection inside
   * the loop body - that would wait forever.
   */
  async *iterate(sql, params = [], options = {}) {
    const { fetchSize } = resolveQueryOptions(options);
    const release = await this.#lock();
    let st = null;
    try {
      st = await this.#call('prepare', sql);
      await this.#executeOn(st, params);
      if (!st.hasResultSet) return;
      for (;;) {
        const batch = await this.#call('fetch', st.id, fetchSize);
        for (const r of toObjects(st.columns, batch.rows)) yield r;
        if (batch.done) return;
      }
    } finally {
      if (st) await this.#closeQuietly(st.id);
      release();
    }
  }

  /** Runs INSERT/UPDATE/DELETE/DDL; returns the affected row count. */
  async execute(sql, params = []) {
    return this.#run(() =>
      this.#withStatement(sql, async (st) => {
        const exec = await this.#executeOn(st, params);
        return exec.rowCount;
      }),
    );
  }

  /** Runs several parameterless statements in order; returns their row counts. */
  async batch(statements) {
    return this.#run(async () => {
      const counts = [];
      for (const sql of statements) {
        counts.push(await this.#withStatement(sql, async (st) => (await this.#executeOn(st, [])).rowCount));
      }
      return counts;
    });
  }

  /** Prepares a statement for repeated execution. Call close() when done. */
  async prepare(sql) {
    const st = await this.#run(() => this.#call('prepare', sql));
    return new PreparedStatement(st, {
      run: (fn) => this.#run(fn),
      call: (m, ...a) => this.#call(m, ...a),
      drain: (s, o, cb) => this.#drain(s, o, cb),
      closeQuietly: (id) => this.#closeQuietly(id),
    });
  }

  // --- transactions --------------------------------------------------------

  async beginTransaction(isolationLevel) {
    return this.#run(async () => {
      if (this.#inTransaction) throw new InformixError('a transaction is already open on this connection');
      if (isolationLevel) {
        const sql = ISOLATION_SQL[isolationLevel];
        if (!sql) throw new InformixError(`unknown isolation level: ${isolationLevel}`);
        await this.#call('command', sql);
      }
      await this.#call('begin');
      this.#inTransaction = true;
    });
  }

  async commit() {
    return this.#run(async () => {
      if (!this.#inTransaction) throw new InformixError('no transaction is open');
      try {
        await this.#call('commit');
      } finally {
        this.#inTransaction = false;
      }
    });
  }

  async rollback() {
    return this.#run(async () => {
      if (!this.#inTransaction) throw new InformixError('no transaction is open');
      try {
        await this.#call('rollback');
      } finally {
        this.#inTransaction = false;
      }
    });
  }

  /** Runs fn(conn) inside a transaction: commits on success, rolls back on throw. */
  async transaction(fn, isolationLevel) {
    await this.beginTransaction(isolationLevel);
    let result;
    try {
      result = await fn(this);
    } catch (err) {
      if (this.#inTransaction && this.isConnected) {
        try {
          await this.rollback();
        } catch {
          // Surface the original failure, not the rollback's.
        }
      }
      throw err;
    }
    await this.commit();
    return result;
  }

  // --- misc ----------------------------------------------------------------

  async ping() {
    const start = process.hrtime.bigint();
    try {
      await this.query('select 1 from systables where tabid = 1');
      return { alive: true, latencyMs: Number(process.hrtime.bigint() - start) / 1e6 };
    } catch {
      return { alive: false, latencyMs: Number(process.hrtime.bigint() - start) / 1e6 };
    }
  }

  /**
   * Lists user tables (no argument), or describes one table's columns.
   * Same shape as informix-proxy's GetMetadata: [{ name, schema, type, columns }].
   */
  async getMetadata(tableName = '') {
    if (!tableName) {
      const { rows } = await this.query(
        "select tabname, trim(owner) as owner from systables where tabtype = 'T' and tabid > 99 order by tabname",
      );
      return rows.map((r) => ({ name: r.tabname, schema: r.owner, type: 'TABLE', columns: [] }));
    }
    const { rows } = await this.query(
      'select trim(t.owner) as owner, c.colname, c.coltype, c.collength, c.extended_id, trim(x.name) as xname' +
        ' from systables t join syscolumns c on t.tabid = c.tabid' +
        ' left outer join sysxtdtypes x on x.extended_id = c.extended_id and c.extended_id > 0' +
        ' where t.tabname = ? order by c.colno',
      [tableName],
    );
    if (!rows.length) return [];
    const columns = rows.map((r) => {
      const base = r.coltype & 0xff;
      let precision = r.collength;
      let scale = 0;
      if (base === 5 || base === 8) {
        precision = r.collength >> 8;
        scale = (r.collength & 0xff) === 0xff ? 0 : r.collength & 0xff;
      } else if (base === 13 || base === 16) {
        precision = r.collength & 0xff;
      }
      return {
        name: r.colname,
        type: r.xname || COLTYPE_NAMES[base] || `type${base}`,
        precision,
        scale,
        nullable: (r.coltype & 0x100) === 0,
      };
    });
    return [{ name: tableName, schema: rows[0].owner, type: 'TABLE', columns }];
  }

  /** Closes the session. Any open transaction is rolled back by the server. */
  async close() {
    return this.#run(async () => {
      this.#inTransaction = false;
      await this.#call('disconnect');
    });
  }
}

class PreparedStatement {
  #st;
  #ops;
  #closed = false;

  constructor(st, ops) {
    this.#st = st;
    this.#ops = ops;
  }

  get parameterCount() {
    return this.#st.paramCount;
  }

  get columns() {
    return this.#st.hasResultSet ? publicColumns(this.#st.columns) : null;
  }

  #check() {
    if (this.#closed) throw new InformixError('prepared statement is closed');
  }

  /** Executes with params; returns { rows, columns, rowCount } like query(). */
  async query(params = [], options = {}) {
    this.#check();
    return this.#ops.run(async () => {
      const st = this.#st;
      const exec = await this.#ops.call('execute', st.id, params);
      if (!st.hasResultSet) return { rows: [], columns: null, rowCount: exec.rowCount };
      const rows = [];
      await this.#ops.drain(st, options, async (batch) => {
        for (const r of toObjects(st.columns, batch)) rows.push(r);
      });
      return { rows, columns: publicColumns(st.columns), rowCount: rows.length };
    });
  }

  /** Executes with params; returns the affected row count. */
  async execute(params = []) {
    this.#check();
    return this.#ops.run(async () => (await this.#ops.call('execute', this.#st.id, params)).rowCount);
  }

  async close() {
    if (this.#closed) return;
    this.#closed = true;
    await this.#ops.run(() => this.#ops.closeQuietly(this.#st.id));
  }
}

module.exports = { Connection, PreparedStatement, ISOLATION_SQL };
