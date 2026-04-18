/**
 * @file SPI decoder (degraded for 2-channel hardware).
 *
 * Full SPI needs 4 signals: SCLK, MOSI, MISO, CS. The PS2204A has only
 * 2 analog inputs, so we support these reduced modes:
 *
 *   1. CLK + DATA   — capture SCLK + one data line (MOSI or MISO). No CS:
 *                     we infer frame boundaries from SCLK idle gaps.
 *   2. CS + DATA    — rare, but sometimes useful for long-clock-period
 *                     debugging where edges are coarse.
 *
 * For a full 4-wire capture you'd need two scopes or a logic analyzer.
 *
 * Scaffold: decode() returns []. Fill the state machine below.
 */

import { Kind, Level } from './types.js';
import {
  schmittTrigger, findEdges, defaultLogicThresholds,
} from './primitives.js';

/** @type {import('./types.js').Decoder} */
export const spi = {
  id: 'spi',
  name: 'SPI (2-wire)',
  description: 'SPI with CLK + one data line (no CS). 2-channel scope limitation.',
  channels: [
    { role: 'CLK',  required: true, help: 'SCLK. Bits sampled on configurable edge.' },
    { role: 'DATA', required: true, help: 'MOSI or MISO line.' },
  ],
  configSchema: [
    { key: 'cpol', label: 'CPOL (clock polarity)', type: 'select',
      default: 0, options: [
        {value: 0, label: '0 — idle LOW'},
        {value: 1, label: '1 — idle HIGH'},
      ] },
    { key: 'cpha', label: 'CPHA (clock phase)',  type: 'select',
      default: 0, options: [
        {value: 0, label: '0 — sample on first edge'},
        {value: 1, label: '1 — sample on second edge'},
      ] },
    { key: 'bitOrder', label: 'Bit order', type: 'select',
      default: 'msb', options: [
        {value: 'msb', label: 'MSB first'},
        {value: 'lsb', label: 'LSB first'},
      ] },
    { key: 'wordBits', label: 'Word size',  type: 'select',
      default: 8, options: [8, 16, 24, 32].map(v => ({value:v,label:String(v)+' bits'})) },
    { key: 'frameGap_us', label: 'Frame gap µs', type: 'number',
      default: 50, min: 1, max: 10000, step: 10,
      help: 'SCLK idle longer than this terminates a word boundary (CS inferred).' },
    { key: 'thresholdMv', label: 'Threshold mV', type: 'number',
      default: 1500, step: 100 },
  ],

  /** @param {import('./types.js').DecoderContext} ctx */
  decode(ctx) {
    /** @type {import('./types.js').DecodedEvent[]} */
    const events = [];

    const clkKey = ctx.channelMap.CLK  || 'A';
    const datKey = ctx.channelMap.DATA || 'B';
    const clk = clkKey === 'B' ? ctx.samplesB : ctx.samplesA;
    const dat = datKey === 'B' ? ctx.samplesB : ctx.samplesA;
    if (!clk || !dat || clk.length === 0 || dat.length === 0) return events;

    const vMid = ctx.config.thresholdMv;
    const clkLogic = schmittTrigger(clk, vMid * 1.10, vMid * 0.90,
                                    ctx.config.cpol ? 1 : 0);
    const datLogic = schmittTrigger(dat, vMid * 1.10, vMid * 0.90, 0);

    // TODO — scaffold. Algorithm:
    //
    //   sampleEdge = (cpol == 0) ? 'rising' : 'falling'   if cpha == 0
    //                else the other edge                   if cpha == 1
    //   for each edge of CLK:
    //     if its type matches sampleEdge:
    //       sample DATA, push to currentWord (MSB or LSB order)
    //       if count == wordBits: emit {BYTE, value, annotation: hex}, reset
    //     if gap between this and previous edge > frameGap_ns:
    //       emit {MARKER, annotation: 'frame gap'}; reset word buffer

    return events;
  },
};
