/**
 * OCPP 1.6J Central System.
 *
 * One WebSocket listener that chargers connect to. The charge point id is the
 * last path segment, which is the convention every 1.6J charger uses:
 *
 *     wss://csms.example.com:9220/ocpp/EVREST-0001
 *
 * Each connection gets a ChargerSession that owns its socket, its pending
 * outbound CALLs and its liveness timer. Nothing else in the process touches
 * a socket directly.
 */
import { WebSocketServer, WebSocket } from 'ws';
import { createServer, type IncomingMessage, type Server } from 'node:http';
import type { Duplex } from 'node:stream';
import { randomUUID } from 'node:crypto';
import type Database from 'better-sqlite3';
import {
  MessageType, OcppError, OcppFrameError, parseFrame, ocppNow,
  INBOUND_SCHEMAS, type InboundAction, type OutboundAction,
} from './types.js';
import { handleInbound } from './handlers.js';
import { verifyChargerKey } from '../db/schema.js';
import type { EventBus } from '../api/events.js';

/** How long a charger may be silent before we consider it gone. */
const LIVENESS_GRACE_FACTOR = 2.5;
const MIN_LIVENESS_MS = 60_000;
/** How long to wait for a charger to answer a CALL we sent. */
const CALL_TIMEOUT_MS = 30_000;
/** Cap on stored log rows per charger; trimmed opportunistically. */
const LOG_RETENTION_ROWS = 2000;

interface PendingCall {
  action: OutboundAction;
  resolve: (payload: unknown) => void;
  reject: (err: Error) => void;
  timer: NodeJS.Timeout;
}

export class ChargerSession {
  readonly pending = new Map<string, PendingCall>();
  private livenessTimer: NodeJS.Timeout | null = null;
  /** Seconds; updated from whatever we told the charger in BootNotification. */
  heartbeatInterval = 300;

  constructor(
    readonly chargerId: string,
    readonly socket: WebSocket,
    private readonly server: OcppServer,
  ) {}

  get isOpen(): boolean {
    return this.socket.readyState === WebSocket.OPEN;
  }

  /**
   * Restart the liveness timer.
   *
   * A charger is declared offline when nothing arrives for 2.5 heartbeat
   * intervals. Using the negotiated interval rather than a fixed timeout means
   * a charger configured for a 15-minute heartbeat is not marked offline every
   * 60 seconds — a real problem on metered cellular links where operators
   * lengthen the interval deliberately.
   */
  touch(): void {
    if (this.livenessTimer) clearTimeout(this.livenessTimer);
    const ms = Math.max(this.heartbeatInterval * 1000 * LIVENESS_GRACE_FACTOR, MIN_LIVENESS_MS);
    this.livenessTimer = setTimeout(() => {
      this.server.log(`${this.chargerId}: silent for ${Math.round(ms / 1000)}s, closing`);
      this.socket.close(1001, 'No heartbeat');
    }, ms);
  }

  /** Send a CALL and resolve with the charger's payload. */
  call(action: OutboundAction, payload: Record<string, unknown>): Promise<unknown> {
    return new Promise((resolve, reject) => {
      if (!this.isOpen) {
        reject(new Error('Charger is not connected'));
        return;
      }
      const id = randomUUID();
      const frame = JSON.stringify([MessageType.CALL, id, action, payload]);

      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${action} timed out after ${CALL_TIMEOUT_MS / 1000}s`));
      }, CALL_TIMEOUT_MS);

      this.pending.set(id, { action, resolve, reject, timer });
      this.socket.send(frame);
      this.server.record(this.chargerId, 'out', action, id, frame);
    });
  }

  sendRaw(frame: string, action: string | null, id: string): void {
    if (!this.isOpen) return;
    this.socket.send(frame);
    this.server.record(this.chargerId, 'out', action, id, frame);
  }

  dispose(): void {
    if (this.livenessTimer) clearTimeout(this.livenessTimer);
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(new Error('Charger disconnected'));
    }
    this.pending.clear();
  }
}

export interface OcppServerOptions {
  port: number;
  db: Database.Database;
  events: EventBus;
  /** Reject chargers that do not present valid HTTP Basic credentials. */
  requireAuth: boolean;
  logger?: (msg: string) => void;
}

export class OcppServer {
  private readonly wss: WebSocketServer;
  private readonly http: Server;
  private readonly sessions = new Map<string, ChargerSession>();
  readonly db: Database.Database;
  readonly events: EventBus;
  private readonly requireAuth: boolean;
  private readonly logger: (msg: string) => void;

  constructor(opts: OcppServerOptions) {
    this.db = opts.db;
    this.events = opts.events;
    this.requireAuth = opts.requireAuth;
    this.logger = opts.logger ?? ((m) => console.log(`[ocpp] ${m}`));

    /*
     * noServer plus a hand-rolled upgrade handler, rather than letting ws own
     * the port. Authentication has to happen BEFORE the handshake completes:
     * closing the socket from the 'connection' event still lets the client see
     * a successful open, and a charger that believes it connected will sit
     * there retrying instead of surfacing an auth failure. Rejecting during
     * the upgrade sends a real HTTP 401 that shows up in the charger's log.
     */
    this.wss = new WebSocketServer({
      noServer: true,
      // Only the subprotocol we actually speak. Accepting anything offered
      // would let a 2.0.1 charger connect and then fail confusingly on its
      // first message.
      handleProtocols: (protocols) => (protocols.has('ocpp1.6') ? 'ocpp1.6' : false),
    });

    this.http = createServer((_req, res) => {
      // Nothing but WebSocket upgrades belongs on this port.
      res.writeHead(426, { 'Content-Type': 'text/plain', Upgrade: 'websocket' });
      res.end('This endpoint speaks OCPP 1.6J over WebSocket only.\n');
    });

    this.http.on('upgrade', (req, socket, head) => this.onUpgrade(req, socket, head));
    this.http.listen(opts.port, () => this.log(`listening on :${opts.port}`));
  }

  /** Reject an upgrade with a real HTTP response the charger can log. */
  private denyUpgrade(socket: Duplex, status: number, reason: string): void {
    const body = `${reason}\n`;
    socket.write(
      `HTTP/1.1 ${status} ${reason}\r\n` +
      'Connection: close\r\n' +
      'Content-Type: text/plain\r\n' +
      `Content-Length: ${Buffer.byteLength(body)}\r\n` +
      (status === 401 ? 'WWW-Authenticate: Basic realm="ocpp"\r\n' : '') +
      `\r\n${body}`,
    );
    socket.destroy();
  }

  private onUpgrade(req: IncomingMessage, socket: Duplex, head: Buffer): void {
    const chargerId = this.chargerIdFromUrl(req.url);
    if (!chargerId) {
      this.log(`rejected upgrade: bad path ${req.url}`);
      this.denyUpgrade(socket, 400, 'Bad Request');
      return;
    }
    if (!this.authorise(chargerId, req)) {
      this.log(`rejected ${chargerId}: authentication failed`);
      // No detail in the response: telling an unauthenticated peer whether the
      // id exists is free reconnaissance.
      this.denyUpgrade(socket, 401, 'Unauthorized');
      return;
    }
    this.wss.handleUpgrade(req, socket, head, (ws) => this.onConnection(ws, req, chargerId));
  }

  log(msg: string): void { this.logger(msg); }

  getSession(chargerId: string): ChargerSession | undefined {
    return this.sessions.get(chargerId);
  }

  listOnline(): string[] {
    return [...this.sessions.keys()];
  }

  async close(): Promise<void> {
    for (const s of this.sessions.values()) {
      s.dispose();
      s.socket.close(1001, 'Server shutting down');
    }
    this.sessions.clear();
    await new Promise<void>((resolve) => this.wss.close(() => resolve()));
    await new Promise<void>((resolve) => this.http.close(() => resolve()));
  }

  /* ---------------------------------------------------------------- */

  private chargerIdFromUrl(url: string | undefined): string | null {
    if (!url) return null;
    const path = url.split('?')[0] ?? '';
    const segments = path.split('/').filter(Boolean);
    const last = segments[segments.length - 1];
    if (!last) return null;

    // The id ends up in file paths and log lines, and is used as a primary key.
    // Constrain it rather than trusting whatever the charger put in its URL.
    return /^[A-Za-z0-9._-]{1,48}$/.test(last) ? last : null;
  }

  /**
   * Check HTTP Basic credentials against the charger's stored key hash.
   *
   * A charger with no stored hash is treated as unprovisioned: allowed only
   * when auth is not required, so a bench setup works but a production one
   * cannot silently accept an unknown device.
   */
  private authorise(chargerId: string, req: IncomingMessage): boolean {
    if (!this.requireAuth) return true;

    const header = req.headers.authorization;
    if (!header?.startsWith('Basic ')) return false;

    let decoded: string;
    try {
      decoded = Buffer.from(header.slice(6), 'base64').toString('utf8');
    } catch {
      return false;
    }
    const sep = decoded.indexOf(':');
    if (sep < 0) return false;

    const user = decoded.slice(0, sep);
    const pass = decoded.slice(sep + 1);
    if (user !== chargerId) return false;

    const row = this.db
      .prepare('SELECT auth_key_hash FROM chargers WHERE id = ?')
      .get(chargerId) as { auth_key_hash: string | null } | undefined;

    if (!row?.auth_key_hash) return false;
    return verifyChargerKey(pass, row.auth_key_hash);
  }

  /** Called only after onUpgrade() has established who this is. */
  private onConnection(ws: WebSocket, req: IncomingMessage, chargerId: string): void {
    // A charger that reconnects without the old socket closing (NAT rebind,
    // power blip) would otherwise leave a zombie session holding the id.
    const existing = this.sessions.get(chargerId);
    if (existing) {
      this.log(`${chargerId}: replacing an existing session`);
      existing.dispose();
      existing.socket.terminate();
    }

    const session = new ChargerSession(chargerId, ws, this);
    this.sessions.set(chargerId, session);
    session.touch();

    this.log(`${chargerId}: connected`);
    this.markOnline(chargerId, true);

    ws.on('message', (data) => {
      session.touch();
      this.onMessage(session, data.toString());
    });

    ws.on('pong', () => session.touch());

    ws.on('close', (code) => {
      this.log(`${chargerId}: disconnected (${code})`);
      session.dispose();
      if (this.sessions.get(chargerId) === session) {
        this.sessions.delete(chargerId);
        this.markOnline(chargerId, false);
      }
    });

    ws.on('error', (err) => this.log(`${chargerId}: socket error: ${err.message}`));
  }

  private markOnline(chargerId: string, online: boolean): void {
    this.db.prepare(
      `UPDATE chargers SET online = ?, last_seen = ? WHERE id = ?`,
    ).run(online ? 1 : 0, Date.now(), chargerId);

    // A charger that has gone away is not "Charging" any more, whatever it last
    // told us. Leaving a stale status is how a dashboard ends up showing a
    // session that ended hours ago.
    if (!online) {
      this.db.prepare(
        `UPDATE chargers SET status = 'Unavailable' WHERE id = ? AND status != 'Faulted'`,
      ).run(chargerId);
    }
    this.events.emit('charger.online', { chargerId, online });
  }

  private onMessage(session: ChargerSession, raw: string): void {
    let frame;
    try {
      frame = parseFrame(raw);
    } catch (err) {
      const e = err as OcppFrameError;
      this.log(`${session.chargerId}: unparseable frame: ${e.message}`);
      // No id to reply against, so there is nothing useful to send back.
      return;
    }

    this.record(session.chargerId, 'in', frame.action ?? null, frame.id, raw);

    switch (frame.type) {
      case MessageType.CALL:
        this.onCall(session, frame.id, frame.action!, frame.payload);
        break;

      case MessageType.CALLRESULT: {
        const pending = session.pending.get(frame.id);
        if (!pending) {
          this.log(`${session.chargerId}: result for unknown call ${frame.id}`);
          return;
        }
        clearTimeout(pending.timer);
        session.pending.delete(frame.id);
        pending.resolve(frame.payload);
        break;
      }

      case MessageType.CALLERROR: {
        const pending = session.pending.get(frame.id);
        if (!pending) return;
        clearTimeout(pending.timer);
        session.pending.delete(frame.id);
        pending.reject(new Error(`${frame.errorCode}: ${frame.errorDescription}`));
        break;
      }
    }
  }

  private onCall(session: ChargerSession, id: string, action: string, payload: unknown): void {
    const schema = INBOUND_SCHEMAS[action as InboundAction];
    if (!schema) {
      this.replyError(session, id, OcppError.NOT_IMPLEMENTED, action);
      return;
    }

    const parsed = schema.safeParse(payload ?? {});
    if (!parsed.success) {
      // Being specific here is worth it: a charge point that sends a bad
      // payload is usually a firmware bug, and "PropertyConstraintViolation on
      // meterStop" is the difference between a five-minute fix and a week.
      const detail = parsed.error.issues
        .map((i) => `${i.path.join('.')}: ${i.message}`)
        .join('; ')
        .slice(0, 200);
      this.log(`${session.chargerId}: invalid ${action}: ${detail}`);
      this.replyError(session, id, OcppError.PROPERTY_CONSTRAINT_VIOLATION, detail);
      return;
    }

    let response: Record<string, unknown>;
    try {
      response = handleInbound(this, session, action as InboundAction, parsed.data);
    } catch (err) {
      this.log(`${session.chargerId}: ${action} handler failed: ${(err as Error).message}`);
      this.replyError(session, id, OcppError.INTERNAL_ERROR, (err as Error).message);
      return;
    }

    const frame = JSON.stringify([MessageType.CALLRESULT, id, response]);
    session.sendRaw(frame, action, id);
  }

  private replyError(session: ChargerSession, id: string, code: string, description: string): void {
    const frame = JSON.stringify([MessageType.CALLERROR, id, code, description, {}]);
    session.sendRaw(frame, null, id);
  }

  /** Append to the OCPP log and push it to any live dashboard. */
  record(chargerId: string, direction: 'in' | 'out', action: string | null,
         messageId: string, payload: string): void {
    const ts = Date.now();
    this.db.prepare(
      `INSERT INTO ocpp_log (charger_id, ts, direction, action, message_id, payload)
       VALUES (?, ?, ?, ?, ?, ?)`,
    ).run(chargerId, ts, direction, action, messageId, payload);

    this.events.emit('ocpp.message', { chargerId, ts, direction, action, messageId, payload });

    // Trim occasionally rather than on every row: the DELETE is the expensive
    // part and the log does not need to be exactly at the limit.
    if (Math.random() < 0.01) {
      this.db.prepare(
        `DELETE FROM ocpp_log
          WHERE charger_id = ?
            AND id NOT IN (
              SELECT id FROM ocpp_log WHERE charger_id = ? ORDER BY id DESC LIMIT ?
            )`,
      ).run(chargerId, chargerId, LOG_RETENTION_ROWS);
    }
  }
}

export { ocppNow };
