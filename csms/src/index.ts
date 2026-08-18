/**
 * Entry point: starts the OCPP listener and the API server.
 *
 * Two separate ports on purpose. Chargers reach the OCPP port and nothing else;
 * browsers and phones reach the API port and nothing else. On a real
 * deployment they can then be firewalled differently, which matters because the
 * charger port is exposed to field devices on untrusted networks.
 */
import { createServer } from 'node:http';
import { randomUUID } from 'node:crypto';
import { openDatabase, hashPassword } from './db/schema.js';
import { OcppServer } from './ocpp/server.js';
import { EventBus } from './api/events.js';
import { createRestApi } from './api/rest.js';
import { createLiveHub } from './api/live.js';

function env(name: string, fallback?: string): string {
  const v = process.env[name] ?? fallback;
  if (v === undefined) {
    console.error(`Missing required environment variable ${name}`);
    process.exit(1);
  }
  return v;
}

const OCPP_PORT = Number(env('PORT', '9220'));
const API_PORT = Number(env('API_PORT', '9221'));
const DB_PATH = env('DB_PATH', './evrest.db');
const JWT_SECRET = env('JWT_SECRET');
const REQUIRE_CHARGER_AUTH = env('REQUIRE_CHARGER_AUTH', 'true') !== 'false';

if (JWT_SECRET.length < 32) {
  console.error('JWT_SECRET must be at least 32 characters. Generate one with:');
  console.error('  node -e "console.log(require(\'crypto\').randomBytes(32).toString(\'hex\'))"');
  process.exit(1);
}
if (!REQUIRE_CHARGER_AUTH) {
  console.warn('WARNING: charger authentication is disabled. Any device that can');
  console.warn('         reach this port can register itself as a charge point.');
}

const db = openDatabase(DB_PATH);
const events = new EventBus();

/*
 * Seed an admin on an empty database so a fresh install is usable. The
 * generated password is printed once and never stored in plaintext; if it is
 * lost, the account has to be reset by hand.
 */
const userCount = (db.prepare('SELECT COUNT(*) AS n FROM users').get() as { n: number }).n;
if (userCount === 0) {
  const password = randomUUID();
  db.prepare(
    `INSERT INTO users (id, email, password_hash, role, name, created_at)
     VALUES (?, 'admin@evrest.local', ?, 'admin', 'Administrator', ?)`,
  ).run(randomUUID(), hashPassword(password), Date.now());

  console.log('');
  console.log('  Created the initial administrator account:');
  console.log('    email:    admin@evrest.local');
  console.log(`    password: ${password}`);
  console.log('  This is shown once. Change it after signing in.');
  console.log('');
}

const ocpp = new OcppServer({
  port: OCPP_PORT,
  db,
  events,
  requireAuth: REQUIRE_CHARGER_AUTH,
});

const app = createRestApi({ db, ocpp, jwtSecret: JWT_SECRET });
const httpServer = createServer(app);
createLiveHub(httpServer, events, JWT_SECRET);

httpServer.listen(API_PORT, () => {
  console.log(`[api]  REST + live WebSocket on :${API_PORT}`);
});

/*
 * Close the database explicitly on shutdown. Under WAL, an unclean exit leaves
 * a -wal file that the next start has to recover; closing checkpoints it.
 */
async function shutdown(signal: string): Promise<void> {
  console.log(`\nReceived ${signal}, shutting down.`);
  await ocpp.close();
  await new Promise<void>((resolve) => httpServer.close(() => resolve()));
  db.close();
  process.exit(0);
}

process.on('SIGINT', () => void shutdown('SIGINT'));
process.on('SIGTERM', () => void shutdown('SIGTERM'));
