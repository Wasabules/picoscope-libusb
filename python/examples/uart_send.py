#!/usr/bin/env python3
"""
Reliable UART sender for PicoScope decoder testing.

Sends bytes over /dev/ttyUSB0 at a chosen baud, with timing/echo logging.
Flushes the OS buffer after every write so data hits the wire immediately.

Examples:
    # Send "Hello" once
    python3 uart_send.py --text "Hello"

    # Send "U" continuously 10 times per second
    python3 uart_send.py --text "U" --rate 10

    # Run forever, cycling through 0x00..0xFF
    python3 uart_send.py --pattern ramp --loop

    # Stress test: random printable ASCII, 9600 baud
    python3 uart_send.py --pattern random --loop --rate 100

    # Change baud
    python3 uart_send.py --text "HI" --baud 115200

    # Burst of 1000 bytes then stop
    python3 uart_send.py --text "ABC" --count 333
"""
import argparse
import random
import sys
import time
import termios

import serial


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='/dev/ttyUSB0')
    ap.add_argument('--baud', type=int, default=9600,
                    help='baud rate (default 9600)')
    ap.add_argument('--bits', type=int, default=8, choices=[7, 8])
    ap.add_argument('--parity', choices=['N', 'E', 'O'], default='N')
    ap.add_argument('--stop',   type=int, default=1, choices=[1, 2])

    # Payload sources (mutually exclusive)
    grp = ap.add_mutually_exclusive_group()
    grp.add_argument('--text', help='literal text to send (latin-1)')
    grp.add_argument('--hex', help='hex bytes, e.g. 48656c6c6f')
    grp.add_argument('--pattern', choices=['U', 'ramp', 'alt', 'random'],
                     help='built-in test pattern')

    ap.add_argument('--count', type=int, default=1,
                    help='how many times to send the payload (default 1)')
    ap.add_argument('--rate', type=float, default=0,
                    help='payloads per second when --loop; 0 = as fast as possible')
    ap.add_argument('--loop', action='store_true',
                    help='send forever until Ctrl-C (overrides --count)')
    ap.add_argument('--verify-settings', action='store_true',
                    help='read back the kernel-side termios after opening')
    args = ap.parse_args()

    # Build payload.
    if args.text is not None:
        payload = args.text.encode('latin-1')
    elif args.hex is not None:
        payload = bytes.fromhex(args.hex.replace(' ', ''))
    elif args.pattern == 'U':
        payload = b'U'
    elif args.pattern == 'ramp':
        payload = bytes(range(256))
    elif args.pattern == 'alt':
        payload = bytes([0xAA, 0x55] * 16)
    elif args.pattern == 'random':
        random.seed(0xC0FFEE)
        payload = bytes(random.randint(0x20, 0x7E) for _ in range(32))
    else:
        ap.error('provide --text, --hex, or --pattern')
        return

    parity_map = {'N': serial.PARITY_NONE, 'E': serial.PARITY_EVEN,
                  'O': serial.PARITY_ODD}
    bits_map = {7: serial.SEVENBITS, 8: serial.EIGHTBITS}
    stop_map = {1: serial.STOPBITS_ONE, 2: serial.STOPBITS_TWO}

    ser = serial.Serial(
        port=args.port,
        baudrate=args.baud,
        bytesize=bits_map[args.bits],
        parity=parity_map[args.parity],
        stopbits=stop_map[args.stop],
        timeout=0,
        write_timeout=1.0,
        xonxoff=False, rtscts=False, dsrdtr=False,
    )

    if args.verify_settings:
        attrs = termios.tcgetattr(ser.fileno())
        print(f"[verify] baud input/output: {attrs[4]}/{attrs[5]} "
              f"(requested {args.baud})", file=sys.stderr)

    # Flush any junk in kernel buffers.
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # Compute how long a payload takes at line-rate so we can estimate
    # real throughput.
    bits_per_byte = 1 + args.bits + (1 if args.parity != 'N' else 0) + args.stop
    payload_ns = len(payload) * bits_per_byte * 1e9 / args.baud
    print(f"[tx] {args.port} {args.baud} {args.bits}{args.parity}{args.stop}, "
          f"payload={len(payload)} bytes ({payload_ns/1e3:.1f} µs on-air per send)",
          file=sys.stderr)
    if len(payload) <= 64:
        print(f"[tx] payload = {payload!r}", file=sys.stderr)

    sent = 0
    started = time.time()
    period = 1.0 / args.rate if args.rate > 0 else 0.0
    try:
        while args.loop or sent < args.count:
            t0 = time.time()
            ser.write(payload)
            ser.flush()           # drain pyserial buffer
            # Wait for the kernel TX FIFO to actually empty — this is what
            # makes the call "reliable" rather than "queued somewhere".
            termios.tcdrain(ser.fileno())
            sent += 1
            if sent % 100 == 0 or not args.loop:
                elapsed = time.time() - started
                rate = sent / elapsed if elapsed > 0 else 0
                print(f"[tx] sent #{sent}  "
                      f"({rate:.1f} payloads/s, {rate*len(payload):.1f} B/s)",
                      file=sys.stderr)
            if period > 0:
                dt = period - (time.time() - t0)
                if dt > 0:
                    time.sleep(dt)
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcdrain(ser.fileno())
        ser.close()
        elapsed = time.time() - started
        print(f"[tx] done. {sent} payloads in {elapsed:.2f} s "
              f"({sent * len(payload)} bytes total)", file=sys.stderr)


if __name__ == '__main__':
    main()
