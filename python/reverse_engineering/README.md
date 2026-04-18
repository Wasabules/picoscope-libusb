# Reverse-engineering archive

Every script here was written to answer one specific question about the
PicoScope 2204A USB protocol. Most are no longer actively run — the answers
they produced are encoded in the C driver (`driver/picoscope2204a.c`) and the
protocol notes (`docs/protocol.md`).

They are preserved because they document **the method**, not just the result.
If you want to adapt this driver to another PicoScope model (2202, 2205,
2205A, 3000-series, …), this is a cookbook.

## Directory map

| Folder | Question it answered | Where the answer lives today |
|---|---|---|
| `pga/` | How does the programmable-gain amplifier map range → gain bytes? | `PGA_TABLE` in `driver/picoscope2204a.c`; `docs/protocol.md` §"PGA lookup" |
| `raw_format/` | Is the ADC 8-bit or 16-bit? How is the buffer laid out? | 8-bit; `uint8 - 128` → signed; `docs/protocol.md` §"Hardware Facts" |
| `dual_channel/` | How are CH A and CH B interleaved in the shared 16 KB buffer? | Tail-interleaved B,A,B,A with B at even offsets; `docs/protocol.md` §"Dual Channel" |
| `native_streaming/` | Can we sustain gap-free streaming above ~100 S/s? | No — FPGA native mode is hardware-capped. Use `PS_STREAM_FAST` (block bursts) or `PS_STREAM_SDK` (1 MS/s via LUT) instead |
| `fast_block/` | How fast can we cycle back-to-back block captures? | ~330 kS/s verified (16× faster than naive polling) |
| `sdk_capture/` | Capture reference USB traces from the official SDK for byte-by-byte diff | Run under `LD_PRELOAD=./usb_interceptor.so` to produce `usb_trace.log` |
| `experiments/` | One-off probes (10 MHz OCXO, async FPGA upload, skip-waveform-upload) | N/A — exploratory |

## Running

Scripts resolve imports from `python/picoscope_libusb_full.py` via their
own `__file__`, so you can invoke them from anywhere:

```bash
python3 python/reverse_engineering/pga/test_pga_debug.py
```

A few scripts (`pga/test_sdk_pga.py`, `sdk_capture/*`) use `ctypes` against
the official `libps2000.so` instead of our driver — they require the Pico
suite installed at `/opt/picoscope/`.

## Common recipe: trace a new SDK feature

1. Write a minimal C or Python driver that exercises the feature against
   `libps2000.so` (see `python/trace_sdk_features.c` for the template).
2. Build `tools/firmware-extractor/usb_interceptor.so`.
3. Run the harness under `LD_PRELOAD=./usb_interceptor.so`.
4. Diff the resulting `usb_trace.log` against a baseline trace to isolate
   the new opcodes.
5. Port the sequence to `driver/picoscope2204a.c` and round-trip against
   the device.

This is how every feature in `PGA_TABLE`, `PS_STREAM_NATIVE`, and
`PS_STREAM_SDK` was originally reverse-engineered.
