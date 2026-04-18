# PicoScope 2204A - Driver Android/libusb

## Résumé du Reverse Engineering

Ce document décrit le protocole USB du PicoScope 2204A découvert par analyse des captures USB.

## Architecture Hardware

Le PicoScope 2204A utilise:
- **Microcontrôleur**: Cypress EZ-USB FX2 (CY7C68013A)
- **FPGA**: Xilinx (pour le traitement du signal)
- **ADC**: Non identifié précisément

## Identifiants USB

| Paramètre | Valeur |
|-----------|--------|
| VID | 0x0CE9 (Pico Technology) |
| PID | 0x1007 |
| Class | Vendor Specific (0xFF) |

## Endpoints

| EP | Direction | Type | Usage |
|----|-----------|------|-------|
| 0x01 | OUT | Bulk | Commandes (64 bytes) |
| 0x81 | IN | Bulk | Réponses/Status |
| 0x82 | IN | Bulk | Données waveform |
| 0x06 | OUT | Bulk | Firmware FPGA / Waveform table |

## Firmware

### FX2 Firmware (8.8 KB)
- Fichier: `fx2_firmware.txt`
- Format: Chunks avec adresse + données
- Upload: Via control transfers (bRequest=0xA0)
- Séquence:
  1. Écrire 0x01 à 0xE600 (HALT CPU)
  2. Uploader les chunks aux bonnes adresses
  3. Écrire 0x00 à 0xE600 (RUN CPU)
- Le device se réénumère après le démarrage du nouveau firmware

### FPGA Firmware (185.6 KB)
- Fichier: `fpga_firmware.bin`
- Format: Bitstream Xilinx (signature 0x5599AA66)
- Upload: Bulk transfer sur EP 0x06 en chunks de 32KB
- Déclenché par commande spéciale `04 00 95 02 00`

## Séquence d'Initialisation Complète

```
1. Trouver le device USB (VID=0x0CE9, PID=0x1007)
2. Détacher le driver kernel si nécessaire
3. Réclamer l'interface 0

4. Upload FX2 Firmware:
   a. Control transfer: HALT CPU (0xE600 = 0x01)
   b. Upload chunks via control transfers
   c. Control transfer: RUN CPU (0xE600 = 0x00)
   d. Attendre re-enumeration (~1s)
   e. Retrouver le device (nouvelle adresse)

5. Init ADC:
   - Commande: 02 81 03 80 08 18 ...
   - Commande: 02 81 03 b0 ff 80 ...
   - Attendre ACK (0x01)

6. Lire infos device:
   - 4 cycles de lecture à adresses 0x00, 0x40, 0x80, 0xC0
   - Commande: 02 83 02 50 [addr]
   - Commande: 02 03 02 50 40
   - Extraire serial (JOxxxxxxxx) et date calibration

7. Upload FPGA Firmware:
   - Commande: 02 85 04 99 00 00 00 0a
   - Commande: 02 81 03 b2 ff 08
   - Commande spéciale: 04 00 95 02 00
   - Upload firmware en chunks 32KB sur EP 0x06

8. Config post-FPGA:
   - Commande: 02 85 04 80 00 00 00 81 03 b2 00 08
   - Commande: 02 85 04 80 00 00 00 05 04 8f 00 10
   - Lire réponse FPGA (0xCAAC)

9. Upload waveform table:
   - 8192 bytes (pattern 0xEE07 répété) sur EP 0x06

10. Device prêt pour les commandes
```

## Commandes Principales

### Format général
```
[Type: 1 byte] [Opcode: 1 byte] [Params...] [Padding to 64 bytes]
```

### Commandes identifiées

| Type | Opcode | Description | Exemple |
|------|--------|-------------|---------|
| 0x02 | 0x01 | Flash LED | `02 01 01 80` |
| 0x02 | 0x81 | Init/Config | `02 81 03 80 08 18...` |
| 0x02 | 0x83 | Request info | `02 83 02 50 [addr]` |
| 0x02 | 0x03 | Get info | `02 03 02 50 40` |
| 0x02 | 0x85 | Config/Capture | Voir sous-opcodes |
| 0x02 | 0x07 | Trigger | `02 07 06 00 40...` |
| 0x04 | 0x00 | Special (FPGA) | `04 00 95 02 00` |

### Sous-opcodes 0x85

| Sub | Description |
|-----|-------------|
| 0x04 | Config capture/channel |
| 0x05 | Get data |
| 0x07 | Run block |
| 0x08 | Trigger/Buffer config |
| 0x09 | Timebase |
| 0x0b | Buffer setup |
| 0x0C | Signal generator |
| 0x21 | Channel setup |

## Commandes Détaillées

### Configuration Canal (0x85 0x04 0x9b + 0x85 0x21)
```
02 85 04 9b 00 00 00              # Config header
85 21 8c 00 e8 00 00 00 [CH]      # Channel setup
   [CH] = 0x80 | (channel << 4) | (range & 0x0F)
   - bit 7: enabled
   - bits 4-5: channel (0=A, 1=B)
   - bits 0-3: range (0=10mV, 6=1V, 8=5V, etc.)
```

### Timebase + Run Block (0x85 0x04 0x9a + 0x85 0x07 0x97)
```
02 85 04 9a 00 00 00              # Timebase config
85 07 97 00 [SH] [SL] [TB] 04 40  # Run block
   [SH][SL] = sample count (big-endian)
   [TB] = timebase byte (0x30 | timebase_index)
```

### Trigger (0x02 0x07)
```
02 07 06 [CH] 40 [TH] [TL] [DIR] [EN] 00
   [CH] = channel (0=A, 1=B)
   [TH][TL] = threshold (signed int16, big-endian)
   [DIR] = direction (0=rising, 1=falling)
   [EN] = enable (0=auto, 1=enabled)
```

### Acquisition Compound Command
Pour une acquisition fiable, envoyer un compound command:
```
02 85 08 85 00 20 00 00 00 01 00    # Trigger config
85 08 93 00 20 00 00 00 01 57       # Channel config
85 08 89 00 20 00 00 00 00 20       # Buffer config
85 05 82 00 08 00 01                # Get data setup
85 04 9a 00 00 00                   # Timebase
85 07 97 00 [SH] [SL] 23 c4 40      # Run block
85 05 95 00 08 00 ff 00 00 00 00    # Status config
```

## Format des Données

Les données sont lues sur EP 0x82 en buffer de 16KB:
- **Header**: 2 bytes `57 a7` (sync pattern)
- **Buffer**: 8192 samples int16 little-endian
- **Position**: Les données valides peuvent être à la fin du buffer (circular buffer)
- **Valeurs**: Signées 16-bit, 0 = ~0V, ±32767 = ±pleine échelle

## Fichiers du Projet

| Fichier | Description |
|---------|-------------|
| `fx2_firmware.txt` | Firmware FX2 extrait (chunks) |
| `fpga_firmware.bin` | Firmware FPGA extrait |
| `picoscope_libusb_full.py` | Driver libusb complet |
| `extract_fx2_firmware.py` | Script d'extraction FX2 |
| `extract_fpga_firmware.py` | Script d'extraction FPGA |
| `decode_protocol.py` | Analyseur de protocole |
| `capture_control.pcap` | Capture USB de référence |

## Pour Android

Pour porter ce driver sur Android:

1. Utiliser `libusb-android` ou `usb-serial-for-android`
2. Demander permission USB via `UsbManager`
3. Implémenter la même séquence d'init
4. Attention: control transfers pour FX2, bulk pour FPGA
5. Gérer la re-enumeration après upload FX2

## Problèmes Connus et Solutions

### Buffer Overflow USB
**Problème**: Les commandes timeout après ~5 opérations.
**Solution**: Lire la réponse sur EP 0x81 après chaque commande envoyée sur EP 0x01.

### Waveform Table Timeout
**Problème**: L'upload sur EP 0x06 timeout après le FPGA.
**Solution**: Continuer malgré l'erreur, le device fonctionne quand même.

### Device Bloqué
**Problème**: "Entity not found" ou device non répondant.
**Solution**: Déconnexion physique USB requise pour reset.

### Données à Zéro
**Problème**: Capture retourne tous zéros.
**Causes possibles**:
1. Compound command d'acquisition incorrect
2. Délais insuffisants entre commandes
3. Configuration canal non appliquée
**Solution**: Utiliser les compound commands exactes des captures USB.

### Position des Données dans le Buffer
**Problème**: Données valides à la fin du buffer, pas au début.
**Explication**: Le device utilise un circular buffer. Les données apparaissent à une position dépendant du trigger et de la taille demandée.
**Solution**: Scanner le buffer pour trouver les samples non-nuls.

### Status 0x7b au lieu de 0x33/0x3b
**Problème**: Après connexion libusb (sans SDK), le device retourne status 0x7b au lieu de 0x33/0x3b.
**Analyse des bits**:
- 0x33 = 0011 0011 (capture pending)
- 0x3b = 0011 1011 (data ready, bit 3 = données prêtes)
- 0x7b = 0111 1011 (bit 6 = 0x40 additionnel = état erreur/overflow)

**Cause probable**: Le device conserve l'état de la session précédente. Le bit 6 indique des données non lues ou un buffer overflow.

**Solution**:
1. **TOUJOURS faire l'upload FX2 firmware** - la ré-énumération USB qui suit reset l'état du device
2. Ne pas essayer d'optimiser en skippant l'upload même si le firmware semble déjà chargé
3. Alternative (non confirmée): chercher une commande de "clear buffer"

### Reconnexion sans déconnexion physique
**Problème**: Impossible de réutiliser le device après fermeture du SDK officiel.
**Cause**: L'état interne du device (bits de status, buffers) n'est pas réinitialisé.
**Solution**:
1. Déconnexion physique USB (rebrancher le câble)
2. Ou ré-upload du firmware FX2 complet (force re-enumeration)

## Séquence de Capture Complète (Détaillée)

Basé sur l'analyse des frames 49543-49620 de capture_full.pcap:

### Phase 1: Config Compound Commands
```
Frame 49543 (Compound 1):
02 85 08 85 00 20 00 00 00 01 00    # Trigger buffer config
85 08 93 00 20 00 00 00 01 57       # Channel config
85 08 89 00 20 00 00 00 00 20       # Buffer config
85 05 82 00 08 00 01                # Get data setup (flag=1)
85 04 9a 00 00 00                   # Timebase
85 07 97 00 [SH] [SL] 23 c4 40      # Run block
85 05 95 00 08 00 ff 00 00 00 00    # Status config

Frame 49545 (Compound 2):
02 85 0c 86 00 40 00 01...          # Extended config
85 05 87 00 08 00 00                # Data request
85 0b 90 00 38 00...                # Buffer setup
85 08 8a 00 20 00...0b 01 02 0c 03 0a 00 00  # ADC config
85 04 81 00 00 00 00                # Config end
```

### Phase 2: Status Polling
```
Frame 49551: LED command (02 01 01 80) → poll status
Frame 49604: Response 0x33 (capture en cours)
Frame 49609: LED command → poll again
Frame 49612: Response 0x3b (données prêtes)
```

### Phase 3: Trigger + Data
```
Frame 49613: Trigger (02 07 06 00 40 00 00 02 01 00)
Frame 49620: Data sur EP 0x82 (16384 bytes, header 57a7)
```

## Test

```bash
sudo python3 picoscope_libusb_full.py
```

ou

```bash
sudo ./test_picoscope.sh
```
