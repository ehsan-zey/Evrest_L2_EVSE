/**
 * Single-series column chart for magnitude over time.
 *
 * One series, so one hue and no legend — the card title names it. The bars are
 * thin with 4px rounded tops anchored square to the baseline, a 2px surface gap
 * between them, and a hairline grid. Hover gives a per-bar tooltip; only the
 * maximum is direct-labelled, because a number on every column is noise.
 */
import { useState, useId } from 'react';
import { niceScale } from '../lib/scale';

export interface BarDatum {
  label: string;
  value: number;
  /** Longer label for the tooltip, when the axis label is abbreviated. */
  fullLabel?: string;
}

interface Props {
  data: BarDatum[];
  height?: number;
  /** Formats values for the tooltip and the direct label. */
  format: (v: number) => string;
  /** Series colour. Defaults to categorical slot 1. */
  color?: string;
  /** Show at most this many x labels, thinning evenly. */
  maxXLabels?: number;
}

const PAD = { top: 18, right: 12, bottom: 26, left: 52 };
/** Gap between adjacent bars, in px of surface. */
const BAR_GAP = 2;
const CORNER = 4;

/** Bar path: square at the baseline, rounded at the data end. */
function barPath(x: number, y: number, w: number, h: number): string {
  const r = Math.min(CORNER, w / 2, h);
  if (h <= 0) return '';
  return `M ${x} ${y + h}
          L ${x} ${y + r}
          Q ${x} ${y} ${x + r} ${y}
          L ${x + w - r} ${y}
          Q ${x + w} ${y} ${x + w} ${y + r}
          L ${x + w} ${y + h} Z`;
}

export function BarChart({ data, height = 220, format, color, maxXLabels = 10 }: Props) {
  const [hover, setHover] = useState<number | null>(null);
  const clipId = useId();

  const W = 720;                       // viewBox width; the SVG scales to fit
  const plotW = W - PAD.left - PAD.right;
  const plotH = height - PAD.top - PAD.bottom;

  if (data.length === 0) {
    return <div className="empty">No data for this period</div>;
  }

  const rawMax = Math.max(...data.map((d) => d.value), 0);
  const { max: yMax, ticks } = niceScale(rawMax);
  const slot = plotW / data.length;
  const barW = Math.max(2, slot - BAR_GAP);
  const yOf = (v: number) => PAD.top + plotH - (v / yMax) * plotH;

  const labelEvery = Math.max(1, Math.ceil(data.length / maxXLabels));
  const peakIndex = data.reduce((best, d, i) => (d.value > data[best]!.value ? i : best), 0);

  const hovered = hover !== null ? data[hover] : null;
  const stroke = color ?? 'var(--series-1)';

  return (
    <div className="chart-wrap">
      <svg viewBox={`0 0 ${W} ${height}`} role="img"
           aria-label={`Column chart, ${data.length} periods, peak ${format(rawMax)}`}>
        <defs>
          <clipPath id={clipId}>
            <rect x={PAD.left} y={PAD.top - 4} width={plotW} height={plotH + 4} />
          </clipPath>
        </defs>

        {/* Grid and y axis. Solid hairlines -- dashing reads as noise. */}
        {ticks.map((t) => (
          <g key={t}>
            <line className="grid-line" x1={PAD.left} x2={W - PAD.right} y1={yOf(t)} y2={yOf(t)} />
            <text className="axis-text" x={PAD.left - 8} y={yOf(t) + 3.5} textAnchor="end">
              {format(t)}
            </text>
          </g>
        ))}

        <g clipPath={`url(#${clipId})`}>
          {data.map((d, i) => {
            const h = (d.value / yMax) * plotH;
            const x = PAD.left + i * slot + BAR_GAP / 2;
            const y = PAD.top + plotH - h;
            return (
              <path
                key={i}
                d={barPath(x, y, barW, h)}
                fill={stroke}
                opacity={hover === null || hover === i ? 1 : 0.35}
              />
            );
          })}
        </g>

        {/* The peak is direct-laballed; everything else is on hover only. */}
        {rawMax > 0 && (
          <text
            className="series-label"
            fill="var(--text-secondary)"
            x={PAD.left + peakIndex * slot + barW / 2 + BAR_GAP / 2}
            y={yOf(data[peakIndex]!.value) - 6}
            textAnchor="middle"
          >
            {format(data[peakIndex]!.value)}
          </text>
        )}

        <line className="axis-line" x1={PAD.left} x2={W - PAD.right}
              y1={PAD.top + plotH} y2={PAD.top + plotH} />

        {data.map((d, i) => (
          i % labelEvery === 0 ? (
            <text key={i} className="axis-text"
                  x={PAD.left + i * slot + slot / 2} y={height - 8} textAnchor="middle">
              {d.label}
            </text>
          ) : null
        ))}

        {/*
          Hit targets are the full column slot, not the bar. A 3px-tall bar is
          impossible to hover otherwise, and those are exactly the days someone
          wants to inspect.
        */}
        {data.map((_, i) => (
          <rect key={i} x={PAD.left + i * slot} y={PAD.top} width={slot} height={plotH}
                fill="transparent"
                onMouseEnter={() => setHover(i)}
                onMouseLeave={() => setHover(null)} />
        ))}
      </svg>

      {hovered && hover !== null && (
        <div
          className="tooltip"
          style={{
            left: `${((PAD.left + hover * slot + slot / 2) / W) * 100}%`,
            top: 4,
            transform: hover > data.length / 2 ? 'translateX(-105%)' : 'translateX(5%)',
          }}
        >
          <div className="tt-title">{hovered.fullLabel ?? hovered.label}</div>
          <div className="tt-row">
            <span className="tt-swatch" style={{ background: stroke }} />
            <span>Energy</span>
            <span className="tt-val">{format(hovered.value)}</span>
          </div>
        </div>
      )}
    </div>
  );
}
