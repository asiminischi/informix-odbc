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

**Important, non-obvious**: the server version string this Docker image returns
is literally `"Informix Dynamic Server Version 9.59.FC7W1"` — **this is not a
parse bug**. Confirmed byte-for-byte against the golden capture (the real JDBC
driver receives the exact same string from the exact same server). `onstat -V`
inside the container reports the real product version, `14.10.FC7W1DE` — the
two are simply different fields with different purposes (a wire/compatibility
version stamp vs. the actual product build). Don't "fix" this if it recurs;
verify against a capture first, as was done here, before assuming a parsing
offset error.

**SQ_PROTOCOLS negotiation** (confirmed): immediately after the handshake, send
opcode `126` (`SQ_PROTOCOLS`) — `u16(126)`, `u16(8)` (payload length), then this
exact 8-byte client capability offer (matches the real driver byte-for-byte):
`FF FC 7F FC 3C 8C AA 97`. **Critically, this is followed by a separate bare
`u16(12)` (`SQ_EOT`) write** — omitting it leaves the server waiting forever for
more input with zero reply (confirmed live: this is what an earlier, EOT-less
version of this exchange did). The server replies with `u16(126)` echoed back,
`u16 len` (observed `9`, odd), `len` bytes of capability response (padded with
one `0x00` byte since `len` is odd — the same even-alignment padding used
throughout this protocol, see below), then its own `u16(12)` EOT in the same
message. Bit 50 overall (byte index 6, bit `0x20`) is the one this driver reads:
whether the server uses **4-byte** (vs 2-byte) offset/length fields in
DESCRIBE/TUPLE/etc — see "Narrow vs. wide framing" below. This bit was `1`
(wide) against the Docker 14.10 engine.

**SQ_INFO / INFO_ENV** (confirmed): immediately after SQ_PROTOCOLS, send opcode
`81` (`SQ_INFO`), subtype `u16(6)` (`INFO_ENV`), `u16 totalLen`, `u16 longestNameLen`,
`u16 longestValueLen`, then `ifxChar2(name)`/`ifxChar2(value)` pairs (see the
statement-protocol "char" field shape below), terminated by `u16(0)`/`u16(0)`,
then a trailing `u16(12)` EOT (same omit-and-hang trap as SQ_PROTOCOLS). **The
server rejects an empty env set outright** — `SQ_ERROR` sqlcode `-208` — so at
least one entry is required. This driver sends the same `DBTEMP=/tmp`,
`SUBQCACHESZ=10` pair the real driver sends when no connInfo properties
override them (arbitrary stand-in values; not verified to matter beyond
satisfying the non-empty requirement). `totalLen` follows the formula
`4 + Σ(paddedNameLen + paddedValueLen + 4) + 2` — for these two entries, `38`.
Server replies with a bare `u16(12)` EOT on success.

**Narrow vs. wide framing**: multiple otherwise-fixed-shape fields toggle
between a 2-byte and 4-byte wire form depending on the negotiated SQ_PROTOCOLS
capability (`SqliConnection::has4ByteOffsets()` in this codebase — see that
accessor's doc comment for the important caveat that it's really a *proxy* for
several capability bits that happen to co-occur, not a single verified bit).
Confirmed affected fields, all toggling together on this server:
- DESCRIBE's `stringTableSize` and per-field `fieldIndex`.
- DESCRIBE's per-field extended metadata block (`extId`/`owner`/`extName`/
  `reference`/`alignment`/`sourceType`/`encLen`) — present at all only when wide;
  narrow mode has just a 2-byte `encLen` and nothing else after `colType`.
- TUPLE's `size` field.
- SQ_NFETCH's tuple-buffer-size field, **plus a trailing `u16(0)` "fetch array
  size" field that only exists in wide mode** (narrow mode omits it entirely).
- SQ_COMMAND's and SQ_PREPARE's "# of values"/"numqmarks" reserved field. The
  decompiled JDBC source shows `writeSmallInt(0)` (2 bytes) unconditionally
  here with no visible conditional — **the golden capture proves this is
  wrong** for a wide connection, where it's genuinely 4 bytes. Trust the
  capture over the decompiled source when they conflict (this was a real,
  confirmed discrepancy, not a misreading on our part) — CFR-decompiled
  bytecode can misresolve overloaded integer-widening calls.

Getting any of these wrong doesn't error cleanly — it silently desyncs the byte
stream, and depending on which field is wrong, the effect can range from
"reads garbage values" to "server or client blocks forever waiting for bytes
that were already consumed/never coming" (`onstat -g ses` shows the stuck side
in `cond wait netnorm`). When a message hangs after a previously-working step,
suspect a field-width mismatch first and re-derive it from a fresh capture
rather than guessing.

`columnStart`'s width (u16 vs u32) in DESCRIBE is separately gated by isUSVER,
which — per the decompiled source — is unconditionally true whenever the
server echoes a nonzero protocol level (our case, always). It has always been
true in practice, so it's folded into the same `has4ByteOffsets()` proxy above
rather than tracked as an independent flag.

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

**Status: verified** — a full prepare→describe→execute→fetch(single- and
multi-row)→close→release cycle round-trips correctly against the live Docker
server (`sqli/statement.cpp`, `sqli/connection.cpp`), confirmed against
`select tabid, tabname from systables where tabid = 1` (1 row) and `... where
tabid <= 5` (5 rows, exercising multi-tuple fetch and cursor exhaustion).

After the handshake (+ SQ_PROTOCOLS + SQ_INFO/ENV + database-select, see
above), both sides exchange a stream of *tagged records*: a `u16` opcode tag
followed by an opcode-specific payload. The client buffers one or more tagged
records and flushes them together, terminated by an `EOT` (`u16(12)`) tag. The
server replies with its own stream of tagged records, terminated by `EOT`.

Confirmed opcodes (see `sqli/protocol.h`):

| Meaning | Value | Direction |
|---|---|---|
| SQ_COMMAND (resultless SQL text, e.g. `SET ISOLATION`) | 1 | C→S |
| SQ_PREPARE (SQL text + placeholder count) | 2 | C→S |
| SQ_CURSOR_NAME | 3 | C→S |
| SQ_ID (selects which prepared statement subsequent messages target) | 4 | C→S |
| SQ_BIND | 5 | C→S (unused — no bind params in Phase 2 scope) |
| SQ_OPEN (opens a cursor for a prepared SELECT) | 6 | C→S |
| SQ_EXECUTE | 7 | C→S |
| SQ_DESCRIBE | 8 | S→C |
| SQ_FETCH (a.k.a. NFETCH; buffered, driven by a byte budget) | 9 | C→S |
| SQ_CLOSE | 10 | C→S |
| SQ_RELEASE | 11 | C→S |
| SQ_EOT | 12 | C→S, S→C |
| SQ_ERROR | 13 | S→C |
| SQ_TUPLE (one row's bytes) | 14 | S→C |
| SQ_DONE | 15 | S→C |
| SQ_NON_CURSOR_DESCRIBE (requests a DESCRIBE even for non-SELECT) | 22 | C→S |
| SQ_RETURN_TYPE (tells the server how to render each fetched column) | 100 | C→S |
| SQ_WANT_DONE | 49 | C→S |
| SQ_COST | 55 | S→C |
| SQ_EXIT | 56 | C→S |
| SQ_PROTOCOLS | 126 | C→S, S→C |

**Statement-protocol "char" field** (`ByteWriter::ifxChar2`/`ByteReader::ifxChar2`,
distinct from the handshake's NUL-terminated length-prefixed strings): `u16`
length **not including** any terminator, that many raw bytes, then a single
`0x00` pad byte **if and only if** the length is odd (2-byte wire alignment).
This same even-alignment rule applies to **TUPLE row data too** — confirmed
live: a 1-row fetch with a 14-byte tuple needed no pad, but a 15-byte tuple in
a multi-row fetch did; omitting the pad byte desyncs every subsequent read in
the batch. Always check `size & 1` after reading any length-prefixed blob.

**SQ_COMMAND** (executed via `SqliConnection::executeCommand`, confirmed for
`SET ISOLATION ...`): `u16(1)`, reserved field (u16 or u32 — see "Narrow vs.
wide framing" above), `ifxChar2(sql)`, `u16(22)` (SQ_NON_CURSOR_DESCRIBE),
`u16(7)` (SQ_EXECUTE), `u16(11)` (SQ_RELEASE), `u16(12)` (EOT). Server replies
with a DESCRIBE (nfields=0 for a non-SELECT), then DONE, then EOT.

**SQ_PREPARE** (`SqliStatement::prepare`): `u16(2)`, numqmarks (u16 or u32 —
same toggle), `ifxChar2(sql)`, `u16(22)` (SQ_NON_CURSOR_DESCRIBE), `u16(49)`
(SQ_WANT_DONE), `u16(12)` (EOT). Server replies with a DESCRIBE carrying real
column metadata, then EOT (statementId comes back inside the DESCRIBE record).

**SQ_DESCRIBE reply** (S→C, read generically in both `executeCommand` and
`SqliStatement::processResponseBatch`): `statementType`(u16), `statementId`(u16),
`estCost`(u32, unused), `tupleSize`(u16, unused), `nfields`(u16), then
`stringTableSize` (u16/u32 toggle), then `nfields` × per-field records (layout
depends on the same toggle — see above), then `stringTableSize` bytes of a
NUL-separated column-name table (one name per field, in order). `colType`'s raw
u16 value packs the base Informix type code in the low byte and a `NOT NULL`
flag in bit `0x100` (`columns_[i].ifxType = colType & 0xFF`,
`notNull = colType & 0x100`).

**SQ_ID / SQ_CURSOR_NAME / SQ_OPEN** (`SqliStatement::execute`, opens a
cursor): `u16(4)`, `statementId`(**always u16** — confirmed, not subject to the
narrow/wide toggle), `u16(3)`, `ifxChar2(cursorName)`, `u16(6)`, `u16(12)`
(EOT). Cursor names in this codebase are `"_odbc" + 13-digit zero-padded
counter`; the real driver uses `"_ifxc" + 11-digit counter` — either scheme
seems to be accepted, the server doesn't appear to validate the format. Server
replies with a bare EOT on success (no DESCRIBE/DONE for a plain open).

**SQ_RETURN_TYPE + SQ_FETCH** (`SqliStatement::fetchNextBatch`): `u16(4)`,
`statementId`(u16), then — **only on the first fetch for a given cursor** —
`u16(100)` (SQ_RETURN_TYPE), `u16(1)` (direction: fetch), `u16(nfields)`, then
per-column `ifxType`(u16) + `encodedLength`(**always u32**, not gated by the
toggle) pairs. Then unconditionally: `u16(9)` (SQ_FETCH), tuple-buffer-size
(u16 or u32 toggle; `4096` observed), **a trailing `u16(0)` "fetch array size"
field present only in wide mode**, `u16(12)` (EOT). Server replies with zero
or more SQ_TUPLE records (one per fetched row, terminated within the batch by
a DONE-then-EOT once the cursor is exhausted for this fetch call, or by an EOT
alone if more rows remain for a subsequent fetch) — this driver treats "no
TUPLE seen in a fetch batch" as cursor exhaustion (`fetchNextBatch` sets
`exhausted_` when `processResponseBatch` reports no tuple).

**SQ_TUPLE** (S→C): `warning`(u16), `size` (u16/u32 toggle), then `size` raw
bytes of row data (all columns concatenated in column order, no per-column
framing beyond what each scalar type's own encoding implies — see below),
padded per the even-alignment rule above.

**SQ_CLOSE / SQ_RELEASE** (`SqliStatement::close`/`release`): `u16(4)`,
`statementId`(u16), `u16(10)`/`u16(11)`, `u16(12)` (EOT). Server replies with
DESCRIBE/DONE/EOT (close) matching the generic batch reader.

Errors surface as a dedicated `SQ_ERROR`(13) record: `sqlcode`(u16, signed),
then (unless `sqlcode == -368`, a special case with no message) `isamcode`(u16),
`offset`(u32), message (`ifxChar2`). A `sqlcode == -619` is a special "resync"
marker (per the decompiled `receiveError`) with a slightly different trailing
shape — this driver drains it and surfaces it as a distinct error rather than
trying to auto-resync.

## Scalar type wire encodings (MVP set)

Two of these got incidental live confirmation in Phase 2 (`sqli/statement.cpp`'s
`splitRow`, exercised against real `systables` rows); the rest remain
hypotheses for Phase 3 to verify properly (including NULL handling, which
Phase 2's queries never exercised — `systables.tabid`/`tabname` are never
NULL):

| Type | Wire form | NULL sentinel | Status |
|---|---|---|---|
| SMALLINT | 2 bytes, fixed | reserved bit pattern (`INT16_MIN`) | hypothesis |
| INTEGER / **SERIAL** | 4 bytes, fixed, big-endian | reserved bit pattern (`INT32_MIN`) | **confirmed for SERIAL** (`systables.tabid`, e.g. `00 00 00 01` = 1) |
| BIGINT | 8 bytes, fixed | reserved bit pattern (`INT64_MIN`) | hypothesis |
| CHAR(n) | fixed width, space-padded | leading `0x00` byte | hypothesis |
| **VARCHAR(n)** | **1-byte length prefix + that many raw bytes (no NUL)** | **unconfirmed** | **confirmed shape** (`systables.tabname`, e.g. `09 73 79 73 74 61 62 6c 65 73` = length 9 + `"systables"`); NULL convention still unverified |
| DECIMAL(p,s) | 1 exponent/sign byte (base-100 floating point, bias-64) + 1-8 mantissa bytes, each a base-100 digit; negative values 100's-complemented with sign bit cleared | leading two zero bytes | hypothesis |
| DATE | 4 bytes, signed day count from a fixed epoch | reserved bit pattern (`INT32_MIN`) | hypothesis |
| DATETIME | same packed base-100 machinery as DECIMAL, fields selected by a start/end qualifier byte pair | leading two zero bytes | hypothesis |

Column metadata (from a DESCRIBE reply) packs the base type code and nullability/
distinct/etc. flags into a single 16-bit word (low byte = base type, high bits =
flags — **confirmed**: bit `0x100` = NOT NULL); precision/scale for DECIMAL/
DATETIME travel in a separate length/qualifier field per column (unverified).

Status: each remaining type gets a round-trip unit test plus a live-query test
against a populated test table (including NULLs) before being considered done.
This table gets updated in place with confirmed layouts as Phase 3 lands.

## Open questions / risks

- VARCHAR NULL convention on the wire (length-prefix sentinel vs. something else) —
  not yet exercised by any Phase 2 query.
- Whether `has4ByteOffsets()`'s constituent capability bits (see "Narrow vs. wide
  framing" above) ever diverge on a real server — only tested against one engine
  (Docker `ibmcom/informix-developer-database`, IDS 14.10.FC7W1DE) where they all
  happen to co-occur as true. Revisit if a server is found where DESCRIBE and
  TUPLE/FETCH negotiate differently.
