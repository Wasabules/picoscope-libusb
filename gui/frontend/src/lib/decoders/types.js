/**
 * @file Core types for protocol decoders.
 *
 * A decoder receives analog samples from one or two channels, along with
 * the inter-sample time step, and produces a list of `DecodedEvent`
 * objects describing the logical / protocol-level content.
 *
 * Everything in this file is JSDoc — no TypeScript toolchain required.
 */

/**
 * A single decoded event. `t_ns` is the timestamp relative to the start
 * of the analysed sample window. For point-like events (a START condition,
 * a single-bit parity error) `t_end_ns` equals `t_ns` or is omitted. For
 * wider events (a byte, a frame) set `t_end_ns` to mark the span.
 *
 * @typedef {Object} DecodedEvent
 * @property {number}   t_ns        — timestamp of the event (ns, relative)
 * @property {number}   [t_end_ns]  — end timestamp for span events
 * @property {EventKind} kind       — category, see EventKind
 * @property {number}   [value]     — numeric payload (bit, byte, word)
 * @property {string}   [text]      — human-readable rendering
 * @property {string}   [annotation]— short tag (START, STOP, ACK, ...)
 * @property {EventLevel} [level]   — display severity
 */

/** @typedef {'bit'|'byte'|'frame'|'symbol'|'marker'|'error'} EventKind */
/** @typedef {'info'|'warn'|'error'} EventLevel */

/**
 * One field of a decoder's configuration schema. The UI renders an input
 * based on `type`; the value is read back into the config object keyed
 * by `key`.
 *
 * @typedef {Object} ConfigField
 * @property {string} key          — config object key
 * @property {string} label        — UI label
 * @property {'number'|'select'|'boolean'} type
 * @property {*}      [default]    — initial value
 * @property {number} [min]        — numeric min
 * @property {number} [max]        — numeric max
 * @property {number} [step]       — numeric step
 * @property {Array<{value:*,label:string}>} [options] — for 'select'
 * @property {string} [unit]       — UI hint (e.g. 'Hz', 'mV')
 * @property {string} [help]       — tooltip / description
 */

/**
 * Descriptor for a channel that a decoder consumes. PS2204A has 2 channels
 * total (A and B); decoders that need more (SPI needs 4 ideal) declare the
 * extras as `required: false` and treat missing channels as unconfigured.
 *
 * @typedef {Object} ChannelSpec
 * @property {string} role         — 'TX','RX','CLK','MOSI','MISO','CS','SDA','SCL','CANH','CANL',...
 * @property {boolean} required
 * @property {string} [help]       — e.g. "falling edge = clock tick"
 */

/**
 * Context passed to a decoder's `decode()` function. The decoder should
 * treat this as read-only.
 *
 * @typedef {Object} DecoderContext
 * @property {number[]} samplesA       — CH A samples in mV (null if disabled)
 * @property {number[]} [samplesB]     — CH B samples (null/undefined if disabled)
 * @property {number}   dt_ns          — ns between consecutive samples
 * @property {number}   rangeMvA       — CH A full-scale voltage (mV)
 * @property {number}   [rangeMvB]
 * @property {Object<string,*>} config — current decoder config values
 * @property {Object<string,string>} channelMap — role→channel ('A'|'B')
 */

/**
 * The top-level decoder descriptor. Each protocol exposes one of these
 * from its own module; the registry aggregates them.
 *
 * @typedef {Object} Decoder
 * @property {string}       id           — unique short id ('uart','i2c',...)
 * @property {string}       name         — display name
 * @property {string}       description  — one-line summary
 * @property {ChannelSpec[]} channels    — which signals this decoder needs
 * @property {ConfigField[]} configSchema
 * @property {(ctx:DecoderContext)=>DecodedEvent[]} decode
 */

export const Kind = {
  BIT:     'bit',
  BYTE:    'byte',
  FRAME:   'frame',
  SYMBOL:  'symbol',
  MARKER:  'marker',
  ERROR:   'error',
};

export const Level = {
  INFO:  'info',
  WARN:  'warn',
  ERROR: 'error',
};
