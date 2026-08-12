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

- This image's entrypoint ends in an interactive shell that the server process
  depends on staying attached to stdin: without a TTY, the shell hits EOF and
  exits a few seconds after the server reports on-line, taking the container
  down with it (`Shutdown Signal Received 15` in `docker logs`). The
  `tty: true`/`stdin_open: true` lines in `docker-compose.yml` are required,
  not optional, for `docker compose up -d` to stay running.
- The image reinitializes its data directory (`INFORMIX_CLEAN=ALWAYS`) on
  every fresh boot of the entrypoint, even when `/opt/ibm/data` is a
  persistent named volume with prior data in it - only `sysmaster`/
  `sysutils`/`sysuser`/`sysadmin` survive. Any database created for testing
  (e.g. `ifxodbc_test`) does not persist across a container recreate/restart
  and needs to be recreated each time:
  `echo "create database ifxodbc_test with log;" | dbaccess - -` (run inside
  the container, after sourcing `/etc/profile.d/informix.sh`).
- The default healthcheck command needs the Informix environment sourced
  first (`onstat` isn't on the bare `docker exec` `PATH`) - already fixed in
  `docker-compose.yml`'s `CMD-SHELL` form.
