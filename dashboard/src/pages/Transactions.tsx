import { useEffect, useState } from 'react';
import { Link } from 'react-router-dom';
import { api, type Transaction } from '../lib/api';
import { formatEnergy, formatDuration, formatDateTime } from '../lib/format';

export function Transactions() {
  const [rows, setRows] = useState<Transaction[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    api.get<Transaction[]>('/api/transactions?limit=200')
      .then(setRows)
      .catch((e) => setError(e.message))
      .finally(() => setLoading(false));
  }, []);

  const completed = rows.filter((r) => r.stopped_at !== null);
  const totalEnergy = completed.reduce((sum, r) => sum + (r.energy_wh ?? 0), 0);

  return (
    <>
      <div className="page-head">
        <div>
          <h1 className="page-title">Sessions</h1>
          <p className="page-sub">Charging history across the fleet</p>
        </div>
      </div>

      {error && <div className="error-box">{error}</div>}

      <div className="kpi-row">
        <div className="stat-tile">
          <div className="stat-label">Sessions shown</div>
          <div className="stat-value">{rows.length}</div>
          <div className="stat-note">{rows.length - completed.length} still running</div>
        </div>
        <div className="stat-tile">
          <div className="stat-label">Energy delivered</div>
          <div className="stat-value">{formatEnergy(totalEnergy)}</div>
          <div className="stat-note">across completed sessions</div>
        </div>
      </div>

      <div className="card">
        {loading ? (
          <div className="empty">Loading…</div>
        ) : rows.length === 0 ? (
          <div className="empty">No sessions recorded yet</div>
        ) : (
          <table className="data">
            <thead>
              <tr>
                <th>Started</th>
                <th>Charge point</th>
                <th>Tag</th>
                <th className="num">Duration</th>
                <th className="num">Energy</th>
                <th>Ended</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((t) => (
                <tr key={t.id}>
                  <td>{formatDateTime(t.started_at)}</td>
                  <td>
                    <Link to={`/chargers/${t.charger_id}`} style={{ color: 'var(--series-1)' }}>
                      {t.charger_name ?? t.charger_id}
                    </Link>
                  </td>
                  <td className="mono">{t.id_tag}</td>
                  <td className="num">
                    {formatDuration((t.stopped_at ?? Date.now()) - t.started_at)}
                  </td>
                  <td className="num">
                    {t.stopped_at
                      ? formatEnergy(t.energy_wh)
                      : <span className="muted">in progress</span>}
                  </td>
                  <td className="small muted">
                    {t.stopped_at
                      ? (t.stop_reason ?? 'Local')
                      : <span style={{ color: 'var(--success-text)', fontWeight: 600 }}>running</span>}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </>
  );
}
