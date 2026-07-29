# informix-odbc

A from-scratch, cross-platform (Windows + Linux) ODBC driver for IBM Informix,
written in C++26. IBM does not ship a native ODBC driver for Informix (only
JDBC), so this implements Informix's proprietary "SQLI" wire protocol directly.

See `C:\Users\SImi\.claude\plans\harmonic-wobbling-harp.md` for the full phased
plan, and `docs/protocol-notes.md` for wire-format research notes (written from
scratch, not derived from any vendor source code).

## Building

Requires `clang++` (C++26 support), `cmake` >= 3.28, `ninja`.

```
cmake -G Ninja -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
```

## Layout

- `sqli/` — the SQLI wire protocol implementation (transport, handshake,
  statement protocol, type codecs). No ODBC dependency.
- `driver/` — the ODBC driver surface (`SQL*` entry points), built on `sqli/`.
- `tools/ifx_probe/` — a small CLI for exercising the protocol layer directly,
  without going through ODBC.
- `tests/` — unit tests (codecs) and integration tests (against a live
  Informix instance).
- `docker/` — `docker-compose.yml` for a local dev Informix instance; see
  `docker/README.md` for confirmed connection details.

## Status

Early development (Phase 0/1 of the plan). Not yet usable.
