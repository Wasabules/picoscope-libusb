/**
 * @file CAN bus decoder (Classic 2.0A/2.0B).
 *
 * Single signal on a scope probe:
 *   - CAN-H alone (dominant = high, recessive = low)   — or
 *   - CAN-L alone (inverted)                           — or
 *   - A logic-level CAN_TX from a transceiver
 *
 * Optionally two channels for CAN-H + CAN-L differential mode.
 *
 * Bit timing recap (for bit-rates 125k / 250k / 500k / 1M):
 *   - NRZ with bit-stuffing: after 5 identical consecutive bits, the
 *     transmitter inserts one opposite bit (receiver removes it).
 *   - Frame begins with SOF = single dominant bit.
 *   - Arbitration field = 11-bit ID (+ extended 18-bit if IDE=1).
 *   - Control (DLC), Data (0-8 bytes), CRC, ACK, EOF (7 recessive bits).
 *
 * Scaffold only — real implementation needs careful bit-stuff handling
 * and CRC verification. See https://en.wikipedia.org/wiki/CAN_bus
 */

import { Kind, Level } from './types.js';
import {
  schmittTrigger, findEdges, sampleAtIntervals,
} from './primitives.js';

/** @type {import('./types.js').Decoder} */
export const can = {
  id: 'can',
  name: 'CAN',
  description: 'Controller Area Network. Single-ended or differential (2-wire).',
  channels: [
    { role: 'CANH',    required: true,
      help: 'CAN-H, CAN-L (inverted), or logic-level TX from a transceiver.' },
    { role: 'CANL',    required: false,
      help: 'Optional — provide for differential decoding (CANH − CANL).' },
  ],
  configSchema: [
    { key: 'bitrate', label: 'Bit rate', type: 'select',
      default: 500000, options: [
        { value: 125000,  label: '125 kbit/s' },
        { value: 250000,  label: '250 kbit/s' },
        { value: 500000,  label: '500 kbit/s' },
        { value: 1000000, label: '1 Mbit/s'   },
      ] },
    { key: 'polarity', label: 'Polarity', type: 'select',
      default: 'high', options: [
        {value: 'high', label: 'Dominant = HIGH (CANH, TX)'},
        {value: 'low',  label: 'Dominant = LOW  (CANL alone)'},
      ] },
    { key: 'differential', label: 'Differential (CANH − CANL)', type: 'boolean',
      default: false,
      help: 'Requires CANL on the second channel.' },
    { key: 'thresholdMv', label: 'Threshold mV', type: 'number',
      default: 1500, step: 100 },
  ],

  /** @param {import('./types.js').DecoderContext} ctx */
  decode(ctx) {
    /** @type {import('./types.js').DecodedEvent[]} */
    const events = [];

    const hKey = ctx.channelMap.CANH || 'A';
    const lKey = ctx.channelMap.CANL || 'B';
    const h = hKey === 'B' ? ctx.samplesB : ctx.samplesA;
    if (!h || h.length === 0) return events;

    // Optionally compute the differential signal H - L.
    const useDiff = !!ctx.config.differential;
    let driver = h;
    if (useDiff) {
      const l = lKey === 'B' ? ctx.samplesB : ctx.samplesA;
      if (l && l.length === h.length) {
        driver = new Float64Array(h.length);
        for (let i = 0; i < h.length; i++) driver[i] = h[i] - l[i];
      }
    }

    const vMid = ctx.config.thresholdMv;
    const invert = ctx.config.polarity === 'low';
    let logic = schmittTrigger(driver, vMid * 1.10, vMid * 0.90,
                                invert ? 0 : 1);
    if (invert) {
      const inv = new Uint8Array(logic.length);
      for (let i = 0; i < logic.length; i++) inv[i] = logic[i] ? 0 : 1;
      logic = inv;
    }

    // TODO — scaffold. Real algorithm:
    //
    //   1. Scan for falling edge (SOF).
    //   2. Sample at bit_period × (0.5, 1.5, ...), resync on each dominant
    //      → recessive edge that lands within the sync-jump window.
    //   3. Undo bit-stuffing: every 5th identical bit is dropped.
    //   4. Parse fields: ID(11 or 29) | RTR | IDE | r0 | DLC(4) | DATA(8*DLC) | CRC(15) | CRCdel | ACK | ACKdel | EOF(7)
    //   5. Verify CRC-15. Emit one Event per field, plus a frame-level
    //      Event with {value: id, text: 'ID=0x123 DLC=8 DATA=...'}.

    return events;
  },
};
