/** Presentation helpers, kept out of components so units are consistent. */

/**
 * Energy, scaled to the unit a human would use.
 * Chargers report watt-hours; a 40 kWh session shown as "40000 Wh" is harder to
 * read at a glance, and that glance is the whole point of the dashboard.
 */
export function formatEnergy(wh: number | null | undefined): string {
  if (wh === null || wh === undefined) return '—';
  if (wh >= 1_000_000) return `${(wh / 1_000_000).toFixed(2)} MWh`;
  if (wh >= 1000) return `${(wh / 1000).toFixed(wh >= 10_000 ? 1 : 2)} kWh`;
  return `${Math.round(wh)} Wh`;
}

export function formatPower(w: number | null | undefined): string {
  if (w === null || w === undefined) return '—';
  if (Math.abs(w) >= 1000) return `${(w / 1000).toFixed(2)} kW`;
  return `${Math.round(w)} W`;
}

export function formatAmps(a: number | null | undefined): string {
  return a === null || a === undefined ? '—' : `${a.toFixed(1)} A`;
}

export function formatVolts(v: number | null | undefined): string {
  return v === null || v === undefined ? '—' : `${v.toFixed(0)} V`;
}

/** A duration in the largest two units that are non-zero: "1h 24m", "3m 12s". */
export function formatDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return '—';
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${sec}s`;
  return `${sec}s`;
}

export function formatTime(ts: number | null | undefined): string {
  if (!ts) return '—';
  return new Date(ts).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

export function formatDateTime(ts: number | null | undefined): string {
  if (!ts) return '—';
  return new Date(ts).toLocaleString([], {
    month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit',
  });
}

export function formatDate(ts: number): string {
  return new Date(ts).toLocaleDateString([], { month: 'short', day: 'numeric' });
}

/** "3 minutes ago", for last-seen timestamps. */
export function formatRelative(ts: number | null | undefined): string {
  if (!ts) return 'never';
  const delta = Date.now() - ts;
  if (delta < 60_000) return 'just now';
  if (delta < 3_600_000) return `${Math.floor(delta / 60_000)} min ago`;
  if (delta < 86_400_000) return `${Math.floor(delta / 3_600_000)} h ago`;
  return `${Math.floor(delta / 86_400_000)} d ago`;
}
