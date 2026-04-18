# Python examples

Runnable scripts that use the Python driver (`python/picoscope_libusb_full.py`).
Good entry points if you want to script the scope directly without going
through the Wails GUI.

Requires the driver's Python dependencies:

```bash
pip install pyusb numpy pyserial
```

And the firmware extracted from your device (see
[`docs/firmware-extraction.md`](../../docs/firmware-extraction.md)).

| Script | What it does |
|---|---|
| `validation.py` | Smoke test — captures a few blocks across timebases, checks the 8-bit ADC conversion and sample counts |
| `uart_send.py` | Reliable UART byte sender on `/dev/ttyUSB0` for decoder tests (also called from `gui/cmd/uart-diag`) |
| `uart_capture.py` | Captures a UART signal on CH A, measures bit period + voltage levels |

Run from the repository root:

```bash
python3 python/examples/validation.py
```

Scripts resolve `picoscope_libusb_full` via `sys.path` relative to their
own location, so they work from any CWD.
