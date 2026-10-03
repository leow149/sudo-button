#!/bin/bash
# Remove sudo-button host side. Run as: sudo ./uninstall.sh   (keeps /etc/sudo-btn/secret unless --purge)
set -uo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }

systemctl disable --now sudo-btn.service 2>/dev/null
rm -f /etc/sudoers.d/sudo-button /etc/systemd/system/sudo-btn.service \
      /etc/udev/rules.d/99-sudo-button.rules /usr/local/bin/sbsudo
rm -rf /usr/local/lib/sudo-btn
[[ ${1:-} == --purge ]] && rm -rf /etc/sudo-btn
systemctl daemon-reload
udevadm control --reload
udevadm trigger --subsystem-match=tty --property-match=ID_VENDOR_ID=303a
echo "removed"
