'use strict';

const { Connection, PreparedStatement } = require('./connection');
const { Pool } = require('./pool');
const { InformixClient } = require('./client');
const { InformixError } = require('./errors');
const { getDefaults, setDefaults, resetDefaults, BUILTIN_DEFAULTS } = require('./defaults');

function connect(config) {
  return Connection.connect(config);
}

function createPool(config, options) {
  return new Pool(config, options);
}

module.exports = {
  connect,
  createPool,
  Connection,
  PreparedStatement,
  Pool,
  InformixClient,
  InformixError,
  getDefaults,
  setDefaults,
  resetDefaults,
  BUILTIN_DEFAULTS,
};
