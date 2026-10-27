'use strict';

// Error raised by every operation in this package.
//
// kind:
//   'server'    - the server rejected the request; the connection is fine.
//   'transport' - network failure or timeout; the connection is now closed.
//   'protocol'  - unexpected bytes on the wire; the connection is now closed.
//   'usage'     - a client-side mistake (bad argument, closed connection...).
// sqlcode: the Informix SQL error code for kind 'server' (e.g. -206), else null.
class InformixError extends Error {
  constructor(message, kind = 'usage', sqlcode = null) {
    super(message);
    this.name = 'InformixError';
    this.kind = kind;
    this.sqlcode = sqlcode;
  }

  get isFatal() {
    return this.kind === 'transport' || this.kind === 'protocol';
  }
}

const SQLCODE_RE = /^sqlcode (-?\d+): ?([\s\S]*)$/;

// The server usually sends only the offending object's name (or nothing),
// not a sentence, so common codes get a description prepended.
const DESCRIPTIONS = {
  '-201': 'syntax error',
  '-206': 'table not found',
  '-217': 'column not found',
  '-239': 'duplicate value in a unique index',
  '-268': 'unique constraint violated',
  '-271': 'could not insert row',
  '-310': 'table already exists',
  '-329': 'database not found or no permission',
  '-349': 'no database selected',
  '-387': 'no connect permission',
  '-391': 'cannot insert NULL into a NOT NULL column',
  '-691': 'missing key in referenced table (foreign key)',
  '-692': 'key is still referenced (foreign key)',
  '-761': 'INFORMIXSERVER name does not match the server',
  '-951': 'incorrect user name or password',
  '-9814': 'smart large object space (sbspace) not available',
};

function wrapNativeError(err) {
  if (err instanceof InformixError) return err;
  let message = err && err.message ? err.message : String(err);
  const kind = (err && err.kind) || 'usage';
  const m = SQLCODE_RE.exec(message);
  const sqlcode = m ? Number(m[1]) : null;
  if (m) {
    const detail = m[2].trim();
    const desc = DESCRIPTIONS[m[1]];
    message = `sqlcode ${m[1]}: ${[desc, detail].filter(Boolean).join(': ') || 'server error'}`;
  }
  const wrapped = new InformixError(message, kind, sqlcode);
  if (err && err.stack) wrapped.stack = `InformixError: ${message}\n${err.stack.split('\n').slice(1).join('\n')}`;
  return wrapped;
}

module.exports = { InformixError, wrapNativeError };
