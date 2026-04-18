#!/usr/bin/env python3
"""
Décodeur de protocole PicoScope 2204A
=====================================
Analyse détaillée des commandes USB capturées.
"""

import subprocess
import sys
from collections import defaultdict


def get_packets(pcap_file: str, device_addr: int = 23) -> list:
    """Extrait les paquets USB du fichier pcap."""
    cmd = [
        "tshark", "-r", pcap_file,
        "-Y", f"usb.device_address == {device_addr} and usb.capdata",
        "-T", "fields",
        "-e", "frame.number",
        "-e", "frame.time_relative",
        "-e", "usb.endpoint_address",
        "-e", "usb.data_len",
        "-e", "usb.capdata"
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)

    packets = []
    for line in result.stdout.strip().split('\n'):
        if not line:
            continue
        parts = line.split('\t')
        if len(parts) >= 5 and parts[4]:
            ep = int(parts[2], 16) if parts[2] else 0
            data_hex = parts[4].replace(':', '')
            packets.append({
                'frame': int(parts[0]),
                'time': float(parts[1]) if parts[1] else 0,
                'ep': ep,
                'dir': 'OUT' if ep in [0x01, 0x06] else 'IN',
                'len': int(parts[3]) if parts[3] else 0,
                'data': bytes.fromhex(data_hex),
                'hex': data_hex
            })
    return packets


def decode_command(data: bytes) -> dict:
    """Décode une commande OUT (EP 0x01)."""
    if len(data) < 2:
        return {'type': 'unknown', 'raw': data.hex()}

    cmd_type = data[0]
    opcode = data[1]

    result = {
        'type': f'0x{cmd_type:02x}',
        'opcode': f'0x{opcode:02x}',
        'params': data[2:].hex() if len(data) > 2 else '',
        'decoded': None
    }

    # Décoder les commandes connues
    if cmd_type == 0x02:
        if opcode == 0x81:
            # Init/Config ADC
            result['decoded'] = 'INIT_CONFIG'
        elif opcode == 0x83:
            if len(data) >= 4:
                result['decoded'] = f'REQUEST_INFO (addr=0x{data[2]:02x}{data[3]:02x})'
        elif opcode == 0x03:
            if len(data) >= 5:
                result['decoded'] = f'GET_STATUS (addr=0x{data[2]:02x}{data[3]:02x}, flags=0x{data[4]:02x})'
        elif opcode == 0x01:
            result['decoded'] = 'FLASH_LED'
        elif opcode == 0x02:
            result['decoded'] = 'GET_CALIBRATION'
        elif opcode == 0x07:
            # Trigger config
            if len(data) >= 8:
                result['decoded'] = f'SET_TRIGGER (src={data[2]}, level={data[3:5].hex()})'
        elif opcode == 0x85:
            sub = data[2] if len(data) > 2 else 0
            if sub == 0x04:
                if len(data) >= 10:
                    # Configuration capture
                    result['decoded'] = f'CONFIG_CAPTURE (params={data[3:10].hex()})'
            elif sub == 0x05:
                result['decoded'] = 'GET_DATA'
            elif sub == 0x07:
                if len(data) >= 10:
                    result['decoded'] = f'RUN_BLOCK (params={data[3:10].hex()})'
            elif sub == 0x08:
                result['decoded'] = f'SET_TRIGGER_ADV (params={data[3:12].hex() if len(data) >= 12 else ""})'
            elif sub == 0x09:
                result['decoded'] = f'SET_TIMEBASE (params={data[3:10].hex() if len(data) >= 10 else ""})'
            elif sub == 0x0c:
                result['decoded'] = f'SET_SIGGEN (params={data[3:16].hex() if len(data) >= 16 else ""})'
            else:
                result['decoded'] = f'CMD_85_{sub:02x}'
    elif cmd_type == 0x04:
        result['decoded'] = 'SPECIAL_CMD'

    return result


def decode_response(data: bytes, ep: int) -> dict:
    """Décode une réponse IN."""
    result = {'len': len(data), 'preview': data[:32].hex()}

    if ep == 0x81:
        if len(data) == 1:
            result['decoded'] = f'ACK (0x{data[0]:02x})'
        elif len(data) >= 16:
            # Chercher des infos lisibles
            try:
                text = data.decode('latin-1', errors='ignore')
                # Chercher serial
                if 'JO' in text:
                    idx = text.find('JO')
                    result['serial'] = text[idx:idx+10].strip('\x00')
                # Chercher date
                for m in ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec']:
                    if m in text:
                        idx = text.find(m)
                        result['cal_date'] = text[idx-2:idx+5].strip('\x00')
                        break
            except:
                pass
            result['decoded'] = 'INFO_RESPONSE'
    elif ep == 0x82:
        result['decoded'] = 'WAVEFORM_DATA'
        # Interpréter comme int16
        if len(data) >= 4:
            samples = []
            for i in range(0, min(10, len(data)), 2):
                val = int.from_bytes(data[i:i+2], 'little', signed=True)
                samples.append(val)
            result['samples_preview'] = samples

    return result


def analyze_capture(pcap_file: str):
    """Analyse complète de la capture."""
    print("=" * 80)
    print("ANALYSE DU PROTOCOLE USB PICOSCOPE 2204A")
    print("=" * 80)

    packets = get_packets(pcap_file)
    print(f"\nTotal paquets avec données: {len(packets)}")

    # Statistiques
    by_ep = defaultdict(list)
    for p in packets:
        by_ep[p['ep']].append(p)

    print("\nPar endpoint:")
    for ep, pkts in sorted(by_ep.items()):
        direction = "OUT" if ep in [0x01, 0x06] else "IN"
        print(f"  EP 0x{ep:02x} ({direction}): {len(pkts)} paquets")

    # Analyser les commandes OUT
    print("\n" + "=" * 80)
    print("COMMANDES OUT (EP 0x01)")
    print("=" * 80)

    commands = defaultdict(list)
    for p in by_ep[0x01]:
        cmd = decode_command(p['data'])
        key = (cmd['type'], cmd['opcode'])
        commands[key].append({
            'frame': p['frame'],
            'time': p['time'],
            'params': cmd['params'],
            'decoded': cmd['decoded'],
            'raw': p['hex'][:64]
        })

    for (ctype, opcode), instances in sorted(commands.items()):
        print(f"\n--- Type={ctype} Opcode={opcode} ({len(instances)} occurrences) ---")
        if instances[0]['decoded']:
            print(f"    Decoded: {instances[0]['decoded']}")

        # Afficher quelques exemples
        for inst in instances[:3]:
            print(f"    [{inst['frame']:5d}] {inst['raw']}")

        # Si plusieurs instances, chercher les variations
        if len(instances) > 1:
            params_set = set(inst['params'] for inst in instances)
            if len(params_set) > 1:
                print(f"    Variations détectées ({len(params_set)} différentes):")
                for p in list(params_set)[:5]:
                    print(f"      - {p[:32]}...")

    # Analyser les réponses
    print("\n" + "=" * 80)
    print("RÉPONSES IN (EP 0x81)")
    print("=" * 80)

    for p in by_ep[0x81][:20]:
        resp = decode_response(p['data'], 0x81)
        info = resp.get('decoded', '')
        extra = ''
        if 'serial' in resp:
            extra += f" serial={resp['serial']}"
        if 'cal_date' in resp:
            extra += f" cal={resp['cal_date']}"
        print(f"  [{p['frame']:5d}] len={resp['len']:4d} | {resp['preview'][:40]}... {info}{extra}")

    # Analyser les données waveform
    print("\n" + "=" * 80)
    print("DONNÉES WAVEFORM (EP 0x82)")
    print("=" * 80)

    for p in by_ep[0x82][:10]:
        resp = decode_response(p['data'], 0x82)
        samples = resp.get('samples_preview', [])
        print(f"  [{p['frame']:5d}] {resp['len']:5d} bytes | samples: {samples}")

    # Chercher des patterns
    print("\n" + "=" * 80)
    print("SÉQUENCES IDENTIFIÉES")
    print("=" * 80)

    # Trouver la séquence d'init
    print("\nSéquence d'initialisation (premiers paquets OUT):")
    for p in by_ep[0x01][:5]:
        cmd = decode_command(p['data'])
        print(f"  {p['hex'][:64]}")

    # Exporter les commandes uniques pour documentation
    print("\n" + "=" * 80)
    print("DICTIONNAIRE DES COMMANDES")
    print("=" * 80)

    unique_cmds = {}
    for (ctype, opcode), instances in commands.items():
        key = f"{ctype}_{opcode}"
        if key not in unique_cmds:
            unique_cmds[key] = {
                'example': instances[0]['raw'],
                'decoded': instances[0]['decoded'],
                'count': len(instances)
            }

    for key, info in sorted(unique_cmds.items()):
        print(f"\n{key}:")
        print(f"  Count: {info['count']}")
        print(f"  Decoded: {info['decoded']}")
        print(f"  Example: {info['example']}")


if __name__ == "__main__":
    pcap = sys.argv[1] if len(sys.argv) > 1 else "capture_full.pcap"
    analyze_capture(pcap)
