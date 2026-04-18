#!/usr/bin/env python3
"""
Analyse du protocole USB PicoScope 2000
Extrait et décode les paquets capturés
"""

import subprocess
import sys

def parse_capture(pcap_file: str):
    """Parse le fichier pcap et extrait les paquets PicoScope."""

    # Extraire les paquets avec tshark
    cmd = [
        "tshark", "-r", pcap_file,
        "-Y", "usb.device_address == 17 and usb.capdata",
        "-T", "fields",
        "-e", "frame.number",
        "-e", "frame.time_relative",
        "-e", "usb.endpoint_address",
        "-e", "usb.data_len",
        "-e", "usb.capdata"
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)

    packets = []
    for line in result.stdout.strip().split('\n'):
        if not line:
            continue
        parts = line.split('\t')
        if len(parts) >= 5 and parts[4]:
            packets.append({
                'frame': int(parts[0]),
                'time': float(parts[1]) if parts[1] else 0,
                'endpoint': int(parts[2], 16) if parts[2] else 0,
                'length': int(parts[3]) if parts[3] else 0,
                'data': bytes.fromhex(parts[4].replace(':', ''))
            })

    return packets


def analyze_packet(pkt):
    """Analyse un paquet et tente de le décoder."""
    ep = pkt['endpoint']
    data = pkt['data']

    direction = "OUT" if ep == 0x01 else "IN"
    ep_name = {0x01: "CMD_OUT", 0x81: "RESP_IN", 0x82: "DATA_IN"}.get(ep, f"EP_{ep:02x}")

    info = ""

    # Analyser les commandes OUT (EP 0x01)
    if ep == 0x01 and len(data) >= 2:
        cmd_type = data[0]
        cmd_code = data[1]

        if cmd_type == 0x02:
            # Commandes de type 0x02
            if cmd_code == 0x81:
                info = "CMD: Init/Config sequence"
            elif cmd_code == 0x83:
                info = f"CMD: Get info (type={data[2]:02x})"
            elif cmd_code == 0x03:
                info = f"CMD: Get info/status (type={data[2]:02x})"

        elif cmd_type == 0x83:
            info = "CMD: Set channel/config"

    # Analyser les réponses IN (EP 0x81)
    elif ep == 0x81:
        if len(data) == 1:
            info = f"RESP: ACK ({data[0]:02x})"
        elif len(data) >= 16:
            # Vérifier si c'est une réponse d'info
            if data[0:2] == b'\xc0\xe9':  # VID Pico = 0x0ce9 little-endian
                info = "RESP: Device info"
                # Extraire le numéro de série
                try:
                    serial_start = data.find(b'JO')
                    if serial_start >= 0:
                        serial = data[serial_start:serial_start+10].decode('ascii', errors='ignore')
                        info += f" serial={serial}"
                except:
                    pass

    # Analyser les données IN (EP 0x82)
    elif ep == 0x82:
        info = f"DATA: Waveform ({len(data)} bytes)"

    return ep_name, direction, info


def main():
    pcap_file = "usb_capture.pcap"

    print("=" * 80)
    print("ANALYSE DU PROTOCOLE USB PICOSCOPE 2204A")
    print("=" * 80)

    packets = parse_capture(pcap_file)
    print(f"\nPaquets avec données: {len(packets)}")

    # Grouper par direction
    out_packets = [p for p in packets if p['endpoint'] == 0x01]
    in_resp = [p for p in packets if p['endpoint'] == 0x81]
    in_data = [p for p in packets if p['endpoint'] == 0x82]

    print(f"  - OUT (EP 0x01): {len(out_packets)} commandes")
    print(f"  - IN  (EP 0x81): {len(in_resp)} réponses")
    print(f"  - IN  (EP 0x82): {len(in_data)} paquets data")

    # Afficher les premiers paquets
    print("\n" + "-" * 80)
    print("PREMIERS ÉCHANGES (séquence d'init)")
    print("-" * 80)

    for pkt in packets[:50]:
        ep_name, direction, info = analyze_packet(pkt)
        data_hex = pkt['data'][:32].hex()

        print(f"[{pkt['frame']:5d}] {ep_name:8s} {direction:3s} len={pkt['length']:4d} | {data_hex}{'...' if len(pkt['data']) > 32 else ''}")
        if info:
            print(f"         -> {info}")

    # Identifier les types de commandes
    print("\n" + "-" * 80)
    print("TYPES DE COMMANDES IDENTIFIÉS")
    print("-" * 80)

    cmd_types = {}
    for pkt in out_packets:
        if len(pkt['data']) >= 3:
            key = (pkt['data'][0], pkt['data'][1], pkt['data'][2])
            if key not in cmd_types:
                cmd_types[key] = {
                    'count': 0,
                    'example': pkt['data'][:16].hex()
                }
            cmd_types[key]['count'] += 1

    for (t, c, p), info in sorted(cmd_types.items()):
        print(f"  Type=0x{t:02x} Cmd=0x{c:02x} Param=0x{p:02x} : {info['count']:4d}x  | {info['example']}")

    # Chercher les données de waveform
    print("\n" + "-" * 80)
    print("DONNÉES WAVEFORM (EP 0x82)")
    print("-" * 80)

    for pkt in in_data[:5]:
        data = pkt['data']
        print(f"[{pkt['frame']:5d}] {len(data)} bytes")

        # Interpréter comme des int16 (échantillons)
        if len(data) >= 4:
            samples = []
            for i in range(0, min(20, len(data)), 2):
                val = int.from_bytes(data[i:i+2], 'little', signed=True)
                samples.append(val)
            print(f"         Samples (int16): {samples}")


if __name__ == "__main__":
    main()
