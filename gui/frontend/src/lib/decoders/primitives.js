/**
 * @file Signal-processing primitives shared by all protocol decoders.
 *
 * The functions here have no protocol knowledge — they turn analog sample
 * arrays into discrete timing / logic-level events that protocol-specific
 * code then interprets.
 *
 * All timestamps returned are in nanoseconds, relative to the start of
 * the input array (sample 0 == t = 0 ns).
 */

/**
 * Apply a Schmitt trigger to an analog waveform and return its logic
 * level sample-by-sample (0 or 1). `vHigh` / `vLow` are the two thresholds
 * (vHigh > vLow). Intermediate values keep the previous state, so noise
 * around a single threshold doesn't create spurious transitions.
 *
 * @param {number[]} samples
 * @param {number} vHigh      upper threshold in mV
 * @param {number} vLow       lower threshold in mV
 * @param {number} [initial]  starting state (0 or 1); default 0
 * @returns {Uint8Array}      one bit per input sample
 */
export function schmittTrigger(samples, vHigh, vLow, initial = 0) {
  const n = samples.length;
  const out = new Uint8Array(n);
  let s = initial ? 1 : 0;
  for (let i = 0; i < n; i++) {
    const v = samples[i];
    if (s === 0 && v >= vHigh) s = 1;
    else if (s === 1 && v <= vLow) s = 0;
    out[i] = s;
  }
  return out;
}

/**
 * A single detected edge.
 * @typedef {Object} Edge
 * @property {number} sample     — sample index where the edge was detected
 * @property {number} t_ns       — timestamp in ns (= sample * dt_ns)
 * @property {'rising'|'falling'} type
 */

/**
 * Find every rising and falling edge in a logic-level track (from
 * `schmittTrigger`). The first sample is used as the initial state and is
 * never reported as an edge.
 *
 * @param {Uint8Array} logic
 * @param {number} dt_ns
 * @returns {Edge[]}
 */
export function findEdges(logic, dt_ns) {
  const edges = [];
  let prev = logic[0] || 0;
  for (let i = 1; i < logic.length; i++) {
    const cur = logic[i];
    if (cur !== prev) {
      edges.push({
        sample: i,
        t_ns: i * dt_ns,
        type: cur === 1 ? 'rising' : 'falling',
      });
      prev = cur;
    }
  }
  return edges;
}

/**
 * Turn a list of edges into a list of pulse-widths (gap between two
 * consecutive edges, regardless of direction). Useful for UART bit-width
 * auto-detection or for CAN bit quanta inference.
 *
 * @param {Edge[]} edges
 * @returns {Array<{t_ns:number, width_ns:number, from:'rising'|'falling'}>}
 */
export function pulseWidths(edges) {
  const out = [];
  for (let i = 1; i < edges.length; i++) {
    out.push({
      t_ns: edges[i - 1].t_ns,
      width_ns: edges[i].t_ns - edges[i - 1].t_ns,
      from: edges[i - 1].type,
    });
  }
  return out;
}

/**
 * Sample a logic track at regular intervals starting from `t0_ns`. Used
 * for UART / CAN bit sampling once the bit clock is known.
 *
 * @param {Uint8Array} logic
 * @param {number} dt_ns
 * @param {number} t0_ns       ns at which to take the first sample
 * @param {number} period_ns   ns between consecutive sample points
 * @param {number} count       how many samples to collect
 * @returns {number[]}         0/1 values
 */
export function sampleAtIntervals(logic, dt_ns, t0_ns, period_ns, count) {
  const out = [];
  for (let k = 0; k < count; k++) {
    const idx = Math.round((t0_ns + k * period_ns) / dt_ns);
    if (idx < 0 || idx >= logic.length) {
      out.push(-1);
    } else {
      out.push(logic[idx]);
    }
  }
  return out;
}

/**
 * Pack an LSB-first array of bits into an integer. {0,1,1,0} → 0b0110
 * with LSB convention. For MSB-first use `.reverse()` before calling.
 *
 * @param {number[]} bits
 * @returns {number}
 */
export function bitsToInt(bits) {
  let v = 0;
  for (let i = 0; i < bits.length; i++) {
    if (bits[i] === 1) v |= (1 << i);
  }
  return v;
}

/**
 * Convenience: choose reasonable Schmitt thresholds from a range value.
 * Default split at 40 % / 60 % of the range — works for typical 3.3/5 V
 * logic on scope ranges of 5V or 10V.
 *
 * @param {number} rangeMv
 * @returns {{vHigh:number, vLow:number}}
 */
export function defaultLogicThresholds(rangeMv) {
  return {
    vHigh: rangeMv * 0.60,
    vLow:  rangeMv * 0.40,
  };
}
