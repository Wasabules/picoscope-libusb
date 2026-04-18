# Firmware extraction

The PicoScope 2204A needs four firmware blobs uploaded at every boot:

| File             | Size    | Purpose                                             |
|------------------|---------|-----------------------------------------------------|
| `fx2.bin`        | ~11 KiB | Cypress EZ-USB FX2 microcode (packed chunks)        |
| `fpga.bin`       | ~165 KiB| Xilinx FPGA bitstream                               |
| `waveform.bin`   | 8192 B  | Channel LUT (uploaded during `set_channel`)         |
| `stream_lut.bin` | 8192 B  | Continuous-streaming LUT (byte-identical to above)  |

These blobs are **copyrighted by Pico Technology Ltd.** and are not
redistributed with this project. Reverse-engineering the USB protocol is
legal in most jurisdictions; redistributing the firmware is not.

The good news: **you already own a licensed copy** — it ships with your
scope and with the official PicoSDK. This page walks you through extracting
it so this driver can use it.

## Method 1 — Live interception (recommended)

Requires the official Pico suite installed. Takes under a minute.

```bash
# 1. Build the interception shim
cd tools/firmware-extractor
make

# 2. Run any Pico program that opens the scope, under LD_PRELOAD:
LD_PRELOAD=./libps_intercept.so picoscope
# (or any binary from /opt/picoscope/)
# Wait until the scope's LED is solid green, then close the program.

# 3. The firmware is now at:
ls ~/.config/picoscope-libusb/firmware/
#   fx2.bin
#   fpga.bin
#   waveform.bin
#   stream_lut.bin
```

The shim watches every `libusb_control_transfer` (FX2 upload,
`bRequest=0xA0`) and `libusb_bulk_transfer` on EP 0x06 (FPGA + LUT uploads),
and writes the extracted blobs on process exit. It also produces a
`usb_trace.log` of the session in the current directory, useful for
diagnosing a missing blob.

The output directory can be overridden with `PS2204A_FIRMWARE_DIR` or
`XDG_CONFIG_HOME`.

## Method 2 — Offline from a USB capture

If you already have a `usbmon` pcap recorded while using the official SDK:

```bash
./tools/firmware-extractor/extract-from-pcap.py my_capture.pcap
```

To record a fresh capture:

```bash
sudo modprobe usbmon
# Find the bus your scope is on (look for VID 0ce9):
lsusb | grep -i pico
# Replace 1 with your bus number:
sudo tcpdump -i usbmon1 -w capture.pcap
# In another terminal: open the scope with the official SDK once,
# then Ctrl+C the tcpdump.
./tools/firmware-extractor/extract-from-pcap.py capture.pcap
```

The script requires `tshark` to parse the pcap.

## Method 3 — Manual

If the automated tools don't fit your workflow, here is the recipe the
scripts implement:

| Blob         | Transfer type        | Where                                |
|--------------|----------------------|--------------------------------------|
| **FX2**      | Control `bReq=0xA0`  | `wValue` = target RAM address        |
| **FPGA**     | Bulk OUT, ≥ 1 KiB    | EP 0x06                              |
| **LUT**      | Bulk OUT, 8 KiB      | EP 0x06 (first 8 KiB transfer)       |

The FX2 transfers bracket a CPU-halt (`wValue=0xE600`, data=`0x01`) and
CPU-release (`wValue=0xE600`, data=`0x00`). Drop those two markers; pack
the remaining chunks as `[addr_be16][len_u8][data]+` — that's `fx2.bin`.

The FPGA bitstream contains the Xilinx sync word `0x55 0x99 0xAA 0x66`
within its first 32 bytes (after the `0xFFFFFFFF` filler).

The 8 KiB LUT is the same blob for `waveform.bin` and `stream_lut.bin` on
the 2204A — save one copy under both names.

## Verification

Once you've extracted the firmware, verify it loads:

```bash
$ ls -l ~/.config/picoscope-libusb/firmware/
-rw-r--r-- 1 user user  10771 ... fx2.bin
-rw-r--r-- 1 user user 169216 ... fpga.bin
-rw-r--r-- 1 user user   8192 ... waveform.bin
-rw-r--r-- 1 user user   8192 ... stream_lut.bin
```

Then build and run the driver:

```bash
cd driver && make
LD_LIBRARY_PATH=. ./test_driver
```

If any file is missing or unreadable, `ps2204a_open()` returns
`PS_ERROR_FW` and prints a hint pointing back at this page.

## FAQ

**Q: Is this legal?**
Reverse-engineering for interoperability is generally protected (EU
Directive 2009/24/EC Art. 6; US DMCA §1201(f)). Extracting firmware from a
device you own, for your own use, with software you are licensed to run,
is covered in most jurisdictions. Redistributing the extracted firmware is
not — which is why we don't.

**Q: I don't have a Windows/Linux machine with the Pico software installed.**
Borrow a USB capture from someone who does (method 2). The pcap file itself
contains the firmware and is personal to your use — don't republish it.

**Q: Will this work with other PicoScope models?**
The extraction technique is generic. The current driver is only validated
on the 2204A — other 2000-series models use the same FX2+FPGA architecture
and likely different firmware. PRs welcome.
