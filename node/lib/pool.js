'use strict';

const { Connection } = require('./connection');
const { InformixError } = require('./errors');
const defaults = require('./defaults');

// A fixed-maximum pool of Connections. Each query borrows one connection
// for its duration; transaction(fn) holds one for the whole callback.
class Pool {
  #config;
  #max;
  #idle = [];
  #waiters = [];
  #total = 0;
  #closed = false;

  constructor(config, { max } = {}) {
    if (!config || typeof config !== 'object') throw new InformixError('pool config is required');
    max ??= config.poolSize ?? defaults.current().poolSize;
    if (!(max >= 1)) throw new InformixError('pool max must be >= 1');
    this.#config = config;
    this.#max = max;
  }

  get size() {
    return this.#total;
  }

  get idleCount() {
    return this.#idle.length;
  }

  get closed() {
    return this.#closed;
  }

  async acquire() {
    if (this.#closed) throw new InformixError('pool is closed');
    while (this.#idle.length) {
      const conn = this.#idle.pop();
      if (conn.isConnected) return conn;
      this.#total--;
    }
    if (this.#total < this.#max) return this.#create();
    return new Promise((resolve, reject) => this.#waiters.push({ resolve, reject }));
  }

  async #create() {
    this.#total++;
    try {
      return await Connection.connect(this.#config);
    } catch (err) {
      this.#total--;
      throw err;
    }
  }

  // Returns a connection to the pool. A broken connection, or one with a
  // transaction still open (a caller forgot commit/rollback), is closed
  // instead of being handed to the next borrower.
  release(conn) {
    if (this.#closed || !conn.isConnected || conn.inTransaction) {
      this.#total--;
      conn.close().catch(() => {});
      this.#refill();
      return;
    }
    const waiter = this.#waiters.shift();
    if (waiter) waiter.resolve(conn);
    else this.#idle.push(conn);
  }

  #refill() {
    if (this.#closed || !this.#waiters.length || this.#total >= this.#max) return;
    const waiter = this.#waiters.shift();
    this.#create().then(waiter.resolve, waiter.reject);
  }

  async withConnection(fn) {
    const conn = await this.acquire();
    try {
      return await fn(conn);
    } finally {
      this.release(conn);
    }
  }

  query(sql, params, options) {
    return this.withConnection((c) => c.query(sql, params, options));
  }

  queryStream(sql, params, onRow, options) {
    return this.withConnection((c) => c.queryStream(sql, params, onRow, options));
  }

  execute(sql, params) {
    return this.withConnection((c) => c.execute(sql, params));
  }

  batch(statements) {
    return this.withConnection((c) => c.batch(statements));
  }

  getMetadata(tableName) {
    return this.withConnection((c) => c.getMetadata(tableName));
  }

  ping() {
    return this.withConnection((c) => c.ping());
  }

  transaction(fn, isolationLevel) {
    return this.withConnection((c) => c.transaction(fn, isolationLevel));
  }

  async close() {
    this.#closed = true;
    for (const w of this.#waiters.splice(0)) w.reject(new InformixError('pool is closed'));
    const idle = this.#idle.splice(0);
    this.#total -= idle.length;
    await Promise.allSettled(idle.map((c) => c.close()));
  }
}

module.exports = { Pool };
