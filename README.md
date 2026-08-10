# informix-odbc

A from-scratch, cross-platform (Windows + Linux) ODBC driver for IBM Informix,
written in C++26. IBM does not ship a native ODBC driver for Informix (only
JDBC), so this implements Informix's proprietary "SQLI" wire protocol directly.

See `docs/protocol-notes.md` for wire-format research notes (written from
scratch, not derived from any vendor source code).

## Status

**Working prototype, not yet production-grade.** Connect, prepare, execute,
fetch, and single-column bind parameters all work end-to-end and are verified
live against a real Informix server, through the real ODBC Driver Manager
(not just unit tests). Transactions, catalog functions
(`SQLTables`/`SQLColumns`), most bind-parameter types, Unicode, and Windows
driver registration are not implemented yet.

See [`STATUS.md`](STATUS.md) for the full status report: what's done,
what's missing, and how it was verified. See
[`docs/production-plan.md`](docs/production-plan.md) for the phased plan to
close the gaps.

When you're done testing, `docker compose down` (from `docker/`) stops the
dev container.

## Layout

- `sqli/`: the SQLI wire protocol implementation (transport, handshake,
  statement protocol, type codecs). No ODBC dependency.
- `driver/`: the ODBC driver surface (`SQL*` entry points), built on `sqli/`.
- `tools/ifx_probe/`: a small CLI for exercising the protocol layer directly,
  without going through ODBC.
- `tools/odbc_dm_probe/`: a CLI that links against the *real* platform ODBC
  Driver Manager, to prove the driver actually works through the standard
  `SQLAllocHandle`/`SQLDriverConnect`/... path an app would use.
- `tests/`: unit tests (codecs) and integration tests (against a live
  Informix instance).
- `docker/`: `docker-compose.yml` for a local dev Informix instance; see
  `docker/README.md` for confirmed connection details.

## Quickstart: run it yourself (no prior C++/ODBC experience needed)

This walks through building the driver and proving it actually talks to a
real Informix server, in three levels of "how real do you want the test to
be." Every command below was run and confirmed working as part of writing
this doc.

### 0. Prerequisites

- `clang++` with C++26 support, `cmake` >= 3.28, `ninja`, and Docker.
- On Windows you additionally need the Windows SDK (for `sql.h`/`sqlext.h`,
  usually already present if you have Visual Studio Build Tools installed).
- On Linux, you need `unixodbc-dev` (`sudo apt install unixodbc unixodbc-dev
  odbcinst`) for the ODBC headers and the `isql` CLI test tool used below.

### 1. Start a local Informix server

```
cd docker
docker compose up -d
```

First boot takes a few minutes (it's building `sysmaster`/`sysadmin`/etc. from
scratch). Watch for it to come up:

```
docker exec ifxodbc_dev bash -lc 'onstat -'
```

Wait until you see `-- On-Line --`. (The container's own Docker healthcheck
is flaky and can report "unhealthy" even once the server is actually online;
trust `onstat -`, not `docker ps`.) Connection details for this dev instance
(baked into the image, dev-only): host `localhost`, port `9088`, server name
`informix`, user `informix`, password `in4mix`. See `docker/README.md` for
more.

### 2. Build the driver

```
cmake -G Ninja -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

This builds the shared library (`build/driver/ifxodbc.dll` on Windows,
`build/driver/libifxodbc.so` on Linux), the two CLI probe tools, and the unit
tests.

### 3. Run the unit tests (no server needed)

```
ctest --test-dir build --output-on-failure
```

Should print `100% tests passed` (12 tests: handshake encoding plus all 8
scalar type codecs, run against frozen known-good byte captures).

### 4. Smoke-test against the live server, protocol-level (simplest, works on any platform)

`tools/ifx_probe` talks the wire protocol directly, no ODBC layer, no Driver
Manager, so it's the fastest way to confirm the driver can actually reach and
query your server:

```
./build/tools/ifx_probe/ifx_probe host port user password [servername] [database] [sql]
```

Example, against the Docker dev server:

```
./build/tools/ifx_probe/ifx_probe 127.0.0.1 9088 informix in4mix informix sysmaster \
  "select tabid, tabname from systables where tabid <= 3"
```

Expected output: connection info, a column list, then 3 rows
(`systables`/`syscolumns`/`sysindices`) and `disconnected cleanly.`

### 5. Full ODBC-level test through a real Driver Manager (the real deal)

This is the test that matters: does a *standard ODBC application*, not this
project's own code, see correct results through `SQLDriverConnect`/
`SQLFetch`/etc.?

**Important, confirmed while writing this doc**: the native **Windows** ODBC
Driver Manager currently refuses to load this driver via a `DRIVER={path}`
connection string (no registry registration exists yet; see `STATUS.md`).
The verified path today is **Linux**, either natively or via **WSL2** on
Windows:

```
# inside WSL2 / a Linux shell, with unixodbc-dev installed
cmake -G Ninja -B build-linux -DCMAKE_CXX_COMPILER=clang++
cmake --build build-linux

isql -k "DRIVER=build-linux/driver/libifxodbc.so;HOST=127.0.0.1;SERVICE=9088;SERVER=informix;UID=informix;PWD=in4mix;DATABASE=sysmaster"
```

At the `SQL>` prompt:

```
select tabid, tabname from systables where tabid <= 3;
```

You should see a real table with headers and 3 correct rows, then
`SQLRowCount returns -1` (expected for a SELECT) and `3 rows fetched`. Type
`quit` to exit.

If WSL2's clang++ picks the wrong (headerless) GCC installation and the build
fails with missing standard library headers, see the note in `STATUS.md` and
`docs/protocol-notes.md`'s build history: you may need to pin
`CMAKE_CXX_FLAGS`/`CMAKE_EXE_LINKER_FLAGS`/`CMAKE_SHARED_LINKER_FLAGS` to a
specific GCC version's include/lib paths.

### 6. Optional: run the live integration test

Exercises every MVP scalar type (including NULLs) through the real
prepare-execute-fetch path against your server, asserting each decoded value:

```
cmake -G Ninja -B build -DCMAKE_CXX_COMPILER=clang++ -DIFX_ENABLE_INTEGRATION_TESTS=ON
cmake --build build --target type_probe_integration_test
./build/tests/integration/type_probe_integration_test
```

It creates its own fixture table on the target database (default:
`ifxodbc_test`) but does **not** create the database itself; create it once
before the first run:

```
docker exec ifxodbc_dev bash -lc 'echo "create database ifxodbc_test with buffered log;" | dbaccess sysmaster -'
```

("database already exists" is a harmless error if you've run this before.)
Override the target with environment variables if needed: `IFX_HOST`,
`IFX_PORT`, `IFX_USER`, `IFX_PASSWORD`, `IFX_SERVER`, `IFX_DATABASE`.

Expect `all assertions passed` at the end.

### Troubleshooting

- **`isql` / `SQLDriverConnect` fails with `IM002`** on Windows: expected,
  see step 5, this only works through Linux/unixODBC today.
- **Connect hangs or times out**: confirm the container is actually
  `On-Line` (step 1); first boot takes longer than the Docker healthcheck
  window.
- **`SQ_ERROR` sqlcode `-208` on connect**: this is a real protocol
  requirement (the server rejects an empty environment-variable set), not a
  bug in your setup; shouldn't occur with an unmodified build.
