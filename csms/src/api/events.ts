/**
 * In-process event bus.
 *
 * The OCPP server emits, the API's WebSocket hub relays to dashboards and
 * phones. Keeping this in one place means the OCPP layer never learns that
 * browsers exist.
 *
 * Deliberately not Node's EventEmitter: the default max-listener warning fires
 * at 10, and a hub with 30 connected dashboards is normal, not a leak.
 */
export type EventName =
  | 'charger.online' | 'charger.status' | 'charger.boot' | 'charger.firmware'
  | 'transaction.start' | 'transaction.stop'
  | 'meter.values'
  | 'ocpp.message';

export interface EventEnvelope {
  event: EventName;
  data: Record<string, unknown>;
  ts: number;
}

type Listener = (e: EventEnvelope) => void;

export class EventBus {
  private readonly listeners = new Set<Listener>();

  subscribe(fn: Listener): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  emit(event: EventName, data: Record<string, unknown>): void {
    const envelope: EventEnvelope = { event, data, ts: Date.now() };
    for (const fn of this.listeners) {
      try {
        fn(envelope);
      } catch (err) {
        // One broken subscriber must not stop the others, and must never
        // propagate back into an OCPP handler mid-transaction.
        console.error('[events] listener threw:', (err as Error).message);
      }
    }
  }

  get listenerCount(): number {
    return this.listeners.size;
  }
}
