import { useEffect, useState, useCallback } from 'react';
import { useNavigate } from 'react-router-dom';
import { api, type Charger, type Summary } from '../lib/api';
import { live, type LiveEvent } from '../lib/live';
import { BarChart } from '../components/BarChart';
import { StatusBadge } from '../components/StatusBadge';
import { formatEnergy, formatPower, formatDate, formatRelative } from '../lib/format';

/**
 * Fleet overview.
 *
 * The KPI row is stat tiles rather than a chart: four headline numbers are not
 * a bar chart's job, and a one-bar chart is never the right answer. The one
 * chart on this page is daily energy, which is genuinely change-over-time.
 */
export function Overview() {
  const [chargers, setChargers] = useState<Charger[]>([]);
  const [summary, setSummary] = useState<Summary | null>(null);
  const [error, setError] = useState<string | null>(null);
  const navigate = useNavigate();

  const load = useCallback(() => {
    api.get<Charger[]>('/api/chargers').then(setChargers).catch((e) => setError(e.message));
    api.get<Summary>('/api/stats/summary').then(setSummary).catch(() => { /* non-fatal */ });
  }, []);

  useEffect(() => { load(); }, [load]);

  /*
   * Live events patch the rows in place rather than refetching the list. A
   * fleet page that refetches on every meter sample would issue a request per
   * charger per interval, which is exactly the load a live feed exists to
   * avoid.
   */
  useEffect(() => live.subscribe((e: LiveEvent) => {
    if (e.event === 'charger.status') {
      setChargers((prev) => prev.map((c) =>
        c.id === e.data.chargerId
          ? { ...c, status: e.data.status as Charger['status'],
              error_code: e.data.errorCode === 'NoError' ? null : e.data.errorCode,
              vendor_error: e.data.vendorErrorCode }
          : c));
    } else if (e.event === 'charger.online') {
      setChargers((prev) => prev.map((c) =>
        c.id === e.data.chargerId ? { ...c, online: e.data.online, last_seen: Date.now() } : c));
    } else if (e.event === 'transaction.start' || e.event === 'transaction.stop') {
      // A session boundary changes derived totals, so this one does refetch.
      load();
    }
  }), [load]);

  const daily = (summary?.last30Days.daily ?? []).map((d) => ({
    label: formatDate(d.day),
    fullLabel: new Date(d.day).toLocaleDateString([], {
      weekday: 'short', month: 'short', day: 'numeric',
    }),
    value: d.energy_wh,
  }));

  return (
    <>
      <div className="page-head">
        <div>
          <h1 className="page-title">Overview</h1>
          <p className="page-sub">Live status across every charge point</p>
        </div>
        <button onClick={load}>Refresh</button>
      </div>

      {error && <div className="error-box">{error}</div>}

      <div className="kpi-row">
        <StatTile label="Chargers online"
                  value={`${summary?.chargers.online ?? 0}`}
                  note={`of ${summary?.chargers.total ?? 0} registered`} />
        <StatTile label="Charging now"
                  value={`${summary?.chargers.charging ?? 0}`}
                  note="active sessions" />
        <StatTile label="Faulted"
                  value={`${summary?.chargers.faulted ?? 0}`}
                  note={summary?.chargers.faulted ? 'needs attention' : 'all clear'}
                  emphasis={(summary?.chargers.faulted ?? 0) > 0 ? 'critical' : undefined} />
        <StatTile label="Energy, 30 days"
                  value={formatEnergy(summary?.last30Days.energyWh ?? 0)}
                  note={`${summary?.last30Days.sessions ?? 0} sessions`} />
      </div>

      <div className="card" style={{ marginBottom: 18 }}>
        <h2 className="card-title">Energy delivered per day — last 30 days</h2>
        {/* One formatter for the axis and the tooltip, so "0" does not appear
            as "0 Wh" beside "150 kWh". */}
        <BarChart data={daily} height={220}
                  format={(v) => (v === 0 ? '0' : `${(v / 1000).toFixed(v < 10_000 ? 1 : 0)} kWh`)} />
      </div>

      <div className="card">
        <h2 className="card-title">Charge points</h2>
        {chargers.length === 0 ? (
          <div className="empty">
            No charge points yet. A charger registers itself the first time it boots
            against this server.
          </div>
        ) : (
          <table className="data">
            <thead>
              <tr>
                <th>Charge point</th>
                <th>Status</th>
                <th>Session</th>
                <th className="num">Max current</th>
                <th>Last seen</th>
              </tr>
            </thead>
            <tbody>
              {chargers.map((c) => (
                <tr key={c.id} className="clickable" onClick={() => navigate(`/chargers/${c.id}`)}>
                  <td>
                    <div style={{ fontWeight: 600 }}>{c.name}</div>
                    <div className="small muted mono">{c.id}</div>
                  </td>
                  <td>
                    <StatusBadge status={c.status} online={c.online} />
                    {c.vendor_error && (
                      <div className="small muted" style={{ marginTop: 4 }}>{c.vendor_error}</div>
                    )}
                  </td>
                  <td>
                    {c.active_transaction_id ? (
                      <>
                        <div className="mono">{c.active_id_tag}</div>
                        <div className="small muted">
                          since {new Date(c.active_started_at!).toLocaleTimeString([], {
                            hour: '2-digit', minute: '2-digit' })}
                        </div>
                      </>
                    ) : <span className="muted">—</span>}
                  </td>
                  <td className="num">{c.max_current_a} A</td>
                  <td className="small muted">{formatRelative(c.last_seen)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </>
  );
}

function StatTile({ label, value, note, emphasis }: {
  label: string; value: string; note?: string;
  emphasis?: 'critical';
}) {
  return (
    <div className="stat-tile">
      <div className="stat-label">{label}</div>
      <div className="stat-value"
           style={emphasis === 'critical' ? { color: 'var(--status-critical)' } : undefined}>
        {value}
      </div>
      {note && <div className="stat-note">{note}</div>}
    </div>
  );
}

export { formatPower };
