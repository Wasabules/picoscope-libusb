#!/usr/bin/env python3
"""Capture a USB trace of the official PicoSDK running streaming + siggen
simultaneously, to reverse-engineer the commands that keep the DAC alive
during 1 MS/s streaming.

Must be run with LD_PRELOAD=<path>/libps_intercept.so so every
libusb_bulk_transfer / libusb_control_transfer ends up in usb_trace.log
in the CWD.

Run via `run_trace.sh` in this directory which sets the env correctly.
"""
from __future__ import annotations

import ctypes
import time
from ctypes import c_int16, c_int32, c_uint32, c_float, c_void_p, CFUNCTYPE, POINTER


# PS2000 constants / enums
PS2000_CHANNEL_A = 0
PS2000_CHANNEL_B = 1
PS2000_DC        = 1
PS2000_2V        = 7   # ±2 V range (enum ordinal, matches SDK)
PS2000_NS        = 2
PS2000_US        = 3
PS2000_SINE      = 0
PS2000_UP        = 0   # sweep type (unused when start==stop)

OVERVIEW_CB = CFUNCTYPE(None,
                        POINTER(POINTER(c_int16)),  # overviewBuffers
                        c_int16,                    # overflow
                        c_uint32,                   # triggeredAt
                        c_int16,                    # triggered
                        c_int16,                    # auto_stop
                        c_uint32)                   # nValues


def _overview_cb(buffers, overflow, trig_at, triggered, auto_stop, n_values):
    _overview_cb.total_samples += int(n_values)
    n = int(n_values)
    if n <= 0 or not buffers:
        return
    # buffers is int16** with up to 4 buffers [max, min] * [ch A, ch B]
    try:
        buf0 = buffers[0]
        if buf0:
            for i in range(min(n, 1024)):
                v = buf0[i]
                _overview_cb.samples_a.append(v)
    except Exception:
        pass
_overview_cb.total_samples = 0
_overview_cb.samples_a = []
cb_holder = OVERVIEW_CB(_overview_cb)


def main():
    lib = ctypes.CDLL("libps2000.so.2")

    lib.ps2000_open_unit.restype = c_int16
    lib.ps2000_open_unit.argtypes = []
    lib.ps2000_close_unit.restype = c_int16
    lib.ps2000_close_unit.argtypes = [c_int16]
    lib.ps2000_set_channel.restype = c_int16
    lib.ps2000_set_channel.argtypes = [c_int16, c_int16, c_int16, c_int16, c_int16]
    lib.ps2000_set_sig_gen_built_in.restype = c_int16
    lib.ps2000_set_sig_gen_built_in.argtypes = [
        c_int16, c_int32, c_uint32, c_int32,   # handle, offset_uv, pkpk_uv, wave
        c_float, c_float, c_float, c_float,    # start, stop, inc, dwell
        c_int32, c_uint32,                     # sweep_type, sweeps
    ]
    lib.ps2000_run_streaming_ns.restype = c_int16
    lib.ps2000_run_streaming_ns.argtypes = [
        c_int16, c_uint32, c_int32, c_uint32, c_int16, c_uint32, c_uint32,
    ]
    lib.ps2000_get_streaming_last_values.restype = c_int16
    lib.ps2000_get_streaming_last_values.argtypes = [c_int16, OVERVIEW_CB]
    lib.ps2000_stop.restype = c_int16
    lib.ps2000_stop.argtypes = [c_int16]

    print("[trace] ps2000_open_unit...")
    h = lib.ps2000_open_unit()
    if h <= 0:
        raise SystemExit(f"open_unit failed: {h}")
    print(f"[trace] handle={h}")

    try:
        # CH A enabled DC 2V, CH B disabled (we still expect streaming to work).
        r = lib.ps2000_set_channel(h, PS2000_CHANNEL_A, 1, PS2000_DC, PS2000_2V)
        print(f"[trace] set_channel A -> {r}")
        r = lib.ps2000_set_channel(h, PS2000_CHANNEL_B, 1, PS2000_DC, PS2000_2V)
        print(f"[trace] set_channel B -> {r}")

        # Siggen ON: sine 1 kHz, 2 Vpp, 0 offset. Fixed freq (start==stop).
        r = lib.ps2000_set_sig_gen_built_in(
            h,
            0,                  # offset_uv
            2_000_000,          # pkpk_uv
            PS2000_SINE,        # waveType
            ctypes.c_float(1000.0),  # startFreq
            ctypes.c_float(1000.0),  # stopFreq
            ctypes.c_float(0.0),     # increment
            ctypes.c_float(0.0),     # dwell
            PS2000_UP,          # sweepType
            0,                  # sweeps
        )
        print(f"[trace] set_sig_gen_built_in -> {r}")

        # Give the DAC a moment to start, then begin streaming at 1 µs/sample.
        time.sleep(0.2)

        # 1 µs interval -> 1 MS/s, 15 M max samples, no auto-stop, no aggregation.
        r = lib.ps2000_run_streaming_ns(
            h,
            1,                 # sample_interval
            PS2000_US,         # time_units
            15_000_000,        # max_samples
            0,                 # auto_stop
            1,                 # noOfSamplesPerAggregate
            15_000,            # overview_buffer_size
        )
        print(f"[trace] run_streaming_ns -> {r}")

        # Poll for ~1.5 s so the USB trace contains the steady-state pattern
        # (not just the startup burst).
        t0 = time.monotonic()
        while time.monotonic() - t0 < 1.5:
            lib.ps2000_get_streaming_last_values(h, cb_holder)
            time.sleep(0.01)

        print(f"[trace] streamed samples (per CB total) = {_overview_cb.total_samples}")
        sa = _overview_cb.samples_a
        if sa:
            mn, mx = min(sa), max(sa)
            print(f"[trace] CH A samples captured: n={len(sa)} min={mn} max={mx} pkpk={mx-mn}")
        lib.ps2000_stop(h)
        print("[trace] ps2000_stop OK")
    finally:
        lib.ps2000_close_unit(h)
        print("[trace] ps2000_close_unit OK")


if __name__ == "__main__":
    main()
