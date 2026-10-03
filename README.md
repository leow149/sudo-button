# sudo-button

A physical "approve" button for root commands. A touchscreen gadget (Waveshare ESP32-S3-Touch-LCD-4.3)
shows the **exact command** that is about to run as root, and only a tap on **APPROVE** lets it through.
Built so an AI coding agent (or any unprivileged script) can ask for root without ever having a password,
and without being able to fake the approval.

```
 caller (user) ──sudo──▶ sudo-btn-run ──unix socket──▶ sudo-btn-daemon ──USB serial──▶ ESP32 touchscreen
 (no password)           (runs as root)   (root-only)    (root, owns port)               (you tap APPROVE)
                              ▲                                  │
                              └─────── approved ◀── HMAC checked ◀── signed answer ◀────┘
                              └── exec(command) as root
```

## How it works

1. A single sudoers line lets your user run `sudo-btn-run` as root without a password. Nothing else.
2. `sudo-btn-run COMMAND...` resolves the command to an absolute path, builds a message
   (`user`, `cwd`, shell-quoted command, plus a warning if the binary isn't root-owned) and sends it to the daemon.
3. `sudo-btn-daemon` (root) is the only thing that talks to the board. It creates a fresh random nonce and sends
   `nonce + message` over USB serial.
4. The firmware shows the message. Non-printable / non-ASCII bytes are rendered as `\xNN` so nothing can hide in it.
   If the command doesn't fit, APPROVE stays disabled until you scroll to the end; it is also disabled for the first
   0.7 s so a stray tap can't approve. A tap sends back `HMAC-SHA256(secret, "sudo-btn-v1\n" + nonce + "\n" + message)`.
5. The daemon holds the same secret (root-only file) and verifies the HMAC. Only then does the wrapper `exec` the command.

Everything fails closed: deny, timeout, missing device, daemon down, bad signature, or a command too long to display
means nothing runs. There is no password fallback.

The pairing secret is generated on the PC and sent to the board once. A paired board refuses a new secret; the only
reset is holding the on-screen "forget host" button for 3 s.

## Layout

| Path | What |
|---|---|
| `firmware/` | PlatformIO + ESP-IDF 5.x project (LVGL 8.4 UI, GT911 touch, RGB panel, USB Serial/JTAG protocol) |
| `firmware/fonts/` | Generator for the command font (Hack, converted with `lv_font_conv`) + its license |
| `host/sudo-btn-run` | root wrapper that requests approval, then execs the command |
| `host/sudo-btn-daemon` | root daemon owning the serial port, verifying HMACs (`sudo-btn.service`) |
| `host/sudo-btn-provision` | one-time pairing |
| `host/install.sh`, `uninstall.sh` | install/remove sudoers rule, udev rule, systemd unit, files |

The wire protocol is documented in `firmware/src/proto.h`.

## Hardware

Waveshare ESP32-S3-Touch-LCD-4.3 (800x480 RGB panel, GT911 touch, CH422G I/O expander), connected via its native USB
port. The pin map in `firmware/src/hw.c` is specific to this board. Note: some units ship with 8 MB flash although the
vendor page says 16 MB; `partitions.csv` is sized for 8 MB.

## Build and flash

```sh
cd firmware
pio run                    # PlatformIO; ESP-IDF and LVGL are fetched automatically
pio run -t upload          # first flash, before the udev rule is installed
```

After installation the board's serial port is root-only (see below), so reflash through the helper, which also stops
and restarts the daemon:

```sh
pkexec "$PWD/flash.sh"
```

## Install (Linux, systemd, udev)

With the board plugged in:

```sh
sudo host/install.sh                      # or: --serial <USB serial> if several 303a:1001 boards are connected
sudo /usr/local/lib/sudo-btn/sudo-btn-provision   # pair once
sudo -n /usr/local/lib/sudo-btn/sudo-btn-run id   # tap APPROVE on the device
```

The installer binds a udev rule to **that one board's USB serial**: the node becomes `root:root 0600` and gets a
`/dev/sudo-button` symlink. Other ESP32 boards are untouched, and other tools (esptool, PlatformIO, ...) running as your
user can't open it, so it can't be flashed by accident. Read `install.sh` before running it; it writes to `/etc` and `/usr/local`.

## Security notes and limits

- Approval pins the **command line**, not the contents of files it runs. Approving `/path/script.sh` shows only the path
  (plus a warning that it is user-writable); the script could change between approval and execution.
- The pairing secret is stored in plain flash on the ESP32 (no flash encryption / secure boot), so someone with physical
  access to the chip can extract it.
- You can still tap APPROVE on something harmful. The screen guarantees you saw exactly what will run, nothing more.
- Opening the board's serial port resets the chip (USB Serial/JTAG behaviour), which is why the daemon keeps it open.
- This is a personal project, not an audited security product.

## License

[MIT](LICENSE). The bundled font data has its own license, see below.

## Third-party

The command font is generated from [Hack](https://sourcefoundry.org/hack/) (MIT / Bitstream Vera license, see
`firmware/fonts/LICENSE-Hack.txt`). Firmware dependencies (ESP-IDF, LVGL, `esp_lcd_touch`) are fetched at build time under their own licenses.
