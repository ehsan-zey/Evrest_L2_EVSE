/**
 * Multi-series line chart with a crosshair tooltip.
 *
 * One y axis, always. Two measures of different scale get two charts rather
 * than a second axis — a dual-axis plot invents a correlation by choosing where
 * the two scales line up.
 *
 * Series carry a legend and, at two or three series, direct labels at the last
 * point as well, so identity never rests on colour alone.
 */
import { useState, useRef, useId } from 'react';
import { niceScale, spreadLabels } from '../lib/scale';

export interface Series {
  name: string;
  color: string;
  points: Array<{ x: number; y: number | null }>;
  /** Draw as a step rather than a slope: right for a commanded limit, which
   *  changes instantaneously rather than ramping. */
  step?: boolean;
}

interface Props {
  series: Series[];
  height?: number;
  format: (v: number) => string;
  formatX?: (v: number) => string;
  /** Force the axis to start at zero even when the data does not reach it. */
  zeroBased?: boolean;
  yUnit?: string;
}

const PAD = { top: 16, right: 58, bottom: 26, left: 54 };

export function LineChart({
  series, height = 200, format, formatX, zeroBased = true, yUnit,
}: Props) {
  const [hoverX, setHoverX] = useState<number | null>(null);
  const svgRef = useRef<SVGSVGElement>(null);
  const clipId = useId();

  const W = 720;
  const plotW = W - PAD.left - PAD.right;
  const plotH = height - PAD.top - PAD.bottom;

  const allPoints = series.flatMap((s) => s.points.filter((p) => p.y !== null));
  if (allPoints.length === 0) {
    return <div className="empty">Waiting for data…</div>;
  }

  const xs = allPoints.map((p) => p.x);
  const ys = allPoints.map((p) => p.y as number);
  const xMin = Math.min(...xs);
  const xMax = Math.max(...xs);
  const yMaxRaw = Math.max(...ys);
  const { max: yMax, ticks } = niceScale(yMaxRaw || 1);
  const yMin = zeroBased ? 0 : Math.min(...ys);

  // A single-sample series has no x extent; give it one so the point still
  // renders instead of collapsing to a divide-by-zero.
  const xSpan = xMax - xMin || 1;
  const ySpan = yMax - yMin || 1;

  const xOf = (x: number) => PAD.left + ((x - xMin) / xSpan) * plotW;
  const yOf = (y: number) => PAD.top + plotH - ((y - yMin) / ySpan) * plotH;

  function pathFor(s: Series): string {
    let d = '';
    let penDown = false;
    let prev: { x: number; y: number } | null = null;

    for (const p of s.points) {
      if (p.y === null) {
        // A gap in the data is drawn as a gap. Bridging it would invent
        // readings across an outage.
        penDown = false;
        prev = null;
        continue;
      }
      const x = xOf(p.x);
      const y = yOf(p.y);
      if (!penDown) {
        d += `M ${x.toFixed(1)} ${y.toFixed(1)}`;
        penDown = true;
      } else if (s.step && prev) {
        d += ` L ${x.toFixed(1)} ${prev.y.toFixed(1)} L ${x.toFixed(1)} ${y.toFixed(1)}`;
      } else {
        d += ` L ${x.toFixed(1)} ${y.toFixed(1)}`;
      }
      prev = { x, y };
    }
    return d;
  }

  function onMove(e: React.MouseEvent<SVGSVGElement>): void {
    const svg = svgRef.current;
    if (!svg) return;
    const rect = svg.getBoundingClientRect();
    const rel = ((e.clientX - rect.left) / rect.width) * W;
    if (rel < PAD.left || rel > W - PAD.right) { setHoverX(null); return; }
    setHoverX(xMin + ((rel - PAD.left) / plotW) * xSpan);
  }

  /** Nearest real sample to the crosshair, per series. */
  function nearest(s: Series, x: number): { x: number; y: number } | null {
    let best: { x: number; y: number } | null = null;
    let bestDist = Infinity;
    for (const p of s.points) {
      if (p.y === null) continue;
      const d = Math.abs(p.x - x);
      if (d < bestDist) { bestDist = d; best = { x: p.x, y: p.y }; }
    }
    return best;
  }

  const crosshairPx = hoverX !== null ? xOf(hoverX) : null;
  const showDirectLabels = series.length <= 3;

  // Last real value per series, with label positions nudged apart.
  const lastValues = series
    .map((s) => {
      const last = [...s.points].reverse().find((p) => p.y !== null);
      return last && last.y !== null ? { name: s.name, color: s.color, lastY: last.y } : null;
    })
    .filter((v): v is { name: string; color: string; lastY: number } => v !== null);

  const labelled = spreadLabels(lastValues.map((v) => yOf(v.lastY)), 13)
    .map((y, i) => ({ s: lastValues[i]!, y }));

  return (
    <div className="chart-wrap">
      {series.length > 1 && (
        <div className="legend">
          {series.map((s) => (
            <span className="legend-item" key={s.name}>
              <span className="legend-swatch" style={{ background: s.color }} />
              {s.name}
            </span>
          ))}
        </div>
      )}

      <svg
        ref={svgRef}
        viewBox={`0 0 ${W} ${height}`}
        onMouseMove={onMove}
        onMouseLeave={() => setHoverX(null)}
        role="img"
        aria-label={`Line chart: ${series.map((s) => s.name).join(', ')}`}
      >
        <defs>
          <clipPath id={clipId}>
            <rect x={PAD.left - 2} y={PAD.top - 6} width={plotW + 4} height={plotH + 12} />
          </clipPath>
        </defs>

        {ticks.map((t) => (
          <g key={t}>
            <line className="grid-line" x1={PAD.left} x2={W - PAD.right} y1={yOf(t)} y2={yOf(t)} />
            <text className="axis-text" x={PAD.left - 8} y={yOf(t) + 3.5} textAnchor="end">
              {format(t)}
            </text>
          </g>
        ))}

        {yUnit && (
          <text className="axis-text" x={PAD.left - 8} y={PAD.top - 5} textAnchor="end">
            {yUnit}
          </text>
        )}

        <line className="axis-line" x1={PAD.left} x2={W - PAD.right}
              y1={PAD.top + plotH} y2={PAD.top + plotH} />

        {crosshairPx !== null && (
          <line className="grid-line" x1={crosshairPx} x2={crosshairPx}
                y1={PAD.top} y2={PAD.top + plotH}
                stroke="var(--baseline)" />
        )}

        <g clipPath={`url(#${clipId})`}>
          {series.map((s) => (
            <path key={s.name} d={pathFor(s)} fill="none" stroke={s.color}
                  strokeWidth={2} strokeLinejoin="round" strokeLinecap="round" />
          ))}
        </g>

        {/* Marker on the crosshair, ringed in the surface colour so it stays
            readable where two series cross. */}
        {crosshairPx !== null && series.map((s) => {
          const p = nearest(s, hoverX!);
          if (!p) return null;
          return (
            <circle key={s.name} cx={xOf(p.x)} cy={yOf(p.y)} r={4.5}
                    fill={s.color} stroke="var(--surface-1)" strokeWidth={2} />
          );
        })}

        {/*
          Direct labels at the final point, so identity is not colour-only.
          Their y positions are spread apart first: two series whose last values
          are close -- exactly when a reader wants to compare them -- would
          otherwise render on top of each other.
        */}
        {showDirectLabels && labelled.map(({ s, y }) => (
          <text key={s.name} className="series-label" fill={s.color}
                x={W - PAD.right + 6} y={y + 4}>
            {format(s.lastY)}
          </text>
        ))}

        {formatX && (
          <>
            <text className="axis-text" x={PAD.left} y={height - 8} textAnchor="start">
              {formatX(xMin)}
            </text>
            <text className="axis-text" x={W - PAD.right} y={height - 8} textAnchor="end">
              {formatX(xMax)}
            </text>
          </>
        )}
      </svg>

      {crosshairPx !== null && hoverX !== null && (
        <div
          className="tooltip"
          style={{
            left: `${(crosshairPx / W) * 100}%`,
            top: 4,
            transform: crosshairPx > W / 2 ? 'translateX(-105%)' : 'translateX(8%)',
          }}
        >
          {formatX && <div className="tt-title">{formatX(hoverX)}</div>}
          {series.map((s) => {
            const p = nearest(s, hoverX);
            return (
              <div className="tt-row" key={s.name}>
                <span className="tt-swatch" style={{ background: s.color }} />
                <span>{s.name}</span>
                <span className="tt-val">{p ? format(p.y) : '—'}</span>
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}
