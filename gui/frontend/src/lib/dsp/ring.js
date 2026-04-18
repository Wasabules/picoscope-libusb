/**
 * Circular sample ring. Pure functions over an externally-owned
 * Float32Array + (head, len) cursors. The backing buffer is allocated
 * once and reused; only the cursors change on each push, so GC stays
 * quiet at 55+ blocks/s.
 */

/** Ring capacity in samples. 4 M × 4 B = 16 MiB per channel. */
export const RING_CAP = 4_194_304;

/**
 * Cap the materialised slice length. The canvas is ~1200 px wide and draws
 * a min/max envelope per column, so anything above ~4 samples/px wastes GC
 * budget with no visual benefit. 16 k output samples keeps the allocation
 * small (64 kB per channel per frame) while preserving every spike via
 * interleaved max/min pairs. Without this cap, a 4 M-sample slice churns
 * 16 MB per frame per channel and the event loop stalls.
 */
export const MATERIALIZE_MAX = 16_384;

export function createRing() {
  return new Float32Array(RING_CAP);
}

/**
 * Append `arr` to the ring at cursor (head, len). Returns the new cursor.
 * Mutates `buf` in place.
 */
export function pushRing(buf, head, len, arr) {
  const n = arr.length;
  if (!n) return { head, len };
  if (n >= RING_CAP) {
    // Incoming block is larger than the ring — keep only its tail.
    const start = n - RING_CAP;
    for (let i = 0; i < RING_CAP; i++) buf[i] = arr[start + i];
    return { head: 0, len: RING_CAP };
  }
  const firstLen = Math.min(n, RING_CAP - head);
  for (let i = 0; i < firstLen; i++) buf[head + i] = arr[i];
  const rem = n - firstLen;
  for (let i = 0; i < rem; i++) buf[i] = arr[firstLen + i];
  return {
    head: (head + n) % RING_CAP,
    len:  Math.min(RING_CAP, len + n),
  };
}

/**
 * Copy logical samples [start, end) out of the circular ring into a
 * fresh Float32Array (chronological: index 0 = oldest in the slice).
 * When the requested range is much larger than MATERIALIZE_MAX, return
 * a min/max-envelope compaction — the canvas's per-column envelope logic
 * still reconstructs the trace identically from it.
 */
export function materializeRing(buf, head, len, start, end) {
  if (!buf || len <= 0) return null;
  start = Math.max(0, Math.min(start, len));
  end   = Math.max(start, Math.min(end, len));
  const n = end - start;
  if (n === 0) return null;
  const oldest = ((head - len) % RING_CAP + RING_CAP) % RING_CAP;

  if (n <= MATERIALIZE_MAX) {
    const readStart = (oldest + start) % RING_CAP;
    const out = new Float32Array(n);
    const firstLen = Math.min(n, RING_CAP - readStart);
    out.set(buf.subarray(readStart, readStart + firstLen), 0);
    if (firstLen < n) out.set(buf.subarray(0, n - firstLen), firstLen);
    return out;
  }

  // Downsample: MATERIALIZE_MAX/2 buckets, each emitting [max, min] so the
  // canvas envelope preserves the signal extremes.
  const buckets = MATERIALIZE_MAX >> 1;
  const out = new Float32Array(buckets * 2);
  const baseIdx = oldest + start;
  for (let b = 0; b < buckets; b++) {
    const sStart = Math.floor((b * n) / buckets);
    const sEnd   = Math.floor(((b + 1) * n) / buckets);
    let mn = Infinity, mx = -Infinity;
    for (let i = sStart; i < sEnd; i++) {
      const v = buf[(baseIdx + i) % RING_CAP];
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    if (mn === Infinity) { mn = 0; mx = 0; }
    out[2 * b]     = mx;
    out[2 * b + 1] = mn;
  }
  return out;
}
