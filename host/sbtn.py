"""Shared device protocol for sudo-button (see firmware/src/proto.h)."""
import base64
import hashlib
import hmac
import re
import time

import serial

DEVICE = "/dev/sudo-button"
SECRET_PATH = "/etc/sudo-btn/secret"
MAGIC = b"sudo-btn-v1\n"
MIN_TIMEOUT, MAX_TIMEOUT = 5, 60
MAX_PAYLOAD = 3072  # must match firmware

_REPLY = re.compile(rb"@(PONG|OK|NO|PROVOK|PROVERR)\b(.*)")


class DeviceError(Exception):
    pass


def load_secret(path=SECRET_PATH):
    with open(path, "r") as f:
        s = bytes.fromhex(f.read().strip())
    if len(s) != 32:
        raise DeviceError("secret file malformed")
    return s


def expected_mac(secret, nonce, payload):
    return hmac.new(secret, MAGIC + nonce.encode() + b"\n" + payload, hashlib.sha256).hexdigest()


class Device:
    def __init__(self, path=DEVICE):
        self.path = path
        self.ser = None

    def open(self):
        s = serial.Serial()
        s.port = self.path
        s.baudrate = 115200
        s.timeout = 0.2
        # USB Serial/JTAG can reset the chip on DTR/RTS edges: keep both low
        s.dtr = False
        s.rts = False
        try:
            s.open()
        except (serial.SerialException, OSError) as e:
            raise DeviceError(f"cannot open {self.path}: {e}")
        self.ser = s

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def _send(self, line):
        try:
            self.ser.write(line.encode() + b"\n")
            self.ser.flush()
        except (serial.SerialException, OSError) as e:
            raise DeviceError(f"write failed: {e}")

    def _replies(self, deadline):
        """Yield (kind, rest) for protocol lines until deadline."""
        buf = b""
        while time.monotonic() < deadline:
            try:
                chunk = self.ser.read(256)
            except (serial.SerialException, OSError) as e:
                raise DeviceError(f"read failed: {e}")
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                m = _REPLY.search(line.rstrip(b"\r"))
                if m:
                    yield m.group(1).decode(), m.group(2).decode().split()
            if len(buf) > 8192:
                buf = buf[-1024:]

    def ping(self, tries=3):
        """Returns True if the device is paired, False if unpaired."""
        for _ in range(tries):
            self.ser.reset_input_buffer()
            self._send("PING")
            for kind, rest in self._replies(time.monotonic() + 1.5):
                if kind == "PONG":
                    return rest[-1] == "1"
        raise DeviceError("device did not answer PING")

    def provision(self, key_hex):
        self.ser.reset_input_buffer()
        self._send("PROV " + key_hex)
        for kind, rest in self._replies(time.monotonic() + 3):
            if kind == "PROVOK":
                return True, ""
            if kind == "PROVERR":
                return False, rest[0] if rest else "unknown"
        raise DeviceError("no answer to PROV")

    def request(self, nonce, timeout, payload, secret):
        """Ask for approval. Returns (approved, reason)."""
        timeout = max(MIN_TIMEOUT, min(MAX_TIMEOUT, int(timeout)))
        self.ser.reset_input_buffer()
        self._send(f"REQ {nonce} {timeout} " + base64.b64encode(payload).decode())
        for kind, rest in self._replies(time.monotonic() + timeout + 5):
            if kind not in ("OK", "NO") or not rest or rest[0] != nonce:
                continue  # stale reply from an earlier request
            if kind == "NO":
                return False, rest[1] if len(rest) > 1 else "denied"
            mac = rest[1] if len(rest) > 1 else ""
            if hmac.compare_digest(mac, expected_mac(secret, nonce, payload)):
                return True, "ok"
            return False, "bad-hmac"
        return False, "no-response"
