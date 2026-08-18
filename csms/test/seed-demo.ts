/**
 * Seed a database with plausible demo data, for looking at the dashboard
 * without a real charger. Not used in production.
 *
 *   npx tsx test/seed-demo.ts ./demo.db
 */
import { openDatabase, hashPassword, hashChargerKey } from '../src/db/schema.js';
import { randomUUID } from 'node:crypto';

const path = process.argv[2] ?? './demo.db';
const db = openDatabase(path);
const now = Date.now();

db.exec('DELETE FROM meter_samples; DELETE FROM transactions; DELETE FROM tags; DELETE FROM chargers; DELETE FROM users;');

db.prepare(
  `INSERT INTO users (id, email, password_hash, role, name, created_at)
   VALUES (?, 'admin@evrest.local', ?, 'admin', 'Administrator', ?)`,
).run(randomUUID(), hashPassword('demo-password-1234'), now);

const chargers = [
  { id: 'EVREST-0001', name: 'Bay 1 — North', status: 'Charging',     max: 48, online: 1, err: null,     verr: null },
  { id: 'EVREST-0002', name: 'Bay 2 — North', status: 'Available',    max: 32, online: 1, err: null,     verr: null },
  { id: 'EVREST-0003', name: 'Bay 3 — South', status: 'SuspendedEV',  max: 40, online: 1, err: null,     verr: null },
  { id: 'EVREST-0004', name: 'Bay 4 — South', status: 'Faulted',      max: 32, online: 1, err: 'GroundFailure', verr: 'ResidualCurrent' },
  { id: 'EVREST-0005', name: 'Visitor bay',   name2: '', status: 'Preparing', max: 16, online: 1, err: null, verr: null },
  { id: 'EVREST-0006', name: 'Workshop',      status: 'Unavailable',  max: 48, online: 0, err: null,     verr: null },
];

for (const c of chargers) {
  db.prepare(
    `INSERT INTO chargers (id, name, vendor, model, firmware, status, error_code, vendor_error,
                           online, last_seen, last_boot, heartbeat_interval, auth_key_hash,
                           max_current_a, created_at)
     VALUES (?, ?, 'EVREST', 'EVREST-L2', '1.0.0', ?, ?, ?, ?, ?, ?, 300, ?, ?, ?)`,
  ).run(c.id, c.name, c.status, c.err, c.verr, c.online,
        c.online ? now - 20_000 : now - 5 * 3_600_000,
        now - 3 * 86_400_000, hashChargerKey('demo-key'), c.max, now - 40 * 86_400_000);
}

const tags = [
  { id: '04A2B3C4D5', label: 'Fleet van 3', parent: 'FLEET' },
  { id: '04F1E2D3C4', label: 'Fleet van 4', parent: 'FLEET' },
  { id: '0499887766', label: 'J. Okonkwo',  parent: null },
  { id: '04AABBCCDD', label: 'Visitor card', parent: null },
  { id: '0400000000', label: 'Lost card',   parent: null, status: 'Blocked' },
];
for (const t of tags) {
  db.prepare(
    `INSERT INTO tags (id_tag, parent_id_tag, label, status, created_at) VALUES (?, ?, ?, ?, ?)`,
  ).run(t.id, t.parent, t.label, (t as any).status ?? 'Accepted', now);
}

/* Thirty days of completed sessions, weekday-heavy so the chart has shape. */
const insertTxn = db.prepare(
  `INSERT INTO transactions (charger_id, connector_id, id_tag, meter_start_wh, meter_stop_wh,
                             started_at, stopped_at, stop_reason, energy_wh)
   VALUES (?, 1, ?, ?, ?, ?, ?, ?, ?)`,
);
const insertSample = db.prepare(
  `INSERT INTO meter_samples (charger_id, transaction_id, ts, energy_wh, power_w, current_a, voltage_v, offered_a)
   VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
);

let meter = 1_000_000;
for (let d = 29; d >= 0; d--) {
  const dayStart = now - d * 86_400_000;
  const weekday = new Date(dayStart).getDay();
  const sessions = weekday === 0 || weekday === 6 ? 1 + (d % 2) : 3 + (d % 4);

  for (let s = 0; s < sessions; s++) {
    const charger = chargers[(d + s) % 4]!;
    const tag = tags[(d + s) % 4]!;
    const started = dayStart - 8 * 3_600_000 + s * 2_400_000 + (d % 5) * 600_000;
    const durationMs = (45 + ((d * 7 + s * 13) % 180)) * 60_000;
    const energy = Math.round(3000 + ((d * 911 + s * 373) % 28_000));

    const start = meter;
    meter += energy;
    insertTxn.run(charger.id, tag.id, start, meter, started, started + durationMs, 'Local', energy);
  }
}

/* A live session on bay 1, with a throttle partway through so the
 * drawn-vs-offered chart shows something worth seeing. */
const liveStart = now - 52 * 60_000;
const liveTxnId = Number(
  db.prepare(
    `INSERT INTO transactions (charger_id, connector_id, id_tag, meter_start_wh, started_at)
     VALUES ('EVREST-0001', 1, '04A2B3C4D5', ?, ?)`,
  ).run(meter, liveStart).lastInsertRowid,
);

let live = meter;
for (let i = 0; i <= 52 * 4; i++) {
  const ts = liveStart + i * 15_000;
  const minutes = i / 4;
  // Ramp in over the first two minutes, then a load-management step down at 30.
  const offered = minutes < 30 ? 48 : 24;
  const ramp = Math.min(1, minutes / 2);
  const drawn = Math.min(offered, 46 * ramp) * (0.97 + 0.03 * Math.sin(i / 5));
  const volts = 241 + 2 * Math.sin(i / 9);
  const watts = drawn * volts;
  live += (watts * 15) / 3600;
  insertSample.run('EVREST-0001', liveTxnId, ts, Math.round(live), watts, drawn, volts, offered);
}

console.log(`Seeded ${path}: ${chargers.length} chargers, ` +
            `${(db.prepare('SELECT COUNT(*) n FROM transactions').get() as any).n} sessions, ` +
            `${(db.prepare('SELECT COUNT(*) n FROM meter_samples').get() as any).n} samples.`);
console.log('Sign in as admin@evrest.local / demo-password-1234');
console.log('Every seeded charger accepts the auth key: demo-key');
console.log('Run a simulated charger with:');
console.log('  npx tsx test/fake-charger.ts EVREST-0001 demo-key');
db.close();
