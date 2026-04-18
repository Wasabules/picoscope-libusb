#!/usr/bin/env python3
"""Capture SDK USB commands during streaming mode.
Run with: LD_PRELOAD=./usb_interceptor.so sudo python3 test_sdk_streaming_capture.py
Compare the USB trace with block capture to identify streaming-specific opcodes."""
import ctypes
import time
import os

# Ensure trace log is fresh
trace_log = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         '..', '..', '..', 'usb_trace.log')

sdk = ctypes.CDLL('/opt/picoscope/lib/libps2000.so')

# Set function signatures
sdk.ps2000_open_unit.restype = ctypes.c_int16
sdk.ps2000_run_streaming.restype = ctypes.c_int16
sdk.ps2000_run_streaming_ns.restype = ctypes.c_int16
sdk.ps2000_get_values.restype = ctypes.c_int32
sdk.ps2000_stop.restype = ctypes.c_int16

# Callback: void(int16_t**, int16_t, uint32_t, int16_t, int16_t, uint32_t)
CALLBACK_TYPE = ctypes.CFUNCTYPE(
    None,
    ctypes.POINTER(ctypes.POINTER(ctypes.c_int16)),  # overviewBuffers
    ctypes.c_int16,   # overflow
    ctypes.c_uint32,  # triggeredAt
    ctypes.c_int16,   # triggered
    ctypes.c_int16,   # auto_stop
    ctypes.c_uint32,  # nValues
)

sdk.ps2000_get_streaming_last_values.restype = ctypes.c_int16
sdk.ps2000_get_streaming_last_values.argtypes = [ctypes.c_int16, CALLBACK_TYPE]

sdk.ps2000_get_streaming_values_no_aggregation.restype = ctypes.c_int32

total_samples = 0
auto_stopped = False

def streaming_callback(overviewBuffers, overflow, triggeredAt, triggered, auto_stop, nValues):
    global total_samples, auto_stopped
    total_samples += nValues
    if auto_stop:
        auto_stopped = True

cb = CALLBACK_TYPE(streaming_callback)

handle = sdk.ps2000_open_unit()
print(f"Handle: {handle}")
if handle <= 0:
    print("Cannot open device")
    exit(1)

# Setup channels
print("\n--- Setting up channels ---")
sdk.ps2000_set_channel(handle, ctypes.c_int16(0), ctypes.c_int16(1),
                       ctypes.c_int16(1), ctypes.c_int16(7))   # A: enabled, DC, 5V
sdk.ps2000_set_channel(handle, ctypes.c_int16(1), ctypes.c_int16(0),
                       ctypes.c_int16(1), ctypes.c_int16(7))   # B: disabled

# Disable trigger
sdk.ps2000_set_trigger(handle, ctypes.c_int16(5), ctypes.c_int16(0),
                       ctypes.c_int16(0), ctypes.c_int16(0), ctypes.c_int16(0))

# --- Test 1: Legacy streaming ---
print("\n=== Legacy streaming (ps2000_run_streaming) ===")
print("Starting streaming: interval=10ms, max=1000, windowed=0")
ok = sdk.ps2000_run_streaming(handle, ctypes.c_int16(10), ctypes.c_int32(1000), ctypes.c_int16(0))
print(f"run_streaming returned: {ok}")

if ok:
    buf_a = (ctypes.c_int16 * 1024)()
    buf_b = (ctypes.c_int16 * 1024)()
    overflow = ctypes.c_int16(0)

    for i in range(5):
        time.sleep(0.1)
        n = sdk.ps2000_get_values(handle, ctypes.byref(buf_a), ctypes.byref(buf_b),
                                   None, None, ctypes.byref(overflow), ctypes.c_int32(1024))
        print(f"  Poll {i}: got {n} values")

    sdk.ps2000_stop(handle)
    print("Stopped legacy streaming")

time.sleep(0.5)

# --- Test 2: Fast streaming (ns) ---
print("\n=== Fast streaming (ps2000_run_streaming_ns) ===")

# PS2000_US = 4
print("Starting fast streaming: interval=10us, buffer=10000, auto_stop=1, ratio=1, overview=30000")
total_samples = 0
auto_stopped = False

ok = sdk.ps2000_run_streaming_ns(
    handle,
    ctypes.c_uint32(10),        # sample_interval = 10
    ctypes.c_int16(4),          # time_units = PS2000_US (microseconds)
    ctypes.c_uint32(10000),     # max_samples
    ctypes.c_int16(1),          # auto_stop
    ctypes.c_uint32(1),         # noOfSamplesPerAggregate (1 = no aggregation)
    ctypes.c_uint32(30000),     # overview_buffer_size
)
print(f"run_streaming_ns returned: {ok}")

if ok:
    for i in range(500):
        ret = sdk.ps2000_get_streaming_last_values(handle, cb)
        if auto_stopped:
            print(f"  Auto-stopped after {total_samples} samples (poll {i})")
            break
        if i % 50 == 0:
            print(f"  Poll {i}: total_samples={total_samples}, ret={ret}")
        time.sleep(0.001)  # 1ms polling like SDK (Sleep(0))

    sdk.ps2000_stop(handle)
    print(f"Stopped fast streaming, total={total_samples}")

    # Try to get raw values
    values_a = (ctypes.c_int16 * 10000)()
    values_b = (ctypes.c_int16 * 10000)()
    overflow2 = ctypes.c_int16(0)
    trigger_at = ctypes.c_uint32(0)
    triggered = ctypes.c_int16(0)
    start_time = ctypes.c_double(0)

    n_raw = sdk.ps2000_get_streaming_values_no_aggregation(
        handle,
        ctypes.byref(start_time),
        ctypes.byref(values_a),
        ctypes.byref(values_b),
        None, None,
        ctypes.byref(overflow2),
        ctypes.byref(trigger_at),
        ctypes.byref(triggered),
        ctypes.c_uint32(10000),
    )
    print(f"  Raw values retrieved: {n_raw}")

sdk.ps2000_close_unit(handle)
print("\nDone! Check usb_trace.log for USB commands.")
