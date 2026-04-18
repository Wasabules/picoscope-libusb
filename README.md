# picoscope-libusb

Open-source libusb driver + cross-platform GUI for the **PicoScope 2204A** USB
oscilloscope. Built from scratch by reverse-engineering the USB protocol —
no proprietary PicoSDK runtime required.

Targets **Linux desktop** and **Android** (via NDK + JNI). Windows/macOS build
through Wails but have not yet been validated against hardware.

> ⚠️ **Firmware not included.** The FX2 microcode, FPGA bitstream, and
> waveform LUTs are copyrighted by Pico Technology Ltd. You must extract
> them from your own PicoScope before the driver will initialise. See
> [`docs/firmware-extraction.md`](docs/firmware-extraction.md).
>
> This project is not affiliated with Pico Technology Ltd.

---

## What's in the box

| Path | Description |
|---|---|
| `driver/` | C driver (`libpicoscope2204a.{so,a}`) — the core, plain libusb |
| `gui/` | Wails desktop GUI (Go + Svelte), cgo-linked to the driver |
| `android-lib/` | Gradle module packaging the driver + JNI shim as an AAR (distributed via JitPack) |
| `python/` | Reference Python driver (PyUSB) — useful for quick experiments |
| `tools/firmware-extractor/` | Extract firmware from your own device |
| `docs/` | Protocol reverse-engineering notes + how-to guides |

## Quick start (Linux)

```bash
# 1. udev rule (once, as root)
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0ce9", MODE="0666"' \
  | sudo tee /etc/udev/rules.d/99-picoscope.rules
sudo udevadm control --reload && sudo udevadm trigger

# 2. Build the driver
cd driver && make

# 3. Build & run the GUI
cd ../gui && wails build -tags webkit2_41
./build/bin/picoscope-gui
```

On first launch, the GUI detects missing firmware and offers to extract it
for you (requires the official Pico SDK installed once). CLI alternative:

```bash
./tools/firmware-extractor/extract.sh
```

See [`docs/firmware-extraction.md`](docs/firmware-extraction.md) for the
offline (pcap) extraction path if you don't have the SDK.

## Hardware

- **Model**: PicoScope 2204A (VID=`0x0CE9`, PID=`0x1007`)
- **ADC**: 8-bit, 100 MSPS (single channel), 50 MSPS (dual)
- **Channels**: 2 analog (A + B), 1 signal generator
- **Ranges**: 50 mV – 20 V (9 ranges)

Should also work on closely related models (2202, 2203, 2205, 2205A) with
minor tweaks — untested.

## Status

### Driver

| Feature | Status |
|---|---|
| Block capture (single / dual channel) | ✅ |
| Fast block streaming (~330 kS/s) | ✅ |
| Native streaming (~100 S/s, DC monitoring) | ✅ |
| SDK-style continuous streaming (1 MS/s, gap-free) | ✅ |
| Trigger: edge / window / PWQ | ✅ |
| Trigger: ETS (equivalent-time sampling) | ✅ |
| Enhanced resolution (up to 12-bit via oversampling) | ✅ |
| Signal generator: frequency | ✅ |
| Signal generator: arbitrary waveform (AWG + LUT) | ✅ code path integrated |
| Signal generator: amplitude / offset | ⚠️ HW unresponsive on tested unit |
| DC offset calibration (factory table + runtime auto-fit) | ✅ |
| Per-range gain/offset override API | ✅ |
| EEPROM readout (serial, cal-date, raw pages) | ✅ |

### GUI

| Feature | Status |
|---|---|
| Live scope view with time/div, pan, zoom, pause | ✅ |
| Measurements (Vpp, mean, RMS, freq, duty, rise/fall…) | ✅ |
| FFT + cursors (X/Y) + persistence + XY mode + math | ✅ |
| Protocol decoders: UART, I²C, SPI, CAN | ✅ |
| Time-code decoders: DCF77, IRIG-B, AFNOR | ✅ |
| CSV + PNG export, preset save/load | ✅ |
| Firmware extraction wizard (first-launch) | ✅ |

See [`docs/protocol.md`](docs/protocol.md) for the reverse-engineered protocol
reference.

## License

MIT — see [`LICENSE`](LICENSE). Firmware blobs are explicitly excluded.

## Contributing

Pull requests welcome. Keep commits focused, please run `go test ./...` in
`gui/` and `make test` in `driver/` before submitting. New protocol
discoveries belong in `docs/protocol.md`.
