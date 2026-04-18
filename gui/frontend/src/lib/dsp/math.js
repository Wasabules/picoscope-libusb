/**
 * Math channel on two aligned mV arrays. Returns a new Float64Array
 * matching the shorter of the two (or null for noop / missing inputs).
 *
 * Operations:
 *   add   a[i] + b[i]
 *   sub   a[i] - b[i]
 *   mul   a[i] * b[i] / 1000     (mV² / 1000 ≈ V·mV — keeps numbers on-scale)
 *   inva  -a[i]
 *   invb  -b[i]
 */
export function computeMath(op, a, b) {
  if (op === 'none') return null;
  const na = a ? a.length : 0;
  const nb = b ? b.length : 0;
  if (op === 'inva' && !a) return null;
  if (op === 'invb' && !b) return null;
  if ((op === 'add' || op === 'sub' || op === 'mul') && (!a || !b)) return null;
  const n = op === 'inva' ? na : op === 'invb' ? nb : Math.min(na, nb);
  if (n <= 0) return null;
  const out = new Float64Array(n);
  if (op === 'add')       for (let i = 0; i < n; i++) out[i] = a[i] + b[i];
  else if (op === 'sub')  for (let i = 0; i < n; i++) out[i] = a[i] - b[i];
  else if (op === 'mul')  for (let i = 0; i < n; i++) out[i] = (a[i] * b[i]) / 1000;
  else if (op === 'inva') for (let i = 0; i < n; i++) out[i] = -a[i];
  else if (op === 'invb') for (let i = 0; i < n; i++) out[i] = -b[i];
  return out;
}
