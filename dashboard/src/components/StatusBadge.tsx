/**
 * Charger status badge.
 *
 * Always a glyph plus a word plus a dot — never colour alone, so it survives
 * colourblindness, greyscale printing and forced-colors mode.
 */
import type { ChargerStatus } from '../lib/api';

const STYLE: Record<string, { cls: string; glyph: string; text: string }> = {
  Charging:      { cls: 'st-charging',  glyph: '⚡', text: 'Charging' },
  Available:     { cls: 'st-available', glyph: '○',  text: 'Available' },
  Preparing:     { cls: 'st-preparing', glyph: '◐',  text: 'Preparing' },
  Finishing:     { cls: 'st-finishing', glyph: '◑',  text: 'Finishing' },
  SuspendedEV:   { cls: 'st-suspended', glyph: '‖',  text: 'Paused by car' },
  SuspendedEVSE: { cls: 'st-suspended', glyph: '‖',  text: 'Paused by charger' },
  Reserved:      { cls: 'st-reserved',  glyph: '◆',  text: 'Reserved' },
  Faulted:       { cls: 'st-faulted',   glyph: '▲',  text: 'Faulted' },
  Unavailable:   { cls: 'st-offline',   glyph: '—',  text: 'Unavailable' },
};

export function StatusBadge({ status, online }: { status: ChargerStatus; online: boolean }) {
  // Offline outranks whatever the charger last reported. Showing a stale
  // "Charging" for a unit that has dropped off the network is worse than
  // showing nothing.
  if (!online) {
    return (
      <span className="badge st-offline">
        <span className="dot" /><span className="glyph">⦸</span>Offline
      </span>
    );
  }
  const s = STYLE[status] ?? STYLE.Unavailable!;
  return (
    <span className={`badge ${s.cls}`}>
      <span className="dot" /><span className="glyph">{s.glyph}</span>{s.text}
    </span>
  );
}
