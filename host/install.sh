#!/bin/bash
# Install sudo-button host side. Run as: sudo ./install.sh [--serial USB_SERIAL]
# The udev rule is bound to ONE board's USB serial, so other ESP32s are left alone.
# With the board plugged in the serial is detected; pass --serial if several candidates are connected.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }
USER_NAME=${SUDO_USER:-}
[[ -z $USER_NAME && -n ${PKEXEC_UID:-} ]] && USER_NAME=$(getent passwd "$PKEXEC_UID" | cut -d: -f1)
[[ -n $USER_NAME && $USER_NAME != root ]] || { echo "run via sudo/pkexec from your normal user"; exit 1; }
cd "$(dirname "$0")"

SERIAL=""
[[ ${1:-} == --serial ]] && SERIAL=${2:-}
if [[ -z $SERIAL ]]; then
    found=()
    for d in /dev/ttyACM*; do
        [[ -e $d ]] || continue
        props=$(udevadm info -q property -n "$d")
        grep -qx 'ID_VENDOR_ID=303a' <<<"$props" && grep -qx 'ID_MODEL_ID=1001' <<<"$props" || continue
        found+=("$(sed -n 's/^ID_SERIAL_SHORT=//p' <<<"$props")")
    done
    if [[ ${#found[@]} -ne 1 ]]; then
        echo "Expected exactly one Espressif USB-JTAG board (303a:1001), found ${#found[@]}: ${found[*]:-none}"
        echo "Plug in only the sudo-button, or pass:  --serial <USB serial>"
        exit 1
    fi
    SERIAL=${found[0]}
fi
[[ $SERIAL =~ ^[0-9A-Za-z:._-]+$ ]] || { echo "refusing odd-looking serial: $SERIAL"; exit 1; }
echo "binding udev rule to board with USB serial $SERIAL"

LIB=/usr/local/lib/sudo-btn
install -d -m 0755 -o root -g root "$LIB"
install -m 0644 -o root -g root sbtn.py "$LIB/sbtn.py"
install -m 0755 -o root -g root sudo-btn-run sudo-btn-daemon sudo-btn-provision "$LIB/"

install -m 0644 -o root -g root sudo-btn.service /etc/systemd/system/sudo-btn.service
RULE=$(mktemp)
sed "s/@SERIAL@/$SERIAL/" 99-sudo-button.rules.in > "$RULE"
install -m 0644 -o root -g root "$RULE" /etc/udev/rules.d/99-sudo-button.rules
rm -f "$RULE"
install -d -m 0700 -o root -g root /etc/sudo-btn

# Only the wrapper gets NOPASSWD, nothing else.
TMP=$(mktemp)
echo "$USER_NAME ALL=(root) NOPASSWD: $LIB/sudo-btn-run" > "$TMP"
visudo -c -q -f "$TMP"
install -m 0440 -o root -g root "$TMP" /etc/sudoers.d/sudo-button
rm -f "$TMP"

# Convenience: `sbsudo CMD...` == `sudo sudo-btn-run CMD...`
printf '#!/bin/sh\nexec sudo %s/sudo-btn-run "$@"\n' "$LIB" > /usr/local/bin/sbsudo
chmod 0755 /usr/local/bin/sbsudo

systemctl daemon-reload
udevadm control --reload
udevadm trigger --subsystem-match=tty --property-match=ID_VENDOR_ID=303a

sleep 1
ls -l /dev/sudo-button 2>/dev/null && stat -Lc 'device node: %U:%G %a' /dev/sudo-button \
    || echo "NOTE: /dev/sudo-button not present yet - replug the device"

cat <<EOF

Installed. Next:  sudo $LIB/sudo-btn-provision
Then test with:   sbsudo id
EOF
