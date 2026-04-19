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

Lives partly in `cmd1` (`85 05 95` status byte toggles `0x55` ↔ `0xff`)
and partly in a dedicated `cmd2` sub-command `85 0c 86` carrying
direction, threshold, and hysteresis.

`02 07 06` is the data-commit signal sent identically in block and
streaming.

### Window trigger

`cmd2[9..10]` and `cmd2[13..14]` carry the `[lo, hi]` threshold pair,
with `cmd[21] = 0x0d` selecting window mode.

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

EEPROM pages `0x40`–`0xC0` carry factory calibration data. The driver
reads them into `dev->eeprom_raw[256]`; the internal layout is not yet
fully decoded so the bytes are exposed via `ps2204a_get_eeprom_raw()`
rather than being applied automatically.

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
