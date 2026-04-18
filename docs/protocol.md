# PicoScope 2204A - Notes de Reverse Engineering

## Informations USB

| Paramètre | Valeur |
|-----------|--------|
| VID | 0x0CE9 (Pico Technology) |
| PID | 0x1007 (PicoScope 2000 series) |
| USB | 2.0 High-Speed |
| Classe | Vendor Specific (0xFF) |

## Endpoints

| Endpoint | Direction | Type | Usage |
|----------|-----------|------|-------|
| 0x01 | OUT | Bulk | Commandes (64 bytes) |
| 0x81 | IN | Bulk | Réponses/ACK |
| 0x82 | IN | Bulk | Données waveform |
| 0x06 | OUT/IN | Bulk | Firmware/calibration |

## Structure des commandes

```
Offset  Size  Description
------  ----  -----------
0       1     Type (0x02 = commande standard)
1       1     Opcode
2       1     Sous-opcode / paramètre 1
3+      N     Paramètres additionnels
        ...   Padding avec 0x00 jusqu'à 64 bytes
```

## Opcodes identifiés

### Initialisation (0x81)
```
02 81 03 80 08 18 ...  - Séquence d'init 1
02 81 03 b0 ff 80 ...  - Séquence d'init 2 (config ADC?)
02 81 03 b2 ff 08 ...  - Config canal?
```

### Information (0x83, 0x03)
```
02 83 02 50            - Request device info
02 03 02 50 40         - Get device status/info
```

Réponse info device (64 bytes):
```
Offset  Content
------  -------
0-1     VID inversé (0xC0E9 = 0x0CE9)
2-3     PID (0x0C07 = 0x1007 en little endian inversé)
...
~20     Numéro de série "JOxxxxxxxx"
~30     Date calibration "DDMmmYY"
```

### Configuration (0x85)
```
02 85 04 99 00 00 00 0a ...  - Config ADC/channels
02 85 04 80 00 00 00 ...     - Set timebase
02 85 07 97 00 14 00 33 ...  - Run acquisition
02 85 05 82 00 08 00 01 ...  - Get data
02 85 08 85 00 20 ...        - Trigger config?
02 85 0c 86 00 40 ...        - Signal generator?
```

### Contrôle (0x01, 0x04)
```
02 01 01 80            - Flash LED
04 00 95 02 00         - Commande spéciale (firmware?)
```

## Réponses

### ACK simple
```
01                     - Commande acceptée
```

### Données waveform (EP 0x82)
- Chunks de 16384 bytes
- Format: int16 little-endian
- Valeur max: 32767 (correspond à Vmax de la plage)

## Séquence d'opération typique

1. **Ouverture**
   - Init séquence 1 (0x02 0x81 ...)
   - Init séquence 2 (0x02 0x81 ...)
   - Attendre ACK (0x01)

2. **Get Info**
   - Envoyer 0x02 0x83 0x02 0x50
   - Attendre ACK
   - Envoyer 0x02 0x03 0x02 0x50 0x40
   - Lire 64 bytes d'info

3. **Configuration**
   - Set channels (0x02 0x85 0x04 ...)
   - Set timebase (0x02 0x85 0x04 0x80 ...)
   - Set trigger (0x02 0x85 0x07 ...)

4. **Acquisition**
   - Run block (0x02 0x85 0x07 0x97 ...)
   - Poll status ou attendre
   - Read data sur EP 0x82

5. **Fermeture**
   - Release interface

## TODO pour implémentation Android

1. [ ] Décoder complètement les commandes de configuration canal
2. [ ] Décoder le format exact des paramètres timebase
3. [ ] Comprendre le mécanisme de polling/status
4. [ ] Décoder les commandes trigger
5. [ ] Décoder les commandes signal generator
6. [ ] Tester sur Android avec libusb-android

## Ressources utiles

- Wireshark avec filtre: `usb.device_address == X`
- Capture: `tcpdump -i usbmon1 -w capture.pcap`
- Analyse: `tshark -r capture.pcap -Y "usb.capdata"`

## Fichiers

- `usb_capture.pcap` - Capture USB brute
- `analyze_protocol.py` - Script d'analyse
- `picoscope_libusb.py` - Driver prototype libusb
