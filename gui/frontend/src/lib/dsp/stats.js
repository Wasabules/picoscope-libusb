/**
 * Rolling statistics over the last N frames of measurements. Bags are
 * plain objects keyed by measurement name; each value is a bounded array.
 */

export const STATS_WINDOW = 50;
export const STATS_FIELDS = ['vpp', 'mean', 'rms', 'freqHz', 'duty'];

export function makeEmptyStats() {
  const out = {};
  for (const k of STATS_FIELDS) out[k] = [];
  return out;
}

export function pushStat(bag, m) {
  if (!m) return;
  for (const k of STATS_FIELDS) {
    const v = m[k];
    if (typeof v !== 'number' || !isFinite(v)) continue;
    bag[k].push(v);
    if (bag[k].length > STATS_WINDOW) bag[k].shift();
  }
}

export function statAgg(arr) {
  if (!arr || arr.length === 0) return null;
  let mn = arr[0], mx = arr[0], s = 0;
  for (const v of arr) { if (v < mn) mn = v; if (v > mx) mx = v; s += v; }
  return { min: mn, max: mx, avg: s / arr.length, n: arr.length };
}

export function aggregateStatsBag(bag) {
  const out = {};
  for (const k of STATS_FIELDS) out[k] = statAgg(bag[k]);
  return out;
}
