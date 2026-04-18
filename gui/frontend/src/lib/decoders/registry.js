/**
 * @file Central registry of decoders. The GUI imports this to populate
 * the protocol-selector dropdown and to look up a Decoder by id.
 *
 * Adding a new decoder: write a module exposing a Decoder-shaped object,
 * import it here, append to `decoders`.
 */

import { uart } from './uart.js';
import { i2c  } from './i2c.js';
import { spi  } from './spi.js';
import { can  } from './can.js';

/** @type {import('./types.js').Decoder[]} */
export const decoders = [uart, i2c, spi, can];

/**
 * Look up a decoder by its `id`. Returns `null` if unknown.
 * @param {string} id
 * @returns {import('./types.js').Decoder|null}
 */
export function getDecoder(id) {
  return decoders.find(d => d.id === id) || null;
}

/**
 * Build a default config object from a decoder's schema — every field is
 * populated with its declared `default`. This is what the UI seeds the
 * config form with.
 * @param {import('./types.js').Decoder} d
 * @returns {Object<string,*>}
 */
export function defaultConfig(d) {
  const out = {};
  for (const f of d.configSchema) out[f.key] = f.default;
  return out;
}

/**
 * Build a default channel map: assign required roles to CH A and B in
 * declaration order, leave optional roles unassigned. Callers can
 * override.
 * @param {import('./types.js').Decoder} d
 * @returns {Object<string,string|null>}
 */
export function defaultChannelMap(d) {
  const out = {};
  const avail = ['A', 'B'];
  let i = 0;
  for (const ch of d.channels) {
    if (ch.required && i < avail.length) {
      out[ch.role] = avail[i++];
    } else {
      out[ch.role] = null;
    }
  }
  return out;
}
