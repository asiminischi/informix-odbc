'use strict';

// Drop-in replacement for @postarodiy/informix-client (the informix-proxy
// gRPC client): same class name, methods and result shapes, but it talks
// to Informix directly instead of through the proxy.
//
// Migration: change the import. The constructor's (host, port) used to be
// the proxy's address and are now ignored; the Informix server's address
// comes from connect({ host, port, ... }) exactly as before.

const { randomUUID } = require('node:crypto');
const { Pool } = require('./pool');
const { InformixError } = require('./errors');
const defaults = require('./defaults');

const noopLogger = { info() {}, warn() {}, error() {} };

class InformixClient {
  #pool = null;
  #txConn = null;
  #connectionId = null;
  #serverVersion = null;
  #prepared = new Map();
  #logger;

  constructor(_proxyHost, _proxyPort, options = {}) {
    this.#logger = options.logger ?? noopLogger;
  }

  get isConnected() {
    return this.#pool !== null;
  }

  get connectionId() {
    return this.#connectionId;
  }

  get serverVersion() {
    return this.#serverVersion;
  }

  /**
   * config: { host, port, database, username, password, server?, poolSize?,
   *           connectTimeoutMs?, queryTimeoutMs?, int64?, decimal? }
   */
  async connect(config) {
    if (this.#pool) await this.disconnect();
    const pool = new Pool(
      {
        host: config.host,
        port: config.port,
        database: config.database,
        username: config.username,
        password: config.password,
        server: config.server,
        appName: config.appName,
        connectTimeoutMs: config.connectTimeoutMs,
        queryTimeoutMs: config.queryTimeoutMs,
        int64: config.int64,
        decimal: config.decimal,
      },
      { max: config.poolSize },
    );
    // Open one connection up front so bad credentials fail here, not on
    // the first query.
    const conn = await pool.acquire();
    this.#serverVersion = conn.serverVersion;
    pool.release(conn);
    this.#pool = pool;
    this.#connectionId = randomUUID();
    this.#logger.info('informix connected', {
      connectionId: this.#connectionId,
      host: config.host,
      database: config.database,
    });
    return { connectionId: this.#connectionId, serverVersion: this.#serverVersion };
  }

  async disconnect() {
    const pool = this.#pool;
    const tx = this.#txConn;
    const id = this.#connectionId;
    this.#pool = null;
    this.#txConn = null;
    this.#connectionId = null;
    this.#prepared.clear();
    if (tx) await tx.close().catch(() => {});
    if (pool) await pool.close();
    if (id) this.#logger.info('informix disconnected', { connectionId: id });
  }

  // Kept for API compatibility with the proxy client.
  resetConnection(reason = 'unknown') {
    this.#logger.warn('informix connection reset', { connectionId: this.#connectionId, reason });
    this.disconnect().catch(() => {});
  }

  #checkConnection() {
    if (!this.#pool) throw new InformixError('not connected: call connect() first');
  }

  // Inside beginTransaction()/commit() every call goes to the transaction's
  // connection, matching the proxy's connection-scoped transactions.
  #target() {
    this.#checkConnection();
    return this.#txConn ?? this.#pool;
  }

  async ping() {
    this.#checkConnection();
    return this.#target().ping();
  }

  async query(sql, params = [], options = {}) {
    return this.#target().query(sql, params, options);
  }

  async queryStream(sql, params = [], onRow, options = {}) {
    return this.#target().queryStream(sql, params, onRow, options);
  }

  async execute(sql, params = []) {
    return this.#target().execute(sql, params);
  }

  async batch(sqlStatements) {
    return this.#target().batch(sqlStatements);
  }

  async beginTransaction(isolationLevel = defaults.current().isolationLevel) {
    this.#checkConnection();
    if (this.#txConn) throw new InformixError('a transaction is already open');
    const conn = await this.#pool.acquire();
    try {
      await conn.beginTransaction(isolationLevel);
    } catch (err) {
      this.#pool.release(conn);
      throw err;
    }
    this.#txConn = conn;
  }

  async commit() {
    await this.#endTransaction('commit');
  }

  async rollback() {
    await this.#endTransaction('rollback');
  }

  async #endTransaction(which) {
    this.#checkConnection();
    const conn = this.#txConn;
    if (!conn) throw new InformixError('no transaction is open');
    this.#txConn = null;
    try {
      await conn[which]();
    } finally {
      this.#pool.release(conn);
    }
  }

  /** Callback-style transaction: commits if fn resolves, rolls back if it throws. */
  async transaction(fn, isolationLevel) {
    this.#checkConnection();
    return this.#pool.transaction(fn, isolationLevel);
  }

  // Prepared statements are remembered by SQL text and re-prepared on
  // whichever pooled connection runs them, so they survive pool churn.
  async prepareStatement(sql) {
    const target = this.#target();
    const run = async (conn) => {
      const st = await conn.prepare(sql);
      const parameterCount = st.parameterCount;
      await st.close();
      return parameterCount;
    };
    const parameterCount = this.#txConn ? await run(this.#txConn) : await target.withConnection(run);
    const statementId = randomUUID();
    this.#prepared.set(statementId, { sql, parameterCount });
    return { statementId, parameterCount };
  }

  async executePrepared(statementId, params = [], options = {}) {
    const entry = this.#prepared.get(statementId);
    if (!entry) throw new InformixError(`unknown prepared statement: ${statementId}`);
    return this.#target().query(entry.sql, params, { fetchSize: options.fetchSize });
  }

  async closePrepared(statementId) {
    this.#checkConnection();
    this.#prepared.delete(statementId);
  }

  async getMetadata(tableName = '') {
    return this.#target().getMetadata(tableName);
  }
}

module.exports = { InformixClient };
