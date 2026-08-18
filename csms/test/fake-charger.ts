/**
 * A charge point simulator.
 *
 * Speaks real OCPP 1.6J to the CSMS so the backend, dashboard and mobile app
 * can be exercised without hardware on the bench. It implements the same
 * message flow the firmware does, including responding to remote start/stop,
 * charging profiles and resets.
 *
 *   npx tsx test/fake-charger.ts EVREST-0001 <auth-key> [ws://host:9220/ocpp]
 */
import { WebSocket } from 'ws';
import { randomUUID } from 'node:crypto';

const chargerId = process.argv[2] ?? 'EVREST-0001';
const authKey = process.argv[3] ?? '';
const base = process.argv[4] ?? 'ws://127.0.0.1:9220/ocpp';

const HEARTBEAT_FALLBACK = 60;
const METER_INTERVAL_MS = 5_000;

type State = 'Available' | 'Preparing' | 'Charging' | 'SuspendedEV' | 'Finishing' | 'Faulted';

let ws: WebSocket;
let heartbeatInterval = HEARTBEAT_FALLBACK;
let heartbeatTimer: NodeJS.Timeout | null = null;
let meterTimer: NodeJS.Timeout | null = null;

let state: State = 'Available';
let transactionId: number | null = null;
let meterWh = 1_000_000;
let offeredA = 32;
let drawnA = 0;
const voltage = 240;
const pending = new Map<string, (payload: any) => void>();

function send(frame: unknown[]): void {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(frame));
}

function call(action: string, payload: Record<string, unknown>): Promise<any> {
  const id = randomUUID();
  return new Promise((resolve) => {
    pending.set(id, resolve);
    send([2, id, action, payload]);
    console.log(`→ ${action}`);
  });
}

function reply(id: string, payload: Record<string, unknown>): void {
  send([3, id, payload]);
}

async function setState(next: State, errorCode = 'NoError'): Promise<void> {
  state = next;
  await call('StatusNotification', {
    connectorId: 1, errorCode, status: next, timestamp: new Date().toISOString(),
  });
  console.log(`   state -> ${next}`);
}

async function startTransaction(idTag: string): Promise<void> {
  await setState('Preparing');
  const res = await call('StartTransaction', {
    connectorId: 1, idTag, meterStart: Math.round(meterWh),
    timestamp: new Date().toISOString(),
  });

  if (res?.idTagInfo?.status !== 'Accepted') {
    console.log(`   start refused: ${res?.idTagInfo?.status}`);
    await setState('Available');
    return;
  }
  transactionId = res.transactionId;
  drawnA = 0;
  await setState('Charging');
}

async function stopTransaction(reason: string): Promise<void> {
  if (transactionId === null) return;
  await call('StopTransaction', {
    transactionId, meterStop: Math.round(meterWh),
    timestamp: new Date().toISOString(), reason,
  });
  transactionId = null;
  drawnA = 0;
  await setState('Finishing');
  setTimeout(() => void setState('Available'), 3000);
}

/** Accumulate energy and report it, the way the firmware's meter task does. */
function meterTick(): void {
  if (state === 'Charging') {
    // Ramp toward the offer rather than stepping, so the dashboard's
    // drawn-vs-offered chart shows the same shape a real car produces.
    const target = offeredA * 0.96;
    drawnA += (target - drawnA) * 0.35;
    meterWh += (drawnA * voltage * METER_INTERVAL_MS) / 3_600_000;
  } else {
    drawnA = 0;
  }

  void call('MeterValues', {
    connectorId: 1,
    ...(transactionId !== null ? { transactionId } : {}),
    meterValue: [{
      timestamp: new Date().toISOString(),
      sampledValue: [
        { value: String(Math.round(meterWh)), measurand: 'Energy.Active.Import.Register', unit: 'Wh' },
        { value: (drawnA * voltage).toFixed(1), measurand: 'Power.Active.Import', unit: 'W' },
        { value: drawnA.toFixed(2), measurand: 'Current.Import', unit: 'A' },
        { value: voltage.toFixed(1), measurand: 'Voltage', unit: 'V' },
        { value: offeredA.toFixed(1), measurand: 'Current.Offered', unit: 'A' },
      ],
    }],
  });
}

/** Handle a CALL from the CSMS. */
async function onCall(id: string, action: string, payload: any): Promise<void> {
  console.log(`← ${action}`);

  switch (action) {
    case 'RemoteStartTransaction':
      if (transactionId !== null) { reply(id, { status: 'Rejected' }); return; }
      reply(id, { status: 'Accepted' });
      setTimeout(() => void startTransaction(payload.idTag), 500);
      return;

    case 'RemoteStopTransaction':
      if (transactionId !== payload.transactionId) { reply(id, { status: 'Rejected' }); return; }
      reply(id, { status: 'Accepted' });
      setTimeout(() => void stopTransaction('Remote'), 500);
      return;

    case 'SetChargingProfile': {
      const period = payload?.csChargingProfiles?.chargingSchedule?.chargingSchedulePeriod?.[0];
      if (typeof period?.limit === 'number') {
        offeredA = Math.min(48, period.limit);
        console.log(`   offer -> ${offeredA} A`);
        reply(id, { status: 'Accepted' });
      } else {
        reply(id, { status: 'Rejected' });
      }
      return;
    }

    case 'ClearChargingProfile':
      offeredA = 32;
      reply(id, { status: 'Accepted' });
      return;

    case 'ChangeAvailability':
      reply(id, {
        // Match the firmware: a change during a session is scheduled, not immediate.
        status: transactionId !== null && payload.type === 'Inoperative'
          ? 'Scheduled' : 'Accepted',
      });
      return;

    case 'Reset':
      reply(id, { status: 'Accepted' });
      console.log(`   ${payload.type} reset, reconnecting`);
      setTimeout(() => ws.close(), 500);
      return;

    case 'TriggerMessage':
      reply(id, { status: 'Accepted' });
      if (payload.requestedMessage === 'MeterValues') meterTick();
      if (payload.requestedMessage === 'StatusNotification') void setState(state);
      if (payload.requestedMessage === 'Heartbeat') void call('Heartbeat', {});
      return;

    case 'GetConfiguration':
      reply(id, {
        configurationKey: [
          { key: 'HeartbeatInterval', readonly: false, value: String(heartbeatInterval) },
          { key: 'MeterValueSampleInterval', readonly: false, value: '60' },
          { key: 'NumberOfConnectors', readonly: true, value: '1' },
          { key: 'MaxCurrentA', readonly: true, value: '48' },
          { key: 'SupportedFeatureProfiles', readonly: true,
            value: 'Core,SmartCharging,LocalAuthListManagement,Reservation,RemoteTrigger' },
        ],
      });
      return;

    case 'ChangeConfiguration':
      if (payload.key === 'HeartbeatInterval') {
        heartbeatInterval = Number(payload.value) || HEARTBEAT_FALLBACK;
        reply(id, { status: 'Accepted' });
      } else {
        reply(id, { status: 'NotSupported' });
      }
      return;

    case 'UnlockConnector':
      // Same answer the firmware gives: a J1772 handle has no motorised latch.
      reply(id, { status: 'NotSupported' });
      return;

    default:
      send([4, id, 'NotImplemented', action, {}]);
      return;
  }
}

function connect(): void {
  const url = `${base}/${chargerId}`;
  const headers = authKey
    ? { Authorization: 'Basic ' + Buffer.from(`${chargerId}:${authKey}`).toString('base64') }
    : {};

  console.log(`connecting to ${url}`);
  ws = new WebSocket(url, ['ocpp1.6'], { headers });

  ws.on('unexpected-response', (_req, res) => {
    console.error(`refused: HTTP ${res.statusCode}. Check the charger id and auth key.`);
  });

  ws.on('open', async () => {
    console.log('connected');
    const boot = await call('BootNotification', {
      chargePointVendor: 'EVREST',
      chargePointModel: 'EVREST-L2',
      chargePointSerialNumber: chargerId,
      firmwareVersion: '1.0.0',
    });

    if (boot?.status !== 'Accepted') {
      console.log(`boot ${boot?.status}, retrying in ${boot?.interval ?? 30}s`);
      setTimeout(() => ws.close(), (boot?.interval ?? 30) * 1000);
      return;
    }
    heartbeatInterval = boot.interval ?? HEARTBEAT_FALLBACK;

    await setState('Available');

    heartbeatTimer = setInterval(() => void call('Heartbeat', {}), heartbeatInterval * 1000);
    meterTimer = setInterval(meterTick, METER_INTERVAL_MS);
  });

  ws.on('message', (raw) => {
    const msg = JSON.parse(raw.toString());
    const [type, id] = msg;
    if (type === 3) { pending.get(id)?.(msg[2]); pending.delete(id); }
    else if (type === 4) { console.error(`   CALLERROR ${msg[2]}: ${msg[3]}`); pending.get(id)?.(null); pending.delete(id); }
    else if (type === 2) void onCall(id, msg[2], msg[3]);
  });

  ws.on('close', () => {
    console.log('disconnected, retrying in 5s');
    if (heartbeatTimer) clearInterval(heartbeatTimer);
    if (meterTimer) clearInterval(meterTimer);
    pending.clear();
    setTimeout(connect, 5000);
  });

  ws.on('error', (e) => console.error('socket error:', e.message));
}

/* Type 's' + Enter to plug in / unplug, so the whole flow can be driven by
 * hand from the terminal without a car. */
process.stdin.on('data', (buf) => {
  const cmd = buf.toString().trim();
  if (cmd === 's') {
    if (transactionId === null) void startTransaction('04A2B3C4D5');
    else void stopTransaction('Local');
  } else if (cmd === 'f') {
    void setState('Faulted', 'GroundFailure');
  } else if (cmd === 'a') {
    void setState('Available');
  }
});

console.log('commands: s = start/stop a session, f = fault, a = available');
connect();
