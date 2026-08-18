/** Shared formatting, matching the dashboard's units so the two agree. */

export function formatEnergy(wh: number | null | undefined): string {
  if (wh === null || wh === undefined) return '—';
  if (wh >= 1000) return `${(wh / 1000).toFixed(wh >= 10_000 ? 1 : 2)} kWh`;
  return `${Math.round(wh)} Wh`;
}

export function formatPower(w: number | null | undefined): string {
  if (w === null || w === undefined) return '—';
  if (Math.abs(w) >= 1000) return `${(w / 1000).toFixed(1)} kW`;
  return `${Math.round(w)} W`;
}

export function formatAmps(a: number | null | undefined): string {
  return a === null || a === undefined ? '—' : `${a.toFixed(1)} A`;
}

export function formatDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return '—';
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s % 60}s`;
  return `${s}s`;
}

export function formatDateTime(ts: number | null | undefined): string {
  if (!ts) return '—';
  return new Date(ts).toLocaleString([], {
    month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit',
  });
}

/**
 * A rough time-to-full estimate.
 *
 * Deliberately conservative and clearly labelled as an estimate: the app has no
 * idea what the battery's capacity or current state of charge is — only what
 * the charger is delivering — so this answers "how long to add N kWh at the
 * present rate", which is the honest version of the question.
 */
export function estimateTimeFor(energyWh: number, powerW: number | null): string | null {
  if (!powerW || powerW < 100 || energyWh <= 0) return null;
  const hours = energyWh / powerW;
  if (hours > 24) return null;
  return formatDuration(hours * 3_600_000);
}
