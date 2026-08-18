/**
 * Integration tests: a fake charge point speaking real OCPP 1.6J to the real
 * server over a real WebSocket, against an in-memory database.
 *
 * These are the tests that matter for a CSMS. Unit-testing the handlers in
 * isolation would miss the things that actually break — framing, validation
 * rejection, transaction lifecycle, and the ordering guarantees that only hold
 * because the handlers are synchronous.
 */
import { test, describe, before, after, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { WebSocket } from 'ws';
import { openDatabase, hashChargerKey } from '../src/db/schema.js';
import { OcppServer } from '../src/ocpp/server.js';
import { EventBus } from '../src/api/events.js';
import type Database from 'better-sqlite3';

const PORT = 19220;
const CHARGER_ID = 'TEST-CP-001';
const CHARGER_KEY = 'test-key-abcdefgh';

let db: Database.Database;
let events: EventBus;
let server: OcppServer;

/** A minimal charge point: connect, send CALLs, await CALLRESULTs. */
class FakeCharger {
  private ws!: WebSocket;
  private seq = 0;
  private readonly pending = new Map<string, (payload: any) => void>();
  /** CALLs the server sent us, in order. */
  readonly received: Array<{ id: string; action: string; payload: any }> = [];

  async connect(id = CHARGER_ID, key: string | null = CHARGER_KEY): Promise<void> {
    const headers: Record<string, string> = {};
    if (key !== null) {
      headers.Authorization =
        'Basic ' + Buffer.from(`${id}:${key}`).toString('base64');
    }
    this.ws = new WebSocket(`ws://127.0.0.1:${PORT}/ocpp/${id}`, ['ocpp1.6'], { headers });

    await new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('connect timed out')), 3000);
      this.ws.once('open', () => { clearTimeout(timer); resolve(); });
      // A rejected upgrade surfaces as an 'error' carrying the HTTP status;
      // a rejection after the handshake surfaces as 'close'. Both are failures
      // to connect, and the tests below assert which one they expect.
      this.ws.once('close', (code) => {
        clearTimeout(timer);
        reject(new Error(`refused: close ${code}`));
      });
      this.ws.once('error', (e) => {
        clearTimeout(timer);
        reject(new Error(`refused: ${e.message}`));
      });
    });

    this.ws.on('message', (raw) => {
      const msg = JSON.parse(raw.toString());
      const [type, id2] = msg;
      if (type === 3) {                       // CALLRESULT
        this.pending.get(id2)?.(msg[2]);
        this.pending.delete(id2);
      } else if (type === 4) {                // CALLERROR
        this.pending.get(id2)?.({ __error: msg[2], description: msg[3] });
        this.pending.delete(id2);
      } else if (type === 2) {                // server-initiated CALL
        this.received.push({ id: id2, action: msg[2], payload: msg[3] });
      }
    });
  }

  call(action: string, payload: unknown): Promise<any> {
    const id = `m${++this.seq}`;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`${action} timed out`)), 3000);
      this.pending.set(id, (p) => { clearTimeout(timer); resolve(p); });
      this.ws.send(JSON.stringify([2, id, action, payload]));
    });
  }

  /** Send a raw frame, for malformed-input tests. */
  sendRaw(frame: string): void { this.ws.send(frame); }

  /** Answer a CALL the server sent us. */
  reply(id: string, payload: unknown): void {
    this.ws.send(JSON.stringify([3, id, payload]));
  }

  async waitForCall(action: string, timeoutMs = 2000): Promise<{ id: string; payload: any }> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const hit = this.received.find((r) => r.action === action);
      if (hit) return { id: hit.id, payload: hit.payload };
      await new Promise((r) => setTimeout(r, 20));
    }
    throw new Error(`server never sent ${action}`);
  }

  close(): void { this.ws?.close(); }
  get isOpen(): boolean { return this.ws?.readyState === WebSocket.OPEN; }
}

function seedCharger(): void {
  db.prepare(
    `INSERT OR REPLACE INTO chargers
       (id, name, status, online, heartbeat_interval, auth_key_hash, max_current_a, created_at)
     VALUES (?, ?, 'Unavailable', 0, 300, ?, 32, ?)`,
  ).run(CHARGER_ID, 'Test charger', hashChargerKey(CHARGER_KEY), Date.now());
}

function seedTag(idTag: string, status = 'Accepted', expiresAt: number | null = null): void {
  db.prepare(
    `INSERT OR REPLACE INTO tags (id_tag, status, expires_at, created_at)
     VALUES (?, ?, ?, ?)`,
  ).run(idTag, status, expiresAt, Date.now());
}

before(() => {
  db = openDatabase(':memory:');
  events = new EventBus();
  server = new OcppServer({
    port: PORT, db, events, requireAuth: true, logger: () => {},
  });
});

after(async () => {
  await server.close();
  db.close();
});

beforeEach(() => {
  db.exec('DELETE FROM meter_samples; DELETE FROM transactions; DELETE FROM tags; DELETE FROM chargers; DELETE FROM ocpp_log;');
  seedCharger();
});

/* ==================================================================== */

describe('connection and authentication', () => {
  test('accepts a charger with correct credentials', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    assert.ok(cp.isOpen);
    cp.close();
  });

  /*
   * These must be rejected during the HTTP upgrade, not after it. A charger
   * whose handshake completes believes it is connected and will sit retrying
   * OCPP messages instead of surfacing an auth failure, so asserting on the
   * 401 specifically is the point of these tests.
   */
  test('rejects a wrong auth key with a 401 before the handshake completes', async () => {
    const cp = new FakeCharger();
    await assert.rejects(() => cp.connect(CHARGER_ID, 'wrong-key'), /401/);
  });

  test('rejects a charger with no credentials', async () => {
    const cp = new FakeCharger();
    await assert.rejects(() => cp.connect(CHARGER_ID, null), /401/);
  });

  test('rejects an unknown charger id', async () => {
    const cp = new FakeCharger();
    await assert.rejects(() => cp.connect('NOT-PROVISIONED', 'anything'), /401/);
  });

  test('rejects a path traversal attempt in the charger id', async () => {
    const cp = new FakeCharger();
    await assert.rejects(() => cp.connect('..%2Fetc%2Fpasswd', CHARGER_KEY), /400|401/);
  });

  test('a reconnect replaces the previous session rather than stacking', async () => {
    const first = new FakeCharger();
    await first.connect();
    await first.call('BootNotification', { chargePointVendor: 'EVREST', chargePointModel: 'L2' });

    const second = new FakeCharger();
    await second.connect();
    await second.call('BootNotification', { chargePointVendor: 'EVREST', chargePointModel: 'L2' });

    assert.equal(server.listOnline().length, 1, 'only one session for the id');
    second.close();
  });
});

describe('BootNotification', () => {
  test('accepts a known charger and returns an interval', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    const res = await cp.call('BootNotification', {
      chargePointVendor: 'EVREST',
      chargePointModel: 'EVREST-L2',
      firmwareVersion: '1.0.0',
    });
    assert.equal(res.status, 'Accepted');
    assert.equal(typeof res.interval, 'number');
    assert.ok(Date.parse(res.currentTime) > 0, 'currentTime is a valid timestamp');

    const row = db.prepare('SELECT * FROM chargers WHERE id = ?').get(CHARGER_ID) as any;
    assert.equal(row.vendor, 'EVREST');
    assert.equal(row.firmware, '1.0.0');
    assert.equal(row.online, 1);
    cp.close();
  });

  test('rejects a payload missing a required field', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    const res = await cp.call('BootNotification', { chargePointVendor: 'EVREST' });
    assert.equal(res.__error, 'PropertyConstraintViolation');
    assert.match(res.description, /chargePointModel/);
    cp.close();
  });

  test('rejects an over-length vendor string', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    const res = await cp.call('BootNotification', {
      chargePointVendor: 'x'.repeat(64),
      chargePointModel: 'L2',
    });
    assert.equal(res.__error, 'PropertyConstraintViolation');
    cp.close();
  });
});

describe('authorisation', () => {
  test('an unknown tag is Invalid, never Accepted', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    const res = await cp.call('Authorize', { idTag: 'NEVER-SEEN' });
    assert.equal(res.idTagInfo.status, 'Invalid');
    cp.close();
  });

  test('a known tag is accepted', async () => {
    seedTag('GOOD-TAG');
    const cp = new FakeCharger();
    await cp.connect();
    const res = await cp.call('Authorize', { idTag: 'GOOD-TAG' });
    assert.equal(res.idTagInfo.status, 'Accepted');
    cp.close();
  });

  test('a blocked tag stays blocked', async () => {
    seedTag('BAD-TAG', 'Blocked');
    const cp = new FakeCharger();
    await cp.connect();
    const res = await cp.call('Authorize', { idTag: 'BAD-TAG' });
    assert.equal(res.idTagInfo.status, 'Blocked');
    cp.close();
  });

  test('an expired tag reports Expired even though it is marked Accepted', async () => {
    seedTag('OLD-TAG', 'Accepted', Date.now() - 1000);
    const cp = new FakeCharger();
    await cp.connect();
    const res = await cp.call('Authorize', { idTag: 'OLD-TAG' });
    assert.equal(res.idTagInfo.status, 'Expired');
    cp.close();
  });
});

describe('transaction lifecycle', () => {
  test('start, meter values, stop', async () => {
    seedTag('DRIVER-1');
    const cp = new FakeCharger();
    await cp.connect();
    await cp.call('BootNotification', { chargePointVendor: 'EVREST', chargePointModel: 'L2' });

    const start = await cp.call('StartTransaction', {
      connectorId: 1, idTag: 'DRIVER-1', meterStart: 1000,
      timestamp: new Date().toISOString(),
    });
    assert.equal(start.idTagInfo.status, 'Accepted');
    assert.ok(start.transactionId > 0);

    await cp.call('MeterValues', {
      connectorId: 1,
      transactionId: start.transactionId,
      meterValue: [{
        timestamp: new Date().toISOString(),
        sampledValue: [
          { value: '2500', measurand: 'Energy.Active.Import.Register', unit: 'Wh' },
          { value: '7200', measurand: 'Power.Active.Import', unit: 'W' },
          { value: '30.0', measurand: 'Current.Import', unit: 'A' },
          { value: '240.0', measurand: 'Voltage', unit: 'V' },
          { value: '32.0', measurand: 'Current.Offered', unit: 'A' },
        ],
      }],
    });

    const samples = db.prepare('SELECT * FROM meter_samples WHERE transaction_id = ?')
      .all(start.transactionId) as any[];
    assert.equal(samples.length, 1);
    assert.equal(samples[0].energy_wh, 2500);
    assert.equal(samples[0].power_w, 7200);
    assert.equal(samples[0].offered_a, 32);

    await cp.call('StopTransaction', {
      transactionId: start.transactionId,
      idTag: 'DRIVER-1', meterStop: 9000,
      timestamp: new Date().toISOString(), reason: 'Local',
    });

    const txn = db.prepare('SELECT * FROM transactions WHERE id = ?')
      .get(start.transactionId) as any;
    assert.equal(txn.meter_stop_wh, 9000);
    assert.equal(txn.energy_wh, 8000, 'energy is stop minus start');
    assert.equal(txn.stop_reason, 'Local');
    assert.ok(txn.stopped_at > 0);
    cp.close();
  });

  test('a meter register that goes backwards cannot produce negative energy', async () => {
    seedTag('DRIVER-1');
    const cp = new FakeCharger();
    await cp.connect();
    const start = await cp.call('StartTransaction', {
      connectorId: 1, idTag: 'DRIVER-1', meterStart: 50_000,
      timestamp: new Date().toISOString(),
    });
    // Meter replaced or charger reset mid-session: stop reads lower than start.
    await cp.call('StopTransaction', {
      transactionId: start.transactionId, meterStop: 10,
      timestamp: new Date().toISOString(),
    });
    const txn = db.prepare('SELECT energy_wh FROM transactions WHERE id = ?')
      .get(start.transactionId) as any;
    assert.equal(txn.energy_wh, 0, 'clamped at zero, not negative');
    cp.close();
  });

  test('an unauthorised tag gets a transactionId but a refusing idTagInfo', async () => {
    seedTag('BLOCKED', 'Blocked');
    const cp = new FakeCharger();
    await cp.connect();
    const start = await cp.call('StartTransaction', {
      connectorId: 1, idTag: 'BLOCKED', meterStart: 0,
      timestamp: new Date().toISOString(),
    });
    assert.equal(start.idTagInfo.status, 'Blocked');
    assert.equal(start.transactionId, 0);
    assert.equal(
      (db.prepare('SELECT COUNT(*) AS n FROM transactions').get() as any).n, 0,
      'no transaction row is created for a refused start',
    );
    cp.close();
  });

  test('a reboot mid-session closes the stale transaction instead of leaving two open', async () => {
    seedTag('DRIVER-1');
    const cp = new FakeCharger();
    await cp.connect();

    const first = await cp.call('StartTransaction', {
      connectorId: 1, idTag: 'DRIVER-1', meterStart: 100,
      timestamp: new Date().toISOString(),
    });
    // Charger reboots and starts a new session without stopping the old one.
    const second = await cp.call('StartTransaction', {
      connectorId: 1, idTag: 'DRIVER-1', meterStart: 5000,
      timestamp: new Date().toISOString(),
    });

    const open = db.prepare(
      'SELECT id FROM transactions WHERE charger_id = ? AND stopped_at IS NULL',
    ).all(CHARGER_ID) as any[];
    assert.equal(open.length, 1, 'exactly one open transaction');
    assert.equal(open[0].id, second.transactionId);

    const stale = db.prepare('SELECT * FROM transactions WHERE id = ?')
      .get(first.transactionId) as any;
    assert.equal(stale.stop_reason, 'PowerLoss');
    cp.close();
  });

  test('stopping twice is idempotent', async () => {
    seedTag('DRIVER-1');
    const cp = new FakeCharger();
    await cp.connect();
    const start = await cp.call('StartTransaction', {
      connectorId: 1, idTag: 'DRIVER-1', meterStart: 0,
      timestamp: new Date().toISOString(),
    });
    await cp.call('StopTransaction', {
      transactionId: start.transactionId, meterStop: 5000,
      timestamp: new Date().toISOString(),
    });
    await cp.call('StopTransaction', {
      transactionId: start.transactionId, meterStop: 999_999,
      timestamp: new Date().toISOString(),
    });
    const txn = db.prepare('SELECT * FROM transactions WHERE id = ?')
      .get(start.transactionId) as any;
    assert.equal(txn.energy_wh, 5000, 'the second stop did not overwrite the first');
    cp.close();
  });

  test('a stop for an unknown transaction is acknowledged, not an error', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    // A charger replaying a session from before this database existed.
    const res = await cp.call('StopTransaction', {
      transactionId: 999_999, meterStop: 1000,
      timestamp: new Date().toISOString(),
    });
    assert.equal(res.__error, undefined, 'acknowledged so the charger stops retrying');
    cp.close();
  });
});

describe('status', () => {
  test('a connector status updates the charger row', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    await cp.call('StatusNotification', {
      connectorId: 1, errorCode: 'NoError', status: 'Charging',
      timestamp: new Date().toISOString(),
    });
    const row = db.prepare('SELECT status, error_code FROM chargers WHERE id = ?')
      .get(CHARGER_ID) as any;
    assert.equal(row.status, 'Charging');
    assert.equal(row.error_code, null);
    cp.close();
  });

  test('connectorId 0 does not overwrite the connector status', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    await cp.call('StatusNotification', {
      connectorId: 1, errorCode: 'NoError', status: 'Charging',
      timestamp: new Date().toISOString(),
    });
    // A charge-point-level notice must not clobber the live session state.
    await cp.call('StatusNotification', {
      connectorId: 0, errorCode: 'NoError', status: 'Available',
      timestamp: new Date().toISOString(),
    });
    const row = db.prepare('SELECT status FROM chargers WHERE id = ?').get(CHARGER_ID) as any;
    assert.equal(row.status, 'Charging');
    cp.close();
  });

  test('a fault records the vendor error code', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    await cp.call('StatusNotification', {
      connectorId: 1, errorCode: 'GroundFailure', status: 'Faulted',
      vendorErrorCode: 'ResidualCurrent', vendorId: 'EVREST',
      timestamp: new Date().toISOString(),
    });
    const row = db.prepare('SELECT * FROM chargers WHERE id = ?').get(CHARGER_ID) as any;
    assert.equal(row.status, 'Faulted');
    assert.equal(row.error_code, 'GroundFailure');
    assert.equal(row.vendor_error, 'ResidualCurrent');
    cp.close();
  });
});

describe('server-initiated calls', () => {
  test('RemoteStartTransaction reaches the charger and its reply resolves', async () => {
    seedTag('DRIVER-1');
    const cp = new FakeCharger();
    await cp.connect();
    await cp.call('BootNotification', { chargePointVendor: 'EVREST', chargePointModel: 'L2' });

    const session = server.getSession(CHARGER_ID)!;
    const pending = session.call('RemoteStartTransaction', { connectorId: 1, idTag: 'DRIVER-1' });

    const call = await cp.waitForCall('RemoteStartTransaction');
    assert.equal(call.payload.idTag, 'DRIVER-1');
    cp.reply(call.id, { status: 'Accepted' });

    assert.deepEqual(await pending, { status: 'Accepted' });
    cp.close();
  });

  test('a call to an offline charger rejects rather than hanging', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    const session = server.getSession(CHARGER_ID)!;
    cp.close();
    await new Promise((r) => setTimeout(r, 100));
    await assert.rejects(() => session.call('Reset', { type: 'Soft' }), /not connected/);
  });
});

describe('malformed input', () => {
  test('an unknown action gets a NotImplemented CALLERROR', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    const res = await cp.call('SomethingInvented', {});
    assert.equal(res.__error, 'NotImplemented');
    cp.close();
  });

  test('a non-JSON frame does not take the server down', async () => {
    const cp = new FakeCharger();
    await cp.connect();
    cp.sendRaw('this is not json');
    cp.sendRaw('[]');
    cp.sendRaw('{"not":"an array"}');
    cp.sendRaw('[2,"id"]');
    await new Promise((r) => setTimeout(r, 100));

    // Still alive and still serving.
    const res = await cp.call('Heartbeat', {});
    assert.ok(Date.parse(res.currentTime) > 0);
    cp.close();
  });
});

describe('events', () => {
  test('a transaction start is published on the bus', async () => {
    seedTag('DRIVER-1');
    const seen: any[] = [];
    const unsubscribe = events.subscribe((e) => { if (e.event === 'transaction.start') seen.push(e); });

    const cp = new FakeCharger();
    await cp.connect();
    await cp.call('StartTransaction', {
      connectorId: 1, idTag: 'DRIVER-1', meterStart: 0,
      timestamp: new Date().toISOString(),
    });

    assert.equal(seen.length, 1);
    assert.equal(seen[0].data.chargerId, CHARGER_ID);
    assert.equal(seen[0].data.idTag, 'DRIVER-1');
    unsubscribe();
    cp.close();
  });

  test('a listener that throws does not break the others', async () => {
    const seen: any[] = [];
    const un1 = events.subscribe(() => { throw new Error('boom'); });
    const un2 = events.subscribe((e) => seen.push(e));

    const cp = new FakeCharger();
    await cp.connect();
    await cp.call('Heartbeat', {});
    await cp.call('StatusNotification', {
      connectorId: 1, errorCode: 'NoError', status: 'Available',
      timestamp: new Date().toISOString(),
    });

    assert.ok(seen.some((e) => e.event === 'charger.status'), 'second listener still ran');
    un1(); un2();
    cp.close();
  });
});

describe('API does not leak credentials', () => {
  test('charger rows never carry auth_key_hash', async () => {
    const { createRestApi } = await import('../src/api/rest.js');
    const app = createRestApi({ db, ocpp: server, jwtSecret: 'x'.repeat(40) });

    // Sign a token directly rather than going through the login route, so this
    // test does not depend on a seeded user.
    const { signToken } = await import('../src/auth/jwt.js');
    const token = signToken('x'.repeat(40), {
      sub: 'test-admin', email: 'a@b.c', role: 'admin',
    });

    const srv = app.listen(19299);
    try {
      const list = await fetch('http://127.0.0.1:19299/api/chargers', {
        headers: { Authorization: `Bearer ${token}` },
      }).then((r) => r.json()) as any[];

      assert.ok(list.length > 0, 'the seeded charger is listed');
      for (const c of list) {
        assert.equal(c.auth_key_hash, undefined, 'no key hash in the list response');
      }

      const one = await fetch(`http://127.0.0.1:19299/api/chargers/${CHARGER_ID}`, {
        headers: { Authorization: `Bearer ${token}` },
      }).then((r) => r.json()) as any;
      assert.equal(one.auth_key_hash, undefined, 'no key hash in the detail response');
    } finally {
      await new Promise<void>((r) => srv.close(() => r()));
    }
  });
});
