# PicoScope 2204A Wails GUI

Cross-platform GUI for the reverse-engineered PicoScope 2204A libusb driver,
built with Wails (Go backend) and Svelte (frontend).

## Prerequisites

- Go 1.21+
- Node.js 20+ and npm
- Wails v2.11+: `go install github.com/wailsapp/wails/v2/cmd/wails@latest`
- libusb-1.0 dev headers: `sudo apt install libusb-1.0-0-dev`
- WebKit2GTK: `sudo apt install libwebkit2gtk-4.1-dev` (Ubuntu 24.04+)
  or `libwebkit2gtk-4.0-dev` on older distros.
- Static C driver built: `cd ../driver && make`

## Live Development

```bash
wails dev -tags webkit2_41        # Ubuntu 24.04+
wails dev                          # older distros (webkit2gtk-4.0)
```

Vite hot-reloads the Svelte frontend; Go changes trigger rebuilds.

## Production Build

```bash
wails build -tags webkit2_41       # Ubuntu 24.04+
wails build                        # older distros
```

The binary is produced at `build/bin/picoscope-gui` and links statically
against `../driver/libpicoscope2204a.a`.

## USB Access

The PicoScope 2204A normally requires root. Install the udev rule to allow
non-root access:

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0ce9", MODE="0666"' | \
  sudo tee /etc/udev/rules.d/99-picoscope.rules
sudo udevadm control --reload
```

## Features

- Single-shot block capture (single or dual channel)
- Two streaming modes:
  - **Fast block** (~330 kS/s) — rapid rebuild of block captures
  - **Native FPGA** (~100 S/s) — truly gap-free hardware streaming (CH A only)
- Voltage ranges 50 mV – 20 V with correct calibrated scaling
- Dynamic sample count (8064 single, 3968 dual)
- Trigger configuration (level, edge, source, auto-trigger)
- Built-in signal generator (sine/square/triangle/ramp/DC)
- Diagnostics panel: raw 8-bit capture preview
- Live measurements (min/max/mean/Vpp per channel)
