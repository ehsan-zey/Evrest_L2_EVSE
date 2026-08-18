/**
 * Live update hub for dashboards and phones.
 *
 * A second WebSocket server, on the API port rather than the OCPP port, so a
 * browser can never accidentally reach the charger endpoint and a charger can
 * never reach this one.
 *
 * Clients authenticate with the same JWT the REST API uses, passed as a query
 * parameter because browsers cannot set headers on a WebSocket handshake.
 */
import { WebSocketServer, WebSocket } from 'ws';
import type { Server } from 'node:http';
import type { EventBus, EventEnvelope } from './events.js';
import { verifyToken, type TokenPayload } from '../auth/jwt.js';

interface Client {
  socket: WebSocket;
  user: TokenPayload;
  /** Charger ids this client wants; empty means everything. */
  filter: Set<string>;
  alive: boolean;
}

/** Events only an operator has any business seeing. */
const OPERATOR_ONLY = new Set(['ocpp.message']);

export function createLiveHub(server: Server, events: EventBus, jwtSecret: string): WebSocketServer {
  const wss = new WebSocketServer({ server, path: '/live' });
  const clients = new Set<Client>();

  wss.on('connection', (socket, req) => {
    const url = new URL(req.url ?? '/', 'http://localhost');
    const token = url.searchParams.get('token');
    const user = token ? verifyToken(jwtSecret, token) : null;

    if (!user) {
      socket.close(1008, 'Unauthorized');
      return;
    }

    const client: Client = { socket, user, filter: new Set(), alive: true };
    clients.add(client);

    socket.on('message', (raw) => {
      // The only thing a client may say is which chargers it cares about.
      try {
        const msg = JSON.parse(raw.toString());
        if (msg?.type === 'subscribe' && Array.isArray(msg.chargers)) {
          client.filter = new Set(msg.chargers.filter((c: unknown) => typeof c === 'string'));
        }
      } catch {
        /* ignore malformed client chatter */
      }
    });

    socket.on('pong', () => { client.alive = true; });
    socket.on('close', () => clients.delete(client));
    socket.on('error', () => clients.delete(client));

    socket.send(JSON.stringify({ event: 'connected', data: { role: user.role }, ts: Date.now() }));
  });

  /*
   * A browser tab that is closed without a close frame — laptop lid, phone
   * sleeping, network drop — leaves a socket that looks open forever. Ping
   * every 30 s and drop anything that has not ponged since the last round.
   */
  const heartbeat = setInterval(() => {
    for (const client of clients) {
      if (!client.alive) {
        client.socket.terminate();
        clients.delete(client);
        continue;
      }
      client.alive = false;
      client.socket.ping();
    }
  }, 30_000);

  wss.on('close', () => clearInterval(heartbeat));

  events.subscribe((envelope: EventEnvelope) => {
    const chargerId = envelope.data.chargerId as string | undefined;
    const payload = JSON.stringify(envelope);

    for (const client of clients) {
      if (client.socket.readyState !== WebSocket.OPEN) continue;

      // Raw OCPP frames can contain idTags and configuration; operators only.
      if (OPERATOR_ONLY.has(envelope.event) && client.user.role === 'driver') continue;

      // An empty filter means "everything"; otherwise only what was asked for.
      if (client.filter.size > 0 && chargerId && !client.filter.has(chargerId)) continue;

      client.socket.send(payload);
    }
  });

  return wss;
}
