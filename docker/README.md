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
