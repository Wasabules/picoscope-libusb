# PicoScope 2204A USB protocol

Reverse-engineered from USB traces of the official PicoSDK plus direct
experimentation. Values listed here are **verified against hardware**
unless explicitly marked otherwise.

## Device identity

| Field     | Value                                       |
|-----------|---------------------------------------------|
| VID / PID | `0x0CE9` / `0x1007`                         |
| USB       | 2.0 High-Speed                              |
| Class     | Vendor-Specific (`0xFF`)                    |
| MCU       | Cypress EZ-USB FX2 (CY7C68013A)             |
| FPGA      | Xilinx (185 KB bitstream, sig `0x5599AA66`) |
| ADC       | **8-bit**, 100 MSPS single / 50 MSPS dual   |

> The proprietary SDK scales samples to ±32767 to look 16-bit-ish, but
> the wire format is plain `uint8` centred at 128. `signed = byte - 128`,
> then `mV = signed * range_mV / 128.0`.

## Endpoints

| EP     | Dir | Purpose                                      |
|--------|-----|----------------------------------------------|
| `0x01` | OUT | Commands (always 64-byte zero-padded)        |
| `0x81` | IN  | Command ACKs — **drain after every command** |
| `0x82` | IN  | Waveform data (16 KB chunks, `uint8`)        |
| `0x06` | OUT | Firmware upload (FPGA bitstream + AWG LUT)   |

Failing to drain `0x81` corrupts the command queue after ~5 ops. FX2
firmware is uploaded via **control transfers** (`bRequest=0xA0`), not
EP `0x06`.

## Command framing

All commands are zero-padded to 64 bytes. Byte 0 is the command type:

| Type   | Meaning              |
|--------|----------------------|
| `0x02` | Standard / compound  |
| `0x04` | Bootstrap (FPGA)     |

A compound command (`0x02` ...) embeds several sub-commands back-to-back.
Each sub-command starts with opcode `0x85` and carries its own length.

## Initialisation sequence

Ordering matters — skipping steps leaves the device in status `0x7b`
(needs a physical reconnect to recover).

1. Find device → detach kernel driver → claim interface 0.
2. **Upload FX2 firmware** via control transfers:
   - Halt CPU: write `0x01` to `0xE600`
   - Stream chunks to their target addresses
   - Release CPU: write `0x00` to `0xE600`
3. Wait ~1 s for USB re-enumeration (device gets a new address).
4. Init ADC (opcode `0x81`).
5. Read EEPROM pages `0x00/0x40/0x80/0xC0` → serial `JOxxxxxxxx` and
   calibration date.
6. Upload **FPGA bitstream** (~182 KB) on EP `0x06` in 32 KB chunks.
7. Upload **AWG waveform LUT** (8192 bytes) on EP `0x06`. May time out
   after the FPGA upload — device is still functional.

## Capture setup (`cmd1` compound packet)

The capture setup is one `0x02` packet containing these sub-commands
in order. **Several fields encode differently in block vs.
streaming (SDK / native) mode** — see the per-row notes.

| Sub-cmd        | Role              | Key bytes                                                                     |
|----------------|-------------------|-------------------------------------------------------------------------------|
| `85 08 85 ...` | sample count      | **Block**: 16-bit BE at bytes 9–10 • **SDK/native**: 5-byte BE at bytes 6–10  |
| `85 08 93 ...` | channel config    | **Block**: bytes 19–20 timebase-dependent lookup • **SDK/native**: `00 06`    |
| `85 08 89 ...` | buffer / interval | **Block**: bytes 29–30 = `2^timebase` BE, cap `FFFF` • **SDK/native**: 3-byte BE at bytes 28–30 = `interval_ns / 10` |
| `85 05 82 ...` | get-data / mode   | byte 37 = `0x01` block, `0x41` streaming                                      |
| `85 04 9a ...` | timebase config   | always `00 00 00`                                                             |
| `85 07 97 ...` | run block + gain  | bytes 50–52 = channel enables + PGA (same in all modes)                       |
| `85 05 95 ...` | status config     | byte varies by trigger mode                                                   |

Notes on streaming encoding (verified April 2026 against 22 parametric
`ps2000_run_streaming_ns` captures — see
[`sdk-streaming-protocol.md`](./sdk-streaming-protocol.md)):

- **Sample count in SDK/native is a 5-byte big-endian integer**
  (e.g. 1 000 000 = `00 00 0f 42 40`). A 2-byte BE is enough for
  block mode because `max_samples ≤ 8064` in that mode, but SDK
  streaming accepts arbitrarily large counts.
- **Channel config is `00 06` unconditionally in SDK/native mode** —
  the timebase lookup below applies only to block mode.
- **Buffer/interval field in SDK/native is literally
  `interval_ns / 10`** (count of 10 ns FPGA ticks per sample), not
  `2^timebase`. 500 ns → `00 00 32`; 1 µs → `00 00 64`;
  10 µs → `00 03 e8`.

### Gain bytes in `85 07 97`

- **Byte 50** — `0x20 | (b_enabled<<1) | a_enabled`.
  `0x21` = CH A only · `0x23` = A+B.
- **Byte 51** — `(b_dc<<7) | (a_dc<<6) | (b_bank<<5) | (a_bank<<4) | (b_sel<<1) | b_200`.
- **Byte 52** — `(a_sel<<5) | (a_200<<4)`.
- Disabled channel slot: `bank=0, sel=1, 200=0`.

### PGA table (verified against SDK trace)

| Range   | bank | sel | 200 | Notes                                      |
|---------|------|-----|-----|--------------------------------------------|
| 50 mV   | 0    | 7   | 0   | Shares PGA with 5 V — digital ÷70 scaling  |
| 100 mV  | 1    | 6   | 0   |                                            |
| 200 mV  | 1    | 7   | 0   |                                            |
| 500 mV  | 1    | 2   | 1   |                                            |
| 1 V     | 1    | 3   | 0   |                                            |
| 2 V     | 1    | 1   | 0   |                                            |
| 5 V     | 0    | 7   | 0   | Same PGA as 50 mV                          |
| 10 V    | 0    | 2   | 0   |                                            |
| 20 V    | 0    | 3   | 0   |                                            |

Bank 1 = high-sensitivity (100 mV – 2 V); bank 0 = low-sensitivity
(50 mV, 5 V – 20 V).

### Channel / buffer bytes per timebase — **block mode only**

```
tb=0  → chan=(0x27,0x2f)  buf=(0x00,0x01)
tb=1  → chan=(0x13,0xa7)  buf=(0x00,0x02)
tb=2  → chan=(0x09,0xe3)  buf=(0x00,0x04)
tb=3  → chan=(0x05,0x01)  buf=(0x00,0x08)
tb=5  → chan=(0x01,0x57)  buf=(0x00,0x20)
tb=10 → chan=(0x00,0x28)  buf=(0x04,0x00)
```

`buf` follows `2^tb` capped at `0xFFFF`. This lookup applies **only
to block captures**. Block mode accepts `tb` up to 23.

In SDK and native streaming modes the channel bytes are fixed at
`00 06` and the buffer field carries `interval_ns / 10` as a 3-byte
big-endian integer — see the cmd1 table above and
[`sdk-streaming-protocol.md`](./sdk-streaming-protocol.md).

## Timebase formula

```
interval_ns = 10 * 2^timebase
```

`tb=0` → 10 ns (100 MSPS). `tb=10` → 10240 ns. Dual-channel mode cannot
use `tb=0` — the single ADC alternates, so dual-channel timebase starts
at `tb=1`.

Max samples per block: **8064 single-channel**, **3968 dual-channel**
(shared 16 KB USB buffer).

## Dual-channel buffer layout

SDK returns a single 16 KB buffer (not 2×16 KB). Valid data lives in the
**tail**; the first ~¾ is stale padding.

```
[padding ... padding][B A B A B A ... B A]
                     ^--- last 2*n_samples bytes
```

Even offsets (0, 2, 4, …) in that tail are **CH B**, odd offsets are
**CH A**. Verified: A-only σ ≈ 13.6 ≡ dual-A σ ≈ 13.7, B-only σ ≈ 1.9
≡ dual-B σ ≈ 1.9.

## Status byte (EP `0x81`)

| Byte   | Meaning                                               |
|--------|-------------------------------------------------------|
| `0x33` | Capture pending (`0011 0011`)                         |
| `0x3b` | Data ready — bit 3 set (`0011 1011`)                  |
| `0x7b` | Error / overflow — bit 6 set (`0111 1011`) — re-init  |

Recovery from `0x7b`: flush + re-setup channels. If that fails, only a
physical disconnect or full re-upload of FX2 firmware clears it.

## Streaming modes

Three distinct modes live behind the same EP layout:

| Mode         | Rate           | Mechanism                                     | Use case                     |
|--------------|----------------|-----------------------------------------------|------------------------------|
| `FAST`       | ~330 kS/s      | Rapid back-to-back block captures, 13 ms gap  | High-rate with gaps OK       |
| `NATIVE`     | ~100 S/s       | FPGA native continuous (hardware-capped)      | DC monitoring                |
| `SDK`        | 1 MS/s gap-free| SDK-protocol replay (LUT upload + `85 04 9b`) | Gap-free decode, UART etc.   |

`NATIVE` mode prepends ~3 bytes of framing (`0x00 / 0x01`) to the first
packet; the driver skips the first 32 bytes to avoid persistent
`-range_mV` spikes. `FAST` has a fixed per-sample interval of ~1280 ns
regardless of the requested timebase — always use
`ps2204a_get_streaming_dt_ns()` for rendering. `SDK` halves its rate
when CH B is disabled, so the driver forces CH B on for `SDK` sessions.

## Trigger

### Level trigger (edge)

Lives partly in `cmd1` (`85 05 95` status byte) and partly in a `cmd2`
sub-command `85 0c 86` carrying direction, threshold and hysteresis.

**The status byte selects the source channel**, it is not a simple
armed/free-run flag:

| value | meaning |
|-------|---------|
| `0xff` | free-run, no trigger |
| `0x55` | armed, source = channel A |
| `0x33` | armed, source = channel B |
| `0x05` | pulse-width qualifier active |

Sending `0x55` while asking for a channel-B trigger leaves the device
waiting on a condition it was never told to watch: the capture never
completes and the driver falls back to its poll timeout with a
free-running buffer. That was the behaviour until 2026-07-28.

**Threshold**, measured by sweeping `ps2000_set_trigger` under
`LD_PRELOAD` and reading what the SDK emitted:

```
byte = base + floor(thr_sdk16 / 295)
base = 0x7d (channel A) | 0x81 (channel B)
```

Seven points from −32000 to +32000 reproduce exactly, `floor` included —
the negative side sits one count further out than rounding would give.
Full scale lands near ±111 counts rather than ±128 because the analog
gain is about 1.15.

Slots `cmd2[11..12]` belong to channel B and `cmd2[13..14]` to channel A.
The active pair is `(threshold, threshold − hyst)` rising and
`(threshold + hyst, threshold)` falling. The inactive channel gets its own
centre with the same directional layout and a 4-count band:

```
idle = 0x7c (channel A) | 0x81 (channel B)
```

`cmd2[21]` = `0x09` rising, `0x12` falling.

### Pre-trigger sample count

A triggered block splits in two and the device has to be told both halves:

| field        | carries                                            |
|--------------|----------------------------------------------------|
| `85 08 85`   | post-trigger sample count                          |
| `85 08 93`   | timebase constant **plus** pre-trigger sample count |

Traced across delays and sample counts, `85 08 93` minus the pre-trigger
count is constant per timebase and equals the lookup table above:

| delay | n | tb | post | `85 08 93` | pre | difference |
|-------|------|----|------|------|------|-----|
| 0   | 2000 | 5 | 2000 | 343  | 0    | 343 |
| −50 | 2000 | 5 | 1000 | 1343 | 1000 | 343 |
| −75 | 2000 | 5 | 500  | 1843 | 1500 | 343 |
| −50 | 4000 | 5 | 2000 | 2343 | 2000 | 343 |
| −50 | 2000 | 7 | 1000 | 1109 | 1000 | 109 |

Sending the bare timebase constant asks for **zero** pre-trigger samples.
The device then only guarantees what follows the trigger, and anything a
parser takes from before it is unmanaged circular-buffer residue. Measured
as overlay spread across eight captures, this driver against the SDK on the
same stimulus:

| stimulus | before | after | official SDK |
|----------|--------|-------|--------------|
| square 150 Hz | 3.5 mV | 3.7 mV | 4.0 mV |
| square 2 kHz  | 59.2 mV | 5.5 mV | — |
| square 10 kHz | 23.1 mV | 21.4 mV | 12.8 mV |
| sine 150 Hz   | 5.5 mV | 5.2 mV | 6.0 mV |
| sine 2 kHz    | 71.2 mV | 7.1 mV | — |
| sine 10 kHz   | 79.2 mV | 10.8 mV | 5.9 mV |

### Trigger position in the returned block

The device overshoots the post-trigger sample count it is given: the event
lands earlier than `valid_len − post_captured`. The overshoot is **not** a
constant — it takes one of two values, and **the first two bytes of the
transfer say which**.

Those two bytes are a status marker rather than samples (the parser has
always skipped them). The mapping:

| marker | overshoot | blocks seen |
|--------|-----------|-------------|
| `0x57a7` | 33 samples | 58 |
| `0x52a2` | 30 samples | 32 |

Established by capturing the official SDK with usbmon and correlating what
`ps2000_get_values` returned against the raw block on the wire. Every block
aligns at correlation `1.000000` — the returned array is a plain affine map
of the raw bytes (gain 294.5 about 129) — so the offset the SDK picked is
exact rather than fitted. Over 90 captures spanning timebases 3/5/7 and five
AWG frequencies no third marker appeared, the mapping never varied with
timebase, and `b1 − b0` was 80 throughout.

An earlier measurement read this as "31 ± 2 samples, constant"; the ±2 was
the two states averaging out. Treating it as fixed cost a factor of two in
trigger jitter, since 31 sits between 30 and 33 and is never right:

| stimulus | fixed 31 | by marker | SDK library |
|----------|----------|-----------|-------------|
| square 10 kHz | 1.52 | 0.99 | 0.82 |
| sine 10 kHz | 1.58 | 0.50 | 0.49 |
| sine 2 kHz | 1.37 | 1.20 | 1.21 |

On hardware after the change, ours vs the SDK: 0.90/0.86, 0.51/0.46,
1.16/1.15 samples. Unknown markers fall back to 31.

The parser anchors on the corrected position.

**Dual-channel behaves differently, and needs no marker.** The same
correlation run with both channels enabled — 72 captures over timebases 3/5/7
and three AWG frequencies, every one aligning at correlation `1.000000` on
both channels — gives a flat **31 pairs** of overshoot throughout, with the
marker stuck at `0x57a7`. `0x52a2`, which is 47 % of single-channel blocks,
appeared **0 times** in 72 dual blocks. Dual capture has one state where
single has two, so the fixed constant is the measured answer there rather
than a fallback. (Plausible reading, not established: the marker selects an
interleaved sampling phase that only exists when one channel gets the full
rate.) That run also reconfirms the interleave — channel B on even byte
indices, A on odd, in all 72 blocks.

Measure this with a square wave, not a sine: a slow sine costs the crossing
detector part of an edge to arm, which shows up as tens of samples of
apparent offset belonging to the measurement rather than the device. The
same experiment read 84/143/239 samples on a sine and a flat 31 on a square.

With the correction, `delay_pct` lands where it says:

| delay_pct | requested | measured (single) | measured (dual) |
|-----------|-----------|-------------------|-----------------|
| −80 | 90 % | 89.9 % | — |
| −50 | 75 % | 75.1 % | 74.9 % |
| 0   | 50 % | 50.1 % | 50.0 % |
| +50 | 25 % | 25.0 % | 25.0 % |
| +80 | 10 % | 10.1 % | — |

`02 07 06` is the data-commit signal sent identically in block and
streaming.

### Window trigger

Traced in full from the SDK's advanced-trigger path
(`ps2000SetAdvTriggerChannelConditions` / `Directions` / `Properties`) under
usbmon, one configuration per second so each `cmd2` is attributable by
timestamp.

The active channel's two thresholds go in that channel's own slots; the other
channel gets `00 ff` plus its idle band, exactly as in LEVEL:

| source | `[7..8]` | `[9..10]` | `[11..12]` | `[13..14]` |
|--------|----------|-----------|------------|------------|
| A | `00 ff` | lower pair | idle B | upper pair |
| B | lower pair | `00 ff` | upper pair | idle A |

Direction decides which side of each threshold carries the hysteresis `h`:

| direction | lower pair | upper pair |
|-----------|------------|------------|
| ENTER | `(lo, lo−h)` | `(hi+h, hi)` |
| EXIT | `(lo+h, lo)` | `(hi, hi−h)` |
| ENTER_OR_EXIT | `(lo, lo−1)` | `(hi, hi−1)` |

`cmd[21]` is channel-dependent here, unlike LEVEL:

| | ENTER | EXIT | ENTER_OR_EXIT |
|---|-------|------|---------------|
| source A | `0x0d` | `0x0e` | `0x0f` |
| source B | `0x29` | `0x31` | `0x39` |

Thresholds use the same `base + floor(thr_sdk16 / 295)` as LEVEL, and the
hysteresis divides by 295 too. Confirmed on an asymmetric window
(+16000 / −4000): `0x81 + 54 = 0xb7` upper, `0x81 − 14 = 0x73` lower, both
exact. An earlier version of this driver divided by 288 and rounded, which
drifts by a count over most of the range.

**On the hardware side, window mode is weak.** Against a 5 kHz sine on
channel B, our ENTER on `[+200, +800] mV` locks hard — overlay spread 6.2 mV
against a 684 mV signal — but EXIT never locks. Neither does any of the four
configurations driven through the SDK itself: its best is 462 mV against a
695 mV free-running baseline. So EXIT not locking is a property of this
device or mode, not of the encoding, which matches the SDK byte for byte on
all eleven traced configurations (locked in by `test_parse`). Note also that
a window straddling zero is ambiguous by construction on a sine — the signal
enters it twice per period at different phases, so the spread stays high even
with a perfect trigger.

### Pulse-width qualifier (PWQ)

`cmd1` status byte becomes `0x05`; additional sub-commands encode
min/max pulse width and qualifier mode.

### ETS (equivalent-time sampling)

Requires a repetitive input. Triggered via a flood of
`02 01 01 80` interleave packets whose cadence selects the effective
timebase. The driver implements the basic flow; exotic configurations
remain untested.

## Signal generator

The `85 0c 86` opcode the early drivers used was a dead end — it
**never produced a sine**. The real siggen is an **AWG**: the driver
computes a 8192-byte LUT on the host and uploads it on EP `0x06`.
Frequency is programmed via `freq_param = int(freq_hz * 0x400)`.

### Siggen + `SDK` streaming — 3rd-phase LUT injection

The `SDK` streaming setup emits two identical `85 04 9b + 85 21 8c`
primer packets each followed by a **DC** LUT (all samples = `0x07ee`,
2030). Those two primers initialise the FPGA's DDS pacing and leave the
DAC loaded with DC — so a naïve replay of `SDK` streaming keeps the
generator silent even when the user has configured a waveform.

To keep the DAC alive, the driver injects a **third** `85 04 9b +
85 21 8c` packet carrying the user's frequency, followed by the user's
waveform LUT and a `BUFTYPE1` commit, **between the TB/gain packet and
`cmd1`**:

```
… → TB_GAIN (85 07 97 …)
    → 9B_USER (85 04 9b + 85 21 8c, user freq_param)
    → user LUT (8192 bytes on EP 0x06)
    → BUFTYPE1 commit
    → cmd1 (streaming flag 0x41)
    → cmd2 → trigger
```

Block capture and `FAST` streaming do not need this phase — the DAC is
programmed once by `ps2204a_set_siggen` and remains live across block
captures.

## Calibration

Factory per-range DC-offset table is compiled in, derived on a
reference PS2204A with CH A shorted to GND:
`offset_mV = our_raw_mean − sdk_reported_mean`. 50 mV has an inverted
sign because it shares the 5 V PGA plus a digital ÷70 scaling.

`ps2204a_calibrate_dc_offset()` auto-fits a fresh table from a live
0 V reading; `ps2204a_set_range_calibration()` lets callers store a
per-range `(offset_mV, gain)` pair that applies at capture time.

### EEPROM factory trim

The 256 bytes read at open (`dev->eeprom_raw`) carry the unit's own
factory trim. Partially decoded:

| Offset | Contents |
|--------|----------|
| `0x13` | serial, ASCII (`JOxxxxxxx`) |
| `0x1D` | calibration date, ASCII (`21Feb22`) |
| `0x25` | **9 × int16 LE — per-range DC offset**, in 1/256 ADC code, expressed *after* gain |
| `0x6D` | 9 × int16 LE, Q14 — gain block 1 (≈ 1.112–1.123) |
| `0x7F` | 9 × int16 LE, Q14 — gain block 2 (≈ 1.142–1.151) |
| `0x91` | 18 × `0x4000` — 2 × 9 slots at Q14 unity |
| `0xC5` | 9 × int16 LE, Q14 — gain block 3 (≈ 1.144), unidentified |

All range-indexed blocks run `PS_50MV … PS_20V`.

**Offsets — confirmed.** Correlating the words at `0x25` against the
hand-measured reference table gives Pearson *r* = 0.9989. The conversion is

```
offset_mV = (word / 256) × (range_mV / 128) / gain
```

Reconstructing the hand-measured table from the EEPROM alone lands within
1.1 % on average and 0.1 % on several ranges — inside the noise of the
original measurements.

**But they must not be applied to the samples.** Both tables carry the same
≈ −3.5 ADC counts (EEPROM −3.06…−3.85, built-in −2.85…−3.83), and applying
it walks away from the SDK instead of towards it. Difference from the SDK on
a flat input, in ADC counts:

| range | 200 mV | 500 mV | 1 V | 2 V | 5 V |
|-------|--------|--------|-----|-----|-----|
| offset applied | +4.44 | +4.04 | +4.22 | +4.51 | +4.33 |
| offset not applied | +0.43 | +0.13 | −0.07 | +1.17 | +0.04 |

A constant in counts across ranges is the signature of an ADC zero-point
term, and the device evidently already compensates it — the stored word
records a correction that has been made, not one still owed. Applying it a
second time is exactly the +4.3. Flipping the sign is not the answer either;
that lands at −3.5.

With no 0 V reference available this says we agree with the SDK to well under
a count, not that either is absolutely right. `ps2204a_calibrate_dc_offset()`
against a shorted input remains the honest way to get an absolute zero, and
`ps2204a_set_range_calibration()` still installs one.

**Gains — confirmed, and the block assignment was right.**

This section previously said the opposite. The test behind that was to ask
whether one AWG signal measured the same across four ranges, which gave
2.61 % spread with the EEPROM gains against 1.44 % with the built-in ones.
That test conflates the calibration with the AWG's own accuracy and with the
8-bit quantisation floor — a fixed 250 mV signal is 1.6 counts on the 20 V
range — and it pointed the wrong way.

The clean test compares against the SDK range by range. The SDK applies the
factory calibration, so agreeing with it *is* the goal, and choosing one AWG
amplitude per range (≈60 % of full scale) keeps every point well clear of
quantisation. Nine ranges, three passes each:

| range | SDK | built-in | EEPROM |
|-------|-----|----------|--------|
| 50 mV | 28.41 mV | 33.97 (**+19.6 %**) | 29.23 (+2.9 %) |
| 100 mV | 59.06 | 61.05 (+3.4 %) | 59.89 (+1.4 %) |
| 200 mV | 119.42 | 122.12 (+2.3 %) | 120.22 (+0.7 %) |
| 500 mV | 300.32 | 302.25 (+0.6 %) | 301.60 (+0.4 %) |
| 1 V | 603.75 | 604.71 (+0.2 %) | 600.33 (−0.6 %) |
| 2 V | 1210.49 | 1186.41 (−2.0 %) | 1213.24 (+0.2 %) |
| 5 V | 2024.57 | 1937.10 (−4.3 %) | 2022.62 (−0.1 %) |
| 10 V | 2021.35 | 1975.01 (−2.3 %) | 1993.64 (−1.4 %) |
| 20 V | 2008.99 | 2098.56 (+4.5 %) | 2014.48 (+0.3 %) |

| table | mean abs. error | RMS | worst |
|-------|-----------------|-----|-------|
| built-in | 4.34 % | 7.05 % | +19.57 % |
| EEPROM | **0.88 %** | **1.22 %** | +2.90 % |

The EEPROM table is five times more accurate. It is now the default, with
the built-in table as the fallback when the EEPROM does not read or does not
parse; `ps2204a_use_eeprom_calibration(dev, false)` selects the built-in one.
This also brings the code in line with the header, which had documented
EEPROM-by-default all along.

The +19.6 % at 50 mV vindicates a long-standing suspicion recorded here: the
built-in 50 mV gain (1.4722) is a ~30 % outlier against every other range
(~1.11–1.19) and against all three EEPROM blocks (~1.12). It was measured
against a 28 mV reference — the smallest reference on the smallest range —
and it was indeed a bad measurement.

## USB tracing tips

```bash
# Wireshark / tshark (usbmon kernel module required)
tcpdump -i usbmon1 -w capture.pcap
tshark -r capture.pcap -Y "usb.capdata"

# LD_PRELOAD interceptor against the official SDK
gcc -shared -fPIC -o usb_interceptor.so \
    python/reverse_engineering/usb_interceptor.c -ldl -lusb-1.0
LD_PRELOAD=./usb_interceptor.so ./any_sdk_program   # → usb_trace.log
```

The `python/reverse_engineering/` directory is the archive of scripts
that produced every table above.
