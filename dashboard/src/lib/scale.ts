/**
 * Axis scale helper.
 *
 * Picking a "nice" maximum and then dividing it into four is not enough: a max
 * of 50 gives ticks at 12.5 and 37.5, which render as 13 and 38 and look like
 * arbitrary numbers. Choosing a nice *step* first and letting the max follow
 * gives ticks a reader recognises.
 */
export interface Scale {
  max: number;
  ticks: number[];
}

/** Round up to 1, 2, 2.5 or 5 times a power of ten. */
function niceNumber(v: number): number {
  if (v <= 0) return 1;
  const exp = Math.floor(Math.log10(v));
  const pow = 10 ** exp;
  const frac = v / pow;
  const nice = frac <= 1 ? 1 : frac <= 2 ? 2 : frac <= 2.5 ? 2.5 : frac <= 5 ? 5 : 10;
  return nice * pow;
}

/**
 * @param rawMax        largest value in the data
 * @param targetTicks   how many intervals to aim for; the result may differ by
 *                      one, because a round step matters more than an exact count
 */
export function niceScale(rawMax: number, targetTicks = 4): Scale {
  if (!Number.isFinite(rawMax) || rawMax <= 0) {
    return { max: 1, ticks: [0, 0.25, 0.5, 0.75, 1] };
  }
  const step = niceNumber(rawMax / targetTicks);
  const max = Math.ceil(rawMax / step) * step;

  const ticks: number[] = [];
  // Accumulate in integer multiples rather than repeatedly adding `step`, so
  // floating-point drift does not produce 0.30000000000000004 as a tick label.
  for (let i = 0; i * step <= max + step * 1e-9; i++) ticks.push(i * step);
  return { max, ticks };
}

/**
 * Nudge labels apart so near-equal series values do not overwrite each other.
 *
 * Returns a y for each input, preserving order, with at least `minGap` between
 * neighbours. Without this, "drawn 22.8 A" and "offered 24 A" render on top of
 * one another exactly when the reader most wants to compare them.
 */
export function spreadLabels(ys: number[], minGap: number): number[] {
  const order = ys.map((y, i) => ({ y, i })).sort((a, b) => a.y - b.y);
  let prev = -Infinity;
  for (const item of order) {
    if (item.y - prev < minGap) item.y = prev + minGap;
    prev = item.y;
  }
  const out = new Array<number>(ys.length);
  for (const item of order) out[item.i] = item.y;
  return out;
}
