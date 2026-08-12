# Local dev Informix instance

```
docker compose up -d
```

Confirmed connection details for this dev container (IBM Informix Dynamic Server
14.10.FC7W1DE, `ibmcom/informix-developer-database`):

- Host: `localhost`, port: `9088` (plain `onsoctcp`; `9089` is the SSL/`drsoctcp` variant)
- Server name (`INFORMIXSERVER`): `informix`
- User: `informix`, password: `in4mix` (baked into this image, dev-only)
- Startup takes a few minutes on first run (building `sysmaster`/`sysadmin`/etc.
  databases) — check readiness with:
  ```
  docker compose logs -f
  # or
  docker exec ifxodbc_dev bash -lc 'onstat -'
  ```
  and wait for `-- On-Line --` in the `onstat -` output.

These are the values `ifx_probe` and the integration tests target during
development.

## Known quirks

- `onstat` (and the rest of `$INFORMIXDIR/bin`) is only on `PATH` inside a
  login shell for the `informix` user, not a bare `docker exec`. There's no
  `/etc/profile.d/*.sh` that sets it - it comes from the user's own login
  profile. The healthcheck accounts for this by running `bash -lc "onstat -"`
  rather than a bare `onstat -`; do the same for any manual `docker exec`.
- The image reinitializes its data directory (`INFORMIX_CLEAN=ALWAYS`) on
  at least some fresh boots of the entrypoint, even when `/opt/ibm/data` is a
  persistent named volume with prior data in it - observed once this session
  where only `sysmaster`/`sysutils`/`sysuser`/`sysadmin` survived a restart.
  Not confirmed to happen on every restart. If a database created for
  testing (e.g. `ifxodbc_test`) is missing after bringing the container back
  up, recreate it:
  `docker exec ifxodbc_dev bash -lc 'echo "create database ifxodbc_test with log;" | dbaccess - -'`
- A container was observed dying ~90-100s after reporting on-line, with
  `Shutdown Signal Received 15` in `docker logs` and no corresponding
  external `docker stop`/`kill` from this project's own tooling - root cause
  not identified. `tty: true`/`stdin_open: true` are present in
  `docker-compose.yml` (this image's entrypoint does end in an interactive
  shell in some code paths, which needs one to avoid exiting on stdin EOF),
  but a container with both already set still died with this signature once,
  so they are not confirmed sufficient on their own. If the container dies
  shortly after coming online, check `docker logs` for this signature and
  retry `docker compose up -d`.
