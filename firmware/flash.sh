#!/bin/bash
# Flash a built firmware to the sudo-button. The serial port is root-only and the
# daemon holds it, so run as root:   pkexec "$PWD/flash.sh"   (from the firmware/ directory)
# (build first with: ~/.platformio/penv/bin/pio run)
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root (pkexec)"; exit 1; }
B="$(dirname "$(readlink -f "$0")")/.pio/build/waveshare43"

systemctl stop sudo-btn.service 2>/dev/null || true
trap 'systemctl start sudo-btn.service 2>/dev/null || true' EXIT

esptool --chip esp32s3 --port /dev/sudo-button --before default-reset --after hard-reset \
    write-flash --flash-mode dio --flash-size 8MB \
    0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0x10000 "$B/firmware.bin"
