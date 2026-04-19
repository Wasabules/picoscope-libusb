# SDK streaming protocol — byte-level decoding

Reverse-engineered in April 2026 from a single cold-plug capture of the
official `libps2000` SDK running **22 consecutive `ps2000_run_streaming_ns`
cycles** with one parameter varying at a time, plus 3 siggen calls and one
block capture. All values below are verified on hardware.

The sequence covers the `SDK` streaming mode (1 MS/s gap-free). Block and
native modes share the same skeleton but several fields decode differently
— noted where relevant. `NATIVE` mode details also covered at the end.

## Driver API surface

The decoded protocol is exposed through two public setters on the C
driver (`driver/picoscope2204a.h`), validated on hardware 2026-04-20:

- `ps2204a_set_sdk_stream_interval_ns(dev, ns)` — tune the per-sample
  interval from 500 ns up to 1 ms, in multiples of 10 ns; `ns=0`
  restores the 1 µs / 1 MS/s default. Driver uses fixed 16 384-byte
  async chunks (FPGA commit unit) and scales the libusb timeout from
  500 ms (fast rates) to 30 s (1 ms/sample).
- `ps2204a_set_sdk_stream_auto_stop(dev, max_samples)` — arm a
  client-side sample cap; the stream stops cleanly within one async-
  pool drain once the ring has accumulated `max_samples` entries.
  `max_samples=0` disables (free-running). Overshoot bounded by the
  pool depth (≈ 8 k samples, measured 0.76 % at 1 M samples).

Both setters must be called **before**
`ps2204a_start_streaming_mode(..., PS_STREAM_SDK, ...)`; the streaming
thread freezes `cmd1` at stream start. The Wails GUI re-exposes them
as `SetSdkStreamIntervalNs(ns)` and `SetSdkStreamAutoStop(maxSamples)`.
The headless `uart-diag` tool exposes them via `-sdk-interval-ns` and
`-sdk-max-samples` flags.

Note: **CH B must be enabled** for `PS_STREAM_SDK` or the FPGA halves
the ADC rate to 500 kS/s. The GUI and uart-diag force it on
automatically.

Tooling used:

- `tools/sdk-stream-trace/sdk_stream_param.c` — parametric harness.
- `tools/firmware-extractor/libps_intercept.so` — LD_PRELOAD byte tracer.
- `tools/sdk-stream-trace/phase_table.py`, `active_cmds.py` — analysis.
- Raw trace: `tools/sdk-stream-trace/sdk_param_trace.log`.
- Decoded per-phase table: `tools/sdk-stream-trace/sdk_param_phase_table.txt`.

## Streaming control sequence

Every `ps2000_run_streaming_ns()` call emits exactly four commands on
EP `0x01` back-to-back (µs apart), then 1 MS/s waveform data streams on
EP `0x82` until stop:

1. `cmd1` — compound capture setup (~60 B payload)
2. `cmd2` — compound trigger / waveform-generator params (~56 B)
3. `trigger` — 8-byte "arm" packet
4. *(data flows on EP 0x82)*
5. `stop` — 10-byte halt packet

`trigger` and `stop` are **invariant** across all 22 streaming starts:

```
trigger  02 07 06 00 00 00 00 01          # byte 4=0, byte 7=1
stop     02 0a 00 85 04 99 00 00 00 0a
```

Block mode uses a different trigger — byte 4=`0x40`, byte 7=`0x02`:

```
02 07 06 00 40 00 00 02 01
```

## `cmd1` — capture setup

Compound body with 7 sub-commands in this order:

| Offset range | Sub-cmd       | Variable field                              |
|--------------|---------------|---------------------------------------------|
| 1..10        | `85 08 85`    | **sample_count** — 5-byte BE at offset 5..9 |
| 11..20       | `85 08 93`    | **channel-mode** — `00 06` in SDK mode      |
| 21..30       | `85 08 89`    | **sample_interval_ticks** — 3-byte BE       |
| 31..37       | `85 05 82`    | mode flag (byte 37)                         |
| 38..43       | `85 04 9a`    | always `00 00 00`                           |
| 44..52       | `85 07 97`    | channel enables + PGA (bytes 50..52)        |
| 53..59       | `85 05 95`    | status config                               |
| 60           | `85 04 81`    | tail marker                                 |

### Sample count — `85 08 85` sub-command

**5-byte big-endian integer** starting at offset 5 of the sub-command
payload (= offset 8..12 of the full cmd1 body).

| max\_samples | bytes                |
|--------------|----------------------|
| 10 000       | `00 00 00 27 10`     |
| 100 000      | `00 00 01 86 a0`     |
| 1 000 000    | `00 00 0f 42 40`     |

**This corrects the older "2-byte BE at bytes 9-10" claim** that was
carried over from the 8064-sample block mode. In block mode the value
fits in 2 bytes because `max_samples ≤ 8064`; in SDK streaming it does
not.

### Channel-mode — `85 08 93` sub-command

**In SDK mode this is hard-coded to `00 06`** regardless of interval,
range, or enabled channels.

The old *"timebase-dependent (0x27,0x2f)…(0x00,0x28)"* lookup table
applies **only to block mode**. For `SDK` or `NATIVE` streaming, the
driver should emit `00 06` verbatim.

### Sample-interval ticks — `85 08 89` sub-command

**3-byte big-endian integer = `sample_interval_ns / 10`** (count of
10 ns FPGA ticks per sample).

| Requested interval | bytes           | Decoded |
|--------------------|-----------------|---------|
| 500 ns             | `00 00 32`      | 50      |
| 1 µs               | `00 00 64`      | 100     |
| 2 µs               | `00 00 c8`      | 200     |
| 5 µs               | `00 01 f4`      | 500     |
| 10 µs              | `00 03 e8`      | 1000    |

**This is not `2^timebase`**. That encoding is specific to block mode.
For SDK streaming, compute `ticks = interval_ns / 10` directly and
emit as 3-byte BE.

### Mode flag — `85 05 82` byte 37

- `0x41` — streaming (SDK / NATIVE)
- `0x01` — block capture

### Gain / enable — `85 07 97` bytes 50..52

Unchanged from the previously-documented encoding:

- Byte 50 — `0x20 | (b_enabled<<1) | a_enabled`
- Byte 51 — `(b_dc<<7) | (a_dc<<6) | (b_bank<<5) | (a_bank<<4) | (b_sel<<1) | b_200`
- Byte 52 — `(a_sel<<5) | (a_200<<4)`
- Disabled channel slot: `bank=0, sel=1, 200=0`

Verified against 6 CH A range sweeps, 1 CH B range sweep, and AC/DC toggles
on each channel.

## `cmd2` — trigger / waveform params

Compound body, stable skeleton:

```
02 85 0c 86 00 40 00 [CT1 CT2 CT3] 95 02 65 00 00
   85 05 87 00 08 00 00
   85 0b 90 00 38 00 00 00 00 00 00 00 00
   85 08 8a 00 20 00 00 00 00 00
   0b 03 [AS] 02 02 0c 03 0a 00 00 85 04 81
```

Only one field genuinely varies in streaming mode:

- **Bytes 7..9 (`CT1 CT2 CT3`)** — opaque counter. Drifts by ~0–3 per
  call across a 5-minute session. Almost certainly a host-side
  timestamp or sequence number the device echoes back. Our driver
  emits `00 00 00` (baked into the `SDK_CMD2` template, validated
  stable over 5× repeat streams of 229 360 samples each).

All other bytes are constant across the 22 streaming starts. The long
`85 0b 90 ... 85 08 8a ...` block has unknown semantics but is safe to
replay verbatim.

### Correction — byte 47 and `auto_stop`

An earlier version of this document claimed **byte 47 = `0x28` when
`auto_stop=1`**. That was wrong. The original evidence came from
confusing phase 22 (`ps2000_run_streaming_ns` with `auto_stop=1`) with
phase 28 of the same trace (the legacy `ps2000_run_streaming` ms-interval
call, whose cmd1 and cmd2 encode differently).

A fresh parametric capture (`tools/sdk-stream-trace/sdk_autostop_matrix.c`,
10 phases sweeping `max_samples ∈ {10 k, 50 k, 100 k, 200 k, 1 M}` ×
`interval_ns ∈ {500, 1 000, 2 000, 5 000, 10 000}` with `auto_stop ∈
{0, 1}`) shows:

- `cmd2[47]` is **`0x00` in every phase**, including `auto_stop=1`.
- Phase 1 (`as=0`, 100 k, 1 µs) and phase 3 (`as=1`, 100 k, 1 µs) are
  **byte-identical** — same `cmd1`, same `cmd2`, same trigger packet.

Therefore `auto_stop` is **not transmitted to the device at all**. The
proprietary SDK implements it client-side: `ps2000_get_streaming_last_values`
counts delivered samples and stops polling once `max_samples` is reached.
Our driver mirrors this with a 3-line counter in `sdk_stream_cb`;
overshoot is bounded by the 4× 16 KB async-pool depth (≤ ~8 k samples,
measured 0.76 % at 1 M samples).

Public API: `ps2204a_set_sdk_stream_auto_stop(dev, max_samples)` (0 =
free-running).

## Siggen configuration

Each call to `ps2000_set_sig_gen_built_in` emits on EP `0x01`:

```
02 85 04 9b 00 00 00 85 21 8c 00 e8    # LUT-prime trigger
02 85 05 82 00 08 00 01                # BUFTYPE1 follow-up
```

followed by an 8 192-byte LUT upload on EP `0x06`.

Critically, **the EP 0x01 12-byte command is identical for sine, square,
and DC modes** — only the LUT content changes. Waveform type, frequency,
amplitude, and DC offset all live in the LUT. A `pkpk=0, wave=DC_VOLTAGE,
offset=0` "siggen-off" call emits nothing on EP 0x01 (it is a host-side
no-op in the SDK).

## Idle heartbeat

When idle — between streaming cycles, or after open before any
streaming is armed — the SDK continuously emits at ~166 ms cadence:

- `02 85 07 97 00 14 00 [EN] [B51] [B52]` (the same PGA sub-command)
- Interleaved with `02 85 04 9a 00 00 00 85 ...` every ~1 s

Over the full 500-second capture: 2 335 heartbeat `85 07 97` packets
and 467 `85 04 9a` against 142 "active" command packets.

Our driver does not emit these when idle. Effect on device state is
unverified; it may or may not matter for Android cold-plug stability.

## What did *not* affect anything

Parameters whose changes produced no visible bytes on the wire:

- `overview_buffer_size` (1 000 vs 50 000 vs 500 000 — invalid values
  rejected host-side before emission).
- `aggregate` flag (`aggregate=0` rejected host-side).
- Most range changes on the "other" channel that also toggles bits in
  byte 51 — already captured by the existing encoding.

## References

- Raw trace: [`tools/sdk-stream-trace/sdk_param_trace.log`](../tools/sdk-stream-trace/)
- Decoded phase-by-phase table: [`sdk_param_phase_table.txt`](../tools/sdk-stream-trace/sdk_param_phase_table.txt)
- Analysis scripts: `phase_table.py`, `active_cmds.py` in the same dir
- Broader protocol overview: [`protocol.md`](./protocol.md)
