# PicoScope 2204A Wails GUI

Cross-platform oscilloscope application for the reverse-engineered
PicoScope 2204A libusb driver. Wails v2 (Go backend) + Svelte 3
frontend, statically linked against `../driver/libpicoscope2204a.a`.

## Prerequisites

- Go 1.21+
- Node.js 20+ and npm
- Wails v2.11+: `go install github.com/wailsapp/wails/v2/cmd/wails@latest`
- `libusb-1.0-0-dev` (Debian/Ubuntu)
- WebKit2GTK:
  - Ubuntu 24.04+: `libwebkit2gtk-4.1-dev` → build with `-tags webkit2_41`
  - Older distros: `libwebkit2gtk-4.0-dev` → no build tag
- Static C driver built: `cd ../driver && make`

## Build

```bash
# Live development (Vite HMR on the frontend, Go rebuilds on change)
make dev

# Production — single self-contained binary at build/bin/picoscope-gui
make build

# Sanity check: confirm the binary is linked against the *current* .a
make verify
```

`make build` rebuilds the driver first, then runs `go clean -cache`,
`touch main.go`, and `wails build -clean`. The extra dance exists
because cgo only invalidates its artefact cache when `.h` or `.go`
files change — a rebuilt `libpicoscope2204a.a` with identical headers
gets silently ignored, and a stale `.a` gets relinked. `make verify`
greps the resulting binary for a driver symbol to make sure the relink
actually happened.

If you prefer raw commands, the equivalent is:

```bash
(cd ../driver && make)
go clean -cache && touch main.go && wails build -tags webkit2_41 -clean
```

## USB access

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0ce9", MODE="0666"' | \
  sudo tee /etc/udev/rules.d/99-picoscope.rules
sudo udevadm control --reload && sudo udevadm trigger
```

Without the udev rule the GUI must be launched as root or via `sg dialout`
depending on your distro.

## First launch — firmware extraction

The driver cannot boot the scope without four firmware blobs (FX2
microcode, FPGA bitstream, AWG LUT). They are not distributed with this
project. On first launch the GUI detects the missing files and opens an
extraction wizard that walks you through pulling them from your own
device.

If you prefer the CLI flow, see `tools/firmware-extractor/` or
[`docs/firmware-extraction.md`](../docs/firmware-extraction.md).

## Features

### Acquisition

- Block capture (single or dual channel, 8064 / 3968 samples)
- Three streaming modes, selectable at runtime:
  - **Fast block** (~330 kS/s) — rapid back-to-back blocks, small gaps
  - **Native** (~100 S/s) — FPGA native mode, DC monitoring
  - **SDK** (1 MS/s default, 2 MS/s → 1 kS/s tunable, gap-free) —
    SDK-protocol replay; right mode for decode. Tunables:
    - `SetSdkStreamIntervalNs(ns)` — per-sample interval 500 ns..1 ms
      (multiples of 10); 0 restores the 1 µs default.
    - `SetSdkStreamAutoStop(maxSamples)` — client-side sample cap; the
      stream stops cleanly once the ring has accumulated `maxSamples`
      entries (overshoot ≤ ~8 k = async-pool depth).
    - Call both BEFORE `StartStreamingMode("sdk")` — cmd1 is frozen at
      stream start.
- Timebase 0–23 (10 ns → ~90 ms/sample), 9 voltage ranges (50 mV – 20 V)
- AC/DC coupling per channel
- Enhanced resolution (up to 12-bit via on-driver oversampling)

### Triggers

- Level trigger (rising / falling edge, threshold in mV, delay %, auto-ms)
- Window trigger (enter/exit `[lo, hi]` band)
- Pulse-width qualifier (min/max duration on a level condition)
- ETS (equivalent-time sampling) for repetitive signals

### Signal generator

- Standard waveforms: sine, square, triangle, ramp up/down, DC
- Frequency sweep (start → stop, step, dwell)
- DC offset, peak-to-peak amplitude, square-wave duty cycle
- Arbitrary waveform via AWG LUT upload
- _Note: on some hardware units the amplitude/offset bytes have no
  measurable effect — output is fixed ~13.5 Vpp / +7.24 V. Frequency
  always works._

### Scope view

- Time/div preset ladder + mouse-wheel zoom (cursor-anchored)
- Pan via drag or position slider
- Pause / resume, Fit (auto window)
- Live measurements: min, max, mean, RMS, Vpp, frequency, duty, rise/fall
- Measurements recompute over the **visible window**, not the raw buffer
- FFT with window selection (Rect / Hann / Hamming / Blackman)
- Cursors: X (time) and Y (voltage) pairs with delta readouts
- XY mode (CH A vs CH B)
- Persistence display (phosphor-like trace accumulation)
- Running math: A+B, A−B, A×B, invert A, invert B
- Rolling statistics over N captures

### Protocol decoders

- UART (configurable baud, bits, parity, stop), ASCII + hex view
- I²C (start/stop, address, ACK/NACK, data bytes)
- SPI (SCLK + MOSI + MISO + CS)
- CAN (standard + extended frames, CRC)

### Time-code decoders

- DCF77 (77.5 kHz German time signal)
- IRIG-B (10 pps inter-range instrumentation group)
- AFNOR (French industrial time code)

### Data management

- CSV export of the current capture (raw + scaled, per channel)
- PNG export of the scope canvas
- Per-range calibration table: manual edit, auto-fit from a live 0 V
  reading, import / export as JSON
- Preset save/load: a preset captures every acquisition + display
  setting so a full scope configuration round-trips

### Diagnostics

- Raw 8-bit USB buffer preview (header + sample bytes)
- Streaming statistics (sample rate, ring fill %, dropped blocks)
- Connection status + device info (serial `JOxxxxxxxx`, cal date,
  firmware version)

## Architecture

```
frontend/  Svelte 3 + Vite
  src/
    App.svelte               ← state wiring, event fan-out
    lib/components/          ← presentational panels (Channel, Trigger, Siggen, …)
    lib/modals/              ← overlays (Calibration editor, Firmware wizard, …)
    lib/dsp/                 ← pure JS — ring buffer, FFT, math, measurements
    lib/utils/format.js      ← fmtMv, fmtHz, fmtTime, …
    style.css                ← shared global classes (no scoped CSS in components)

Go backend (package main)
  app.go                     ← *App struct, shared state, lifecycle
  cgo.go + cgo_wrappers.h    ← single source of truth for C bindings
  types.go + constants.go
  app_connection.go          ← open / close / device info
  app_channel.go             ← per-channel config
  app_timebase.go            ← timebase + sample count
  app_trigger.go             ← all trigger modes
  app_capture.go             ← single-shot block + raw
  app_streaming.go           ← 3 streaming modes + poll loop
  app_siggen.go              ← signal generator (standard + AWG)
  app_calibration.go         ← manual / auto / get / apply
  app_measurements.go        ← backend-side window stats
  app_decoder.go             ← decoder session lifecycle
  app_export.go              ← CSV + PNG

decoder/                     ← pure-Go UART / I²C / SPI / CAN / DCF77 / IRIG-B / AFNOR
```

Every public method on `*App` is reflected into JS by Wails — look for
`window.go.main.App.<Method>()` on the frontend side.

## Testing

```bash
go test ./...              # decoder suite (no hardware required)
```

Scope-level behaviour is validated interactively against the hardware;
there is no automated end-to-end test harness.
