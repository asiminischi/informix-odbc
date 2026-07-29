# SQLI wire protocol notes

Working notes on Informix's proprietary client/server protocol ("SQLI", running under
the "ASF" transport framing), reconstructed by black-box reasoning about observed
driver behavior plus general knowledge of TCP-based DB wire protocols, and validated
empirically against a live Informix server as each phase lands. Everything here is
written in our own words and describes *observed wire behavior*, not any vendor's
source code. Treat every byte layout below as a hypothesis until the corresponding
"verified" note is filled in from a live round trip against the Docker test server.

Byte order: all multi-byte integers are big-endian ("network order") unless noted.

## Transport

Plain TCP. No length-delimited framing at the socket level beyond what each layer
below defines explicitly. One connection = one session; there's no multiplexing.

## Connect handshake

One request/response pair, before any statement traffic:

- Client sends a header: `u16 total_length`, `u8 type`, `u8 attribute`, `u16 options`,
  followed by a binary payload containing (in order): a float-format tag, an
  application-type tag, a client version string, a serial number string, an
  application-id tag, a capabilities bitmask (three `u32` reserved/capability words),
  a protocol-options `u16`, the username (u16-length-prefixed, NUL-terminated), the
  password (u16-length-prefixed, NUL-terminated, empty length-prefix of 0 if absent),
  a wire-protocol name tag (`"ol"`, padded), a network-type tag (`"tlitcp"`, padded),
  a statement-options `u32` bitmask, the server name (length-prefixed), the database
  name (length-prefixed, may be absent), then an environment-variable block (count +
  name/value length-prefixed pairs) and process-info block (pid, tid, hostname, cwd).
- Server replies with either: an accept payload carrying its version string, serial
  number, application id, and its own three-word capability bitmask; or an error
  payload (service error code, OS error code, message text); or a redirect payload
  (for HA/connection-manager setups, out of scope for MVP).
- **Auth for MVP**: plain username/password sent in this handshake. No SSL, no PAM,
  in the first pass — see docs/protocol-notes.md updates in Phase 5.

Status: **unverified against live server** — implement in Phase 1, verify by
successful connect+clean-disconnect against the Docker Informix instance, then
update this section with confirmed byte offsets.

## Statement protocol

After the handshake, both sides exchange a stream of *tagged records*: a `u16`
opcode tag followed by an opcode-specific payload. The client buffers one or more
tagged records and flushes them together, terminated by an `EOT` (end-of-transmission)
tag. The server replies with its own stream of tagged records, terminated by `EOT`
(or session-exit).

Known opcodes (values to be confirmed empirically, listed here as the working set
the implementation targets):

| Meaning | Direction |
|---|---|
| PREPARE (SQL text + placeholder count) | C→S |
| DESCRIBE (result column metadata request) | C→S |
| BIND (parameter values for a prepared statement) | C→S |
| EXECUTE | C→S |
| FETCH (buffered, driven by a client-supplied byte budget) | C→S |
| CLOSE (cursor/statement) | C→S |
| RELEASE (free prepared statement) | C→S |
| BEGIN / COMMIT / ROLLBACK (transaction control — dedicated opcodes, not SQL text) | C→S |
| EOT (flush marker) | C→S, S→C |
| DESCRIBE reply (column count + per-column type/length/name) | S→C |
| TUPLE (one batch of row bytes) | S→C |
| DONE (end of result set / rows-affected count / warnings bitmask) | S→C |
| ERROR (sqlcode, secondary/ISAM code, message text) | S→C |

Fetch is **batched**: a single FETCH request can return many TUPLE records up to a
client-specified byte budget; the client keeps issuing FETCH until the server
signals no more rows are available (observed as a CLOSE-equivalent condition rather
than a per-row flag).

Errors surface as a dedicated ERROR record carrying a primary SQL error code, a
secondary code, and a message string; these get mapped to ODBC SQLSTATE values in
`sqli/error.*`. Non-fatal warnings ride along in the DONE record's bitmask rather
than a separate opcode.

Status: **unverified against live server** — implement in Phase 2, verify with a
trivial `SELECT` round trip, then fill in confirmed opcode values/offsets.

## Scalar type wire encodings (MVP set)

Working hypotheses to implement and verify in Phase 3:

| Type | Wire form (hypothesis) | NULL sentinel (hypothesis) |
|---|---|---|
| SMALLINT | 2 bytes, fixed | reserved bit pattern (`INT16_MIN`) |
| INTEGER | 4 bytes, fixed | reserved bit pattern (`INT32_MIN`) |
| BIGINT | 8 bytes, fixed | reserved bit pattern (`INT64_MIN`) |
| CHAR(n) | fixed width, space-padded | leading `0x00` byte |
| VARCHAR(n) | 1-byte length prefix + data | **unconfirmed — verify empirically** |
| DECIMAL(p,s) | 1 exponent/sign byte (base-100 floating point, bias-64) + 1-8 mantissa bytes, each a base-100 digit; negative values 100's-complemented with sign bit cleared | leading two zero bytes |
| DATE | 4 bytes, signed day count from a fixed epoch | reserved bit pattern (`INT32_MIN`) |
| DATETIME | same packed base-100 machinery as DECIMAL, fields selected by a start/end qualifier byte pair | leading two zero bytes |

Column metadata (from a DESCRIBE reply) packs the base type code and nullability/
distinct/etc. flags into a single 16-bit word (low byte = base type, high bits =
flags); precision/scale for DECIMAL/DATETIME travel in a separate length/qualifier
field per column.

Status: **unverified** — each type gets a round-trip unit test plus a live-query
test against a populated test table (including NULLs) before being considered done.
This table gets updated in place with confirmed layouts as Phase 3 lands.

## Open questions / risks

- VARCHAR NULL convention on the wire (length-prefix sentinel vs. something else).
- Exact DESCRIBE reply field ordering/offsets in practice.
- Whether the Docker test server's protocol version negotiates down to an older
  format than a fully current on-prem server would use (matters for anyone using
  this against production instances later).
