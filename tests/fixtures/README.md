# Wire capture fixtures

Real byte captures of a live client/server SQLI conversation, taken by proxying
the actual IBM JDBC driver (talking to our own local dev Docker Informix
instance) through a small logging TCP relay. This is factual recorded network
data (our own traffic against our own database), not vendor source code — used
here as ground truth to build and check the from-scratch C++ implementation
against.

- `handshake_capture_ids14.10.txt` — one full session, captured from the real
  IBM JDBC driver: connect handshake, database-open, a couple of
  `SET ISOLATION` statements the driver issues automatically, one `SELECT`
  with its prepare/describe/execute/fetch/close cycle, and a clean disconnect
  (server replies to `SQ_EXIT`). Captured against
  `ibmcom/informix-developer-database` (IBM Informix Dynamic Server
  14.10.FC7W1DE), server name `informix`, database `sysmaster`, port 9088.
- `probe_db_select_and_exit.txt` — captured from our own `ifx_probe` (Phase 1):
  handshake, database-select, immediate `SQ_EXIT` with no other statement
  traffic in between. Here the server just closes the socket instead of
  replying to `SQ_EXIT` — the discrepancy that showed `disconnectOrderly` must
  tolerate a closed connection as a normal outcome (see protocol-notes.md).
