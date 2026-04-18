/**
 * @file UART / RS-232 / RS-485 (TTL) asynchronous serial decoder.
 *
 * One signal, async: the receiver must already know the baud rate, number
 * of data bits, parity scheme, and stop-bit count. Idle line sits HIGH.
 *
 * Frame layout (LSB first, standard 8-N-1):
 *     IDLE ─┐   ┌──D0─D1─D2─D3─D4─D5─D6─D7─[P]─┐   ┌─ IDLE
 *           └───┘ start                         └───┘ stop
 *
 * Algorithm:
 *   1. Schmitt-trigger the analog samples → logic track.
 *   2. Walk the track; on every falling edge we treat it as a tentative
 *      start bit. Re-sample the middle of that bit-time to confirm it's 0.
 *   3. Sample each data bit, the parity bit (if any), and the stop bit
 *      at bit_period × (1.5, 2.5, ..., N+0.5, N+1.5, ...) relative to the
 *      falling edge.
 *   4. Emit a BYTE event on success, an ERROR event on framing / parity
 *      mismatch. Advance past the frame and resume.
 */

import { Kind, Level } from './types.js';
import { schmittTrigger, bitsToInt } from './primitives.js';

/** @type {import('./types.js').Decoder} */
export const uart = {
  id: 'uart',
  name: 'UART',
  description: 'Asynchronous serial (RS-232 / TTL). One signal per direction.',
  channels: [
    { role: 'DATA', required: true, help: 'TX or RX line; idle HIGH.' },
  ],
  configSchema: [
    { key: 'baud',       label: 'Baud rate', type: 'number',
      default: 9600, min: 50, max: 10000000, unit: 'bd' },
    { key: 'dataBits',   label: 'Data bits', type: 'select',
      default: 8, options: [7,8].map(v => ({value: v, label: String(v)})) },
    { key: 'parity',     label: 'Parity',    type: 'select',
      default: 'none',
      options: [
        {value: 'none', label: 'None'},
        {value: 'even', label: 'Even'},
        {value: 'odd',  label: 'Odd'},
      ] },
    { key: 'stopBits',   label: 'Stop bits', type: 'select',
      default: 1, options: [1,2].map(v => ({value:v,label:String(v)})) },
    { key: 'lsbFirst',   label: 'LSB first', type: 'boolean', default: true },
    { key: 'thresholdMv',label: 'Threshold mV', type: 'number',
      default: 1500, step: 100,
      help: 'Mid-level Schmitt threshold; hysteresis is ±10 %.' },
  ],

  /** @param {import('./types.js').DecoderContext} ctx */
  decode(ctx) {
    /** @type {import('./types.js').DecodedEvent[]} */
    const events = [];

    const chKey = ctx.channelMap.DATA || 'A';
    const samples = chKey === 'B' ? ctx.samplesB : ctx.samplesA;
    if (!samples || samples.length === 0 || ctx.dt_ns <= 0) return events;

    const { baud, dataBits, parity, stopBits, lsbFirst, thresholdMv } = ctx.config;
    const vHigh = thresholdMv * 1.10;
    const vLow  = thresholdMv * 0.90;
    const logic = schmittTrigger(samples, vHigh, vLow, 1);

    const bitPeriodNs  = 1e9 / baud;
    const bitPeriodSmp = bitPeriodNs / ctx.dt_ns;
    if (bitPeriodSmp < 2) {
      events.push({
        t_ns: 0, kind: Kind.ERROR, level: Level.ERROR,
        annotation: 'UNDERSAMPLED',
        text: `bit period = ${bitPeriodSmp.toFixed(2)} samples (<2)`,
      });
      return events;
    }

    const sampleAt = (sampleIdx) => {
      const i = Math.round(sampleIdx);
      if (i < 0 || i >= logic.length) return -1;
      return logic[i];
    };

    const stopOffset = dataBits + (parity !== 'none' ? 1 : 0);
    const frameLenSmp = bitPeriodSmp * (1 + stopOffset + stopBits);

    /**
     * Try to decode a frame assuming `startSample` is the first LOW sample
     * of a start bit. Returns the event (BYTE / ERROR) on success, or null
     * if the frame fails basic validation (mid-start not 0, stop not HIGH,
     * runs off buffer). `null` lets the caller resync by trying the next
     * falling edge instead of committing to a full frame-width skip.
     */
    const tryFrame = (startSample) => {
      if (sampleAt(startSample + bitPeriodSmp * 0.5) !== 0) return null;
      const bits = [];
      for (let b = 0; b < dataBits; b++) {
        const v = sampleAt(startSample + bitPeriodSmp * (1.5 + b));
        if (v < 0) return null;
        bits.push(v);
      }

      let parityOk = true;
      if (parity !== 'none') {
        const pBit = sampleAt(startSample + bitPeriodSmp * (1.5 + dataBits));
        if (pBit < 0) return null;
        const ones = bits.reduce((a, b) => a + b, 0);
        const expected = parity === 'even' ? (ones & 1) : (1 - (ones & 1));
        parityOk = (pBit === expected);
      }

      for (let s = 0; s < stopBits; s++) {
        const sb = sampleAt(startSample + bitPeriodSmp * (1.5 + stopOffset + s));
        if (sb !== 1) return null;
      }

      const value = lsbFirst
        ? bitsToInt(bits)
        : bitsToInt(bits.slice().reverse());
      const startTns = startSample * ctx.dt_ns;
      const endTns   = (startSample + frameLenSmp) * ctx.dt_ns;

      if (!parityOk) {
        return {
          event: {
            t_ns: startTns, t_end_ns: endTns,
            kind: Kind.ERROR, level: Level.ERROR,
            value, annotation: 'PARITY',
            text: `parity mismatch on 0x${value.toString(16).padStart(2, '0')}`,
          },
          endSample: startSample + frameLenSmp,
        };
      }

      const hex = value.toString(16).padStart(2, '0').toUpperCase();
      const printable = value >= 0x20 && value < 0x7f;
      return {
        event: {
          t_ns: startTns, t_end_ns: endTns,
          kind: Kind.BYTE, level: Level.INFO,
          value, annotation: hex,
          text: printable ? `'${String.fromCharCode(value)}' 0x${hex}`
                          : `0x${hex}`,
        },
        endSample: startSample + frameLenSmp,
      };
    };

    const n = logic.length;
    let i = 0;
    let safety = n * 2 + 16;  // upper bound on loop iterations
    while (i < n - 1 && safety-- > 0) {
      let startSample;
      if (logic[i] === 0) {
        // Back-to-back frame: we land on a LOW sample right after a stop.
        // Treat `i` itself as the first LOW of the next start bit.
        startSample = i;
      } else {
        // Walk forward until we see an idle→start (HIGH→LOW) transition.
        while (i < n - 1 && !(logic[i] === 1 && logic[i + 1] === 0)) i++;
        if (i >= n - 1) break;
        startSample = i + 1;
      }

      const res = tryFrame(startSample);
      if (res) {
        events.push(res.event);
        i = Math.round(res.endSample);  // committed frame: jump past stop
      } else {
        i = startSample + 1;            // no frame: retry from next sample
      }
    }

    return events;
  },
};
