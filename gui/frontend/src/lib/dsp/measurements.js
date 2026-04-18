/**
 * Per-slice oscilloscope measurements: min/max/mean/Vpp/RMS, frequency &
 * period via Schmitt-trigger edge detection, duty cycle, 10-90 % rise/fall.
 */

export function computeMeas(d, dtNs) {
  if (!d || d.length < 4 || !dtNs || dtNs <= 0) return null;
  let mn = d[0], mx = d[0], sum = 0;
  for (let i = 0; i < d.length; i++) {
    if (d[i] < mn) mn = d[i];
    if (d[i] > mx) mx = d[i];
    sum += d[i];
  }
  const mean = sum / d.length;
  let sqsum = 0;
  for (let i = 0; i < d.length; i++) sqsum += (d[i] - mean) * (d[i] - mean);
  const rms = Math.sqrt(sqsum / d.length);

  // Frequency via hysteresis rising-edge detection. Schmitt trigger: we
  // only register a rising edge when the signal crosses highT AFTER
  // having been below lowT (5 % hysteresis on each side of the mean).
  const hyst = Math.max(1, (mx - mn) * 0.05);
  const highT = mean + hyst;
  const lowT  = mean - hyst;
  const rising = [];
  let state = 0;
  let nHigh = 0, nLow = 0;
  for (let i = 0; i < d.length; i++) {
    const v = d[i];
    if (v >= highT) {
      if (state === -1) rising.push(i);
      state = 1; nHigh++;
    } else if (v <= lowT) {
      state = -1; nLow++;
    }
  }
  let freqHz = 0, periodNs = 0;
  if (rising.length >= 2) {
    const spanSamples = rising[rising.length - 1] - rising[0];
    const periods = rising.length - 1;
    const periodSamp = spanSamples / periods;
    periodNs = periodSamp * dtNs;
    if (periodNs > 0) freqHz = 1e9 / periodNs;
  }
  const totalActive = nHigh + nLow;
  const duty = totalActive > 0 ? 100 * nHigh / totalActive : 0;

  const { rise: riseNs, fall: fallNs } = edgeTimes(d, mn, mx, dtNs);

  return { min: mn, max: mx, mean, vpp: mx - mn, rms, freqHz, periodNs,
           duty, riseNs, fallNs };
}

/**
 * 10-90 % rise/fall times via fractional linear interpolation between the
 * two samples that bracket the threshold. Returns 0 for either edge when
 * the signal amplitude is too small (< 10 mV Vpp) or no crossing is found.
 */
export function edgeTimes(d, mn, mx, dtNs) {
  const pp = mx - mn;
  if (pp < 10 || d.length < 4) return { rise: 0, fall: 0 };
  const lvl10 = mn + 0.10 * pp;
  const lvl90 = mn + 0.90 * pp;
  const crossAt = (i, lvl) => {
    const a = d[i-1], b = d[i];
    if (b === a) return i;
    return (i - 1) + (lvl - a) / (b - a);
  };
  let rise = 0, fall = 0;
  let found10 = -1;
  for (let i = 1; i < d.length; i++) {
    if (found10 < 0) {
      if (d[i-1] <= lvl10 && d[i] > lvl10) found10 = crossAt(i, lvl10);
    } else {
      if (d[i-1] < lvl90 && d[i] >= lvl90) {
        rise = (crossAt(i, lvl90) - found10) * dtNs;
        break;
      }
    }
  }
  let found90 = -1;
  for (let i = 1; i < d.length; i++) {
    if (found90 < 0) {
      if (d[i-1] >= lvl90 && d[i] < lvl90) found90 = crossAt(i, lvl90);
    } else {
      if (d[i-1] > lvl10 && d[i] <= lvl10) {
        fall = (crossAt(i, lvl10) - found90) * dtNs;
        break;
      }
    }
  }
  return { rise, fall };
}
