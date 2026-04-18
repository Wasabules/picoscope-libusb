#!/bin/bash
# Test script pour le driver PicoScope libusb
# Usage: sudo ./test_picoscope.sh

echo "=== Test PicoScope 2204A libusb Driver ==="

# Vérifier si le device est présent
if ! lsusb | grep -q "0ce9:1007"; then
    echo "PicoScope non détecté. Veuillez le brancher."
    echo "En attente..."

    for i in {1..30}; do
        sleep 1
        if lsusb | grep -q "0ce9:1007"; then
            echo ""
            lsusb | grep -i pico
            break
        fi
        echo -n "."
    done

    if ! lsusb | grep -q "0ce9:1007"; then
        echo ""
        echo "Timeout - PicoScope non trouvé"
        exit 1
    fi
fi

echo ""
echo "PicoScope détecté:"
lsusb | grep -i pico
echo ""

# Lancer le driver
cd "$(dirname "$0")"
python3 picoscope_libusb_full.py
