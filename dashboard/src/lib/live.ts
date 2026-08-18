/**
 * Live event stream.
 *
 * One WebSocket for the whole app, shared by every component that wants
 * updates. Opening one per component would multiply connections by the number
 * of open panels and make reconnection logic something each of them had to
 * repeat.
 */
import { getToken } from './api';

export type LiveEvent =
  | { event: 'connected'; data: { role: string }; ts: number }
  | { event: 'charger.online'; data: { chargerId: string; online: boolean }; ts: number }
  | { event: 'charger.status'; data: { chargerId: string; status: string; errorCode: string; vendorErrorCode: string | null }; ts: number }
  | { event: 'charger.boot'; data: { chargerId: string }; ts: number }
  | { event: 'transaction.start'; data: { chargerId: string; transactionId: number; idTag: string }; ts: number }
  | { event: 'transaction.stop'; data: { chargerId: string; transactionId: number; energyWh: number; reason: string }; ts: number }
  | { event: 'meter.values'; data: { chargerId: string; transactionId: number | null; energyWh?: number; powerW?: number; currentA?: number; voltageV?: number; offeredA?: number; ts?: number }; ts: number }
  | { event: 'ocpp.message'; data: { chargerId: string; direction: 'in' | 'out'; action: string | null; messageId: string; payload: string; ts: number }; ts: number };

type Handler = (e: LiveEvent) => void;

class LiveConnection {
  private socket: WebSocket | null = null;
  private readonly handlers = new Set<Handler>();
  private reconnectDelay = 1000;
  private reconnectTimer: number | null = null;
  private subscriptions: string[] = [];

  subscribe(handler: Handler): () => void {
    this.handlers.add(handler);
    this.ensureConnected();
    return () => {
      this.handlers.delete(handler);
      // Keep the socket open even with no handlers: components mount and
      // unmount constantly during navigation, and tearing the connection down
      // each time would make every page transition reconnect.
    };
  }

  /** Narrow the feed to specific chargers. Empty means everything. */
  setFilter(chargerIds: string[]): void {
    this.subscriptions = chargerIds;
    if (this.socket?.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify({ type: 'subscribe', chargers: chargerIds }));
    }
  }

  private ensureConnected(): void {
    if (this.socket && this.socket.readyState <= WebSocket.OPEN) return;

    const token = getToken();
    if (!token) return;

    const proto = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const url = `${proto}://${window.location.host}/live?token=${encodeURIComponent(token)}`;

    this.socket = new WebSocket(url);

    this.socket.onopen = () => {
      this.reconnectDelay = 1000;
      if (this.subscriptions.length > 0) {
        this.socket?.send(JSON.stringify({ type: 'subscribe', chargers: this.subscriptions }));
      }
    };

    this.socket.onmessage = (ev) => {
      let parsed: LiveEvent;
      try {
        parsed = JSON.parse(ev.data);
      } catch {
        return;
      }
      for (const h of this.handlers) {
        try { h(parsed); } catch (err) { console.error('live handler', err); }
      }
    };

    this.socket.onclose = () => {
      this.socket = null;
      // Back off to 30 s. A CSMS restart should not have thirty dashboards
      // hammering it as it comes up.
      if (this.reconnectTimer !== null) return;
      this.reconnectTimer = window.setTimeout(() => {
        this.reconnectTimer = null;
        this.ensureConnected();
      }, this.reconnectDelay);
      this.reconnectDelay = Math.min(this.reconnectDelay * 2, 30_000);
    };

    this.socket.onerror = () => this.socket?.close();
  }

  /** Drop the connection, e.g. on sign-out so the next user gets a fresh one. */
  reset(): void {
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.socket?.close();
    this.socket = null;
  }
}

export const live = new LiveConnection();
