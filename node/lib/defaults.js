'use strict';

// Every default this package uses, in one place. Values are read at call
// time, so setDefaults() affects every connection, pool and query created
// afterwards (already-open connections keep the settings they opened with).
//
// The native addon (src/addon.cpp) has fallbacks of its own, but the JS
// layer always passes every field explicitly, so this file is the source of
// truth.

const { InformixError } = require('./errors');

const BUILTIN = Object.freeze({
  // Connection
  host: 'localhost',
  port: 9088,
  server: 'informix', // INFORMIXSERVER name
  database: '',
  username: '',
  password: '',
  appName: 'informix-native',
  connectTimeoutMs: 10000, // TCP connect + login handshake; matches informix-proxy's client
  queryTimeoutMs: 0, // per network wait while a statement runs; 0 = no limit
  int64: 'number', // BIGINT/BIGSERIAL: 'number' | 'bigint' | 'string'
  decimal: 'number', // DECIMAL/MONEY: 'number' | 'string'

  // Queries
  fetchSize: 500, // rows per fetch round trip
  maxRows: 0, // 0 = no limit

  // Pool / InformixClient
  poolSize: 10,

  // Transactions (InformixClient.beginTransaction, like informix-proxy)
  isolationLevel: 'READ_COMMITTED',
});

const CHOICES = {
  int64: ['number', 'bigint', 'string'],
  decimal: ['number', 'string'],
  isolationLevel: ['READ_UNCOMMITTED', 'READ_COMMITTED', 'CURSOR_STABILITY', 'REPEATABLE_READ', 'SERIALIZABLE'],
};
const NON_NEGATIVE_INTS = ['port', 'connectTimeoutMs', 'queryTimeoutMs', 'maxRows'];
const POSITIVE_INTS = ['fetchSize', 'poolSize'];
const STRINGS = ['host', 'server', 'database', 'username', 'password', 'appName'];

let current = { ...BUILTIN };

function validate(key, value) {
  if (!(key in BUILTIN)) throw new InformixError(`unknown default: ${key}`);
  if (CHOICES[key] && !CHOICES[key].includes(value)) {
    throw new InformixError(`${key} must be one of ${CHOICES[key].join(', ')}`);
  }
  if (NON_NEGATIVE_INTS.includes(key) && !(Number.isInteger(value) && value >= 0)) {
    throw new InformixError(`${key} must be a non-negative integer`);
  }
  if (POSITIVE_INTS.includes(key) && !(Number.isInteger(value) && value >= 1)) {
    throw new InformixError(`${key} must be a positive integer`);
  }
  if (STRINGS.includes(key) && typeof value !== 'string') throw new InformixError(`${key} must be a string`);
}

/** Overrides some defaults, e.g. setDefaults({ server: 'ol_prod', decimal: 'string' }). */
function setDefaults(overrides) {
  for (const [key, value] of Object.entries(overrides ?? {})) validate(key, value);
  current = { ...current, ...overrides };
  return getDefaults();
}

/** Restores the built-in defaults. */
function resetDefaults() {
  current = { ...BUILTIN };
  return getDefaults();
}

/** A copy of the defaults currently in effect. */
function getDefaults() {
  return { ...current };
}

/** Fills a connection config's missing fields from the current defaults. */
function resolveConnectionConfig(c = {}) {
  const pick = (key) => c[key] ?? current[key];
  return {
    host: pick('host'),
    port: pick('port'),
    server: pick('server'),
    database: pick('database'),
    username: c.username ?? c.user ?? current.username,
    password: pick('password'),
    appName: pick('appName'),
    connectTimeoutMs: pick('connectTimeoutMs'),
    queryTimeoutMs: pick('queryTimeoutMs'),
    int64: pick('int64'),
    decimal: pick('decimal'),
  };
}

/** Fills query options' missing fields from the current defaults. */
function resolveQueryOptions(o = {}) {
  return { fetchSize: o.fetchSize ?? current.fetchSize, maxRows: o.maxRows ?? current.maxRows };
}

module.exports = {
  BUILTIN_DEFAULTS: BUILTIN,
  getDefaults,
  setDefaults,
  resetDefaults,
  resolveConnectionConfig,
  resolveQueryOptions,
  current: () => current,
};
