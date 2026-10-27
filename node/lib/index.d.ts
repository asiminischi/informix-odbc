/// <reference types="node" />

export type ParameterValue = string | number | bigint | boolean | Date | Buffer | null | undefined;

export type IsolationLevel =
  | 'READ_UNCOMMITTED'
  | 'READ_COMMITTED'
  | 'CURSOR_STABILITY'
  | 'REPEATABLE_READ'
  | 'SERIALIZABLE';

export interface ConnectionConfig {
  host?: string;
  /** Default 9088. */
  port?: number;
  database?: string;
  username?: string;
  password?: string;
  /** INFORMIXSERVER name of the instance. Default 'informix'. */
  server?: string;
  /** Client name reported to the server. */
  appName?: string;
  /** Bounds the TCP connect and login handshake, in ms. Default 10 000; 0 waits forever. */
  connectTimeoutMs?: number;
  /**
   * Bounds each network wait while running statements, in ms. The server
   * sends nothing while a query executes, so this also caps query run time.
   * A timeout is fatal: the connection is closed (kind 'transport').
   * Default 0 (no limit).
   */
  queryTimeoutMs?: number;
  /** How BIGINT/BIGSERIAL values are returned. Default 'number' (lossy past 2^53). */
  int64?: 'number' | 'bigint' | 'string';
  /** How DECIMAL/MONEY values are returned. Default 'number' (lossy); 'string' is exact. */
  decimal?: 'number' | 'string';
}

export interface ColumnInfo {
  name: string;
  /** Informix type name, e.g. 'integer', 'varchar', 'decimal', 'datetime', 'blob'. */
  type: string;
  precision: number;
  scale: number;
  nullable: boolean;
}

export type Row = Record<string, any>;

export interface QueryResult<T = Row> {
  rows: T[];
  columns: ColumnInfo[] | null;
  rowCount: number;
}

export interface StreamResult {
  columns: ColumnInfo[] | null;
  rowCount: number;
}

export interface QueryOptions {
  /** Rows fetched per network round trip. Default 500. */
  fetchSize?: number;
  /** Stop after this many rows (0 = no limit). */
  maxRows?: number;
}

export interface TableInfo {
  name: string;
  schema: string;
  type: string;
  columns: ColumnInfo[];
}

export interface PingResult {
  alive: boolean;
  latencyMs: number;
}

export type ErrorKind = 'server' | 'transport' | 'protocol' | 'usage';

export class InformixError extends Error {
  readonly kind: ErrorKind;
  /** Informix SQL error code (e.g. -206) when kind is 'server', else null. */
  readonly sqlcode: number | null;
  /** True for transport/protocol errors: the connection has been closed. */
  readonly isFatal: boolean;
}

export class PreparedStatement {
  readonly parameterCount: number;
  readonly columns: ColumnInfo[] | null;
  query<T = Row>(params?: ParameterValue[], options?: QueryOptions): Promise<QueryResult<T>>;
  execute(params?: ParameterValue[]): Promise<number>;
  close(): Promise<void>;
}

/** One Informix session. Calls are serialized; use a Pool for concurrency. */
export class Connection {
  constructor(config: ConnectionConfig);
  static connect(config: ConnectionConfig): Promise<Connection>;
  open(): Promise<{ serverVersion: string; serialNumber: string }>;
  readonly isConnected: boolean;
  readonly inTransaction: boolean;
  readonly serverVersion: string | null;
  query<T = Row>(sql: string, params?: ParameterValue[], options?: QueryOptions): Promise<QueryResult<T>>;
  queryStream<T = Row>(
    sql: string,
    params: ParameterValue[] | null | undefined,
    onRow: (row: T) => void | Promise<void>,
    options?: QueryOptions,
  ): Promise<StreamResult>;
  /** The connection stays locked until the loop ends; don't query it from inside the loop. */
  iterate<T = Row>(sql: string, params?: ParameterValue[], options?: Pick<QueryOptions, 'fetchSize'>): AsyncGenerator<T>;
  execute(sql: string, params?: ParameterValue[]): Promise<number>;
  batch(statements: string[]): Promise<number[]>;
  prepare(sql: string): Promise<PreparedStatement>;
  beginTransaction(isolationLevel?: IsolationLevel): Promise<void>;
  commit(): Promise<void>;
  rollback(): Promise<void>;
  transaction<R>(fn: (conn: Connection) => Promise<R>, isolationLevel?: IsolationLevel): Promise<R>;
  ping(): Promise<PingResult>;
  getMetadata(tableName?: string): Promise<TableInfo[]>;
  close(): Promise<void>;
}

export interface PoolOptions {
  /** Maximum open connections. Default 10. */
  max?: number;
}

export class Pool {
  constructor(config: ConnectionConfig, options?: PoolOptions);
  readonly size: number;
  readonly idleCount: number;
  readonly closed: boolean;
  acquire(): Promise<Connection>;
  release(conn: Connection): void;
  withConnection<R>(fn: (conn: Connection) => Promise<R>): Promise<R>;
  query<T = Row>(sql: string, params?: ParameterValue[], options?: QueryOptions): Promise<QueryResult<T>>;
  queryStream<T = Row>(
    sql: string,
    params: ParameterValue[] | null | undefined,
    onRow: (row: T) => void | Promise<void>,
    options?: QueryOptions,
  ): Promise<StreamResult>;
  execute(sql: string, params?: ParameterValue[]): Promise<number>;
  batch(statements: string[]): Promise<number[]>;
  transaction<R>(fn: (conn: Connection) => Promise<R>, isolationLevel?: IsolationLevel): Promise<R>;
  ping(): Promise<PingResult>;
  getMetadata(tableName?: string): Promise<TableInfo[]>;
  close(): Promise<void>;
}

// --- informix-proxy compatible client --------------------------------------

export interface InformixConnectionConfig extends ConnectionConfig {
  host: string;
  port: number;
  database: string;
  username: string;
  password: string;
  /** Accepted for compatibility; ignored. */
  properties?: Record<string, string>;
  /** Pool size. Default 10. */
  poolSize?: number;
}

export interface ConnectionInfo {
  connectionId: string;
  serverVersion: string;
}

export interface PreparedStatementInfo {
  statementId: string;
  parameterCount: number;
}

export interface InformixClientLogger {
  info(message: string, meta?: Record<string, unknown>): void;
  warn(message: string, meta?: Record<string, unknown>): void;
  error(message: string, meta?: Record<string, unknown>): void;
}

export interface InformixClientOptions {
  logger?: InformixClientLogger;
  [key: string]: unknown;
}

/**
 * Drop-in replacement for @postarodiy/informix-client. The constructor's
 * host/port (the proxy address) are accepted and ignored.
 */
export class InformixClient {
  constructor(proxyHost?: string, proxyPort?: number, options?: InformixClientOptions);
  readonly isConnected: boolean;
  readonly connectionId: string | null;
  readonly serverVersion: string | null;
  connect(config: InformixConnectionConfig): Promise<ConnectionInfo>;
  disconnect(): Promise<void>;
  resetConnection(reason?: string): void;
  ping(): Promise<PingResult>;
  query<T = Row>(sql: string, params?: ParameterValue[], options?: QueryOptions): Promise<QueryResult<T>>;
  queryStream<T = Row>(
    sql: string,
    params: ParameterValue[] | undefined,
    onRow: (row: T) => void | Promise<void>,
    options?: QueryOptions,
  ): Promise<StreamResult>;
  execute(sql: string, params?: ParameterValue[]): Promise<number>;
  batch(sqlStatements: string[]): Promise<number[]>;
  beginTransaction(isolationLevel?: IsolationLevel): Promise<void>;
  commit(): Promise<void>;
  rollback(): Promise<void>;
  transaction<R>(fn: (conn: Connection) => Promise<R>, isolationLevel?: IsolationLevel): Promise<R>;
  prepareStatement(sql: string): Promise<PreparedStatementInfo>;
  executePrepared<T = Row>(
    statementId: string,
    params?: ParameterValue[],
    options?: Pick<QueryOptions, 'fetchSize'>,
  ): Promise<QueryResult<T>>;
  closePrepared(statementId: string): Promise<void>;
  getMetadata(tableName?: string): Promise<TableInfo[]>;
}

export interface Defaults {
  host: string;
  port: number;
  server: string;
  database: string;
  username: string;
  password: string;
  appName: string;
  connectTimeoutMs: number;
  queryTimeoutMs: number;
  int64: 'number' | 'bigint' | 'string';
  decimal: 'number' | 'string';
  fetchSize: number;
  maxRows: number;
  poolSize: number;
  isolationLevel: IsolationLevel;
}

/** The package's built-in defaults (never changed by setDefaults). */
export const BUILTIN_DEFAULTS: Readonly<Defaults>;
/** The defaults currently in effect. */
export function getDefaults(): Defaults;
/** Overrides defaults for everything created afterwards; validates each value. Returns the new defaults. */
export function setDefaults(overrides: Partial<Defaults>): Defaults;
/** Restores BUILTIN_DEFAULTS. */
export function resetDefaults(): Defaults;

export function connect(config: ConnectionConfig): Promise<Connection>;
export function createPool(config: ConnectionConfig & { poolSize?: number }, options?: PoolOptions): Pool;

