#!/usr/bin/env bash
# Flash the prebuilt 4square demo image over USB.
#
#   release/flash.sh                 # auto-detects the port
#   release/flash.sh /dev/ttyACM0    # or name it
#
# Needs esptool (pip install esptool). The board is an ESP32-C3 with native USB:
# plug it in, it shows up as /dev/ttyACM0 (Linux) or /dev/cu.usbmodem* (macOS).
# If the chip is not found, hold BOOT while plugging in, then run this again.
set -e
cd "$(dirname "$0")"

PORT="${1:-}"
if [ -z "$PORT" ]; then
  for p in /dev/ttyACM0 /dev/ttyACM1 /dev/cu.usbmodem*; do
    [ -e "$p" ] && { PORT="$p"; break; }
  done
fi
[ -n "$PORT" ] || { echo "no serial port found; pass it: flash.sh /dev/ttyACM0"; exit 1; }

if command -v esptool >/dev/null 2>&1; then ESPTOOL=esptool
elif command -v esptool.py >/dev/null 2>&1; then ESPTOOL=esptool.py
else echo "esptool not found. Install it: pip install esptool"; exit 1; fi

sha256sum -c SHA256SUMS --ignore-missing --quiet

echo "== flashing 4square demo image to $PORT =="
"$ESPTOOL" --chip esp32c3 --port "$PORT" --baud 921600 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0 4square-demo-merged.bin

echo
echo "DONE - unplug and replug the board. It boots straight into the clock."
