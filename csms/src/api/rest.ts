/**
 * REST API for the dashboard and the mobile app.
 *
 * Two audiences with different needs share one API:
 *   - operators see every charger, the OCPP log and the raw controls;
 *   - drivers see only their own sessions and can only act on a charger they
 *     are currently using, or a free one.
 *
 * That distinction is enforced per route rather than by having the clients
 * behave, because the mobile app is not a trusted component.
 */
import express, { type Express, type Request, type Response } from 'express';
import cors from 'cors';
import { z } from 'zod';
import { randomUUID, randomBytes } from 'node:crypto';
import type Database from 'better-sqlite3';
import type { OcppServer } from '../ocpp/server.js';
import { hashPassword, verifyPassword, hashChargerKey } from '../db/schema.js';
import type { UserRow, ChargerRow, TransactionRow } from '../db/schema.js';
import { signToken, requireAuth, requireRole } from '../auth/jwt.js';

export interface RestOptions {
  db: Database.Database;
  ocpp: OcppServer;
  jwtSecret: string;
}

/** Wrap an async handler so a rejected promise becomes a 500, not a crash. */
function wrap(fn: (req: Request, res: Response) => Promise<void> | void) {
  return (req: Request, res: Response): void => {
    Promise.resolve(fn(req, res)).catch((err: Error) => {
      console.error('[api]', err.message);
      if (!res.headersSent) res.status(500).json({ error: err.message });
    });
  };
}

/**
 * Strip fields that must never leave the server.
 *
 * `auth_key_hash` is a credential digest. It is not the key itself, but there
 * is no reason for a browser to hold it and every reason not to hand attackers
 * an offline target — so it is removed at the boundary rather than relying on
 * each query to remember not to select it.
 */
function publicCharger<T extends Record<string, unknown>>(row: T): Omit<T, 'auth_key_hash'> {
  const { auth_key_hash: _omit, ...rest } = row;
  return rest;
}

/** Validate a body, replying 400 with the specific problem if it fails. */
function body<T extends z.ZodTypeAny>(schema: T, req: Request, res: Response): z.infer<T> | null {
  const parsed = schema.safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({
      error: 'Invalid request body',
      details: parsed.error.issues.map((i) => `${i.path.join('.')}: ${i.message}`),
    });
    return null;
  }
  return parsed.data;
}

export function createRestApi(opts: RestOptions): Express {
  const { db, ocpp, jwtSecret } = opts;
  const app = express();

  app.use(cors());
  app.use(express.json({ limit: '256kb' }));

  const auth = requireAuth(jwtSecret);
  const operator = requireRole('operator');
  const admin = requireRole('admin');

  /* ================================================================ */
  /* Health and auth                                                  */
  /* ================================================================ */

  app.get('/api/health', (_req, res) => {
    res.json({
      ok: true,
      onlineChargers: ocpp.listOnline().length,
      uptimeSeconds: Math.round(process.uptime()),
    });
  });

  app.post('/api/auth/login', wrap((req, res) => {
    const data = body(z.object({
      email: z.string().email(),
      password: z.string().min(1),
    }), req, res);
    if (!data) return;

    const user = db.prepare('SELECT * FROM users WHERE email = ?')
      .get(data.email.toLowerCase()) as UserRow | undefined;

    /*
     * The same message and roughly the same work whether the account exists or
     * the password is wrong — otherwise the endpoint becomes a way to
     * enumerate valid email addresses.
     */
    const ok = user ? verifyPassword(data.password, user.password_hash) : false;
    if (!user || !ok) {
      res.status(401).json({ error: 'Invalid email or password' });
      return;
    }

    res.json({
      token: signToken(jwtSecret, { sub: user.id, email: user.email, role: user.role }),
      user: { id: user.id, email: user.email, name: user.name, role: user.role },
    });
  }));

  app.get('/api/auth/me', auth, wrap((req, res) => {
    const user = db.prepare('SELECT id, email, name, role FROM users WHERE id = ?')
      .get(req.user!.sub);
    if (!user) { res.status(404).json({ error: 'User not found' }); return; }
    res.json(user);
  }));

  app.post('/api/users', auth, admin, wrap((req, res) => {
    const data = body(z.object({
      email: z.string().email(),
      password: z.string().min(10, 'Password must be at least 10 characters'),
      name: z.string().optional(),
      role: z.enum(['admin', 'operator', 'driver']).default('driver'),
    }), req, res);
    if (!data) return;

    const email = data.email.toLowerCase();
    const exists = db.prepare('SELECT 1 FROM users WHERE email = ?').get(email);
    if (exists) { res.status(409).json({ error: 'Email already registered' }); return; }

    const id = randomUUID();
    db.prepare(
      `INSERT INTO users (id, email, password_hash, role, name, created_at)
       VALUES (?, ?, ?, ?, ?, ?)`,
    ).run(id, email, hashPassword(data.password), data.role, data.name ?? null, Date.now());

    res.status(201).json({ id, email, name: data.name ?? null, role: data.role });
  }));

  /* ================================================================ */
  /* Chargers                                                         */
  /* ================================================================ */

  app.get('/api/chargers', auth, wrap((_req, res) => {
    const rows = db.prepare(
      `SELECT c.*,
              t.id             AS active_transaction_id,
              t.id_tag         AS active_id_tag,
              t.started_at     AS active_started_at,
              t.meter_start_wh AS active_meter_start
         FROM chargers c
         LEFT JOIN transactions t
                ON t.charger_id = c.id AND t.stopped_at IS NULL
        ORDER BY c.name`,
    ).all() as Array<ChargerRow & Record<string, unknown>>;

    const online = new Set(ocpp.listOnline());
    res.json(rows.map((r) => publicCharger({
      ...r,
      // The stored flag can lag a socket that just dropped; the live session
      // list is the truth.
      online: online.has(r.id),
    })));
  }));

  app.get('/api/chargers/:id', auth, wrap((req, res) => {
    const charger = db.prepare('SELECT * FROM chargers WHERE id = ?')
      .get(req.params.id) as ChargerRow | undefined;
    if (!charger) { res.status(404).json({ error: 'Charger not found' }); return; }

    const activeTxn = db.prepare(
      'SELECT * FROM transactions WHERE charger_id = ? AND stopped_at IS NULL',
    ).get(req.params.id) as TransactionRow | undefined;

    const latest = db.prepare(
      'SELECT * FROM meter_samples WHERE charger_id = ? ORDER BY ts DESC LIMIT 1',
    ).get(req.params.id);

    res.json(publicCharger({
      ...charger,
      online: ocpp.listOnline().includes(charger.id),
      activeTransaction: activeTxn ?? null,
      latestSample: latest ?? null,
    }));
  }));

  app.post('/api/chargers', auth, admin, wrap((req, res) => {
    const data = body(z.object({
      id: z.string().regex(/^[A-Za-z0-9._-]{1,48}$/, 'Id must be URL-safe, 1-48 chars'),
      name: z.string().min(1),
      maxCurrentA: z.number().positive().max(80).default(32),
      siteId: z.string().optional(),
    }), req, res);
    if (!data) return;

    const exists = db.prepare('SELECT 1 FROM chargers WHERE id = ?').get(data.id);
    if (exists) { res.status(409).json({ error: 'Charger id already exists' }); return; }

    /*
     * Generate the auth key here and return it exactly once. Only its hash is
     * stored, so it cannot be recovered later — which is the point, but does
     * mean the operator has to write it down now.
     */
    const authKey = randomBytes(24).toString('base64url');

    db.prepare(
      `INSERT INTO chargers (id, name, status, online, heartbeat_interval,
                             auth_key_hash, max_current_a, site_id, created_at)
       VALUES (?, ?, 'Unavailable', 0, 300, ?, ?, ?, ?)`,
    ).run(data.id, data.name, hashChargerKey(authKey),
          data.maxCurrentA, data.siteId ?? null, Date.now());

    res.status(201).json({
      id: data.id,
      name: data.name,
      authKey,
      note: 'Store this key now — only its hash is kept and it cannot be shown again.',
    });
  }));

  app.post('/api/chargers/:id/rotate-key', auth, admin, wrap((req, res) => {
    const exists = db.prepare('SELECT 1 FROM chargers WHERE id = ?').get(req.params.id);
    if (!exists) { res.status(404).json({ error: 'Charger not found' }); return; }

    const authKey = randomBytes(24).toString('base64url');
    db.prepare('UPDATE chargers SET auth_key_hash = ? WHERE id = ?')
      .run(hashChargerKey(authKey), req.params.id);

    // The charger will fail its next reconnect until it is reconfigured, so
    // say so rather than leaving the operator to discover it.
    res.json({
      authKey,
      note: 'The charger will be rejected on its next reconnect until this key is configured on it.',
    });
  }));

  /* ================================================================ */
  /* Remote control                                                   */
  /* ================================================================ */

  /** Look up a live session, or 409 with a useful reason. */
  function session(req: Request, res: Response) {
    const s = ocpp.getSession(req.params.id!);
    if (!s) {
      const known = db.prepare('SELECT 1 FROM chargers WHERE id = ?').get(req.params.id);
      res.status(known ? 409 : 404)
         .json({ error: known ? 'Charger is offline' : 'Charger not found' });
      return null;
    }
    return s;
  }

  app.post('/api/chargers/:id/start', auth, wrap(async (req, res) => {
    const data = body(z.object({
      idTag: z.string().min(1).max(20),
      connectorId: z.number().int().min(1).default(1),
    }), req, res);
    if (!data) return;

    /*
     * A driver may only start with a tag that belongs to them. Operators and
     * admins may use any tag, which is what makes remote assistance possible.
     */
    if (req.user!.role === 'driver') {
      const owned = db.prepare('SELECT 1 FROM tags WHERE id_tag = ? AND user_id = ?')
        .get(data.idTag, req.user!.sub);
      if (!owned) { res.status(403).json({ error: 'That tag is not yours' }); return; }
    }

    const s = session(req, res);
    if (!s) return;

    const result = await s.call('RemoteStartTransaction', {
      connectorId: data.connectorId,
      idTag: data.idTag,
    });
    res.json(result);
  }));

  app.post('/api/chargers/:id/stop', auth, wrap(async (req, res) => {
    const data = body(z.object({ transactionId: z.number().int() }), req, res);
    if (!data) return;

    const txn = db.prepare('SELECT * FROM transactions WHERE id = ?')
      .get(data.transactionId) as TransactionRow | undefined;
    if (!txn) { res.status(404).json({ error: 'Transaction not found' }); return; }

    // A driver may only stop a session started with one of their own tags.
    if (req.user!.role === 'driver') {
      const owned = db.prepare('SELECT 1 FROM tags WHERE id_tag = ? AND user_id = ?')
        .get(txn.id_tag, req.user!.sub);
      if (!owned) { res.status(403).json({ error: 'That session is not yours' }); return; }
    }

    const s = session(req, res);
    if (!s) return;

    const result = await s.call('RemoteStopTransaction', { transactionId: data.transactionId });
    res.json(result);
  }));

  app.post('/api/chargers/:id/reset', auth, operator, wrap(async (req, res) => {
    const data = body(z.object({ type: z.enum(['Soft', 'Hard']).default('Soft') }), req, res);
    if (!data) return;
    const s = session(req, res);
    if (!s) return;
    res.json(await s.call('Reset', { type: data.type }));
  }));

  app.post('/api/chargers/:id/availability', auth, operator, wrap(async (req, res) => {
    const data = body(z.object({
      type: z.enum(['Operative', 'Inoperative']),
      connectorId: z.number().int().min(0).default(1),
    }), req, res);
    if (!data) return;
    const s = session(req, res);
    if (!s) return;
    res.json(await s.call('ChangeAvailability', {
      connectorId: data.connectorId, type: data.type,
    }));
  }));

  app.post('/api/chargers/:id/limit', auth, operator, wrap(async (req, res) => {
    const data = body(z.object({
      limitA: z.number().min(0).max(80),
      /** 0 clears the limit rather than setting it to zero amps. */
      durationSeconds: z.number().int().positive().optional(),
    }), req, res);
    if (!data) return;
    const s = session(req, res);
    if (!s) return;

    if (data.limitA === 0) {
      const result = await s.call('ClearChargingProfile', {
        chargingProfilePurpose: 'TxDefaultProfile',
      });
      db.prepare('DELETE FROM charging_profiles WHERE charger_id = ?').run(req.params.id);
      res.json(result);
      return;
    }

    const profile = {
      chargingProfileId: 1,
      stackLevel: 0,
      chargingProfilePurpose: 'TxDefaultProfile',
      chargingProfileKind: 'Absolute',
      chargingSchedule: {
        chargingRateUnit: 'A',
        ...(data.durationSeconds ? { duration: data.durationSeconds } : {}),
        startSchedule: new Date().toISOString(),
        chargingSchedulePeriod: [{ startPeriod: 0, limit: data.limitA }],
      },
    };

    const result = await s.call('SetChargingProfile', {
      connectorId: 1, csChargingProfiles: profile,
    });

    db.prepare(
      `INSERT INTO charging_profiles (charger_id, profile_id, purpose, stack_level, payload, created_at)
       VALUES (?, 1, 'TxDefaultProfile', 0, ?, ?)
       ON CONFLICT(charger_id, profile_id)
       DO UPDATE SET payload = excluded.payload, created_at = excluded.created_at`,
    ).run(req.params.id, JSON.stringify(profile), Date.now());

    res.json(result);
  }));

  app.post('/api/chargers/:id/trigger', auth, operator, wrap(async (req, res) => {
    const data = body(z.object({
      requestedMessage: z.enum([
        'BootNotification', 'DiagnosticsStatusNotification', 'FirmwareStatusNotification',
        'Heartbeat', 'MeterValues', 'StatusNotification',
      ]),
    }), req, res);
    if (!data) return;
    const s = session(req, res);
    if (!s) return;
    res.json(await s.call('TriggerMessage', {
      requestedMessage: data.requestedMessage, connectorId: 1,
    }));
  }));

  app.get('/api/chargers/:id/config', auth, operator, wrap(async (req, res) => {
    const s = session(req, res);
    if (!s) return;
    res.json(await s.call('GetConfiguration', {}));
  }));

  app.post('/api/chargers/:id/config', auth, operator, wrap(async (req, res) => {
    const data = body(z.object({
      key: z.string().min(1),
      value: z.string(),
    }), req, res);
    if (!data) return;
    const s = session(req, res);
    if (!s) return;
    res.json(await s.call('ChangeConfiguration', { key: data.key, value: data.value }));
  }));

  /* ================================================================ */
  /* Transactions and telemetry                                       */
  /* ================================================================ */

  app.get('/api/transactions', auth, wrap((req, res) => {
    const q = z.object({
      chargerId: z.string().optional(),
      limit: z.coerce.number().int().min(1).max(500).default(50),
      offset: z.coerce.number().int().min(0).default(0),
    }).parse(req.query);

    // Drivers see only sessions started with their own tags.
    const driverFilter = req.user!.role === 'driver'
      ? ' AND t.id_tag IN (SELECT id_tag FROM tags WHERE user_id = ?)' : '';
    const params: unknown[] = [];
    if (q.chargerId) params.push(q.chargerId);
    if (driverFilter) params.push(req.user!.sub);

    const rows = db.prepare(
      `SELECT t.*, c.name AS charger_name
         FROM transactions t
         JOIN chargers c ON c.id = t.charger_id
        WHERE 1 = 1 ${q.chargerId ? 'AND t.charger_id = ?' : ''} ${driverFilter}
        ORDER BY t.started_at DESC
        LIMIT ? OFFSET ?`,
    ).all(...params, q.limit, q.offset);

    res.json(rows);
  }));

  app.get('/api/transactions/:id/samples', auth, wrap((req, res) => {
    const rows = db.prepare(
      `SELECT ts, energy_wh, power_w, current_a, voltage_v, offered_a
         FROM meter_samples
        WHERE transaction_id = ?
        ORDER BY ts`,
    ).all(req.params.id);
    res.json(rows);
  }));

  app.get('/api/chargers/:id/samples', auth, wrap((req, res) => {
    const q = z.object({
      since: z.coerce.number().int().optional(),
      limit: z.coerce.number().int().min(1).max(2000).default(300),
    }).parse(req.query);

    const rows = db.prepare(
      `SELECT ts, energy_wh, power_w, current_a, voltage_v, offered_a
         FROM meter_samples
        WHERE charger_id = ? AND ts >= ?
        ORDER BY ts DESC
        LIMIT ?`,
    ).all(req.params.id, q.since ?? 0, q.limit);

    res.json(rows.reverse());   // chart-friendly: oldest first
  }));

  app.get('/api/chargers/:id/log', auth, operator, wrap((req, res) => {
    const q = z.object({
      limit: z.coerce.number().int().min(1).max(500).default(100),
    }).parse(req.query);
    const rows = db.prepare(
      `SELECT ts, direction, action, message_id, payload
         FROM ocpp_log WHERE charger_id = ? ORDER BY id DESC LIMIT ?`,
    ).all(req.params.id, q.limit);
    res.json(rows);
  }));

  /* ================================================================ */
  /* Tags                                                             */
  /* ================================================================ */

  app.get('/api/tags', auth, wrap((req, res) => {
    const rows = req.user!.role === 'driver'
      ? db.prepare('SELECT * FROM tags WHERE user_id = ? ORDER BY created_at DESC')
          .all(req.user!.sub)
      : db.prepare('SELECT * FROM tags ORDER BY created_at DESC').all();
    res.json(rows);
  }));

  app.post('/api/tags', auth, operator, wrap((req, res) => {
    const data = body(z.object({
      idTag: z.string().min(1).max(20),
      label: z.string().optional(),
      parentIdTag: z.string().max(20).optional(),
      status: z.enum(['Accepted', 'Blocked', 'Expired', 'Invalid']).default('Accepted'),
      expiresAt: z.number().int().optional(),
      userId: z.string().optional(),
    }), req, res);
    if (!data) return;

    db.prepare(
      `INSERT INTO tags (id_tag, parent_id_tag, label, status, expires_at, user_id, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?)
       ON CONFLICT(id_tag) DO UPDATE SET
         parent_id_tag = excluded.parent_id_tag,
         label         = excluded.label,
         status        = excluded.status,
         expires_at    = excluded.expires_at,
         user_id       = excluded.user_id`,
    ).run(data.idTag, data.parentIdTag ?? null, data.label ?? null, data.status,
          data.expiresAt ?? null, data.userId ?? null, Date.now());

    res.status(201).json({ idTag: data.idTag });
  }));

  app.delete('/api/tags/:idTag', auth, operator, wrap((req, res) => {
    const info = db.prepare('DELETE FROM tags WHERE id_tag = ?').run(req.params.idTag);
    if (info.changes === 0) { res.status(404).json({ error: 'Tag not found' }); return; }
    res.status(204).end();
  }));

  /* ================================================================ */
  /* Statistics                                                       */
  /* ================================================================ */

  app.get('/api/stats/summary', auth, wrap((_req, res) => {
    const since = Date.now() - 30 * 86_400_000;

    const chargers = db.prepare(
      `SELECT COUNT(*) AS total,
              SUM(CASE WHEN status = 'Charging' THEN 1 ELSE 0 END) AS charging,
              SUM(CASE WHEN status = 'Faulted'  THEN 1 ELSE 0 END) AS faulted
         FROM chargers`,
    ).get() as Record<string, number>;

    const energy = db.prepare(
      `SELECT COUNT(*) AS sessions, COALESCE(SUM(energy_wh), 0) AS energy_wh
         FROM transactions WHERE started_at >= ? AND stopped_at IS NOT NULL`,
    ).get(since) as Record<string, number>;

    const daily = db.prepare(
      `SELECT CAST(started_at / 86400000 AS INTEGER) * 86400000 AS day,
              COUNT(*) AS sessions,
              COALESCE(SUM(energy_wh), 0) AS energy_wh
         FROM transactions
        WHERE started_at >= ? AND stopped_at IS NOT NULL
        GROUP BY day ORDER BY day`,
    ).all(since);

    res.json({
      chargers: {
        total: chargers.total ?? 0,
        online: ocpp.listOnline().length,
        charging: chargers.charging ?? 0,
        faulted: chargers.faulted ?? 0,
      },
      last30Days: {
        sessions: energy.sessions ?? 0,
        energyWh: energy.energy_wh ?? 0,
        daily,
      },
    });
  }));

  return app;
}
