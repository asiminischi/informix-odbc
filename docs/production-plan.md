# Production-grade development plan

This is the forward-looking plan for taking `informix-odbc` from its current
state (a working prototype, verified live end-to-end for a narrow slice of
ODBC, see `STATUS.md`) to something a real application could depend on in
production. It picks up where the original phased plan (Phases 0 to 5,
summarized in `STATUS.md`) left off.

## What "production-grade" means for this driver

A driver earns that label when all of the following are true, not just the
happy-path query-a-table case that's verified today:

1. **Functionally complete for ordinary application use**: transactions,
   catalog introspection (`SQLTables`/`SQLColumns`/...), the full MVP scalar
   type set bindable as parameters (not just fetchable), and Unicode text.
2. **Installable without hand-editing the registry or guessing flags.** A
   user on either platform can get from "downloaded a release" to "connected"
   using documented, scripted steps, not a developer's personal build
   environment.
3. **Safe under real application usage patterns**: concurrent handles from
   multiple threads, long-lived connections, connection pooling where the
   host application expects it, no server-side handle leaks.
4. **Secure by default for the connections it's likely to be used for**: SSL
   in transit, PAM (or at least a documented auth story beyond plaintext
   username/password) for anything beyond a local dev database.
5. **Debuggable in the field**: a real diagnostics/tracing story, not just
   "add prints and rebuild," and a SQLSTATE table broad enough that an
   application's generic error handling actually works.
6. **Tested at conformance level**, not just against this project's own two
   probe tools: a real ODBC test suite (or a substantial hand-built
   equivalent) run against multiple Informix server versions, plus the
   existing live-server integration tests kept current as coverage grows.

Each phase below has a concrete scope, deliverables, and a verification bar
modeled on the discipline the existing phases used: every phase's "done" bar
requires a real round trip against a live server or a real Driver Manager,
not compile-only or unit-test-only completion.

## Phase 6: Transactions

**Scope**: `SQLEndTran` (commit/rollback), `SQL_ATTR_AUTOCOMMIT` actually
enforced (today it's stored but not acted on), `SQ_BEGIN`/`SQ_CMMTWORK`/
`SQ_RBWORK` wired into `sqli/connection.cpp`.

**Deliverables**:
- `SqliConnection` gains begin/commit/rollback methods over the confirmed
  opcodes.
- `SQLEndTran` maps to them; `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, ...)`
  actually toggles whether each statement executes inside an implicit
  transaction or requires an explicit one.
- Statement/connection cleanup (`SQLDisconnect`, handle destructors) rolls
  back any open transaction rather than leaving server-side state
  ambiguous.

**Verification**: a live test that inserts a row, rolls back, and confirms
the row is absent; inserts a row, commits, and confirms it's visible on a
second connection. Also confirms the Phase 4 slice 3 finding (DROP TABLE
after UPDATE could lock within a session) is now fully explained and
resolved by explicit transaction boundaries, not just the statement-release
fix already in place.

**Why first**: every other phase's own testing gets easier and more
realistic once tests can roll back instead of accumulating fixture data,
and it's the single most-expected piece of ODBC behavior that's currently
entirely absent.

## Phase 7: Catalog functions

**Scope**: `SQLTables`, `SQLColumns`, `SQLGetTypeInfo`, `SQLPrimaryKeys`,
`SQLStatistics`, `SQLSpecialColumns` (roughly that priority order, matching
what schema-introspecting tools ask for first).

**Deliverables**: each maps to queries against Informix's system catalog
(`systables`, `syscolumns`, `sysindexes`, `sysconstraints`, etc.) and returns
a properly-shaped ODBC result set (fixed column names/types per the ODBC
spec) rather than a passthrough query result.

**Verification**: `isql`'s `help` and `help <table>` commands (which call
`SQLTables`/`SQLColumns` under the hood) return correct output against a
live server with a nontrivial schema (multiple tables, a primary key, an
index). A generic ODBC schema browser (e.g. DBeaver via unixODBC, if
available) can enumerate the dev database's tables and columns end to end.

## Phase 8: Full scalar type and bind-parameter coverage

**Scope**: close the "1 of 8 MVP types bindable" gap (`STATUS.md` item 3),
plus the scalar types never implemented at all: FLOAT/SMALLFLOAT, BOOLEAN,
INTERVAL, LOBs (BLOB/CLOB/BYTE/TEXT), and DATETIME with fractional
(sub-second) qualifiers.

**Deliverables**:
- `driver/bind_encode.cpp` gains an encoder per remaining MVP type
  (SMALLINT/BIGINT/CHAR/VARCHAR/DATE are straightforward; DECIMAL/DATETIME
  need their own BIND-specific wire encoding, distinct from their fetch
  encoding, per the open note in `docs/protocol-notes.md`).
- `sqli/types.h`/`.cpp` gains decode/encode support for FLOAT/SMALLFLOAT,
  BOOLEAN, INTERVAL, and fractional DATETIME, each with the same
  "confirmed live against a dedicated fixture table" discipline the
  existing 8 types used, since none of these have been captured from a
  live server yet.
- LOB support: streaming `SQLGetData`/`SQLPutData` for large values (LOBs
  aren't just "a bigger VARCHAR"; Informix's smart-blob mechanism is a
  materially different wire protocol, most similar in spirit to the
  decompiled JDBC driver's `IfxSmartBlob`/`IfxLobDescriptor` machinery,
  reimplemented from scratch per this project's existing rule against
  transliterating the decompile).
- `SQLNumParams`/`SQLDescribeParam` for parameter metadata introspection
  (currently absent).

**Verification**: a live round-trip test per new type, following the same
pattern as `tests/integration/test_type_probe.cpp` (a fixture table with a
positive, negative-if-applicable, and NULL row, fetched and asserted through
the real prepare-execute-fetch path), plus a bind-parameter round trip for
every newly-bindable type.

## Phase 9: Driver registration and packaging

**Scope**: close `STATUS.md` item 5 (no Windows registration) and item 11
(no installer for either platform).

**Deliverables**:
- Windows: an install helper (script or small installer) that writes the
  correct `HKLM\SOFTWARE\ODBC\ODBCINST.INI` entries so the driver is
  selectable from the standard "ODBC Data Source Administrator" UI and
  loadable via `DRIVER={name}` without a hardcoded path. Document the
  admin-elevation requirement plainly rather than trying to silently work
  around it.
- Linux: `odbcinst.ini`/`odbc.ini` templates plus a documented
  `odbcinst -i` registration flow, packaged as part of a `.deb` (or
  equivalent) alongside the `.so`.
- A versioned release process: the driver already exposes a version via
  `sqli/version.h`; wire that into actual tagged releases with build
  artifacts attached, rather than "clone and build yourself" being the only
  distribution mechanism.

**Verification**: on a clean Windows VM with no prior build tooling, running
the installer and then connecting via the Windows ODBC Data Source
Administrator (not just `DRIVER={path}`) succeeds. On a clean Linux VM,
installing the package and running `isql` against a DSN name (not a raw
`DRIVER={path}` string) succeeds.

## Phase 10: Unicode support

**Scope**: `SQLWCHAR`/`W`-suffixed function variants (`SQLDriverConnectW`,
`SQLFetch`+`SQL_C_WCHAR`, etc.), correct behavior across the Windows
(`SQLWCHAR` = 2 bytes, UTF-16) versus unixODBC (`SQLWCHAR` = 4 bytes by
default, UCS-4/UTF-32-ish depending on build) platform difference flagged as
a known trap back in the original plan.

**Deliverables**: a conversion layer between Informix's NVARCHAR/NCHAR wire
encoding and each platform's native wide-character width, exercised by both
the `A` and `W` entry points sharing one internal implementation (same
pattern already used to avoid the ELF-interposition bug: no exported
function calling another exported function directly).

**Verification**: round-trip a non-ASCII string (e.g. a UTF-8 string with
multi-byte characters) through an NVARCHAR column via both the `A` (with a
UTF-8 host application) and `W` entry points, on both platforms.

## Phase 11: SSL and PAM authentication

**Scope**: close `STATUS.md` item 9. Connect over the SSL/`drsoctcp` port
already reserved in `docker/docker-compose.yml` (9089, unused today); PAM
challenge/response per the decompiled driver's `IfmxPAM`/`IfxPAMChallenge`
shape (reimplemented from scratch, per this project's standing rule).

**Deliverables**: `sqli/transport.*` gains a TLS-wrapped socket mode;
`ConnectParams` gains SSL/PAM options; connection string keywords
(`SSLMODE`, or similar, TBD against what other Informix ODBC/JDBC drivers
conventionally use) select the mode.

**Verification**: live connect over port 9089 with SSL enabled, confirmed
via packet capture that the handshake is actually encrypted (not just that
the connect succeeds, since a silent fallback to plaintext would be a real
security bug, not a passing test).

## Phase 12: Concurrency and connection pooling

**Scope**: close `STATUS.md` items 10 and 12.

**Deliverables**:
- An explicit concurrency model decision (most ODBC drivers serialize
  access per-connection-handle via a mutex, since ODBC itself doesn't
  require handles to be thread-safe across threads without
  `SQL_ATTR_CONNECTION_POOLING`-style coordination; document whichever
  model is chosen and why).
- Connection pooling support (`SQLSetEnvAttr(SQL_ATTR_CONNECTION_POOLING)`)
  if the host application expects the driver to participate, or clear
  documentation that pooling should be handled by the Driver Manager
  layer instead, whichever is the correct division of responsibility for
  this DM/driver pair.

**Verification**: a stress test with multiple threads issuing concurrent
queries on separate connection handles against the live server, run under
a thread sanitizer (`-fsanitize=thread`) with zero reported races.

## Phase 13: Observability and release engineering

**Scope**: everything needed to operate this driver in someone else's
production environment, not just to demo it.

**Deliverables**:
- Structured logging/tracing hook (many ODBC drivers support an
  `SQL_ATTR_TRACE`/log-file mechanism DBAs rely on for support tickets).
- CI: automated build and test on both platforms (the existing test suite
  already runs cleanly on both; wire it into an actual CI pipeline against
  a containerized Informix instance, using the same `docker-compose.yml`
  already in the repo).
- Expand `SQLGetInfo`/`SQLGetFunctions` coverage against a real ODBC
  conformance/capability trace (`STATUS.md`'s note that the current set is
  "whatever `isql` happened to need," which is a real gap for other ODBC
  clients with different capability-detection needs).

**Verification**: CI passes on a clean checkout with no local developer
state; a support-style scenario (reproduce a failing query with tracing
enabled) produces a log an engineer could actually use.

## Phase 14: Conformance and hardening pass

**Scope**: the pass that actually earns the "production-grade" label, after
every functional phase above is done.

**Deliverables**:
- Run against multiple Informix server versions (not just the one Docker
  image used throughout development), documenting any version-specific
  wire-protocol differences discovered.
- Fuzz/malformed-input testing against the driver's own parsing (connection
  strings, SQL text handling, wire-response parsing) to catch crashes on
  untrusted or corrupted input, not just the happy-path server responses
  tested so far.
- A documented, tested upgrade/compatibility policy for future SQLI
  protocol or Informix server changes.

**Verification**: a fuzz run with no crashes over a fixed corpus/time
budget; successful connect and basic query round trip against at least one
additional Informix server version beyond the Docker dev image.

## Sequencing notes

Phases 6 to 8 (transactions, catalog functions, type/bind coverage) are the
functional core and should land before the packaging/security/concurrency
phases, since those later phases are much easier to test meaningfully once
the driver can do more than SELECT and single-column INSERT. Phase 9
(packaging) is deliberately sequenced before Unicode/SSL/pooling because it
unblocks real-world testing on machines that aren't the primary development
box, which is exactly the kind of testing the later phases need. Phase 14
(conformance/hardening) is last because it depends on everything else being
functionally complete first.

Every phase inherits the existing project discipline: no phase is
"done" on unit tests or a compile-only checkpoint alone; each needs a real
round trip against a live Informix server, and anything learned from the
decompiled JDBC reference stays a research aid only, never transliterated
into this project's own from-scratch C++.
