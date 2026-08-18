import { useEffect, useState, useCallback } from 'react';
import { useParams, Link } from 'react-router-dom';
import { api, ApiError, type ChargerDetail, type MeterSample, type LogEntry } from '../lib/api';
import { live, type LiveEvent } from '../lib/live';
import { LineChart } from '../components/LineChart';
import { StatusBadge } from '../components/StatusBadge';
import {
  formatEnergy, formatPower, formatAmps, formatVolts,
  formatDuration, formatTime, formatDateTime,
} from '../lib/format';

/** Keep this many live samples in the charts; ~30 min at a 15 s cadence. */
const LIVE_WINDOW = 120;

export function ChargerDetailPage({ role }: { role: 'admin' | 'operator' | 'driver' }) {
  const { id = '' } = useParams();
  const [charger, setCharger] = useState<ChargerDetail | null>(null);
  const [samples, setSamples] = useState<MeterSample[]>([]);
  const [log, setLog] = useState<LogEntry[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  /*
   * MeterValues arrive every few seconds and drown everything else in the log.
   * Hiding them by default makes the panel usable for what an operator is
   * actually there for -- watching a start, a fault or a profile land.
   */
  const [showMeterValues, setShowMeterValues] = useState(false);
  const isOperator = role !== 'driver';

  const load = useCallback(() => {
    api.get<ChargerDetail>(`/api/chargers/${id}`).then(setCharger).catch((e) => setError(e.message));
    api.get<MeterSample[]>(`/api/chargers/${id}/samples?limit=${LIVE_WINDOW}`)
      .then(setSamples).catch(() => { /* non-fatal */ });
    if (isOperator) {
      api.get<LogEntry[]>(`/api/chargers/${id}/log?limit=60`).then(setLog).catch(() => { /* non-fatal */ });
    }
  }, [id, isOperator]);

  useEffect(() => { load(); }, [load]);

  // Narrow the live feed to this charger while the page is open, so a busy
  // fleet does not stream every other charger's samples into this view.
  useEffect(() => {
    live.setFilter([id]);
    return () => live.setFilter([]);
  }, [id]);

  useEffect(() => live.subscribe((e: LiveEvent) => {
    // The 'connected' envelope carries no chargerId, so narrow on the event
    // name before reaching into the payload.
    if (e.event === 'connected') return;
    if (e.data.chargerId !== id) return;

    if (e.event === 'meter.values') {
      const d = e.data;
      setSamples((prev) => [...prev, {
        ts: d.ts ?? e.ts,
        energy_wh: d.energyWh ?? null,
        power_w: d.powerW ?? null,
        current_a: d.currentA ?? null,
        voltage_v: d.voltageV ?? null,
        offered_a: d.offeredA ?? null,
      }].slice(-LIVE_WINDOW));
    } else if (e.event === 'charger.status') {
      setCharger((c) => c && ({
        ...c,
        status: e.data.status as ChargerDetail['status'],
        error_code: e.data.errorCode === 'NoError' ? null : e.data.errorCode,
        vendor_error: e.data.vendorErrorCode,
      }));
    } else if (e.event === 'charger.online') {
      setCharger((c) => c && ({ ...c, online: e.data.online }));
    } else if (e.event === 'transaction.start' || e.event === 'transaction.stop') {
      load();
    } else if (e.event === 'ocpp.message') {
      setLog((prev) => [{
        ts: e.data.ts, direction: e.data.direction, action: e.data.action,
        message_id: e.data.messageId, payload: e.data.payload,
      }, ...prev].slice(0, 60));
    }
  }), [id, load]);

  /** Run a control action, surfacing the charger's own answer. */
  async function act(name: string, fn: () => Promise<unknown>): Promise<void> {
    setBusy(name);
    setError(null);
    setNotice(null);
    try {
      const res = await fn() as { status?: string };
      // Show what the charger actually said. "Accepted" and "Rejected" are both
      // successful round trips, and conflating them would hide a refusal.
      setNotice(res?.status ? `${name}: ${res.status}` : `${name} sent`);
      setTimeout(load, 800);
    } catch (err) {
      setError(err instanceof ApiError ? err.message : String(err));
    } finally {
      setBusy(null);
    }
  }

  if (!charger) {
    return <div className="empty">{error ?? 'Loading…'}</div>;
  }

  const txn = charger.activeTransaction;
  const latest = samples[samples.length - 1] ?? charger.latestSample;

  /*
   * Current and offered current share a y axis because they share a unit --
   * that comparison is the whole point, since a gap between them means the
   * charger is throttling. Power gets its own chart rather than a second axis:
   * a dual-axis plot would invent a correlation by choosing where the scales
   * line up.
   */
  const currentSeries = [
    {
      name: 'Drawn',
      color: 'var(--series-1)',
      points: samples.map((s) => ({ x: s.ts, y: s.current_a })),
    },
    {
      name: 'Offered',
      color: 'var(--series-2)',
      // The offer is a commanded limit: it steps, it does not ramp.
      step: true,
      points: samples.map((s) => ({ x: s.ts, y: s.offered_a })),
    },
  ];

  const visibleLog = showMeterValues
    ? log
    : log.filter((l) => l.action !== 'MeterValues');

  const powerSeries = [{
    name: 'Power',
    color: 'var(--series-1)',
    points: samples.map((s) => ({ x: s.ts, y: s.power_w })),
  }];

  return (
    <>
      <div className="page-head">
        <div>
          <Link to="/" className="small muted" style={{ textDecoration: 'none' }}>← Overview</Link>
          <h1 className="page-title" style={{ marginTop: 6 }}>{charger.name}</h1>
          <p className="page-sub mono">
            {charger.id}
            {charger.vendor && ` · ${charger.vendor} ${charger.model ?? ''}`}
            {charger.firmware && ` · fw ${charger.firmware}`}
          </p>
        </div>
        <StatusBadge status={charger.status} online={charger.online} />
      </div>

      {error && <div className="error-box">{error}</div>}
      {notice && <div className="card small" style={{ marginBottom: 14, padding: '10px 14px' }}>{notice}</div>}

      {charger.vendor_error && (
        <div className="error-box">
          <strong>{charger.error_code}</strong> — {charger.vendor_error}
        </div>
      )}

      <div className="kpi-row">
        <Tile label="Power" value={formatPower(latest?.power_w)} />
        <Tile label="Current drawn" value={formatAmps(latest?.current_a)}
              note={latest?.offered_a != null ? `${formatAmps(latest.offered_a)} offered` : undefined} />
        <Tile label="Voltage" value={formatVolts(latest?.voltage_v)} />
        <Tile
          label="Session energy"
          value={txn && latest?.energy_wh != null
            ? formatEnergy(latest.energy_wh - txn.meter_start_wh)
            : '—'}
          note={txn ? formatDuration(Date.now() - txn.started_at) : 'no active session'}
        />
      </div>

      {txn && (
        <div className="card" style={{ marginBottom: 16 }}>
          <h2 className="card-title">Active session</h2>
          <div className="row" style={{ gap: 28 }}>
            <Field label="Tag"><span className="mono">{txn.id_tag}</span></Field>
            <Field label="Started">{formatDateTime(txn.started_at)}</Field>
            <Field label="Duration">{formatDuration(Date.now() - txn.started_at)}</Field>
            <Field label="Transaction">#{txn.id}</Field>
          </div>
        </div>
      )}

      <div className="grid-2" style={{ marginBottom: 16 }}>
        <div className="card">
          <h2 className="card-title">Current — drawn against offered</h2>
          <LineChart series={currentSeries} format={(v) => `${v.toFixed(0)}`}
                     formatX={formatTime} yUnit="A" height={230} />
        </div>
        <div className="card">
          <h2 className="card-title">Power</h2>
          <LineChart series={powerSeries}
                     format={(v) => (v >= 1000 ? `${(v / 1000).toFixed(1)}k` : v.toFixed(0))}
                     formatX={formatTime} yUnit="W" height={230} />
        </div>
      </div>

      <div className="card" style={{ marginBottom: 16 }}>
        <h2 className="card-title">Controls</h2>
        {!charger.online ? (
          <div className="muted small">
            The charger is offline. Remote commands need a live OCPP connection.
          </div>
        ) : (
          <Controls
            charger={charger}
            isOperator={isOperator}
            busy={busy}
            onAct={act}
          />
        )}
      </div>

      {isOperator && (
        <div className="card">
          <div className="row" style={{ justifyContent: 'space-between', marginBottom: 12 }}>
            <h2 className="card-title" style={{ margin: 0 }}>OCPP traffic</h2>
            <label className="row small muted" style={{ gap: 6, cursor: 'pointer' }}>
              <input type="checkbox" checked={showMeterValues} style={{ width: 'auto' }}
                     onChange={(e) => setShowMeterValues(e.target.checked)} />
              Show MeterValues
            </label>
          </div>
          {visibleLog.length === 0 ? (
            <div className="empty">
              {log.length === 0 ? 'No messages yet' : 'Only MeterValues so far — tick the box to see them'}
            </div>
          ) : (
            <div style={{ maxHeight: 340, overflowY: 'auto' }}>
              <table className="data">
                <thead>
                  <tr><th>Time</th><th>Dir</th><th>Action</th><th>Payload</th></tr>
                </thead>
                <tbody>
                  {visibleLog.map((l, i) => (
                    <tr key={`${l.message_id}-${i}`}>
                      <td className="small muted" style={{ whiteSpace: 'nowrap' }}>
                        {new Date(l.ts).toLocaleTimeString()}
                      </td>
                      <td className="small">
                        {/* Direction is an arrow plus a word, not a colour. */}
                        {l.direction === 'in' ? '← from' : '→ to'}
                      </td>
                      <td className="small" style={{ fontWeight: 600 }}>{l.action ?? '—'}</td>
                      <td className="mono" style={{
                        maxWidth: 460, overflow: 'hidden',
                        textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                      }}>{l.payload}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </div>
      )}
    </>
  );
}

function Controls({ charger, isOperator, busy, onAct }: {
  charger: ChargerDetail;
  isOperator: boolean;
  busy: string | null;
  onAct: (name: string, fn: () => Promise<unknown>) => Promise<void>;
}) {
  const [idTag, setIdTag] = useState('');
  const [limit, setLimit] = useState(String(charger.max_current_a));
  const txn = charger.activeTransaction;

  return (
    <div style={{ display: 'grid', gap: 18 }}>
      <div>
        <div className="label-text" style={{ marginBottom: 8 }}>Session</div>
        {txn ? (
          <button className="danger" disabled={busy !== null}
                  onClick={() => onAct('Stop', () =>
                    api.post(`/api/chargers/${charger.id}/stop`, { transactionId: txn.id }))}>
            {busy === 'Stop' ? 'Stopping…' : 'Stop charging'}
          </button>
        ) : (
          <div className="row">
            <input placeholder="RFID tag to charge against" value={idTag} style={{ maxWidth: 260 }}
                   onChange={(e) => setIdTag(e.target.value)} />
            <button className="primary" disabled={busy !== null || idTag.trim() === ''}
                    onClick={() => onAct('Start', () =>
                      api.post(`/api/chargers/${charger.id}/start`, { idTag: idTag.trim() }))}>
              {busy === 'Start' ? 'Starting…' : 'Start charging'}
            </button>
          </div>
        )}
      </div>

      {isOperator && (
        <>
          <div>
            <div className="label-text" style={{ marginBottom: 8 }}>
              Current limit — applied as an OCPP charging profile
            </div>
            <div className="row">
              <input type="number" min={0} max={80} step={1} value={limit} style={{ maxWidth: 120 }}
                     onChange={(e) => setLimit(e.target.value)} />
              <span className="small muted">A</span>
              <button disabled={busy !== null}
                      onClick={() => onAct('Set limit', () =>
                        api.post(`/api/chargers/${charger.id}/limit`, { limitA: Number(limit) }))}>
                Apply
              </button>
              <button disabled={busy !== null}
                      onClick={() => onAct('Clear limit', () =>
                        api.post(`/api/chargers/${charger.id}/limit`, { limitA: 0 }))}>
                Clear
              </button>
            </div>
            <div className="small muted" style={{ marginTop: 6 }}>
              The charger takes the lowest of this, its DIP-switch setting and its hardware
              rating — a profile can only reduce the offer, never raise it.
            </div>
          </div>

          <div>
            <div className="label-text" style={{ marginBottom: 8 }}>Availability and maintenance</div>
            <div className="row">
              <button disabled={busy !== null}
                      onClick={() => onAct('Set operative', () =>
                        api.post(`/api/chargers/${charger.id}/availability`, { type: 'Operative' }))}>
                Put in service
              </button>
              <button disabled={busy !== null}
                      onClick={() => onAct('Set inoperative', () =>
                        api.post(`/api/chargers/${charger.id}/availability`, { type: 'Inoperative' }))}>
                Take out of service
              </button>
              <button disabled={busy !== null}
                      onClick={() => onAct('Soft reset', () =>
                        api.post(`/api/chargers/${charger.id}/reset`, { type: 'Soft' }))}>
                Soft reset
              </button>
              <button className="danger" disabled={busy !== null}
                      onClick={() => {
                        if (!confirm(
                          'A hard reset reboots the charger immediately, cutting any session in progress. Continue?',
                        )) return;
                        void onAct('Hard reset', () =>
                          api.post(`/api/chargers/${charger.id}/reset`, { type: 'Hard' }));
                      }}>
                Hard reset
              </button>
            </div>
            <div className="small muted" style={{ marginTop: 6 }}>
              Taking a charger out of service during a session is scheduled — it takes effect
              when the car unplugs. A soft reset also waits for the session to close cleanly.
            </div>
          </div>
        </>
      )}
    </div>
  );
}

function Tile({ label, value, note }: { label: string; value: string; note?: string }) {
  return (
    <div className="stat-tile">
      <div className="stat-label">{label}</div>
      <div className="stat-value">{value}</div>
      {note && <div className="stat-note">{note}</div>}
    </div>
  );
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div>
      <div className="small muted">{label}</div>
      <div style={{ fontWeight: 550, marginTop: 2 }}>{children}</div>
    </div>
  );
}
