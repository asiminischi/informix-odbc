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
least one entry is required. This driver sends `DBTEMP=/tmp`, `SUBQCACHESZ=10`
(arbitrary stand-in values matching what the real driver sends when no
connInfo properties override them) plus **`DBDATE=Y4MD-`** (added in Phase 3
— see the "Database DML and DBDATE" note below for why). `totalLen` doesn't
appear to be strictly validated byte-for-byte against the actual payload —
this driver sends `54` for these three entries without having derived an
exact formula for it (an earlier, hand-derived guess didn't match the
working `38` used for the original two-entry set either); treat this field
as "large enough," not as a checksum, until proven otherwise. Server replies
with a bare `u16(12)` EOT on success.

**Database DML and DBDATE** (confirmed live, Phase 3): a session that
doesn't send a `DBDATE` env var still parses date string literals (e.g.
`DATE('2026-07-29')`) successfully, but under a locale that turned out to be
ISO year-month-day order — *not* the MM/DD/YYYY order the Docker image's
`dbaccess` shell environment defaults to. This was surprising and cost real
debugging time: the same literal that works fine via `dbaccess` (which has
its own locale env vars set at the shell level) failed with sqlcode `-1218`
over this driver's own connection, and switching the literal to ISO order
made it work *before* `DBDATE` was even being sent — meaning the two
sessions had two different, independently-defaulted locales. Rather than
depend on whichever default the server happens to pick, this driver now
sends `DBDATE=Y4MD-` explicitly (ISO year-month-day, dash separator) so date
parsing is deterministic and documented rather than incidental. Any SQL this
driver constructs internally (and, later, the ODBC layer's date literal
translation) should assume this format.

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
send opcode `36` (`0x24`) — `u16(36)`, then the database name as an
`ifxChar2` field (**not** a raw length + unconditional trailing `u8(0)` — an
earlier version of this driver wrote `u16(len)` + raw bytes + an
unconditional `u8(0)`, which happens to be byte-identical to `ifxChar2` for
any *odd*-length name but leaves a stray extra byte for an *even*-length one.
This went unnoticed through all of Phase 2 because the only database name
ever tested was `sysmaster`, 9 characters. It surfaced as a hang connecting
to `ifxodbc_test`, 12 characters, in Phase 3 — same failure shape as every
other pad-byte bug here: something that happened to work for the specific
test input used so far.), then `u16(0)` (unclear purpose — sent as 0,
untested otherwise), `u16(12)` (`SQ_EOT`). The server's ack starts with
`u16(15)` (`SQ_DONE`) and is variable-length (28 bytes in the JDBC capture,
18 bytes in ours) — its internal shape is genuine Phase 2 (statement
protocol) territory we don't parse yet; for now we just scan forward reading
`u16`s until one equals `SQ_EOT`(12), which reliably finds the end without
needing to understand the fields in between.

## Statement protocol

**Status: verified** — a full prepare→describe→execute→fetch(single- and
multi-row)→close→release cycle round-trips correctly against the live Docker
server (`sqli/statement.cpp`, `sqli/connection.cpp`), confirmed against
`select tabid, tabname from systables where tabid = 1` (1 row) and `... where
tabid <= 5` (5 rows, exercising multi-tuple fetch and cursor exhaustion).
DDL/DML (`CREATE TABLE`, `INSERT`) via `executeCommand` is also confirmed
live (`tests/integration/test_type_probe.cpp` recreates its whole fixture
table this way on every run).

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
| SQ_BIND | 5 | C→S |
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
| SQ_INSERTDONE (generated-key report after INSERT) | 94 | S→C |
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
the batch. It also applies to **DESCRIBE's `stringTableSize` bytes** —
confirmed live: a 9-column query's 94-byte (even) string table masked this
for all of Phase 2, until a 3-column query with a 25-byte (odd) string table
hung on the very next read. Always check `size & 1` after reading any
length-prefixed blob in this protocol — this has now bitten every
variable-length field encountered so far (SQ_COMMAND/SQ_PREPARE text,
database name, string table, tuple row data), always masked by test data
that happened to avoid the odd case until a differently-shaped query came
along.

**SQ_COMMAND** (executed via `SqliConnection::executeCommand`, confirmed for
`SET ISOLATION ...`): `u16(1)`, reserved field (u16 or u32 — see "Narrow vs.
wide framing" above), `ifxChar2(sql)`, `u16(22)` (SQ_NON_CURSOR_DESCRIBE),
`u16(7)` (SQ_EXECUTE), `u16(11)` (SQ_RELEASE), `u16(12)` (EOT). Server replies
with a DESCRIBE (nfields=0 for a non-SELECT), then DONE, then EOT.

**SQ_PREPARE** (`SqliStatement::prepare`): `u16(2)`, numqmarks, `ifxChar2(sql)`,
`u16(22)` (SQ_NON_CURSOR_DESCRIBE), `u16(49)` (SQ_WANT_DONE), `u16(12)` (EOT).
Server replies with a DESCRIBE carrying metadata, then EOT (statementId comes
back inside the DESCRIBE record). **numqmarks's exact layout, corrected in
Phase 5's first bind-parameter slice**: in wide mode this is **two separate
`u16`s — the real "?" count, then an unused/reserved `u16`** — not one `u32`
as every earlier phase assumed. Every capture before Phase 5 only ever carried
numqmarks=0, where "one big-endian u32 zero" (`00 00 00 00`) and "two u16
zeros" are byte-identical, so the two layouts were indistinguishable until a
live nonzero count actually exercised the field. Confirmed live: sending
`u32(1)` (`00 00 00 01`) makes the server read count=0/reserved=1 and then
block forever waiting for the rest of a message it considers incomplete (no
error, no EOT — `ss`/`onstat -g ses` on the container showed both sides idle
with empty send/recv queues, i.e. a true framing-desync deadlock, not a
crash). Narrow mode is a single `u16`, unaffected. Because a DESCRIBE reply
for a statement with "?" markers reports those markers back as if they were
`nfields` (see the next section), this bug was invisible for four phases:
nothing before Phase 5 ever prepared a statement with a nonzero qmark count.

**SQ_DESCRIBE reply** (S→C, read generically in both `executeCommand` and
`SqliStatement::processResponseBatch`): `statementType`(u16), `statementId`(u16),
`estCost`(u32, unused), `tupleSize`(u16, unused), `nfields`(u16), then
`stringTableSize` (u16/u32 toggle), then `nfields` × per-field records (layout
depends on the same toggle — see above), then `stringTableSize` bytes of a
NUL-separated column-name table (one name per field, in order). `colType`'s raw
u16 value packs the base Informix type code in the low byte and a `NOT NULL`
flag in bit `0x100` (`columns_[i].ifxType = colType & 0xFF`,
`notNull = colType & 0x100`).

**Important, confirmed live in Phase 5's first bind-parameter slice**:
`nfields`/the per-field records describe **input parameter placeholders**,
not output columns, whenever the prepared statement has "?" markers and
isn't a SELECT (e.g. `insert into t (col) values (?)` reports `nfields=1`,
matching its one placeholder, even though an INSERT has zero output
columns). `statementType` is the only reliable discriminator between the
two meanings — confirmed against the decompiled JDBC driver's
`IfxStatementTypes` constants (`SQ_SELECT=2`, `SQ_INSERT=6`,
`SQ_CRETAB=14`, `SQ_DRPTAB=15`, `SQ_UPDATE=4`, all observed live matching
these values) and its own `statementType == 2` branch in
`executeStatementQuery`/`executeBatch`. `SqliStatement::columns()` hides
this internal reuse from every ODBC-facing caller by returning empty
unless `hasResultSet()` (`statementType == 2`) is true — before this was
understood, `columns_.empty()` was (wrongly, if only by coincidence -
nothing before Phase 5 ever prepared a non-SELECT statement with
parameters) used as the "does this statement have a result set" check.

**SQ_ID / SQ_CURSOR_NAME / SQ_OPEN** (`SqliStatement::execute`, opens a
cursor — **only for statements with output columns**; see below): `u16(4)`,
`statementId`(**always u16** — confirmed, not subject to the narrow/wide
toggle), `u16(3)`, `ifxChar2(cursorName)`, `u16(6)`, `u16(12)` (EOT). Cursor
names in this codebase are `"_odbc" + 13-digit zero-padded counter`; the real
driver uses `"_ifxc" + 11-digit counter` — either scheme seems to be
accepted, the server doesn't appear to validate the format. Server replies
with a bare EOT on success (no DESCRIBE/DONE for a plain open).

**SQ_ID / SQ_EXECUTE, no cursor** (`SqliStatement::execute`'s other branch —
confirmed live in Phase 4 slice 3, corrected in Phase 5): for a prepared
statement with no result set (`!hasResultSet()`, i.e. `statementType != 2` —
**not** `columns_.empty()`, see the SQ_DESCRIBE note above for why that's
wrong once bind parameters are involved), opening a cursor at all is wrong,
not just unnecessary: `SQ_CURSORNAME`+`SQ_OPEN` against a `CREATE`/`DROP
TABLE` fails live with sqlcode -363, and against an `INSERT` with a bound
parameter fails live with a confusing sqlcode -255 ("Not in transaction").
Send `u16(4)`, `statementId`(u16), then **SQ_BIND if there are parameters**
(see below), then `u16(7)` (SQ_EXECUTE), `u16(12)` (EOT) — no cursor name,
no open. Server replies with `SQ_DONE` (row count) then EOT. Matches the
decompiled JDBC driver's own branch in `IfxSqli.executeStatementQuery`: only
`statementType == 2` (has a result set) goes through the cursor-based query
path; everything else is a plain `SQ_EXECUTE`. `SqliStatement::close()` is
correspondingly a no-op when no cursor was ever opened this way (there's
nothing for `SQ_CLOSE` to close).

**SQ_BIND** (`SqliStatement::execute`, sent - for both the cursor and
no-cursor branches - right after `statementId`, only when the statement has
bound parameters; confirmed live for `SQL_INTEGER`/`SQL_SMALLINT`/
`SQL_BIGINT`, see `tests/integration/test_bind.cpp`): `u16(5)`, `u16(count)`,
then per parameter: `ifxType`(u16), `indicator`(u16: `0xFFFF`/-1 for NULL,
`0` for a real value). **Only when non-NULL**: `precisionField`(u16) then the
encoded value bytes, padded to even length (same even-alignment rule as
every other variable-length field in this protocol, confirmed via
`IfxDataOutputStream.writePadded`: a plain `write(bytes)` plus a single
0-byte pad if odd, **no length prefix of its own**, so any field whose
byte count isn't implicit from the type alone must carry that information
via `precisionField` or an embedded length inside the value bytes. A NULL
parameter sends nothing beyond the indicator, no precision field, no data
bytes at all.

`precisionField`'s meaning is type-specific metadata:
- **INTEGER/SMALLINT/BIGINT**: a hardcoded constant, confirmed via the
  decompiled JDBC driver's `IfxInteger`/`IfxShort`/`IfxBigInt.
  getEncodedLength()` (`0x0A00`/`0x0500`/`0x1300` respectively, a
  `(precision << 8) | 0` value, the digit count being each type's max
  decimal digits, the same `(precision << 8 | scale)` shape DESCRIBE uses
  elsewhere). The value bytes are the same plain big-endian encoding as the
  TUPLE-format decoders in `sqli/types.h`, confirmed identical for all
  three by live round-trip (encode → SQ_BIND → server storage → plain
  SELECT → decode, matching the original value exactly).
- **CHAR**: confirmed live (`tests/integration/test_bind.cpp`). Traced via
  the decompiled `IfxChar.toIfx()` → `JavaToIfxChar`: the value bytes are
  **self-describing** (`u16` length + raw bytes, the same shape as this
  protocol's own `ifxChar2`). Confirmed empirically that `precisionField`
  can just be the string's own length - the server does not require it to
  match the target column's declared width (a 10-byte value bound into a
  `CHAR(20)` column round-tripped correctly with `precisionField=10`).
- **VARCHAR**: confirmed live (`tests/integration/test_bind.cpp`), same as
  CHAR. First traced to the wrong method (`IfxVarChar.toIfxTuple()`, which
  calls `JavaToIfxVarChar(s, dbEnc, leng, usestrenc)` and *would* fix the
  buffer at the declared column width) before catching that `toIfxTuple()`
  is the bulk PUT-array load path, not `sendBind`'s. `IfxVarChar.toIfx()`
  (the real bind path) calls the exact same `JavaToIfxChar` as CHAR -
  identical self-describing shape, `precisionField` confirmed live to just
  be the string's own length here too, needing no `ColumnSize`. Reuses
  `encodeChar` directly; only `ifxType` differs (13, not 0).
- **DATE**: confirmed live (`tests/integration/test_bind.cpp`). Value
  bytes are the same plain 4-byte big-endian day count as
  `decodeDateUnixDays`'s TUPLE-format input, traced via `IfxDate.toIfx()`
  → `JavaToIfxDate` → `JavaToIfxInt` (unlike DECIMAL/DATETIME below,
  DATE's bind and fetch formats match). `precisionField=4` (the byte
  width) round-trips correctly; whether the server actually inspects this
  field for DATE at all is untested, only that this value doesn't cause an
  error or desync.
- **DECIMAL**: confirmed live for all three exponent-byte shapes -
  negative (sign bit clear), positive (sign bit set), and the canonical
  zero special case - via three separate round trips in
  `tests/integration/test_bind.cpp`, not just one value plus a
  `decodeDecimal` self-round-trip (which by construction can't catch
  bytes that are internally self-consistent but that the server itself
  rejects or misreads). The precision field is the value's scale alone
  (not precision-shifted), confirmed via the decompiled `IfxSqli.
  sendBind`'s dispatch: for DECIMAL specifically it sends
  `((IfxDecimal)ival).getScale()` (`BigDecimal.scale()`), not
  `getEncodedLength()` the way every other type does. Crucially, this
  scale is intrinsic to the *value itself* (however many digits the
  caller wrote after the decimal point), not derived from the target
  column's declared scale - so, like CHAR/VARCHAR/DATE, no column
  metadata is needed. `sqli::encodeDecimal` independently re-derives
  `convFromBigDecimal`'s digit-packing math (matched against
  `decodeDecimal`'s confirmed live test vectors before ever going live)
  rather than copying it, and confirmed live that the server accepts a
  minimally-sized mantissa (exactly as many bytes as the literal's own
  digits need) rather than requiring it padded to the target column's
  declared precision.
- **DATETIME**: the qualifier (same packed `(totalDigits << 8) |
  qualifierByte` DESCRIBE already uses). Not yet implemented.

DECIMAL/DATETIME's BIND value encoding additionally prepends a 2-byte length
(`dec_ndgts + 1`, the byte count of the exponent+mantissa that follows) that
the TUPLE format doesn't have - confirmed for DECIMAL via the decompiled
`IfxDecimal.toIfx()` → `convIfx(true)` (its `convIfx(false)`, used by
`toIfxTuple()`, is the bulk-load path and what `decodeDecimal`'s shape
matches instead). DATETIME's is not yet implemented or live-tested.

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

**SQ_INSERTDONE** (S→C, discovered in Phase 3 while wiring up a DML-based
integration test - not part of the Phase 2 opcode set, since Phase 2 never
exercised `INSERT`): sent after every `INSERT`'s DESCRIBE, unconditionally,
regardless of whether the target table has a SERIAL column. Confirmed live,
18 bytes total:
- 10 bytes: `serial8Inserted`, a legacy dual-32-bit "long int" format (not
  the plain 8-byte BIGINT column encoding): `u16 sign` (`0` = no value,
  `1` = positive, `-1`/`0xFFFF` = negative), then the 8 magnitude bytes
  stored as [byte 6,7,8,9][byte 2,3,4,5] relative to the 10-byte buffer
  (i.e. the upper 4 bytes of the 64-bit value come *after* the lower 4 bytes
  on the wire) — reassembled high-then-low and negated if the sign was `-1`.
  Implemented as `decodeLegacyLongInt` in `sqli/byte_io.h`.
- 8 bytes: `bigSerialInserted`, a plain big-endian 8-byte value, no sign
  field.

**Important caveat, confirmed live**: for a plain `INSERT` (the only kind
this driver sends so far), both values reliably decode to `0`/sign-`1`,
*not* the row's actual generated SERIAL value (confirmed against a table
with an actual `SERIAL` column generating `1`, then `2` — the wire bytes
were byte-for-byte identical both times). Per the decompiled JDBC source,
a real value only shows up when the statement additionally sends a
`sendSGKPrepare` request (JDBC's `Statement.RETURN_GENERATED_KEYS`
mechanism) before executing — this driver doesn't implement that yet
(tracked as a Phase 5+ feature; see the roadmap). Getting the actual
generated identity today requires a `SELECT` (e.g. against
`sysmaster:sysdual` or a `RETURNING`-style clause if the engine supports
one) rather than relying on this field. `SqliConnection::lastSerialInserted()`
/ `SqliStatement::lastSerialInserted()` decode and expose this field
correctly per the confirmed byte layout above, but callers should not treat
the *value* as meaningful yet — only the byte-consumption behavior (which
was the actual bug this fixed: an unhandled `94` tag previously desynced
the stream and surfaced as `"unrecognized tag in executeCommand response"`
on any `INSERT`) is currently trustworthy.

Errors surface as a dedicated `SQ_ERROR`(13) record: `sqlcode`(u16, signed),
then (unless `sqlcode == -368`, a special case with no message) `isamcode`(u16),
`offset`(u32), message (`ifxChar2`). A `sqlcode == -619` is a special "resync"
marker (per the decompiled `receiveError`) with a slightly different trailing
shape — this driver drains it and surfaces it as a distinct error rather than
trying to auto-resync.

## Scalar type wire encodings (MVP set)

**Status: verified** — all eight MVP types (`sqli/types.h`/`types.cpp`) confirmed
live against a dedicated test table (`ifxodbc_test.type_probe`: one row of
positive values, one row of negative values, one all-NULL row, plus a handful
of extra DECIMAL rows used only to pin down the negative encoding), with
round-trip unit tests in `tests/sqli/test_types.cpp` reproducing the exact
captured bytes for each case, **plus** a repeatable live-server integration
test (`tests/integration/test_type_probe.cpp`, gated behind the
`IFX_ENABLE_INTEGRATION_TESTS` CMake option since it needs a running Informix
server) that creates the fixture table itself via `executeCommand`, inserts
the same three rows, fetches them back through the real
prepare→execute→fetch→`splitRow`→decode path, and asserts every decoded
value — this is what actually exercises `splitRow`'s column-slicing logic,
which the unit tests bypass by calling the decoders directly on captured
bytes.

Column metadata (from a DESCRIBE reply) packs the base type code and
nullability/distinct/etc. flags into a single 16-bit word (low byte = base
type, high bits = flags — **confirmed**: bit `0x100` = NOT NULL). The
`encodedLength` field (always a plain `u32`, not gated by the narrow/wide
toggle) is itself a packed pair for the two variable-shape types — confirmed
live: `(byteA << 8) | byteB` where for DECIMAL, byteA=precision and
byteB=scale (e.g. `DECIMAL(10,2)` → `encodedLength = 0x0A02 = 2562`); for
DATETIME, byteA=total decimal digit count and byteB is the standard Informix
TU qualifier byte, `(startField << 4) | endField` (e.g. `YEAR TO SECOND` →
`0x0A`, `encodedLength = 0x0E0A = 3594` since YEAR..SECOND is 14 digits).

| Type | Wire form | NULL sentinel |
|---|---|---|
| SMALLINT | 2 bytes, big-endian two's complement | `INT16_MIN` (`0x8000`) |
| INTEGER / SERIAL | 4 bytes, big-endian two's complement | `INT32_MIN` (`0x80000000`) |
| BIGINT / BIGSERIAL | 8 bytes, big-endian two's complement | `INT64_MIN` |
| CHAR(n) | fixed width `n`, space-padded, no NUL | leading `0x00` byte (rest of the buffer not meaningful) |
| VARCHAR(n) / NVARCHAR(n) | 1-byte length prefix + that many raw bytes (no NUL) | length=1, single data byte `0x00` (distinct from a real empty string, length=0) |
| DATE | 4 bytes, big-endian signed day count, epoch **1899-12-31** | `INT32_MIN`, same sentinel as INTEGER |
| DECIMAL(p,s) | see below | all bytes zero (including the exponent byte — see below) |
| DATETIME(qualifier) | see below | all bytes zero |

**DECIMAL(p,s)** — 1 exponent byte + `ceil(p/2)` mantissa bytes, each mantissa
byte a base-100 digit (0-99). Confirmed with `precision=10` (6 total bytes)
across four independent negative test values plus positive/zero/NULL cases:

- Exponent byte: bit `0x80` is the sign (1 = positive). For positive values,
  the low 7 bits are `64 + trueExp` where `trueExp` is such that
  `value = Σ digit[i] * 100^(trueExp-1-i)` over the mantissa digits (i.e.
  `trueExp` is the power-of-100 place value of the *first* mantissa digit,
  matching the classic biased-exponent floating-point idea but base 100).
  Confirmed: `12345.67` → exponent `0xC3` (low7 `0x43=67` → trueExp `3`),
  mantissa `01 17 2d 43 00` = digits `[1,23,45,67,0]` (the trailing `0` is
  padding out to the fixed 5-mantissa-byte width implied by precision 10,
  not a meaningful digit).
- For **negative** values, the low 7 bits of the exponent byte are
  bitwise-complemented relative to what the positive encoding would have
  been (`storedLow7 = 0x7F - positiveLow7`), and the mantissa is the
  **"hundred's complement"** of what the positive mantissa would have been —
  the base-100 analog of two's complement: 99-complement every digit
  (`99 - digit`), then add 1 to the rightmost digit, propagating carry
  leftward. This transform is its own inverse (applying it twice restores
  the original digits), so decoding negative mantissas just re-applies the
  same transform. Confirmed against four independent values sharing no
  bytes in common: `-99.99` (mantissa `[0,1,0,0,0]` decomplements to
  `[99,99,0,0,0]`), `-12345.67` (the exact negation of the positive example
  above — `[98,76,54,33,0]` decomplements to `[1,23,45,67,0]`, matching the
  positive digits exactly), `-0.01`, and `-100`.
- **Zero is not all-zero-bytes** — it has its own canonical encoding,
  exponent byte `0x80` (sign bit set, magnitude 0) with an all-zero mantissa,
  confirmed live. This is what distinguishes a real `0` from NULL (exponent
  byte `0x00`, sign bit *clear*, which never occurs for any real value).
- Value reconstruction avoids floating point entirely: concatenate the
  mantissa digits as 2-decimal-digit groups into one digit string, place
  the decimal point `2*trueExp` characters in from the left, then take
  exactly `scale` digits after the point (the fixed mantissa width often
  has trailing padding digits beyond the declared scale). Confirmed via
  `sqli/types.cpp`'s `decodeDecimal`, tested in `test_types.cpp`.

**DATETIME(qualifier)** — reuses the exact same packed base-100-digit
mantissa machinery as DECIMAL (this was not obvious going in; it fell out of
decoding the raw bytes and noticing the digit sequence was just the calendar
fields concatenated), but is never negative (no complement step) and the
digit sequence has a fixed, qualifier-derived meaning rather than a
floating decimal point:
- The qualifier byte's nibbles are the standard Informix TU_* constants:
  `YEAR=0, MONTH=2, DAY=4, HOUR=6, MINUTE=8, SECOND=10` (fraction = 11-15,
  **not yet supported** — no live capture with fractional seconds exists
  yet, so the sub-second byte layout is unconfirmed; `decodeDatetime`
  rejects these cleanly rather than guessing).
- Each field from `start` to `end` contributes to the mantissa in order:
  YEAR contributes 2 mantissa bytes (4 decimal digits, e.g. `2026` → bytes
  `20, 26`), every other field contributes 1 mantissa byte (2 decimal
  digits, e.g. month `07` → byte `07`). The exponent byte's biased value
  always equals the total mantissa byte count (e.g. `YEAR TO SECOND` = 7
  mantissa bytes → exponent `0xC7`, confirmed live for both test rows).
- Confirmed live for `YEAR TO SECOND` (qualifier `0x0A`) with two full
  round-trip values: `2026-07-29 12:34:56` (bytes `c7 14 1a 07 1d 0c 22 38`)
  and `2000-01-01 00:00:00` (bytes `c7 14 00 01 01 00 00 00`). Other
  whole-second sub-ranges within YEAR..SECOND (e.g. `MONTH TO DAY`) follow
  the same confirmed mechanism but aren't independently live-tested.

Both algorithms are implemented in `sqli/types.cpp` (`decodeDecimal`,
`decodeDatetime`), with round-trip unit tests in `tests/sqli/test_types.cpp`
reproducing the exact captured bytes above, and exercised live end-to-end via
`tools/ifx_probe` against the full `type_probe` table (including the
all-NULL row).

## Transactions

**Status: verified** - `SqliConnection::beginTransaction`/`commitTransaction`/
`rollbackTransaction` (`sqli/connection.cpp`) confirmed live end-to-end: a row
inserted after `beginTransaction` and then rolled back is invisible even to
the same connection, and a row committed instead is visible from a separate
connection (`tests/integration/test_transactions.cpp`).

Opcodes (`sqli/protocol.h`): `SQ_BEGIN`=35, `SQ_CMMTWORK`=19, `SQ_RBWORK`=20.
Sourced from the decompiled JDBC reference's `IfxMessageTypes`, now confirmed
against a live server.

**Request shape:**
- `SQ_BEGIN` / `SQ_CMMTWORK`: opcode only, then `EOT`.
- `SQ_RBWORK`: opcode, then a trailing `u16` savepoint id, then `EOT`. This
  extra field is easy to miss, it isn't present on begin or commit, and
  omitting it doesn't error; the server just waits forever for the two bytes
  that never arrive, since it's still mid-message and hasn't seen `EOT` yet.
  `0` means "roll back the whole transaction, not a named savepoint" (this
  driver doesn't implement savepoints). Confirmed against the decompiled
  reference's `sendRollback()`, which writes `20` then `0` before its caller
  sends `EOT` separately.

**Reply shape** - all three opcodes reply identically: `SQ_XACTSTAT`(99) tag,
then 3 more `u16` fields, then `DONE`, then `EOT`. Captured live for commit:
`00 63 00 01 00 00 00 01 00 0c` (`SQ_XACTSTAT`, `0001`, `0000`, `0001`, `EOT`;
no separate `DONE` record appeared in this capture, meaning the 3-field
tail is `SQ_XACTSTAT`'s own payload rather than a distinct following record).
This is the same tag `disconnectOrderly` already tolerates around `SQ_EXIT`,
now confirmed to also lead the reply to all three transaction-control
opcodes specifically (not just as a drain-before-close heuristic).

**Debugging note for future protocol work**: when a live round-trip hangs
with no error on either side, check `onstat -g ses` / `onstat -g sql <sid>`
on the server before guessing at another reply shape: an idle server thread
with no `SQL ERR`/`ISAM ERR` after the last statement means it already
consumed a complete, valid request and is waiting for the *next* one, which
rules out request-side bugs and points at either a still-missing request
field (as it was here) or an over-read on the client's *previous* reply
parse. A raw byte capture (write the request, then read whatever comes back
without parsing it) resolves this class of bug in one round trip instead of
iterating on guessed field counts.

## Open questions / risks

- Whether `has4ByteOffsets()`'s constituent capability bits (see "Narrow vs. wide
  framing" above) ever diverge on a real server — only tested against one engine
  (Docker `ibmcom/informix-developer-database`, IDS 14.10.FC7W1DE) where they all
  happen to co-occur as true. Revisit if a server is found where DESCRIBE and
  TUPLE/FETCH negotiate differently.
- DATETIME qualifiers including FRACTION (sub-second precision) are not
  supported — `decodeDatetime` rejects them cleanly rather than guessing the
  byte layout, since no live capture with fractional seconds has been taken.
  Same open question applies to INTERVAL, which reuses DECIMAL/DATETIME-style
  packed digits but *can* be negative (unlike DATETIME) — entirely unverified,
  out of MVP scope.
- DECIMAL/DATETIME qualifiers outside what's been directly tested (precision
  other than 10, qualifiers other than YEAR TO SECOND) are handled by a
  general formula derived from - and consistent with - the tested cases, but
  aren't independently confirmed against a live capture for those specific
  parameter values.
