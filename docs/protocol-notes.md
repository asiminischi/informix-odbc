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

**Status: verified** — confirmed byte-for-byte on 2026-07-29 by proxying the real
IBM JDBC driver (jdbc-4.50.10) through a logging TCP relay against the local dev
Docker Informix instance (IDS 14.10.FC7W1DE) and capturing a full session. Golden
capture: `tests/fixtures/handshake_capture_ids14.10.txt`.

One request/response pair, before any statement traffic. All integers big-endian.

**Request** — 6-byte header + a binary payload:
- Header: `u16 total_length` (payload size + 6), `u8 slType` (`1` = connection
  request), `u8 slAttribute` (`60` = "SQLI 0600" protocol), `u16 slOptions` (`0`).
- Payload, as a sequence of length-prefixed/fixed-width fields (a `u16 n` length
  prefix means "n bytes follow, including a trailing NUL" unless noted fixed-width):
  1. `u16(100)`, `u16(101)` — marker pair opening the binary connect segment.
  2. `u32(61)` — protocol level marker (recurs once more below).
  3. float-format tag, length-prefixed: `"IEEEM"`.
  4. `u16(108)` tag, then a **fixed 12-byte** app-type field: `"sqlexec"` + 5 NUL
     bytes (no length prefix — width is implied by the tag).
  5. client version string, length-prefixed (driver used `"9.280"` — arbitrary,
     informational for the server).
  6. client serial number string, length-prefixed (driver used `"RDS#R000000"` —
     informational).
  7. application-id string, length-prefixed (driver used `"sqli"`).
  8. `u32` protocol version (**316** — confirmed load-bearing: the server echoes it
     back as `Cap_1`; use the same value until proven otherwise), then two reserved
     `u32(0)` fields, then `u16(1)` (protocol sub-option).
  9. username, length-prefixed; password, length-prefixed (`u16(0)` with no bytes
     if absent — not applicable for username/password auth).
  10. `u32(61)` protocol level marker again, then a **fixed 8-byte** wire-protocol
      tag `"ol"` + 6 NUL bytes (`onsoctcp`), then `u32(1)` (network sub-version),
      then a **fixed 8-byte** network-type tag `"tlitcp\0\0"`.
  11. `u16(104)` tag ("init request"), `u16(11)` value ("xconnect").
  12. `u32` statement-options bitmask (`3` baseline; OR in `0x2000000` for a
      sqlhosts connection group, `0x4000000` for trusted-context — neither
      applies to the MVP).
  13. server name, length-prefixed.
  14. **database name is NOT sent here** — confirmed empty (`u16(0)`) even when
      the JDBC URL specified a database. Database selection happens as a
      separate statement-protocol message right after the handshake completes
      (see below).
  15. four reserved `u16(0)` fields.
  16. `u16(106)` tag, `u16 count`, then `count` × (name length-prefixed, value
      length-prefixed) environment-variable pairs. Minimum observed set the real
      driver sends: `DBPATH`, `CLIENT_LOCALE`, `CLNT_PAM_CAPABLE`, `DBDATE`,
      `IFX_UPDDESC`, `NODEFDAC`. Untested whether the server requires any of
      these — start by sending the same set.
  17. `u16(107)` tag ("process info"): `u32(0)` reserved, `u32 pid`, `u32 tid`,
      hostname length-prefixed, `u16(0)` reserved gap, cwd length-prefixed.
  18. `u16(116)` tag ("app name"): `u16` payload length, `u32(0)` × 2 reserved,
      then an application-name string, length-prefixed. Content is purely
      informational (shows up in server-side connection diagnostics).
  19. `u16(127)` — end-of-blob marker. Nothing follows it.

**Response**: `u16 total_length`, then a body starting with `u8 SLType` — `2` =
accepted, `3` = accepted-with-error(?), `13` = redirect (HA, out of scope). On
accept: `skip(3)` (attribute+options), `u16(100)`, `u16(101)` marker pair (must
match), then the same "ASCII-binary" decode as the request's field 2 onward:
`skip(4)`, a length-prefixed float-tag (server sent `"IEEEI"` — differs from the
client's `"IEEEM"`, doesn't matter, just skip it), `u16(108)` tag check, a fixed
12-byte app-type field (server sent `"srvinfx"` + padding), then length-prefixed
**server version string**, length-prefixed **serial number** (this dev image
just returns the literal string `"serial"`), length-prefixed **application id**
(returned `"informix"`), then three raw `u32` fields **Cap_1/Cap_2/Cap_3**
(Cap_1 echoed the requested `316`; Cap_2/Cap_3 were `0` here), then a chain of
skip-by-length-prefix reserved blocks, a fixed 24-byte skip, and a final `u16`
tag: `127` = success (confirmed), `102`/`103` reserved for error/redirect detail
(not yet exercised — implement per the same length-prefixed-then-skip pattern
and revisit if/when a real error response is captured).

**Auth for MVP**: plain username/password sent in this handshake, as above. No
SSL, no PAM, in the first pass — see Phase 5.

**Database selection** (newly discovered from the capture, not something we'd
guessed from the decompile alone): immediately after a successful handshake, the
driver sends one statement-protocol message — opcode `0x24` (36, not in the
`SQ_*` table used elsewhere; tentatively "select database") carrying the
database name as a length-prefixed string, terminated by `SQ_EOT`(12). Server
acks with a `SQ_DONE`(15)-shaped reply. This needs to be sent explicitly by our
driver too if a database was specified.

**Disconnect** (confirmed, with a wrinkle): send `u16(56)` (`SQ_EXIT`), then
read back one `u16` reply, tolerating a leading `SQ_XACTSTAT`(99) chain the
same way the decompiled `disconnectOrderly` does. **Important**: the server
does not always send a reply — in a from-scratch session that only did the
handshake + database-select (no other statement traffic), the server just
closes the TCP connection outright instead of replying. In a session that had
done real statement traffic first (the JDBC golden capture, which ran two
`SET ISOLATION` statements and a full query cycle before disconnecting), the
server replied with `56` as expected. The decompiled driver handles this
correctly: `disconnectOrderly`'s reply-read is wrapped in a bare
`catch (IOException) {}` that swallows exactly this case. **Our implementation
must treat "connection closed while waiting for the EXIT reply" as a normal,
successful disconnect, not an error** — confirmed necessary by hitting this
live (see `tests/fixtures/probe_db_select_and_exit.txt`).

**Database selection** (confirmed): immediately after a successful handshake,
send opcode `36` (`0x24`) — `u16(36)`, `u16(len)` (the database name's raw
byte length, **no +1**), the name's raw bytes (**no NUL**), `u8(0)` (an
explicit NUL byte, separate from the length count), `u16(0)` (unclear
purpose — sent as 0, untested otherwise), `u16(12)` (`SQ_EOT`). Confirmed
byte-for-byte identical between our implementation and the JDBC golden
capture. The server's ack starts with `u16(15)` (`SQ_DONE`) and is
variable-length (28 bytes in the JDBC capture, 18 bytes in ours) — its
internal shape is genuine Phase 2 (statement protocol) territory we don't
parse yet; for now we just scan forward reading `u16`s until one equals
`SQ_EOT`(12), which reliably finds the end without needing to understand the
fields in between.

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
