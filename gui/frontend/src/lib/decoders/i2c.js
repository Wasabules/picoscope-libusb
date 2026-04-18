/**
 * @file I²C (TWI) decoder — two signals: SCL (clock) and SDA (data).
 *
 * Protocol recap (7-bit addressing, standard mode):
 *   - START:    SDA falls while SCL is HIGH
 *   - STOP:     SDA rises while SCL is HIGH
 *   - BIT:      SDA stable while SCL is HIGH; sample SDA on SCL rising
 *   - ADDRESS:  7 MSB-first bits + 1 R/W bit, then ACK from slave
 *   - DATA:     8 MSB-first bits, then ACK from receiver
 *   - Rep.START: START condition without a preceding STOP
 *
 * Two scope channels exactly fit — no downgrade needed.
 *
 * Scaffold only: `decode()` returns []. The primitives module handles all
 * edge detection; the real work is the state machine below.
 */

import { Kind, Level } from './types.js';
import {
  schmittTrigger, findEdges, defaultLogicThresholds,
} from './primitives.js';

/** @type {import('./types.js').Decoder} */
export const i2c = {
  id: 'i2c',
  name: 'I²C',
  description: 'Two-wire serial (SCL + SDA). Open-drain, idle HIGH on both.',
  channels: [
    { role: 'SCL', required: true, help: 'Clock line — bits are sampled on SCL rising edge.' },
    { role: 'SDA', required: true, help: 'Data line — transitions while SCL is LOW; stable while HIGH.' },
  ],
  configSchema: [
    { key: 'addrMode', label: 'Address bits', type: 'select',
      default: '7', options: [
        {value: '7',  label: '7-bit'},
        {value: '10', label: '10-bit'},
      ] },
    { key: 'thresholdMv', label: 'Threshold mV', type: 'number',
      default: 1500, step: 100,
      help: 'Logic mid-level. I²C is open-drain, idle HIGH.' },
  ],

  /** @param {import('./types.js').DecoderContext} ctx */
  decode(ctx) {
    /** @type {import('./types.js').DecodedEvent[]} */
    const events = [];

    const sclKey = ctx.channelMap.SCL || 'A';
    const sdaKey = ctx.channelMap.SDA || 'B';
    const scl = sclKey === 'B' ? ctx.samplesB : ctx.samplesA;
    const sda = sdaKey === 'B' ? ctx.samplesB : ctx.samplesA;
    if (!scl || !sda || scl.length === 0 || sda.length === 0) return events;

    const vMid = ctx.config.thresholdMv;
    const sclLogic = schmittTrigger(scl, vMid * 1.10, vMid * 0.90, 1);
    const sdaLogic = schmittTrigger(sda, vMid * 1.10, vMid * 0.90, 1);

    // TODO — scaffold. The real decoder is a simple state machine:
    //
    //   state: IDLE / ADDR / DATA / ACK
    //   walk sample-by-sample tracking (sclLogic, sdaLogic) pairs:
    //     - IDLE:  wait for SDA falling while SCL=1  → emit START, state=ADDR
    //     - ADDR/DATA: on each SCL rising, capture sdaLogic into the
    //                  current byte buffer (MSB first). After 8 bits,
    //                  next SCL rising samples the ACK bit.
    //     - After 8-bit addr: emit {addr, r/w, ACK}, then DATA state
    //     - On SDA rising while SCL=1 in any state: emit STOP, state=IDLE
    //     - On SDA falling while SCL=1 mid-transaction: emit Rep.START
    //
    // Consider 10-bit addressing: first byte is 0b11110xx0 marker plus
    // the top 2 bits of address; second byte carries the lower 8 bits.

    return events;
  },
};
