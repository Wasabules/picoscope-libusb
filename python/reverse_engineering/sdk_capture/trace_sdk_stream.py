#!/usr/bin/env python3
"""Drive ps2000Con under LD_PRELOAD=usb_interceptor.so and capture a fast-streaming
trace. Uses pexpect so _getch()/termios reads see real keystrokes."""
import os
import sys
import time
import pexpect

MODE = sys.argv[1] if len(sys.argv) > 1 else "F"   # F, D, C, S
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0

os.chdir(os.path.dirname(os.path.abspath(__file__)))
for p in ("usb_trace.log",):
    if os.path.exists(p):
        os.remove(p)

env = os.environ.copy()
env["LD_PRELOAD"] = os.path.abspath("./usb_interceptor.so")

child = pexpect.spawn("./ps2000Con", env=env, encoding="utf-8", timeout=30)
child.logfile_read = sys.stdout
child.expect(r"Operation:")
child.send(MODE)
# Each operation prints "Press a key to start\n" — send any key.
child.expect(r"Press a key to start")
time.sleep(0.3)
child.sendline("")
# Let it stream for DURATION seconds of actual USB traffic.
time.sleep(DURATION)
# Any key to stop (fast streaming loops while !_kbhit()).
child.send("x")
# Wait for it to finish closing and print menu again.
try:
    child.expect(r"Operation:", timeout=10)
except pexpect.TIMEOUT:
    pass
# Exit the program
child.send("X")
child.expect(pexpect.EOF, timeout=5)
print("\n=== trace complete ===")
if os.path.exists("usb_trace.log"):
    sz = os.path.getsize("usb_trace.log")
    print(f"usb_trace.log: {sz} bytes")
