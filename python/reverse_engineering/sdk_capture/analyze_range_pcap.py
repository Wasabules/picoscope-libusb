#!/usr/bin/env python3
"""
Analyze capture_full.pcap for channel configuration commands.

Parses Linux USB MMAPPED pcap (link_type=220) looking for:
  - Bulk OUT EP 0x01 commands containing 0x85 0x21 (channel config)
  - Bulk OUT EP 0x01 commands containing 0x85 0x04 0x9b (channel config wrapper)

Compares all occurrences and highlights byte positions that differ.
"""

import struct
import sys
from pathlib import Path

PCAP_FILE = Path(__file__).parent / "capture_full.pcap"

PCAP_GLOBAL_HDR_SIZE = 24
PCAP_REC_HDR_SIZE = 16
USB_HDR_SIZE = 64  # link_type 220


def read_pcap_packets(filepath):
    """Read all packets from a pcap file (link_type=220)."""
    data = filepath.read_bytes()

    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == 0xa1b2c3d4:
        endian = "<"
    elif magic == 0xd4c3b2a1:
        endian = ">"
    else:
        print(f"Unknown pcap magic: 0x{magic:08x}")
        sys.exit(1)

    ver_major, ver_minor, tz, sigfigs, snaplen, link_type = struct.unpack_from(
        endian + "HHiIII", data, 4
    )
    print(f"PCAP: version {ver_major}.{ver_minor}, link_type={link_type}, snaplen={snaplen}")
    if link_type != 220:
        print(f"WARNING: Expected link_type 220 (Linux USB MMAPPED), got {link_type}")

    offset = PCAP_GLOBAL_HDR_SIZE
    packets = []
    pkt_num = 0

    while offset + PCAP_REC_HDR_SIZE <= len(data):
        ts_sec, ts_usec, incl_len, orig_len = struct.unpack_from(
            endian + "IIII", data, offset
        )
        offset += PCAP_REC_HDR_SIZE

        if offset + incl_len > len(data):
            break

        pkt_data = data[offset : offset + incl_len]
        packets.append({
            "num": pkt_num,
            "ts_sec": ts_sec,
            "ts_usec": ts_usec,
            "incl_len": incl_len,
            "orig_len": orig_len,
            "data": pkt_data,
        })

        offset += incl_len
        pkt_num += 1

    print(f"Total packets in pcap: {pkt_num}")
    return packets


def parse_usb_header(pkt_data):
    """Parse the 64-byte USB header for link_type 220."""
    if len(pkt_data) < USB_HDR_SIZE:
        return None

    urb_type = pkt_data[8]
    transfer_type = pkt_data[9]
    endpoint = pkt_data[10]
    device = pkt_data[11]
    bus_id = struct.unpack_from("<H", pkt_data, 12)[0]
    setup_flag = pkt_data[14]
    data_flag = pkt_data[15]
    urb_len = struct.unpack_from("<I", pkt_data, 32)[0]
    data_len = struct.unpack_from("<I", pkt_data, 36)[0]

    return {
        "urb_type": chr(urb_type) if urb_type in (0x43, 0x53, 0x45) else f"0x{urb_type:02x}",
        "transfer_type": transfer_type,
        "endpoint": endpoint,
        "device": device,
        "bus_id": bus_id,
        "setup_flag": setup_flag,
        "data_flag": data_flag,
        "urb_len": urb_len,
        "data_len": data_len,
    }


def get_payload(pkt_data):
    """Extract payload after the 64-byte USB header."""
    if len(pkt_data) <= USB_HDR_SIZE:
        return b""
    return pkt_data[USB_HDR_SIZE:]


def find_subsequence(data, pattern):
    """Find all occurrences of pattern in data, return list of offsets."""
    offsets = []
    start = 0
    while True:
        idx = data.find(pattern, start)
        if idx == -1:
            break
        offsets.append(idx)
        start = idx + 1
    return offsets


def hex_dump(data, highlight_positions=None):
    """Pretty hex dump with optional highlighting of specific positions."""
    if highlight_positions is None:
        highlight_positions = set()

    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_parts = []
        for j, b in enumerate(chunk):
            pos = i + j
            if pos in highlight_positions:
                hex_parts.append(f"\033[1;31m{b:02x}\033[0m")
            else:
                hex_parts.append(f"{b:02x}")

        hex_str = " ".join(hex_parts)
        ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {i:04x}: {hex_str}  {ascii_str}")

    return "\n".join(lines)


def hex_dump_plain(data):
    """Plain hex dump without highlighting."""
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = " ".join(f"{b:02x}" for b in chunk)
        ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {i:04x}: {hex_str:<48s}  {ascii_str}")
    return "\n".join(lines)


def annotate_85_21(data, offset):
    """Annotate a 0x85 0x21 sub-command starting at given offset."""
    lines = []
    lines.append(f"  Sub-command at offset {offset}:")

    remaining = len(data) - offset
    if remaining < 2:
        lines.append(f"    (only {remaining} byte(s) remaining)")
        return "\n".join(lines)

    lines.append(f"    [{offset:2d}] 0x{data[offset]:02x}  = command prefix (0x85)")
    lines.append(f"    [{offset+1:2d}] 0x{data[offset+1]:02x}  = sub-opcode (0x21 = channel config)")

    for i in range(2, min(20, remaining)):
        pos = offset + i
        b = data[pos]
        annotation = ""
        if i == 2:
            annotation = " (channel param byte 1 - possible channel select?)"
        elif i == 3:
            annotation = " (channel param byte 2 - possible coupling/range?)"
        elif i == 4:
            annotation = " (channel param byte 3)"
        elif i == 5:
            annotation = " (channel param byte 4)"
        elif b == 0x85 or b == 0x97:
            annotation = " (likely next sub-command prefix)"
            lines.append(f"    [{pos:2d}] 0x{b:02x}{annotation}")
            break

        lines.append(f"    [{pos:2d}] 0x{b:02x}{annotation}")

    return "\n".join(lines)


def main():
    print("=" * 80)
    print("PicoScope 2204A - Channel Range Configuration Analysis")
    print("=" * 80)
    print()

    packets = read_pcap_packets(PCAP_FILE)
    print()

    # Collect all bulk OUT EP 0x01 commands with payloads
    bulk_out_ep01 = []

    for pkt in packets:
        hdr = parse_usb_header(pkt["data"])
        if hdr is None:
            continue

        # Bulk OUT to EP 0x01: transfer_type=3 (bulk), endpoint=0x01 (OUT)
        if hdr["transfer_type"] == 3 and hdr["endpoint"] == 0x01:
            payload = get_payload(pkt["data"])
            if len(payload) > 0:
                bulk_out_ep01.append({
                    "pkt_num": pkt["num"],
                    "urb_type": hdr["urb_type"],
                    "payload": payload,
                    "ts": pkt["ts_sec"] + pkt["ts_usec"] / 1e6,
                })

    print(f"Bulk OUT EP 0x01 packets with payload: {len(bulk_out_ep01)}")
    print()

    # =========================================================================
    # PART 1: Find commands containing 0x85 0x21
    # =========================================================================
    print("=" * 80)
    print("PART 1: Commands containing 0x85 0x21 (channel config sub-command)")
    print("=" * 80)
    print()

    cmds_85_21 = []

    for entry in bulk_out_ep01:
        payload = entry["payload"]
        offsets = find_subsequence(payload, bytes([0x85, 0x21]))
        if offsets:
            cmds_85_21.append({
                **entry,
                "offsets_85_21": offsets,
            })

    print(f"Found {len(cmds_85_21)} commands containing 0x85 0x21")
    print()

    for i, cmd in enumerate(cmds_85_21):
        print(f"--- Command #{i+1} (packet #{cmd['pkt_num']}, "
              f"urb={cmd['urb_type']}, t={cmd['ts']:.6f}) ---")
        print(f"Payload length: {len(cmd['payload'])} bytes")
        print("Full hex dump:")
        print(hex_dump_plain(cmd["payload"]))
        print()

        for off in cmd["offsets_85_21"]:
            print(annotate_85_21(cmd["payload"], off))
            print()

    # Compare all 0x85 0x21 commands
    if len(cmds_85_21) > 1:
        print("-" * 60)
        print("COMPARISON of 0x85 0x21 commands - differing byte positions:")
        print("-" * 60)

        max_len = max(len(c["payload"]) for c in cmds_85_21)
        padded = []
        for c in cmds_85_21:
            p = c["payload"] + b"\x00" * (max_len - len(c["payload"]))
            padded.append(p)

        diff_positions = set()
        for pos in range(max_len):
            values = set(p[pos] for p in padded)
            if len(values) > 1:
                diff_positions.add(pos)

        if diff_positions:
            print(f"\nByte positions that differ: {sorted(diff_positions)}")
            print()
            for pos in sorted(diff_positions):
                vals = [f"cmd#{i+1}=0x{padded[i][pos]:02x}" for i in range(len(padded))]
                print(f"  Offset {pos:2d}: {', '.join(vals)}")
            print()

            print("Hex dumps with differing bytes highlighted in RED:")
            for i, cmd in enumerate(cmds_85_21):
                print(f"\n  Command #{i+1} (pkt #{cmd['pkt_num']}):")
                print(hex_dump(cmd["payload"], diff_positions))
        else:
            print("  All commands are IDENTICAL!")
        print()

    # =========================================================================
    # PART 2: Find commands containing 0x85 0x04 0x9b
    # =========================================================================
    print()
    print("=" * 80)
    print("PART 2: Commands containing 0x85 0x04 0x9b (channel config wrapper)")
    print("=" * 80)
    print()

    cmds_85_04_9b = []

    for entry in bulk_out_ep01:
        payload = entry["payload"]
        offsets = find_subsequence(payload, bytes([0x85, 0x04, 0x9b]))
        if offsets:
            cmds_85_04_9b.append({
                **entry,
                "offsets_85_04_9b": offsets,
            })

    print(f"Found {len(cmds_85_04_9b)} commands containing 0x85 0x04 0x9b")
    print()

    for i, cmd in enumerate(cmds_85_04_9b):
        print(f"--- Command #{i+1} (packet #{cmd['pkt_num']}, "
              f"urb={cmd['urb_type']}, t={cmd['ts']:.6f}) ---")
        print(f"Payload length: {len(cmd['payload'])} bytes")
        print("Full hex dump:")
        print(hex_dump_plain(cmd["payload"]))
        print()

        for off in cmd["offsets_85_04_9b"]:
            print(f"  Sub-command 0x85 0x04 0x9b at offset {off}:")
            remaining = len(cmd["payload"]) - off
            for j in range(min(20, remaining)):
                pos = off + j
                b = cmd["payload"][pos]
                annotation = ""
                if j == 0:
                    annotation = " (0x85 prefix)"
                elif j == 1:
                    annotation = " (0x04 sub-opcode)"
                elif j == 2:
                    annotation = " (0x9b marker)"
                elif j >= 3:
                    annotation = f" (param byte {j-2})"
                    if b == 0x85 and j > 3:
                        annotation = " (next sub-command prefix - STOP)"
                        print(f"    [{pos:2d}] 0x{b:02x}{annotation}")
                        break
                print(f"    [{pos:2d}] 0x{b:02x}{annotation}")
            print()

    # Compare all 0x85 0x04 0x9b commands
    if len(cmds_85_04_9b) > 1:
        print("-" * 60)
        print("COMPARISON of 0x85 0x04 0x9b commands - differing byte positions:")
        print("-" * 60)

        max_len = max(len(c["payload"]) for c in cmds_85_04_9b)
        padded = []
        for c in cmds_85_04_9b:
            p = c["payload"] + b"\x00" * (max_len - len(c["payload"]))
            padded.append(p)

        diff_positions = set()
        for pos in range(max_len):
            values = set(p[pos] for p in padded)
            if len(values) > 1:
                diff_positions.add(pos)

        if diff_positions:
            print(f"\nByte positions that differ: {sorted(diff_positions)}")
            print()
            for pos in sorted(diff_positions):
                vals = [f"cmd#{i+1}=0x{padded[i][pos]:02x}" for i in range(len(padded))]
                print(f"  Offset {pos:2d}: {', '.join(vals)}")
            print()

            print("Hex dumps with differing bytes highlighted in RED:")
            for i, cmd in enumerate(cmds_85_04_9b):
                print(f"\n  Command #{i+1} (pkt #{cmd['pkt_num']}):")
                print(hex_dump(cmd["payload"], diff_positions))
        else:
            print("  All commands are IDENTICAL!")
        print()

    # =========================================================================
    # PART 3: Summary of ALL bulk OUT EP 0x01 commands
    # =========================================================================
    print()
    print("=" * 80)
    print("PART 3: Summary of ALL unique bulk OUT EP 0x01 command patterns")
    print("=" * 80)
    print()

    by_prefix = {}
    for entry in bulk_out_ep01:
        payload = entry["payload"]
        if len(payload) >= 2:
            prefix = (payload[0], payload[1])
        elif len(payload) == 1:
            prefix = (payload[0],)
        else:
            prefix = ()

        key = prefix
        if key not in by_prefix:
            by_prefix[key] = []
        by_prefix[key].append(entry)

    for prefix, entries in sorted(by_prefix.items()):
        prefix_hex = " ".join(f"0x{b:02x}" for b in prefix)
        print(f"Prefix [{prefix_hex}]: {len(entries)} occurrences")

    print()
    print("=" * 80)
    print("PART 4: Full comparison of ALL bulk OUT EP 0x01 with 64-byte payloads")
    print("=" * 80)
    print()

    full_64 = [e for e in bulk_out_ep01 if len(e["payload"]) == 64]
    print(f"Total 64-byte bulk OUT EP 0x01 commands: {len(full_64)}")

    if len(full_64) > 1:
        all_diff = set()
        for pos in range(64):
            values = set(e["payload"][pos] for e in full_64)
            if len(values) > 1:
                all_diff.add(pos)

        print(f"Byte positions that vary across ALL 64-byte commands: {sorted(all_diff)}")
        print()

        for i, entry in enumerate(full_64):
            print(f"  64-byte cmd #{i+1} (pkt #{entry['pkt_num']}, "
                  f"urb={entry['urb_type']}, t={entry['ts']:.6f}):")
            print(hex_dump(entry["payload"], all_diff))
            print()


if __name__ == "__main__":
    main()
