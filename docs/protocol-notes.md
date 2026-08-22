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
`0` for a real value), then `precisionField`(u16) - **sent unconditionally,
NULL or not** - and, only when non-NULL, the encoded value bytes, padded to
even length (same even-alignment rule as every other variable-length field
in this protocol, confirmed via `IfxDataOutputStream.writePadded`: a plain
`write(bytes)` plus a single 0-byte pad if odd, **no length prefix of its
own**, so any field whose byte count isn't implicit from the type alone
must carry that information via `precisionField` or an embedded length
inside the value bytes.

A NULL parameter's `precisionField` was originally believed to be omitted
entirely (only `ifxType` + indicator, nothing more) - wrong, and it stayed
wrong through several live-confirmed round trips because every NULL bound
so far was either the sole parameter or the last one in its bind block,
both cases where a missing 2-byte field gets silently absorbed by whatever
follows instead of producing a visible failure. Binding a NULL ahead of
another parameter (e.g. `[NULL smallint, non-null integer]`) hung the
connection - same failure mode as the SQ_RBWORK savepoint-id omission
above: the server sat waiting for 2 bytes that never arrived. Re-reading
`IfxSqli.sendBind`'s `ival.isNull()` branch shows it writes
`writeSmallInt((short)0)` for the precision field right after the `-1`
indicator, matching every other parameter's shape (`type`, `indicator`,
`precision`, `[data]`) rather than truncating it early. Fixed in
`writeBindBlock`; confirmed live via `checkNullBind` in
`tests/integration/test_bind.cpp`, which specifically binds a NULL as the
*first* of several parameters to exercise the case the earlier, narrower
tests couldn't reach.

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
- **DATETIME**: confirmed live for `YEAR TO SECOND` and `YEAR TO
  FRACTION(5)` (`tests/integration/test_bind.cpp`). The precision field is
  the qualifier (same packed `(totalDigits << 8) | qualifierByte` DESCRIBE
  already uses); `driver/bind_encode.cpp` hardcodes `YEAR TO SECOND` when
  `SQL_TIMESTAMP_STRUCT.fraction == 0` and `YEAR TO FRACTION(5)` otherwise,
  rather than deriving either from the target column's actual declared
  qualifier - this driver has no column metadata to consult at bind time
  regardless (same limitation as DATE/CHAR/VARCHAR). `ts.fraction` is
  nanoseconds; it's scaled down by `10^4` (truncating, not rounding) to
  FRACTION(5)'s ten-microsecond resolution, Informix DATETIME's finest.
  `sqli::encodeDatetime`'s exact output bytes were hand-derived and checked
  in a unit test (see `tests/sqli/test_types.cpp`) *before* ever going
  live, reusing the same value already confirmed live on the decode side
  (`2026-07-29 12:34:56`, and separately `2024-06-15 13:45:52.12345` for
  the FRACTION case) - the prediction matched the actual live round trip on
  the first attempt for YEAR TO SECOND; the FRACTION case needed one fix
  first (the exponent byte's biased magnitude has to stay pinned to the
  whole-field pair count even once FRACTION pairs are appended - see the
  DATETIME(qualifier) TUPLE-format section above for the live capture that
  caught it).

DECIMAL/DATETIME's BIND value encoding additionally prepends a 2-byte length
(`dec_ndgts + 1`, the byte count of the exponent+mantissa that follows) that
the TUPLE format doesn't have - confirmed for DECIMAL via the decompiled
`IfxDecimal.toIfx()` → `convIfx(true)` (its `convIfx(false)`, used by
`toIfxTuple()`, is the bulk-load path and what `decodeDecimal`'s shape
matches instead), and for DATETIME via `IfxDateTime.toIfx()` routing
through the same `Decimal.javaToIfx()` encoder `IfxDecimal.toIfx()` uses -
Informix DATETIME reuses DECIMAL's packed-digit wire machinery end to end,
not just its TUPLE-format decode shape.

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
  `YEAR=0, MONTH=2, DAY=4, HOUR=6, MINUTE=8, SECOND=10`, and FRACTION(n) for
  n in 1-5 is `10+n` (11-15) — confirmed live via `IFX_SQLI_TRACE` capture
  of a `YEAR TO FRACTION(5)` column holding `2024-06-15 13:45:52.12345`.
- Each field from `start` to `end` contributes to the mantissa in order:
  YEAR contributes 2 mantissa bytes (4 decimal digits, e.g. `2026` → bytes
  `20, 26`), every other whole field (MONTH..SECOND) contributes 1 mantissa
  byte (2 decimal digits, e.g. month `07` → byte `07`). FRACTION digits
  continue the *same* digit-pair stream immediately after SECOND, packed
  left-justified: `ceil(n/2)` more mantissa bytes for `n` fraction digits,
  the trailing digit of an odd `n` **zero-padded on the low nibble** (not
  left-padded) — e.g. `FRACTION(5)` value `.12345` → 3 more mantissa bytes
  `[12][34][50]`, confirmed against the live capture below.
- The exponent byte's biased value equals the *whole-field* mantissa byte
  count (YEAR..SECOND only) even when FRACTION pairs follow — DATETIME's
  implicit "decimal point" always sits right after SECOND, the same way
  DECIMAL's exponent marks where its digit string's point falls. Confirmed
  live: the `YEAR TO FRACTION(5)` capture below has 10 total mantissa bytes
  (7 whole + 3 fraction) but exponent `0xC7` (biased `7`, not `10`) —
  getting this wrong (using the total mantissa count instead) round-trips
  fine through `decodeDatetime`'s own self-consistency but is rejected by
  the server on `SQ_BIND` with sqlcode -1266 ("Intervals or Datetimes are
  incompatible for the operation"), a mismatch `decodePackedDigits` can't
  catch because it doesn't validate the exponent against the mantissa
  length it's given.
- Confirmed live for `YEAR TO SECOND` (qualifier `0x0A`) with two full
  round-trip values: `2026-07-29 12:34:56` (bytes `c7 14 1a 07 1d 0c 22 38`)
  and `2000-01-01 00:00:00` (bytes `c7 14 00 01 01 00 00 00`); and for
  `YEAR TO FRACTION(5)` (qualifier `0x0F`, `encodedLength = 0x130F = 4879`)
  with `2024-06-15 13:45:52.12345` (bytes
  `c7 14 18 06 0f 0d 2d 34 0c 22 32`), including a bind round trip (encode
  → `SQ_BIND` → server storage → plain SELECT → decode). Other sub-ranges
  within YEAR..FRACTION(5) (e.g. `MONTH TO DAY`, `SECOND TO FRACTION(3)`)
  follow the same confirmed mechanism but aren't independently live-tested.

**INTERVAL(qualifier)** — shares DECIMAL's exact packed base-100-digit-pair
byte format (same exponent-byte-plus-mantissa shape, same hundred's-complement
negative encoding) but is a genuinely different codec from DATETIME's, not a
variant of it, confirmed on the fetch side for both interval classes
(`sqli::decodeIntervalYearMonth`/`decodeIntervalDayFraction`; bind encoding
isn't implemented yet for either). `extendedId=0` in DESCRIBE - a plain
scalar like DECIMAL/DATETIME, not extended like BOOLEAN. Wire type code
`14`, shared by both interval classes (matches the decompiled JDBC driver's
`IfxIntervalYM`/`IfxIntervalDF`, which both set ifxType 14 - which class a
given column is comes from the qualifier's start/end codes, not the type
code).
- `encodedLength` is packed the same `(topByte << 8) | qualifierByte` shape
  DATETIME uses, but `topByte` ("length" in Informix's own terminology)
  is *not* a fixed per-field digit count the way DATETIME's totalDigits is -
  INTERVAL's leading field has a user-declared width of 1-9 digits
  (`INTERVAL YEAR(4) TO MONTH`), so `topByte = leadingWidth + (end - start)`,
  and `leadingWidth` has to be recovered by subtracting `(end - start)` back
  out. Confirmed live: `INTERVAL YEAR(4) TO MONTH` → `encodedLength = 0x0602`
  (topByte 6, qualifier `0x02`, leadingWidth `6-2=4`); `INTERVAL YEAR(2) TO
  MONTH` → `0x0402` (leadingWidth `4-2=2`); `INTERVAL MONTH(3) TO MONTH` →
  `0x0322` (start and end both MONTH, leadingWidth `3-0=3`); `INTERVAL
  YEAR(3) TO YEAR` → `0x0300` (leadingWidth `3-0=3`, no MONTH field at all).
- Mantissa byte count is `ceil((topByte + (end&1) + 1) / 2)`-shaped (the
  `(end&1)` term matters once DAY-FRACTION's odd FRACTION end codes are in
  play; always 0 for YEAR-MONTH's even end codes) - confirmed matching the
  captured byte counts above (3, 2, 2, and 2 bytes respectively).
- The genuinely different part from DATETIME: **INTERVAL reuses DECIMAL's
  floating representation outright**, not DATETIME's fixed-point one -
  leading zero digit-pairs are stripped and the exponent shrinks to match,
  then trailing zero-pairs backfill the freed slots to pad back out to the
  qualifier's fixed mantissa width. This wasn't obvious going in and cost a
  wrong first guess: `INTERVAL YEAR(2) TO MONTH` holding `47-07` (leading
  field already fills its 2-digit width, nothing to strip) decoded with
  exponent `0xC6`'s biased magnitude (`6`) matching the *unnormalized*
  formula `(topByte + 10 - end + 1) / 2` directly, mantissa `[47, 7]` -  but
  `INTERVAL YEAR(4) TO MONTH` holding `3-07` (leading field's high digit
  pair is zero) decoded with the *same* exponent `0xC6` even though its
  unnormalized formula predicts `7` - the leading zero pair was stripped,
  and the mantissa `[3, 7, 0]` has the freed slot backfilled with a
  trailing zero pair rather than left as `[0, 3, 7]`. Un-shifting this
  (prepending `unnormalizedExp - actualExp` zero-pairs, then truncating
  back to the fixed mantissa width) recovers the intended digit string.
- A second, independent gotcha at the same odd/even boundary: when
  `topByte` is odd, the fixed mantissa width has one more digit slot than
  there are real digits, and that padding slot sits at the *front* (most
  significant), not the back - confirmed live via `INTERVAL MONTH(3) TO
  MONTH` holding `43`: the raw reconstructed 4-digit string is `0043`, and
  the correct 3-digit field is `043` (`= 43`), not `004` (`= 4`). The
  initial implementation dropped the back digit instead (reasoning from
  DATETIME FRACTION's padding, which *is* at the back) and silently
  produced `4`, caught only because this specific qualifier shape was
  live-tested rather than assumed.
- Value semantics, confirmed via the decompiled JDBC reference's separate
  branches: `MONTH TO MONTH` (`start == MONTH`) is a single field that's
  already a total-months value, no years/12 split. Any YEAR-anchored
  qualifier (`start == YEAR`) splits into `years * 12`, plus a trailing
  `MONTH` field's remainder if `end == MONTH`. Confirmed live for all three
  qualifier shapes (`YEAR TO YEAR`, `YEAR TO MONTH`, `MONTH TO MONTH`) and
  both signs (six total captures - see `tests/sqli/test_types.cpp`'s
  `INTERVAL YEAR TO MONTH` test for the exact bytes).
- Confirmed live for `sqli::decodeIntervalYearMonth`'s caller in
  `driver/convert.cpp` too (`SQL_C_CHAR` text fetch, e.g. "3-07" - matching
  Informix's own `dbaccess` display, not ISO 8601), checked directly
  against the same captured bytes without a live connection (see
  `docs/protocol-notes.md`'s Windows ODBC DM note above for why).

**INTERVAL DAY-FRACTION class** (`sqli::decodeIntervalDayFraction`, qualifier
start/end both in `{DAY, HOUR, MINUTE, SECOND, FRACTION(1..5)}`, e.g.
`DAY TO SECOND`, `HOUR TO FRACTION(3)`) shares YEAR-MONTH's un-normalization
machinery exactly (same DECIMAL-style floating representation, same
`unnormalizedExp` formula, confirmed to hold unchanged with `end` now a
FRACTION code up to 15) - but layering DATETIME-style FRACTION digits *on
top of* a floating-point leading field surfaced a mistake the YEAR-MONTH
slice's tests didn't catch:
- **Padding is per-field, not a single global correction on the whole
  digit string.** The first implementation reused YEAR-MONTH's "drop the
  front padding digit when totalDigits is odd" fix as a blanket rule and
  it corrupted the leading field: `INTERVAL DAY(2) TO FRACTION(5)` holding
  `12 03:04:05.67890` (totalDigits=13, odd) decoded as day=20, hour=30 (an
  impossible hour) instead of day=12, hour=3. The actual live bytes (`c4
  0c 03 04 05 43 59 00`) needed **no** un-normalization at all (leading
  DAY=12 already fills its 2-digit width, `shift=0`) - the "extra" digit
  came entirely from FRACTION(5)'s own odd digit count, padded at the
  *end of the FRACTION sub-region specifically* (matching
  `decodeDatetime`'s FRACTION convention, confirmed independently there),
  not at the front of the whole mantissa. Truncating from the front
  dropped a real digit of DAY instead of the padding digit of FRACTION.
- The fix: compute each field's own raw byte span independently -
  `ceil(leadingWidth/2)` bytes for the leading field (trim its *own* front
  padding digit if `leadingWidth` is odd), one byte per subsequent whole
  field (HOUR/MINUTE/SECOND, always exactly 2 real digits, no padding
  possible), then `ceil(fractionDigits/2)` bytes for FRACTION (trim its
  *own* trailing padding digit if `fractionDigits` is odd). Never trim the
  concatenation of all of them as one string. This also fixes a latent
  undercount in the mantissa-byte-count formula itself
  (`sqli/statement.cpp`'s `splitRow` and the shared `decodeIntervalDigits`
  helper both moved to this per-field sum): the old
  `ceil(totalDigits/2)`-based formula can undercount by 1 when the leading
  field and FRACTION are *independently* odd-width but their oddness
  happens to cancel out in the combined `totalDigits` parity (e.g. `DAY(1)
  TO FRACTION(3)`, reasoned but not independently live-tested).
- Confirmed live across four captures covering both signs, DAY- and
  HOUR-anchored leading fields, even and odd leading widths, with and
  without un-normalization stripping, and with and without FRACTION -
  see `tests/sqli/test_types.cpp`'s `INTERVAL DAY TO FRACTION` test for
  the exact bytes. `driver/convert.cpp`'s `SQL_C_CHAR` text surface
  (`formatIntervalDayFraction`, e.g. "12 03:04:05.67890") confirmed the
  same way as YEAR-MONTH's, directly against the captured bytes.
  MINUTE/SECOND-anchored leading fields and catalog rows
  (`SQLColumns`/`SQLGetTypeInfo`) are still open; bind encoding is now done
  at the sqli layer (see below), not yet on the ODBC `SQLBindParameter`
  surface.

**INTERVAL SQ_BIND encoding** (`sqli::encodeIntervalYearMonth`/
`encodeIntervalDayFraction`) turns out to be genuinely asymmetric with the
fetch direction above, not its mathematical inverse applied blindly.
Traced via the decompiled JDBC reference's
`Decimal(boolean sign, byte[] b, int blen, short inPrec)` constructor
(what `IfxIntervalYM.toIfx()`/`IfxIntervalDF.toIfx()` route through via
`JavaToIfxType.convertIntervalToDecimal(...).javaToIfx()`): **SQ_BIND does
not strip leading zero digit-pairs down to DECIMAL's floating form.** The
mantissa is built directly at the qualifier's full, fixed field widths -
the same un-normalized digit layout `decodeIntervalDigits` reconstructs as
an intermediate step before the fetch side's own trimming - and the
exponent byte's biased magnitude is unconditionally the qualifier's own
`unnormalizedExp` formula, `(totalDigits + 10 - end + 1) / 2`, never
shifted down for a leading-zero leading field the way a live fetch of the
same value would be. The server evidently does its own normalizing when it
later re-serves the value (exactly as it already does for plain DECIMAL
binds, which also send a non-canonical/non-stripped mantissa width), not
require the client to pre-strip on the way in.

Confirmed live for both interval classes, both signs, via
`tests/integration/test_bind.cpp`'s `checkIntervalYearMonthRoundTrip`/
`checkIntervalDayFractionRoundTrip`, each deliberately using a value that
needs stripping on the fetch side specifically to prove the point:
`INTERVAL YEAR(4) TO MONTH` bound with months=43 (3-07, the leading
field's high digit-pair is zero) sends exponent `0xC7`/mantissa
`[00,03,07]` - one exponent step higher and a differently-shaped mantissa
than the `0xC6`/`[03,07,00]` a live fetch of the same value returns (see
the INTERVAL(qualifier) section above) - and the server accepts it,
storing and re-serving the correct value. The negative case (hundred's
complement applied over this *un-stripped* mantissa, combined with the
*un-stripped* exponent, a combination not exercised by DECIMAL's own
negative bind tests, which use DECIMAL's differently-derived canonical
exponent) is confirmed live too, not just self-round-tripped through this
driver's own decoder - this project's INTERVAL work has hit three separate
bugs so far, each caught only by testing the specific shape rather than
reasoning from a similar one, so the sign/stripping combination got its
own live check rather than being assumed safe.

Field construction otherwise mirrors the fetch side's per-field layout
directly (no un-normalization needed since nothing is stripped): the
leading field is zero-padded to its declared width with a front pad digit
when that width is odd (the same convention decode's `trimLeadingField`
undoes, reproduced here rather than inverted), every subsequent whole
field is a plain 2-digit value, and FRACTION digits (DAY-FRACTION class
only) back-pad with a trailing zero when the qualifier's fraction digit
count is odd - the same convention `encodeDatetime` already uses for
DATETIME's own FRACTION suffix. `IntervalDayFractionValue`'s own
`fractionDigits` field isn't consulted on encode (same reasoning as
`encodeDatetime`: the wire qualifier alone declares FRACTION's width).

**Now wired to the ODBC `SQLBindParameter` surface** (`driver/bind_encode.cpp`).
Unlike DATE/DECIMAL/DATETIME, whose bind encoders need no target-column
metadata, INTERVAL's leading field width is a genuine per-column,
user-declared value (1-9 digits) that isn't recoverable from the bind
value's magnitude alone - `43` bound to `YEAR(2) TO MONTH` and to
`YEAR(9) TO MONTH` need different qualifier bytes and different exponents
even though the numeric value is identical. Correct ODBC-surface binding
therefore needs `SQLBindParameter`'s `ColumnSize`/`DecimalDigits` (which
for `SQL_INTERVAL_*` parameter types carry the interval leading precision
and fractional-seconds precision respectively) - previously received but
discarded outright (`/*ColumnSize*/`/`/*DecimalDigits*/`, since none of
the MVP-8 types need them), now plumbed through `ParamBinding`
(`driver/handles.h`) into `encodeBindParam`'s signature.

`intervalEncodedLengthFor` (`driver/bind_encode.cpp`) reconstructs the
wire qualifier from the `ParameterType`/`ColumnSize`/`DecimalDigits`
triple: `ParameterType` is one of the 13 `SQL_INTERVAL_*` concise type
constants (an independent copy of `driver/catalog.cpp`'s
`IntervalShapeEntry` shape table, kept separate per that file's own stated
precedent), `ColumnSize` supplies the leading field's declared width, and
`DecimalDigits` supplies FRACTION's digit count for the `*_TO_SECOND`
shapes. Confirmed correct against three qualifier values `driver/catalog.cpp`'s
SQLColumns work already captured live (`tests/driver/test_bind_encode.cpp`,
no server needed) - a pure function is the right verification surface for
this piece, since it's the one genuinely new logic in the INTERVAL bind
path (everything downstream already routes through the live-verified
`sqli::encodeIntervalYearMonth`/`encodeIntervalDayFraction`).

The value itself comes through the native `SQL_INTERVAL_STRUCT`
(`SQL_C_INTERVAL_*`), not a `SQL_C_CHAR` text literal like DECIMAL's -
deliberately not this driver's own display-text convention
(`formatIntervalYearMonth`/`DayFraction`): unlike DECIMAL's `"12345.67"`,
which happens to already be the ODBC C-to-SQL representation for that
type, this driver's interval text is a dbaccess-mimicking *output*
convention it built itself, so a parser mirroring it would only prove
self-consistency (any test feeding the formatter's own output back into
its own parser passes circularly), not real interoperability - the whole
point of the ODBC surface. `SQL_INTERVAL_STRUCT`'s layout is externally
defined (`sqltypes.h`: `interval_type`/`interval_sign` plus a
`year_month`/`day_second` union), so encoding/decoding it is meaningfully
verifiable instead. `SQL_DAY_SECOND_STRUCT`'s `fraction` field is
nanoseconds, the same convention `SQL_TIMESTAMP_STRUCT`'s own `fraction`
field already uses (see the DATETIME bind section above) - scaled down to
Informix's own digit count, truncating.

Confirmed live end-to-end through this driver's own exported ODBC entry
points (`SQLAllocHandle`/`SQLDriverConnect`/`SQLPrepare`/
`SQLBindParameter`/`SQLExecute`, called directly against the built
`ifxodbc` library rather than through a Driver Manager - the IM002
registration gap only blocks *DM-mediated* loading, not a program calling
the driver's exported C functions directly): both interval classes, both
signs, a `FRACTION` value, and a NULL interval bind ahead of another
parameter (the same desync-prone ordering `tests/integration/test_bind.cpp`'s
`checkNullBind` already exercises for other types) all round-tripped
correctly - insert via the real ODBC entry points, read back and decode
via the already-trusted sqli layer. INTERVAL needs no extended-type
owner/name for a NULL bind (unlike BOOLEAN): its `extendedId` is 0 (a
plain scalar in DESCRIBE terms, confirmed live earlier in this section),
and `writeBindBlock` never sends `precisionField` for a NULL parameter
regardless of type, so the qualifier is irrelevant to the NULL case too.
One caveat this pass doesn't close: whether a real Driver Manager passes
the `SQL_INTERVAL_*` `ParameterType`/`ColumnSize`/`DecimalDigits` triple
through to this driver unmolested - that needs the Linux/WSL2 unixODBC
path not set up in this environment (same gap noted throughout this
document).

`intervalEncodedLengthFor` rejects `DecimalDigits > 5` on a `*_TO_SECOND`
shape rather than silently truncating: Informix caps `FRACTION` at 5
digits (the same limit DATETIME's `FRACTION` has), and past that the
packed qualifier byte's low nibble (`10 + fractionDigits`) would overflow
past `0xF` and corrupt the wire qualifier rather than just describe an
oversized fraction - a real boundary check on a real ODBC entry point, not
speculative validation. `encodeBindParam`'s `SQL_INTERVAL_STRUCT`
field-reading itself (as opposed to `intervalEncodedLengthFor`'s pure
qualifier math, already unit-tested) was previously verified only by the
throwaway ODBC-surface probe above, which isn't part of the repo -
`tests/driver/test_bind_encode.cpp` now also covers it directly, against
`sqli::encodeIntervalYearMonth`/`encodeIntervalDayFraction` rather than
against itself: the `MONTH TO MONTH` special case (`start == 2` reads
`month` as the already-total value, not `year*12+month` - a struct with a
nonzero `year` alongside `month` catches a regression that would silently
fall through to the wrong branch), and `SQL_DAY_SECOND_STRUCT.fraction`'s
nanosecond-to-`FRACTION`-digit scaling (a non-multiple-of-10^6 nanosecond
value confirms truncation, not rounding).

Both algorithms are implemented in `sqli/types.cpp` (`decodeDecimal`,
`decodeDatetime`), with round-trip unit tests in `tests/sqli/test_types.cpp`
reproducing the exact captured bytes above, and exercised live end-to-end via
`tools/ifx_probe` against the full `type_probe` table (including the
all-NULL row).

## Simple large objects (BYTE/TEXT) - fetch, confirmed live

Informix has two entirely separate LOB mechanisms: *simple* large objects
(BYTE=`ifxtype::kByte`(11)/TEXT=`ifxtype::kText`(12), the older mechanism,
this section) and *smart* large objects (BLOB/CLOB, stored in an sbspace -
see the next section for what's confirmed about those so far, which is much
less). They share nothing at the wire level.

**The TUPLE row does not carry a simple LOB's bytes inline.** DESCRIBE
reports `encodedLength=56` for both BYTE and TEXT (confirmed live), and
that's exactly what's in the row: a fixed 56-byte, mostly-opaque
*descriptor*, not the value. Confirmed live via a raw-bytes capture
(temporary debug instrumentation, not part of the repo) against a real
`TEXT` column holding `"hello-clob\n"` (11 bytes): the row's per-column slice
is 56 bytes regardless of the value's actual length, matching a decompiled
JDBC reference (`IfxBlob.java`, which allocates a same-sized 56-byte
`blobDesc` buffer). Field layout within the 56 bytes, per the decompiled
source and now independently confirmed live for three of the four fields:

- offset 8 (4 bytes, big-endian) = `tb_start` - confirmed live as `1` for
  an 11-byte value (1-based start of the value's byte range).
- offset 12 (4 bytes) = `tb_end` - confirmed live as `11` for the same value
  (1-based inclusive end - `tb_end - tb_start + 1` gives the byte count).
- offset 16 (4 bytes) = `tb_size` - confirmed live as `11`, the value's
  total byte length directly.
- offset 38-39 (2 bytes) = `tb_flags`. **NULL is confirmed live to be
  exactly `tb_flags == 1`** (bytes `00 01`) **with every other byte in the
  56-byte descriptor zero** - captured from a real NULL `TEXT` column,
  distinguishable from a real value's `tb_flags` (confirmed live as `0x0208`
  for an inserted-but-not-yet-fetched value - some other bit pattern, not
  exactly `1`). The rest of `tb_flags`'s bits, and the descriptor's other
  opaque bytes (0-7, 20-37, 40-55), are never interpreted by this driver -
  only echoed back verbatim to the server (see below), matching how the
  decompiled JDBC reference treats them too.

**Fetching the actual content is a second, standalone round trip** -
`SqliConnection::fetchSimpleLob(descriptor)` (`sqli/connection.cpp`), called
after a row carrying a simple-LOB column has already been fetched and
decoded. The real precondition is "no response is currently mid-stream on
this connection" (request/response can't interleave), not "never inside a
fetch loop" per se - `SqliStatement::fetchRow`'s underlying batch read
already drains its whole response to `SQ_EOT` before returning any row, so
calling this once `fetchRow` has returned (even once per row, as
`tools/ifx_probe` does below) is safe; confirmed live this way. **Not**
confirmed live: a result set large enough to span multiple server-side
fetch batches, where the main cursor's own protocol state might not be as
cleanly idle between rows. It sends `SQ_FETCHBLOB`(38) + the 56-byte
descriptor **echoed back verbatim, unmodified** + `SQ_EOT`, then reads the
generic tagged-record response loop
(same shape `executeCommand`'s reply loop uses) accumulating every
`SQ_BLOB`(39) chunk's payload (`u16` length + that many bytes, padded to
even length like every other variable-length field in this protocol) until
`SQ_EOT`. Opcodes 38/39/40 (`kFetchBlob`/`kBlob`/`kFerr`,
`sqli/protocol.h`) are reverse-engineered from the decompiled reference's
`sendFetchBlob`/`receiveText`/`receiveBlob` (`IfxSqli.java`) - `kFerr`(40)
is only relevant to the bind direction (an abort sentinel if the source
stream fails mid-upload, not yet implemented - see below).

**Confirmed live, first attempt, no hang**: fetched `"hello-clob\n"` back
correctly from a `TEXT` column via `tools/ifx_probe` (which now calls
`fetchSimpleLob` for `kByte`/`kText` columns after checking the NULL
sentinel), correctly reported `NULL` for a NULL `TEXT` row, correctly
fetched the same 11 bytes as raw hex from the equivalent `BYTE` column, and
- to exercise chunking, since the decompiled reference shows the server can
split a large value across multiple `SQ_BLOB` messages - fetched a 200,000
byte `BYTE` value and confirmed it came back byte-for-byte identical to the
source file (`cmp`, exact match). `splitRow` (`sqli/statement.cpp`) treats
`kByte`/`kText` as fixed-width at `col.encodedLength` for the purposes of
locating the descriptor within a row - the same shape `kChar` already uses,
just interpreted completely differently downstream.

**Bind (write direction) - now done and confirmed live.** A simple-LOB "?"
parameter's `SQ_BIND` record is an ordinary scalar slot (type/indicator/
precision/data, no extended owner/name - types 11/12 are below the `type >
18` threshold that gates extended-type framing) whose `data` carries a
56-byte *placeholder* descriptor, not the value: all-zero except `tb_size`
(offset 16, 4 bytes big-endian) set to the content's byte length
(`sqli::encodeSimpleLobPlaceholder`, mirroring the decompiled reference's
`IfxBlob.toIfx()`, which patches only that one field into an
otherwise-untouched `blobDesc`). `precisionField` is left at `0` -
`IfxBlob` never overrides `IfxObject.getEncodedLength()`'s zero default,
unlike DECIMAL/CHAR/VARCHAR's own precisionField conventions.

**The real content streams out-of-band, but critically under a header this
project's own prior research summary of the decompiled source had missed
entirely**: immediately after the whole `SQ_BIND` block (every param's
type/indicator/precision/data, in order), IF at least one bound param is a
non-NULL simple LOB, the client sends `SQ_BBIND`(41) + `u16` count of such
params, THEN, for each non-NULL simple-LOB param in order, its content as
`SQ_BLOB`(39) chunks (`u16` length + that many bytes, padded to even
length, capped at 1024 bytes/chunk) terminated by a zero-length `SQ_BLOB`
chunk. A NULL simple-LOB param is skipped entirely in this phase - no
chunk stream, not even an empty terminator - confirmed by reading the
decompiled reference's `sendBlob()` (`IfxSqli.java:3328`) directly rather
than trusting the earlier paraphrase: its per-param loop is explicitly
gated on `!ival.isNull()`, and `blobCount` (the count field) is
incremented under the identical condition inside `sendBind` itself.

**How the missing header was found**: the first live attempt (bind record
+ a bare `SQ_BLOB` chunk stream, no `SQ_BBIND` wrapper - matching this
project's own earlier research summary, which had paraphrased `sendBlob`'s
existence but dropped its own opening `SQ_BBIND`(41)+count header) got a
lone tag-56 (`SQ_EXIT`) response and the connection dropped - the same
generic "malformed request" failure signature already documented for
BOOLEAN's missing owner/name. Isolating which part of the request
triggered it (a terminator-only stream with zero real chunks succeeded; a
single real data chunk, any size, failed identically) pointed at the chunk
stream itself rather than the base bind record, which is what prompted
re-reading `IfxSqli.java` directly instead of the earlier summary -
`sendBlob()`'s own first three lines (`writeSmallInt(41)`,
`writeSmallInt(count)`, `flush()`) were the missing piece.

Confirmed live via `tests/integration/test_bind.cpp`'s `checkSimpleLobBind`
(part of the default `bind_integration` test, not a one-off probe): a
small (21-byte, odd-length, single-chunk) `TEXT` value round-trips
correctly; a `NULL` `TEXT` value bound ahead of another parameter in the
same statement doesn't desync the following parameter (the same
desync-prone ordering already exercised for other types via
`checkNullBind`); and a 5000-byte `BYTE` value (five chunks: four 1024-byte
chunks plus a 904-byte remainder) round-trips byte-for-byte exact,
confirming the client-side chunking loop (`writeBindBlock`,
`sqli/statement.cpp`) is correct for a multi-chunk upload, not just a
single-chunk one.

**ODBC surface - now done and confirmed live too.** Simple LOBs needed a
real architectural addition here, not just another type case: before this
work, `SQLGetData` (`driver/statement.cpp`) was single-shot for every
type - one call copies the whole value (or truncates it) and there was no
way to resume a partial read. That's the wrong contract for a LOB: the
standard ODBC idiom for pulling a large value through a small fixed buffer
is `while (SQLGetData(...) == SQL_SUCCESS_WITH_INFO) append(buf)`, which
needs each call to continue from where the last one left off. Building
that generically for every type would have been overkill (nothing else
needs it, and retrofitting it onto CHAR/VARCHAR's already-correct
single-shot behavior risked regressing something working); scoped instead
to a `LobFetchState` (`driver/handles.h`) - content, offset, and an
`exhausted` flag - keyed per column in a new `OdbcStatement::
lobFetchState` map, cleared on every `SQLFetch` (a new row's LOB isn't a
continuation of the previous row's). `exhausted` is tracked separately
from `offset == content.size()` because a genuinely empty (0-byte,
non-NULL) value would satisfy that condition on its very first call too -
the real "no more data" signal is "every byte has already been handed to
a non-truncated call," which only `SQL_NO_DATA`(100) should follow.

A `SQLBindCol`-driven fetch (`SQLFetch`'s bound-column loop) deliberately
does **not** get this resumable state - it uses a throwaway local
`LobFetchState` per call, matching every other bound column type's
existing single-shot-with-truncation contract (`writeCharResult`'s
truncate-and-report-full-length shape, reused directly). Only the
standalone `SQLGetData` entry point (explicitly called after `SQLFetch`,
not via the bound-column loop) gets a persistent, per-column entry in
`OdbcStatement::lobFetchState`.

BYTE's default C type is `SQL_C_BINARY` (raw bytes via the indicator, no
NUL terminator), distinct from TEXT's `SQL_C_CHAR` (NUL-terminated,
reserves one buffer byte for the NUL) - both were previously entirely
unhandled (`SQL_C_BINARY` wasn't accepted by `SQLBindCol`/`SQLGetData`'s
gate checks at all). `SQLBindParameter`'s mirror: `SQL_LONGVARBINARY`
(BYTE) and `SQL_LONGVARCHAR` (TEXT) are new `encodeBindParam` branches
(`driver/bind_encode.cpp`) that build a `BindParam` with the 56-byte
placeholder descriptor in `data` and the real bytes in
`simpleLobContent`, the same shape the sqli-layer bind work above already
established - `driver/bind_encode.cpp` didn't need any new machinery here,
just two more branches routing into what already existed.
`SQLGetTypeInfo`/`SQLColumns` also now report these two types correctly
(`driver/catalog.cpp`'s `kTypeMap` gained `SQL_LONGVARBINARY`/-4/`"byte"`
and `SQL_LONGVARCHAR`/-1/`"text"` rows - no INTERVAL-style per-qualifier
special-casing needed, since unlike INTERVAL these are plain scalars at
the catalog layer too); `COLUMN_SIZE` for both is `2147483647`, an
unconfirmed first-hypothesis sentinel for "no declared maximum" (matching
a common ODBC driver convention), not traced against a live capture the
way DATE's `precisionField=4` eventually was.

Confirmed live, first attempt, no hang, through this driver's own
exported ODBC entry points called directly (bypassing the Driver Manager,
same technique as INTERVAL's `SQLBindParameter` verification): a
`SQLBindParameter` TEXT insert and a `SQLBindParameter` BYTE insert (5000
bytes, forcing the sqli-layer's multi-chunk bind path from the ODBC
surface, not just directly against `SqliStatement`), a NULL TEXT bind
ahead of another parameter, chunked `SQLGetData` reassembly of both the
TEXT value (`SQL_C_CHAR`, 4-byte buffer, 31 calls) and the 5000-byte BYTE
value (`SQL_C_BINARY`, 7-byte buffer - deliberately not a divisor of 5000,
716 calls) - both byte-for-byte exact against the bound values -
`SQLBindCol` single-shot fetch with both a large-enough buffer (full
value) and a small one (correct truncation, full length still reported
via the indicator), and a NULL value correctly reported via `SQLGetData`'s
indicator (`SQL_NULL_DATA`).

Test fixture note: seeding real BYTE/TEXT row data for live testing turned
out to need `dbaccess`'s `LOAD FROM 'file' INSERT INTO table(col)` command,
not a plain `INSERT ... VALUES ('literal')` - Informix's interactive SQL
parser rejects a literal string for a BYTE/TEXT column outright (`-617 A
blob data type must be supplied within this context`), confirmed live; a
literal insert is only ever accepted via a host-language bind parameter
(exactly the SQ_BIND flow above), never as SQL text. This is why there's no
automated `ctest` integration test for simple-LOB fetch yet - fixture setup
currently depends on `dbaccess`/Docker, not just this driver's own code;
once bind is implemented, a self-contained integration test (seed via this
driver's own bind, verify via `fetchSimpleLob`) becomes possible without an
external dependency.

## Smart large objects (BLOB/CLOB) - fetch confirmed live

DESCRIBE reports smart LOB columns as `ifxType=41` - the same wire type
code `ifxtype::kBoolean` already uses for its own extended-type framing.
Confirmed live: a `BLOB` column reports `extendedId=10 owner="informix"
name="blob"`, a `CLOB` column reports `extendedId=11 owner="informix"
name="clob"`, both `encodedLength=72`. `SqliStatement`'s row splitter
(`splitRow` in `sqli/statement.cpp`) distinguishes the two ifxType-41 cases
by `ColumnDesc::extendedTypeName` ("blob"/"clob" vs "boolean").

The TUPLE row's per-column shape, confirmed live: `nullFlag`(1 byte) +
`length`(4 bytes, big-endian - confirmed live as `72`) + `length` bytes of
opaque payload = 77 bytes total when non-null (the same self-describing
null-flag-byte convention `kBoolean` uses, just with a 4-byte length field
where BOOLEAN has none). When null: `nullFlag`(1)=1 + `length`(4)=0, no
payload - 5 bytes total, confirmed live (not just 1 byte the way a naive
reading of "null means nothing follows" would suggest - `IfxBaseType`'s
`toIfxTuple()` always writes the length field, even for a null value whose
internal buffer was never populated). The 72-byte payload is an
`IfxLocator` per the decompiled reference (`IfxLocator.java`:
`IFX_LOCATOR_SIZE = 72`) - a token this driver never decodes, only
round-trips to the server (needed verbatim, byte-for-byte, by
`SqliConnection::fetchSmartLob` below).

**Getting the actual content needs a fastpath RPC subsystem** - a
materially larger mechanism than simple LOBs' two-opcode fetch, and the
reason smart LOBs were scoped as separate follow-on work. Per the
decompiled reference, `IfxLoOpen`/`IfxLoClose`/etc. aren't dedicated
opcodes at all - they're calls to built-in Informix SPL functions
(`informix.ifx_lo_open`, `informix.ifx_lo_close`, ...) resolved and
invoked generically:

- `SQ_GETROUTINE`(101): resolves a function signature string
  (e.g. `"function informix.ifx_lo_open(blob ,integer)"`, matched
  character-for-character against Informix's own function catalog) to a
  numeric routine handle. Confirmed live: the signature field uses a
  **4-byte** length prefix (`ByteWriter::ifxChar4`), not the 2-byte
  `ifxChar2` this driver uses almost everywhere else - a first attempt
  using `ifxChar2` hung the connection identically regardless of a
  trailing `SQ_EOT` being present or not, traced to
  `IfxDataOutputStream.getJavaToIfxCharBytes()` switching to a 4-byte
  length (`JavaToIfx4BytesChar`) whenever `isRemove64KLimitSupported()`
  (SQ_PROTOCOLS capability bit 62, byte[7] `& 0x02`) is true - true on
  every server tested so far, but **not yet plumbed into this driver as
  its own tracked capability flag** the way `has4ByteOffsets()`/bit 50 is;
  this is unconditional today (see `getRoutineHandle`'s doc comment in
  `sqli/connection.cpp`). The reply, in contrast, echoes the database name
  back via the plain 2-byte `ifxChar2` - a genuine, confirmed-live
  asymmetry between the two directions, not a mistake. Both the request
  and the reply are terminated by an explicit `SQ_EOT`, despite neither
  showing one in `IfxSqli.java`'s own `getHandle()`/`dispatchMsg()`
  source - the same "trust a live capture over an incomplete `send*`
  method" situation `SQ_PROTOCOLS` above already has.
- `SQ_EXFPROUTINE`(102): invokes a resolved handle with bound parameters,
  using the *exact same* `sendBind`-shaped `SQ_BIND`(5) sub-block ordinary
  prepared statements use (this driver's existing `writeBindBlock`
  encoding is reused as-is for parameter bodies) - confirmed live this is
  one combined write with a single trailing `SQ_EOT`, not two separate
  EOT-terminated writes (one after the EXFPROUTINE header, another after
  the bind block). Two parameter shapes this driver's fastpath calls need,
  both confirmed live against a real JDBC golden-capture trace (see
  below):
  - A plain `INTEGER` parameter (`ifxtype::kInteger`, no extended
    owner/name - below `sendBind`'s `type > 18` threshold): indicator
    `0`, **precision field `2560`** - confirmed live, and deliberately
    *not* this driver's own established plain-INTEGER `SQ_BIND` precision
    (`4`, used for ordinary column binds via `driver/bind_encode.cpp`) -
    a genuine asymmetry between fastpath and ordinary bind parameters,
    copied from the trace rather than assumed to match.
  - A UDT-wrapped locator parameter (`ifxtype::kUdt` = 44): extended
    owner is the **empty string**, not `"informix"` - confirmed live,
    unlike BOOLEAN's bind (which does use `"informix"`; see
    `ifxtype::kUdt`'s doc comment in `sqli/types.h`). Extended type name
    is `"blob"` (matching `informix.ifx_lo_open`'s own signature
    literally, regardless of whether the real column is BLOB or CLOB).
    The data payload is exactly the TUPLE row's own `[4-byte length][72
    locator bytes]` slice (leading null-flag byte stripped) - which is
    also, conveniently, exactly `IfxBaseType.toIfx()`'s own non-null
    bind-parameter encoding, confirmed live via the trace showing
    "Data (76 bytes)" (4 + 72) for this parameter - so no re-encoding is
    needed on the way in.
- `SQ_FPROUTINE`(103): the invocation's reply tag, carrying the return
  value(s) - confirmed live, `SQ_EOT`-terminated. `informix.ifx_lo_open`
  returns one plain `INTEGER` (the file descriptor); `informix.ifx_lo_close`
  also returns one (a status code this driver discards). Confirmed live:
  an `INTEGER` return's precision field is *also* `2560`, matching the
  equally-nonstandard bind-direction value, not the `4` an ordinary
  INTEGER column fetch would imply.
- `SQ_LODATA`(97): bulk read/write against an already-open smart LOB file
  descriptor. `LO_READ`(subcommand `0`) request: `u16(97)` + `u16(0)` +
  **`u16(fd)`** (a smallint on the wire, confirmed live, despite `fd`
  arriving from `SQ_FPROUTINE` as a full 4-byte int) + `u32(requested
  byte count)` + `u16(32000)` (a fixed `R_W_BUFSIZE` the golden trace
  showed unconditionally) + `SQ_EOT`. The requested byte count is just an
  upper bound, not a guess that has to be right: the reply's own
  `fileSize` field (a `u32` immediately after a `u16` optype) is
  authoritative, confirmed live - requesting a value comfortably larger
  than any real smart LOB (this driver uses `0x7FFFFFFF`) is safe. The
  chunk stream that follows is simple length-prefixed records (`u16`
  length + that many bytes, padded to even) read back-to-back until the
  reply's own `fileSize` total has been consumed - **no zero-length
  terminator chunk** the way simple LOBs' `SQ_BLOB` stream has one
  (confirmed live and matches the decompiled reference's
  `receiveLODATA`, which tracks a remaining-byte counter rather than
  watching for a sentinel); a trailing `SQ_EOT` follows the last chunk.
  `LO_WRITE`(subcommand `2`) isn't implemented yet (bind/write is
  deliberately out of scope for this first slice - see below); its reply
  shape (confirmed live via the JDBC trace, for when write support is
  added) is simpler: `u16` optype `2` + `u32` bytes-written, no chunk
  stream at all.

**Implementation**: `SqliConnection::fetchSmartLob` (`sqli/connection.cpp`)
does all three round trips (open, `LO_READ`, close) and returns the
assembled content. Confirmed live against this project's IDS 14.10 dev
container: a small single-chunk value, a 75000-byte value spanning
multiple `SQ_LODATA` chunks (byte-for-byte exact, both via a direct
`SqliStatement`/`fetchSmartLob` harness and through `tools/ifx_probe`),
and a NULL value (correctly detected via the TUPLE row's null-flag byte,
never reaching `fetchSmartLob` at all). `tools/ifx_probe`'s
`printColumnValue` dispatches to it for any ifxType-41 column whose
`extendedTypeName` is `"blob"`/`"clob"`.

**How this was found**: reverse-engineering the fastpath framing from the
decompiled source alone hit a real dead end - a first attempt at
`SQ_GETROUTINE` (2-byte signature length, no capability-gating
considered) hung regardless of a trailing `SQ_EOT`, and a follow-up
`SQ_EXFPROUTINE` attempt (correct per a literal reading of `sendBind`,
using this driver's existing plain-INTEGER precision of `4`) also hung -
`onstat -g sql` showed the server's session idle with no lock wait,
which turned out to be *uninformative* for fastpath calls (that column
only reflects SQL-statement lock waits, not a thread blocked in a raw
socket read, and fastpath isn't a SQL statement). Rather than keep
guessing field-by-field, the real IBM JDBC driver
(`jdbc-4.50.10.jar`) was run directly against the same dev container with
its own protocol tracing enabled (`TRACEFILE`/`TRACE=3` connection
properties, plus raising `java.util.logging`'s logger level - the
`TraceFlag` handler alone isn't enough, the `Logger` itself defaults to a
level that filters out `FINE`/`FINER` before the handler ever sees them)
doing a real `PreparedStatement.setBytes()` insert into a BLOB/CLOB
column followed by a `ResultSet.getBytes()`/`getString()` read-back. That
one capture (~17,000 log records) resolved every remaining framing
question in one pass - the 4-byte signature length, the no-owner UDT
param, the `2560` precision magic number, the exact `SQ_LODATA` chunk
loop - none of which would have been found reliably by further guess-and-
probe alone. Recorded here as a reusable technique: when a live attempt
hangs inexplicably and the decompiled source is ambiguous or apparently
contradicted by observed behavior (as it was for both the missing
`SQ_EOT`s and the `getJavaToIfxCharBytes` capability-gating), get a real
protocol trace from the vendor's own driver rather than continuing to
iterate on guessed field shapes.

**ODBC driver-layer fetch surface: confirmed live.** `driver/statement.cpp`'s
`writeSmartLobColumnValue` wires `SQLGetData`/`SQLBindCol` onto
`fetchSmartLob`, mirroring simple LOBs' own ODBC-layer wiring
(`writeSimpleLobColumnValue`) - both now share a `writeLobBytesFromState`
helper for the resumable-offset copy-into-buffer logic once content is in
memory, since that part is identical between the two LOB mechanisms even
though how `content` gets populated differs. `isSmartLobColumn` (checks
`ifxType == kBoolean` plus `extendedTypeName == "blob"/"clob"`) is the
single place that distinguishes a smart LOB column from a real BOOLEAN one
at the driver layer, consulted by `sqlTypeFor`/`columnSizeFor`
(`SQLDescribeCol`/`SQLColAttribute` report `SQL_LONGVARBINARY`/
`SQL_LONGVARCHAR`, matching simple LOBs' own type mapping) and by
`writeColumnValue`'s dispatch. Confirmed live through the real unixODBC
Driver Manager (`isql`, plus a direct `SQLGetData`-loop harness against
libodbc.so): a small BLOB/CLOB value, a NULL value reported correctly via
`SQLGetData`'s indicator, a 75000-byte value spanning 20 chunked
`SQLGetData` calls with a 4096-byte buffer (byte-exact against the source
file), and `SQLBindCol` with a deliberately undersized buffer (correct
`01004`/truncated-string contract).

**`SQLColumns` now reports BLOB/CLOB correctly too** (`driver/catalog.cpp`).
This one isn't optional the way a missing feature would be: `kTypeMap`/
`caseColumnSize`/`caseTypeName` are keyed by `basetype` alone, and BLOB/CLOB
share `basetype`(`ifxBaseType`) 41 with BOOLEAN - without a fix, `SQLColumns`
would have actively mislabeled every BLOB/CLOB column as `SQL_BIT`/
`"boolean"`/`COLUMN_SIZE=1` (confirmed live before the fix, via
`syscolumns`: both `b`/`c` test columns report `coltype=41` same as a real
BOOLEAN would). The discriminator at this layer is `syscolumns.extended_id`
(10=blob, 11=clob, confirmed live - same values `sysxtdtypes` reports for
their `name`/`owner` rows), added to `buildColumnsSql`'s inner derived
table alongside `basetype`/`nullable_flag`. `wrapSmartLob` wraps the
existing basetype-keyed CASE expressions for `DATA_TYPE`/`TYPE_NAME`/
`COLUMN_SIZE`/`SQL_DATA_TYPE` with two higher-priority branches so BLOB/CLOB
report `SQL_LONGVARBINARY`/`"blob"`/`SQL_LONGVARCHAR`/`"clob"` (matching
simple LOBs' own mapping) instead of falling through to BOOLEAN's. Confirmed
live via a direct `SQLColumns` call: `b` now reports
`DATA_TYPE=-4 TYPE_NAME=blob COLUMN_SIZE=2147483647`, `c` reports
`DATA_TYPE=-1 TYPE_NAME=clob COLUMN_SIZE=2147483647`. `SQLGetTypeInfo`
doesn't go through this file's catalog-query mechanism at all (it
synthesizes rows directly from `kTypeMap` in C++), so it still has no
BLOB/CLOB rows - a real, separate gap, not fixed here.

**Not yet implemented**: bind/write (`informix.ifx_lo_create`, `SQ_LODATA`
`LO_WRITE`) and the ODBC `SQLBindParameter` surface for BLOB/CLOB -
deliberately scoped as follow-on work, matching this project's established
fetch-first sequencing (simple LOBs, INTERVAL, DATETIME FRACTION all landed
fetch-then-bind-then-ODBC-surface in that order).

**Write-path framing: mostly decoded, one open question remains (the LO_SPEC
struct).** A second golden-trace session (same technique as the fetch path:
`jdbc-4.50.10.jar`, `TRACE=3`/`TRACEFILE` connection properties plus raised
`java.util.logging` level) captured a real `PreparedStatement.setBytes()`
insert into a BLOB column. This time the JDBC driver's own text trace omitted
byte counts for the one field that mattered (the type-`40` return value showed
no `Data (N bytes)` line at all, unlike every other field), so the capture was
redone through a small raw TCP proxy (`python3`, listening on 19088, forwarding
to the container's 9088 and hex-dumping both directions) so the actual wire
bytes could be inspected directly rather than trusting the driver's own
(incomplete) trace formatter.

- `informix.ifx_lo_create` resolves via `SQ_GETROUTINE` exactly like
  `ifx_lo_open`/`ifx_lo_close` (same 4-byte signature length, same DbName/
  handle reply shape) - signature confirmed live as
  `"function informix.ifx_lo_create( ifx_lo_spec,integer,blob)"`.
- **Bind shape (3 params, confirmed live byte-for-byte)**:
  1. `ifx_lo_spec` (UDT, `ifxtype::kUdt`=44, ExtendedOwner=`""`,
     ExtendedTypeName=`"ifx_lo_spec"`): a **600-byte** opaque creation-spec
     structure, no length prefix on the wire (its size is evidently a fixed,
     out-of-band constant for this UDT, the same way `blob`'s own 76-byte
     locator payload has none - confirmed live via exact byte-count
     reconciliation, see below). **Not yet reverse-engineered field-by-field.**
     The captured 600 bytes are mostly zero/`0xFFFFFFFF` (a common "use
     default" sentinel), but the target column's own sbspace name (`sbspace1`,
     this project's dev container) is embedded verbatim as an ASCII string
     inside the struct - meaning the captured bytes are almost certainly
     environment-specific, not safely reusable verbatim as a portable
     default across any table/sbspace. Whether a genuinely portable
     "use the column's own schema default" spec exists (e.g. an all-zero or
     all-sentinel 600 bytes with no explicit sbspace name) is unresolved and
     blocks a clean implementation - see the open question below.
  2. Plain `INTEGER` (mode flags): precision `2560` (the same fastpath-only
     precision convention `ifx_lo_open`'s mode parameter uses), value `8`
     (matches `fetchSmartLob`'s own `kLoOpenMode`).
  3. `blob`-typed UDT locator (`ExtendedOwner=""`, `ExtendedTypeName="blob"`,
     76-byte `[4-byte length=72][72-byte locator]` payload): a *template*
     locator, not a real one - unclear whether its content matters (worth
     testing with an all-zero payload before assuming it needs to be a real
     captured locator).
- **Return shape (2 values, confirmed live byte-for-byte)**: total message
  size 102 bytes reconciles exactly as opcode(2) + returnCount(2) + return#1
  (86) + return#2(10) + EOT(2) = 102.
  1. **Type `40`** (the "New locator" the JDBC trace's own text formatter
     failed to log any data for - the actual gap this session closed):
     `type(2)=40, ExtendedOwner len(2)=0, ExtendedTypeName len(2)=0,
     indicator(2)=0, precision(2)=0, dataLen(4)=72, data(72 bytes)` - i.e.
     **the exact same `[4-byte length][72-byte locator]` shape** `fetchSmartLob`
     already treats opaquely everywhere else in this driver, just wrapped in
     an extended-type return-value header instead of a TUPLE row's null-flag
     header. `ExtendedOwner`/`ExtendedTypeName` are genuinely empty here
     (confirmed live), unlike a plain UDT-typed row/bind value.
  2. Plain `INTEGER`: the file descriptor (confirmed live as `39` in this
     capture), same shape `receiveFpRoutineIntReturn` already decodes for
     `ifx_lo_open`'s single return value - i.e. `ifx_lo_create` is
     `receiveFpRoutineIntReturn` plus one preceding locator-shaped return,
     not a wholly new reply parser.
- **Locator stability, confirmed live**: the 72 locator bytes returned by
  `ifx_lo_create` are byte-for-byte identical to the 72 locator bytes later
  bound into the real `INSERT`'s blob column parameter, after the
  create/write/close sequence completes - `LO_WRITE` does not mutate the
  locator's own identity. Safe to hold onto the create-time locator bytes
  and bind them as-is once writing is done, no need to re-fetch or
  re-derive them afterward.
- **One real, confirmed-live asymmetry**: the *fastpath* locator bind
  parameter (`ifx_lo_open`/`ifx_lo_create`'s own template/input locator,
  `appendFastPathLocatorParam`'s existing convention) uses
  `ExtendedOwner=""`, but the **ordinary column bind** for the final
  `INSERT`'s blob parameter uses `ExtendedOwner="informix"` - matching
  BOOLEAN's own bind convention, not the fastpath one. These are two
  different bind contexts with two different owner conventions; don't
  conflate them.
- **`SQ_LODATA` `LO_WRITE`, confirmed live, request and reply**:
  - Request: `u16(97 SQ_LODATA) + u16(2 LO_WRITE) + u16(fd) + u32(total bytes
    to write) + u16(32000 R_W_BUFSIZE) + u16(chunk length) + chunk bytes +
    ... + SQ_EOT` - the same length-prefixed-chunk shape `LO_READ`'s reply
    uses, just in the request direction and with an extra total-byte-count
    field up front (mirroring `LO_READ`'s own "declare an upper bound"
    field). Only a single, small (26-byte) chunk was exercised live; larger
    values presumably need multiple chunks summing to the declared total,
    by analogy with `LO_READ`'s reply, but that's extrapolation, not yet
    confirmed for a real multi-chunk write.
  - Reply: `u16(97) + u16(2) + u32(bytes written) + SQ_EOT` - exactly the
    shape already predicted (without live confirmation) in the previous
    revision of this note, now confirmed live.

**Open question blocking a clean implementation**: whether a portable
(non-environment-specific) `ifx_lo_spec` exists. Two ways to resolve it,
neither attempted yet:
1. Cheap: capture a third golden trace against a table with no `PUT ... IN`
   clause (system-default sbspace) and check whether the spec's sbspace
   field comes back empty/sentinel rather than a real name.
2. Definitive: a C++ probe that first replays the captured 600 bytes
   verbatim (guaranteed valid, since it's exactly what the real JDBC driver
   sent) to prove out the create/write/close sequence end-to-end from this
   driver's own code, then bisects individual fields toward zero/sentinel
   values to find the minimal spec that's still accepted.
Shipping the captured 600 bytes verbatim (hardcoding `sbspace1`) would only
work against this exact dev container and was deliberately not done.

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

**ODBC-layer wiring** (`SQLEndTran`/`SQL_ATTR_AUTOCOMMIT`, `driver/`, not the
protocol layer above): confirmed live through the real Driver Manager
(`tools/odbc_dm_probe`'s `runTransactionTest`, run via WSL2/unixODBC - the
native Windows DM refuses to load this driver at all without registry
registration, a separate, pre-existing, unrelated gap - see `STATUS.md`).
`driver/statement.cpp`'s `doExecute` lazily issues `SQ_BEGIN` on the first
statement after `SQL_ATTR_AUTOCOMMIT` goes off; `SQLEndTran` calls the
matching commit/rollback; turning autocommit back on commits whatever's
open; `SQLDisconnect` rolls back anything left unresolved. One specific
question worth recording since it wasn't obvious which way it would go:
**a commit does not close an already-open SELECT cursor** on this server -
this driver never declares cursors `WITH HOLD`, so a server-side close on
commit was plausible (and would have desynced the next fetch the same way
this project's other framing bugs have), but fetching more rows after
`SQLEndTran(SQL_COMMIT)` on a statement with an open cursor keeps working
with no client-side state reset needed. Getting this far through
unixODBC's Driver Manager also required two unrelated fixes:
`driver/info.cpp`'s `SQLGetFunctions` had never listed `SQL_API_SQLENDTRAN`
as implemented, and `SQLGetInfo(SQL_TXN_CAPABLE)` still reported
`SQL_TC_NONE` from before any transaction support existed - unixODBC
checks both before it will even attempt to call `SQLEndTran`, returning
`IM001` itself without reaching this driver's code at all otherwise.

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

## Catalog functions

**Status: SQLTables, SQLColumns, and SQLGetTypeInfo all done and confirmed
live** (`driver/catalog.cpp`) - `tools/odbc_dm_probe`'s `runCatalogTest`/
`runColumnsTest`/`runTypeInfoTest` through the real Driver Manager, plus
`isql`'s `help` (all tables/views in a live multi-table database, correct
TABLE_TYPE per row) and `help <table>` (correct DATA_TYPE/TYPE_NAME/
COLUMN_SIZE/DECIMAL_DIGITS/NULLABLE/IS_NULLABLE per column, including a
`SERIAL` column correctly reported as `integer`/NOT NULL). SQLGetTypeInfo
has no isql-native invocation path, so `odbc_dm_probe` is its verification
bar rather than an `isql` command.

This isn't a new protocol-level mechanism - it's a SQL query synthesized
against `systables` and run through the exact same prepare/execute/fetch
pipeline (`SqliStatement`) every other statement uses, so nothing here
needed new wire-level work. What's worth recording is Informix SQL
behavior discovered along the way, not SQLI protocol behavior:

- `systables.owner` is a space-padded fixed-width `CHAR` column, not
  `VARCHAR` - confirmed live via `owner || '|'` showing padding before the
  pipe. `systables.tabname` is not padded (already `VARCHAR`).
- A `CASE` expression's result type is inferred from its widest branch and
  padded like any other fixed-width `CHAR` - the `CASE` computing
  TABLE_TYPE (`'TABLE'`/`'VIEW'`/`'SYSTEM TABLE'`) came back padded to 12
  characters (`'SYSTEM TABLE'`'s width) for shorter results, confirmed the
  same way. Both are worked around with `trim()` in the generated SQL
  rather than trimmed in C++ after the fact, since the padding is
  Informix's doing, not this driver's decode layer doing anything wrong.
- `tabid <= 99` reliably identifies Informix's own system catalog tables
  (owner `informix`, `tabtype` `'T'`) as distinct from ordinary user
  tables with the same `tabtype` - confirmed live across `systables`
  (`tabid`=1) through `sysxtdtypes` and friends. A few internal pseudo-rows
  (`" GL_COLLATE"`, `" GL_CTYPE"`, `" VERSION"`) carry a blank `tabtype`
  and are excluded by the `tabtype in ('T', 'V')` filter rather than
  surfaced as a table of unknown type.
- Informix supports derived tables (`FROM (SELECT ...) alias`) - needed
  because TABLE_TYPE is computed by the inner query and then filtered on
  by name in the outer one; a single-level `SELECT` can't reference its
  own column alias in its own `WHERE` clause.
- Catalog reads deliberately don't open a transaction of their own (no
  lazy `SQ_BEGIN`, unlike `driver/statement.cpp`'s `doExecute` for
  DML/SELECT) - confirmed live to work the same under
  `SQL_ATTR_AUTOCOMMIT_OFF` as under the default `ON`.
- `syscolumns.coltype` packs the NOT NULL flag as bit `0x100` over the base
  type - confirmed live (`262` for a `SERIAL NOT NULL` column = `6` +
  `256`), the same encoding already relied on at the wire level (see the
  Scalar type wire encodings section above). `syscolumns.collength` packs
  `(precision << 8) | scale` for `DECIMAL` (confirmed live: `2562` for
  `DECIMAL(10,2)` = `10*256 + 2`) and a qualifier value for `DATETIME`
  (confirmed live: `3594` for `YEAR TO SECOND` = `(14 << 8) | 0x0A`,
  matching `driver/bind_encode.cpp`'s `kYearToSecond` exactly) - both the
  same packed-field conventions this driver's wire-level code already
  decodes, not something newly discovered about the protocol itself.
- Informix's arithmetic functions (`trunc()`, `mod()`) return `DECIMAL`,
  not `INTEGER`, even when both arguments are integers - and a `CASE`
  expression mixing an integer-literal branch with a `trunc()`/`mod()`
  branch gets typed `DECIMAL` for *all* branches, not just that one.
  Confirmed live the hard way: a `SQLColumns` column computed this way
  came back as `07006` ("restricted data type attribute violation") on
  `SQL_C_SSHORT`/`SQL_C_SLONG` fetches - the same `DECIMAL`-vs-integer-
  family gap `count(*)` hit earlier (see the Transactions section's
  `countRows` note) - even though the *value* was correct. Worked around
  with an explicit `cast(... as integer)` around the whole `CASE`, not
  just the offending branch.
- A `UNION ALL`-specific variant of the `CASE`-padding quirk above: a
  string column built from `UNION ALL`ing several differently-shaped
  string literals gets coerced to a fixed `CHAR` sized to the widest
  branch and blank-padded, same as `CASE` - but unlike `CASE`, wrapping
  the result in `trim()` does **not** strip it back out, even reading from
  an outer query against a derived table built from the union; the value
  comes back genuinely padded, not just displayed that way. This was hit
  building `SQLGetTypeInfo`'s type list, originally implemented as a
  `UNION ALL` of one literal row per type. Casting each literal to
  `lvarchar` (rather than `varchar`) does dodge the padding - Informix
  keeps it as a true variable-length value through the union - but
  `lvarchar` is a wire type this driver's fetch/decode path has never
  implemented, and returning one desyncs the byte stream and hangs the
  connection forever, exactly as the framing-bug warning at the top of
  this document describes (confirmed live: a lone `SQLGetTypeInfo` call
  with no other statement involved hung inside its own `SQLFetch` loop the
  moment a returned column was `lvarchar`-typed - not a DM-interaction
  issue, reproduced with nothing else on the connection). The actual fix
  was structural, not a cast: `SQLGetTypeInfo` was rewritten to use the
  same derived-table-plus-outer-`CASE` shape `SQLTables`/`SQLColumns`
  already use, ranking real `systables` rows (`tabid <= 99`) via a
  correlated-subquery row count rather than hardcoding tabid values or
  using a window function, and dispatching on that rank the same way
  `basetype` is dispatched on elsewhere in this file. No `UNION ALL` of
  string literals, no `lvarchar`, no hang.

Scope: the plain filtering case (`isql help`/`help <table>` and ordinary
schema browsing) - catalog/schema/table-name/column-name filtering via
`LIKE`, table-type filtering via an `IN` list. Not implemented: the ODBC
"special value" enumeration modes (`CatalogName="%"` alone to list
catalogs, `SchemaName="%"` alone to list schemas) and `COLUMN_DEF`
(always `NULL`, no default-value introspection) - see
`driver/catalog.cpp`'s top comment for why these are deliberate,
documented gaps rather than oversights.

**INTERVAL in `SQLColumns`** (`driver/catalog.cpp`'s `IntervalShapeEntry`/
`intervalCase*` helpers) needed its own dispatch, structurally different
from every other type's flat `kTypeMap` lookup: `syscolumns.coltype`
reports the *same* base type code (14) for every INTERVAL column
regardless of qualifier, but ODBC expects a distinct `DATA_TYPE` per
qualifier *shape* (`SQL_INTERVAL_YEAR` vs `SQL_INTERVAL_DAY_TO_SECOND`,
13 shapes total across both interval classes). `collength` packs the same
`(totalDigits << 8) | qualifierByte` qualifier `sqli::decodeIntervalYearMonth`/
`decodeIntervalDayFraction` already decode at the wire level - confirmed
live via a probe table (`INTERVAL YEAR(4) TO MONTH`/`DAY(4) TO
FRACTION(3)`/`MONTH(2) TO MONTH` produced `collength` 1538/3405/546,
matching `(6<<8)|0x02`/`(13<<8)|0x4D`/`(2<<8)|0x22` exactly) - so
`buildColumnsSql`'s inner derived table decodes `start`/`end`/
`leadingWidth`/`fractionDigits` from `collength` once (the same per-field
arithmetic as the sqli-layer decoders, reconstructed in SQL rather than
shared, since this file has no dependency on `sqli/types.cpp`'s internal
implementation), and every outer `CASE` (`DATA_TYPE`, `TYPE_NAME`,
`SQL_DATA_TYPE`, `SQL_DATETIME_SUB`, `COLUMN_SIZE`, `DECIMAL_DIGITS`)
dispatches on those decoded aliases instead of `basetype` directly.
FRACTION(n) end codes collapse to plain SECOND for this dispatch - ODBC
has no INTERVAL data type distinguishing "TO SECOND" from
"TO FRACTION(n)"; the fractional-seconds precision goes in
`DECIMAL_DIGITS` instead, matching how ODBC's native interval C types
already carry it.

`DATA_TYPE` uses the *positive* `SQL_INTERVAL_*` constants (101-113,
`100 + SQL_CODE_*`), not the negative ODBC-2.x-era values also named
`SQL_INTERVAL_*` in `sqlext.h` - both exist in the header behind an
`#if (ODBCVER >= 0x0300)` branch, and rather than trust which one wins
from memory, a throwaway probe was actually compiled against this
project's toolchain: `ODBCVER` defaults to `0x0380` on this SDK (nothing
in this project overrides it), so the positive branch is what a caller
actually receives.

`COLUMN_SIZE` reports a character *display-size* bound (matching this
file's existing `DATE`(10)/`DATETIME`(19) convention), not the leading
field's own declared digit width alone - confirmed via `sqlext.h` that
leading precision has its own separate ODBC descriptor field
(`SQL_DESC_DATETIME_INTERVAL_PRECISION`, 26), so reporting it as
`COLUMN_SIZE` would both be off-model and risk truncating a `SQL_C_CHAR`
fetch through this driver's own `formatIntervalYearMonth`/
`formatIntervalDayFraction` text surface. Each qualifier shape's
`COLUMN_SIZE` is leading-field width plus a shape-specific constant
(punctuation and fixed-width subfields, e.g. YEAR-MONTH's `+4` for
sign+dash+2-digit-month) plus a FRACTION suffix if present - all four
qualifier shapes captured above were confirmed live to produce the
expected value (8/18/6/12 respectively, the last for a fifth probe column,
`INTERVAL HOUR(5) TO SECOND`). Like `DATETIME`'s own approximate 19, this
is a reasoned upper bound (never smaller than what this driver's own text
formatters can produce for that shape and leading width), not
independently confirmed against a real ODBC client that actually consumes
`COLUMN_SIZE`.

Verified by generating the SQL via `buildColumnsSql` and running it
directly against the live server through `SqliStatement` (same technique
as this project's no-live-DM-connection verification elsewhere) - not yet
through the real Driver Manager, which needs the Linux/WSL2 path not set
up in this environment (see the Windows DM gap noted elsewhere in this
document). Non-INTERVAL columns were re-checked the same way to confirm no
regression to the existing `kTypeMap` dispatch.

**Deliberately not extended to `SQLGetTypeInfo`**: that function's
`kTypeMap`-driven design emits exactly one row per type, and INTERVAL's 13
qualifier shapes don't fit that shape without a larger restructure -
nothing in this project's verification bar (no isql-native invocation
path for `SQLGetTypeInfo`, and the real Driver Manager isn't reachable in
this environment) currently exercises it, so it's an open, documented gap
rather than an oversight.

**A bug found while deriving `COLUMN_SIZE`'s formula, now fixed**:
`driver/convert.cpp`'s `formatIntervalDayFraction` (the `SQL_C_CHAR` text
surface, shipped in the DAY-FRACTION decode commit) originally only
received `hasDayField`, not the qualifier's actual start/end reach. Its
`!hasDayField` branch always fed `iv.hour` as the first printed field
regardless of which field is actually the qualifier's leader - a
MINUTE-anchored or SECOND-anchored qualifier (`MINUTE TO SECOND`,
`SECOND TO SECOND`) printed the true leading value stranded in the wrong
slot (e.g. `0:04:05` instead of a correct minute-leading rendering) rather
than in the first slot - and it always printed the full `HH:MM:SS` shape
regardless of whether the qualifier actually reached that far (e.g.
`HOUR TO MINUTE` showed a spurious `:00` seconds field). Both stemmed from
the same root cause: the formatter needed the qualifier's start *and* end,
not just `hasDayField`. `sqli::decodeIntervalDayFraction` itself was
unaffected (its own field-by-field decode already handles
MINUTE/SECOND-anchored qualifiers correctly, per its own doc comment) -
this was purely a `driver/convert.cpp` text-rendering gap.

Fixed by passing `start`/`wholeEnd` (the same effective-end-with-FRACTION-
collapsed-to-SECOND convention `driver/catalog.cpp`'s INTERVAL dispatch
also uses) into the formatter, which now walks the same start..wholeEnd
field sequence `decodeIntervalDayFraction` itself does: the leading field
(whichever of day/hour/minute/second `start` names) prints unpadded with
the sign, each subsequent field in `{HOUR, MINUTE, SECOND}` up to
`wholeEnd` prints as a zero-padded `:NN` suffix (space instead of colon
for the separator immediately after a DAY leading field, matching
Informix's own `D HH:MM:SS` display), and nothing prints past `wholeEnd`.
`tools/ifx_probe`'s own separate (and more broken - it unconditionally
used `iv.day`, always 0, regardless of qualifier) inline copy of this
logic got the identical fix.

Confirmed live via a probe table (`fmt_probe`) covering the four
previously-unconfirmed leading-field/qualifier-reach combinations plus a
FRACTION and a negative case: `HOUR(3) TO HOUR` -> `123`; `HOUR(3) TO
MINUTE` -> `123:45`; `MINUTE(3) TO MINUTE` -> `123` (previously would have
used `iv.hour`, always 0, as the leader); `MINUTE(3) TO SECOND` ->
`123:45`; `SECOND(3) TO SECOND` -> `123`; `SECOND(2) TO FRACTION(3)` ->
`45.678`; negative `MINUTE(3) TO SECOND` (`-05:09`) -> `-5:09`. The same
six wire captures also became new `sqli::decodeIntervalDayFraction` unit
tests (`tests/sqli/test_types.cpp`'s `INTERVAL DAY TO FRACTION` test),
closing the "HOUR/MINUTE/SECOND-anchored leading fields... aren't
independently live-tested" gap this class's decode side had carried since
the original DAY-FRACTION commit - only `HOUR TO SECOND` (which happens to
reach all the way to SECOND, so the old formatter bug didn't manifest for
that specific shape) was previously confirmed. `driver/convert.cpp`'s own
fixed output was checked directly against these same captured bytes
without a live connection (see this document's Windows ODBC DM note for
why), not yet independently confirmed through the real Driver Manager.

## Open questions / risks

- Whether `has4ByteOffsets()`'s constituent capability bits (see "Narrow vs. wide
  framing" above) ever diverge on a real server — only tested against one engine
  (Docker `ibmcom/informix-developer-database`, IDS 14.10.FC7W1DE) where they all
  happen to co-occur as true. Revisit if a server is found where DESCRIBE and
  TUPLE/FETCH negotiate differently.
- DATETIME FRACTION (sub-second precision) is now supported (`YEAR TO
  FRACTION(5)` confirmed live, decode and bind - see the DATETIME(qualifier)
  section above). INTERVAL is now supported on the fetch side for both
  classes, YEAR-MONTH and DAY-FRACTION, and at the sqli bind-encoder layer
  for both classes and both signs too (see the INTERVAL(qualifier) and
  INTERVAL SQ_BIND encoding sections above) - what remains open is wiring
  INTERVAL binds to the ODBC `SQLBindParameter` surface, which needs
  `ColumnSize`/`DecimalDigits` plumbed through in a way none of the other
  bind types so far have required.
- DECIMAL/DATETIME qualifiers outside what's been directly tested (precision
  other than 10, qualifiers other than YEAR TO SECOND/YEAR TO FRACTION(5))
  are handled by a general formula derived from - and consistent with - the
  tested cases, but aren't independently confirmed against a live capture
  for those specific
  parameter values.
