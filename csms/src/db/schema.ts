/**
 * Database schema and access layer.
 *
 * SQLite via better-sqlite3: synchronous, single-file, no separate process.
 * A CSMS for a site of tens of chargers is not write-bound, and the whole
 * deployment being one file makes backup and restore trivial — which matters
 * more than throughput when the data is billing records.
 *
 * WAL mode is on so the API's reads never block the OCPP server's writes.
 */
import Database from 'better-sqlite3';
import { createHash, randomBytes, pbkdf2Sync } from 'node:crypto';

export type ChargerStatus =
  | 'Available' | 'Preparing' | 'Charging' | 'SuspendedEV' | 'SuspendedEVSE'
  | 'Finishing' | 'Reserved' | 'Unavailable' | 'Faulted';

export interface ChargerRow {
  id: string;
  name: string;
  vendor: string | null;
  model: string | null;
  serial: string | null;
  firmware: string | null;
  status: ChargerStatus;
  error_code: string | null;
  vendor_error: string | null;
  online: 0 | 1;
  last_seen: number | null;
  last_boot: number | null;
  heartbeat_interval: number;
  auth_key_hash: string | null;
  max_current_a: number;
  site_id: string | null;
  created_at: number;
}

export interface TransactionRow {
  id: number;
  charger_id: string;
  connector_id: number;
  id_tag: string;
  meter_start_wh: number;
  meter_stop_wh: number | null;
  started_at: number;
  stopped_at: number | null;
  stop_reason: string | null;
  energy_wh: number | null;
}

export interface MeterSampleRow {
  id: number;
  charger_id: string;
  transaction_id: number | null;
  ts: number;
  energy_wh: number | null;
  power_w: number | null;
  current_a: number | null;
  voltage_v: number | null;
  offered_a: number | null;
}

export interface TagRow {
  id_tag: string;
  parent_id_tag: string | null;
  label: string | null;
  status: 'Accepted' | 'Blocked' | 'Expired' | 'Invalid';
  expires_at: number | null;
  user_id: string | null;
  created_at: number;
}

export interface UserRow {
  id: string;
  email: string;
  password_hash: string;
  role: 'admin' | 'operator' | 'driver';
  name: string | null;
  created_at: number;
}

export function openDatabase(path: string): Database.Database {
  const db = new Database(path);

  // WAL lets the REST API read while the OCPP server writes. NORMAL synchronous
  // is safe under WAL (a crash can lose the last transaction, not corrupt the
  // file) and avoids an fsync on every meter sample.
  db.pragma('journal_mode = WAL');
  db.pragma('synchronous = NORMAL');
  db.pragma('foreign_keys = ON');

  db.exec(`
    CREATE TABLE IF NOT EXISTS chargers (
      id                 TEXT PRIMARY KEY,
      name               TEXT NOT NULL,
      vendor             TEXT,
      model              TEXT,
      serial             TEXT,
      firmware           TEXT,
      status             TEXT NOT NULL DEFAULT 'Unavailable',
      error_code         TEXT,
      vendor_error       TEXT,
      online             INTEGER NOT NULL DEFAULT 0,
      last_seen          INTEGER,
      last_boot          INTEGER,
      heartbeat_interval INTEGER NOT NULL DEFAULT 300,
      auth_key_hash      TEXT,
      max_current_a      REAL NOT NULL DEFAULT 32,
      site_id            TEXT,
      created_at         INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS transactions (
      id             INTEGER PRIMARY KEY AUTOINCREMENT,
      charger_id     TEXT NOT NULL REFERENCES chargers(id) ON DELETE CASCADE,
      connector_id   INTEGER NOT NULL DEFAULT 1,
      id_tag         TEXT NOT NULL,
      meter_start_wh INTEGER NOT NULL,
      meter_stop_wh  INTEGER,
      started_at     INTEGER NOT NULL,
      stopped_at     INTEGER,
      stop_reason    TEXT,
      energy_wh      INTEGER
    );
    CREATE INDEX IF NOT EXISTS idx_txn_charger ON transactions(charger_id, started_at DESC);
    CREATE INDEX IF NOT EXISTS idx_txn_tag     ON transactions(id_tag, started_at DESC);
    -- Finding the open transaction for a charger happens on every StopTransaction
    -- and every MeterValues, so it gets its own partial index.
    CREATE INDEX IF NOT EXISTS idx_txn_open    ON transactions(charger_id) WHERE stopped_at IS NULL;

    CREATE TABLE IF NOT EXISTS meter_samples (
      id             INTEGER PRIMARY KEY AUTOINCREMENT,
      charger_id     TEXT NOT NULL REFERENCES chargers(id) ON DELETE CASCADE,
      transaction_id INTEGER REFERENCES transactions(id) ON DELETE SET NULL,
      ts             INTEGER NOT NULL,
      energy_wh      INTEGER,
      power_w        REAL,
      current_a      REAL,
      voltage_v      REAL,
      offered_a      REAL
    );
    CREATE INDEX IF NOT EXISTS idx_samples_txn    ON meter_samples(transaction_id, ts);
    CREATE INDEX IF NOT EXISTS idx_samples_recent ON meter_samples(charger_id, ts DESC);

    CREATE TABLE IF NOT EXISTS tags (
      id_tag        TEXT PRIMARY KEY,
      parent_id_tag TEXT,
      label         TEXT,
      status        TEXT NOT NULL DEFAULT 'Accepted',
      expires_at    INTEGER,
      user_id       TEXT REFERENCES users(id) ON DELETE SET NULL,
      created_at    INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS users (
      id            TEXT PRIMARY KEY,
      email         TEXT NOT NULL UNIQUE,
      password_hash TEXT NOT NULL,
      role          TEXT NOT NULL DEFAULT 'driver',
      name          TEXT,
      created_at    INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS ocpp_log (
      id         INTEGER PRIMARY KEY AUTOINCREMENT,
      charger_id TEXT NOT NULL,
      ts         INTEGER NOT NULL,
      direction  TEXT NOT NULL,   -- 'in' from charger, 'out' to charger
      action     TEXT,
      message_id TEXT,
      payload    TEXT NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_log_charger ON ocpp_log(charger_id, ts DESC);

    CREATE TABLE IF NOT EXISTS charging_profiles (
      id           INTEGER PRIMARY KEY AUTOINCREMENT,
      charger_id   TEXT NOT NULL REFERENCES chargers(id) ON DELETE CASCADE,
      profile_id   INTEGER NOT NULL,
      purpose      TEXT NOT NULL,
      stack_level  INTEGER NOT NULL DEFAULT 0,
      payload      TEXT NOT NULL,
      created_at   INTEGER NOT NULL,
      UNIQUE(charger_id, profile_id)
    );
  `);

  return db;
}

/* ------------------------------------------------------------------ */
/* Credentials                                                        */
/* ------------------------------------------------------------------ */

/**
 * PBKDF2 rather than a plain hash. These are user passwords; a fast hash makes
 * an offline attack on a stolen database trivial.
 *
 * Stored as `iterations:salt:derived`, all hex, so the iteration count can be
 * raised later without invalidating existing hashes.
 */
const PBKDF2_ITERATIONS = 210_000;

export function hashPassword(password: string): string {
  const salt = randomBytes(16);
  const derived = pbkdf2Sync(password, salt, PBKDF2_ITERATIONS, 32, 'sha256');
  return `${PBKDF2_ITERATIONS}:${salt.toString('hex')}:${derived.toString('hex')}`;
}

export function verifyPassword(password: string, stored: string): boolean {
  const parts = stored.split(':');
  if (parts.length !== 3) return false;
  const [iterStr, saltHex, expectedHex] = parts as [string, string, string];

  const iterations = Number.parseInt(iterStr, 10);
  if (!Number.isFinite(iterations) || iterations <= 0) return false;

  const derived = pbkdf2Sync(
    password, Buffer.from(saltHex, 'hex'), iterations, 32, 'sha256',
  );
  const expected = Buffer.from(expectedHex, 'hex');
  if (derived.length !== expected.length) return false;

  // Constant-time compare so a wrong password cannot be narrowed down by timing.
  let diff = 0;
  for (let i = 0; i < derived.length; i++) diff |= derived[i]! ^ expected[i]!;
  return diff === 0;
}

/**
 * Charger authentication keys are hashed too, but with a plain SHA-256: they
 * are high-entropy machine-generated secrets, not human passwords, so key
 * stretching buys nothing and this runs on every charger reconnect.
 */
export function hashChargerKey(key: string): string {
  return createHash('sha256').update(key).digest('hex');
}

export function verifyChargerKey(key: string, storedHash: string): boolean {
  const computed = Buffer.from(hashChargerKey(key), 'hex');
  const expected = Buffer.from(storedHash, 'hex');
  if (computed.length !== expected.length) return false;
  let diff = 0;
  for (let i = 0; i < computed.length; i++) diff |= computed[i]! ^ expected[i]!;
  return diff === 0;
}
