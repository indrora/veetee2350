"""The VeeTee's automation HID, from the Mac (debug builds only).

Mirrors include/automation_hid.h: one 64-byte report each way, no HID
report IDs, byte 0 saying what the report is. Through it a script can
do what a person at the VeeTee does:

    KEYBOARD  press and release keys on a USB keyboard of its own, decoded
              exactly as the board's real USB keyboard is (hidkbd.c)
    TYPE      send what an xterm would for keys ("\\x1b[1;5R" is Ctrl+F3),
              through the decoder the old "VeeTee keyboard" port used
    BOOTSEL   press and release a BOOTSEL of its own (OR'd with the real
              one): taps, holds, the 5-second factory reset
    LOG       read the debug text (the "[key] F3 -> set-up" lines), which
              debug builds send here instead of down the terminal's line

Uses hidapi (cython-hidapi, `uv add hidapi`), the usual way to talk HID
from Python on macOS, Linux and Windows. A vendor-defined HID needs no
Input Monitoring permission on macOS (that's for keyboards and mice).
"""
import re
import time

import hid

VENDOR_PAGE = 0xFF00          # usage page of the generic in/out descriptor
RASPBERRY_PI_VID = 0x2E8A
REPORT_SIZE = 64
TEXT_MAX = REPORT_SIZE - 2
KEYBOARD, TYPE, BOOTSEL, LOG = 1, 2, 3, 4

# HID keyboard page usages by name (USB HID Usage Tables, page 0x07).
USAGES = {f"f{n}": 0x3A + n - 1 for n in range(1, 13)}
USAGES.update({chr(ord("a") + i): 0x04 + i for i in range(26)})
USAGES.update({str((i + 1) % 10): 0x1E + i for i in range(10)})
USAGES.update({
    "enter": 0x28, "return": 0x28, "esc": 0x29, "escape": 0x29,
    "backspace": 0x2A, "tab": 0x2B, "space": 0x2C, "capslock": 0x39,
    "printscreen": 0x46, "scrolllock": 0x47, "pause": 0x48,
    "insert": 0x49, "home": 0x4A, "pageup": 0x4B, "delete": 0x4C,
    "end": 0x4D, "pagedown": 0x4E, "right": 0x4F, "left": 0x50,
    "down": 0x51, "up": 0x52, "numlock": 0x53,
})
# Printable characters on a US layout: (usage, shifted).
CHARS = {c: (USAGES[c], False) for c in "abcdefghijklmnopqrstuvwxyz0123456789"}
CHARS.update({c.upper(): (USAGES[c], True) for c in "abcdefghijklmnopqrstuvwxyz"})
for plain, shifted, usage in zip("-=[]\\;'`,./", '_+{}|:"~<>?',
                                 (0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34,
                                  0x35, 0x36, 0x37, 0x38)):
    CHARS[plain] = (usage, False)
    CHARS[shifted] = (usage, True)
for digit, shifted in zip("1234567890", "!@#$%^&*()"):
    CHARS[shifted] = (USAGES[digit], True)
CHARS.update({" ": (0x2C, False), "\n": (0x28, False), "\r": (0x28, False),
              "\t": (0x2B, False)})
MODIFIERS = {"ctrl": 0x01, "control": 0x01, "shift": 0x02, "alt": 0x04,
             "option": 0x04, "gui": 0x08, "cmd": 0x08, "meta": 0x08}


def parse_key(name):
    """'ctrl+F3' -> (modifier byte, usage). A single character works too."""
    *mods, key = name.split("+") if name != "+" else ["+"]
    bits = 0
    for mod in mods:
        if mod.lower() not in MODIFIERS:
            raise ValueError(f"unknown modifier {mod!r} in {name!r}")
        bits |= MODIFIERS[mod.lower()]
    if len(key) == 1 and key in CHARS:
        usage, shifted = CHARS[key]
        return bits | (0x02 if shifted else 0), usage
    if key.lower() in USAGES:
        return bits, USAGES[key.lower()]
    raise ValueError(f"unknown key {key!r}")


def find():
    """The VeeTee's automation interface (its hidapi path), or None."""
    for info in hid.enumerate(RASPBERRY_PI_VID, 0):
        if info["usage_page"] == VENDOR_PAGE:
            return info["path"]
    return None


class VeeTee:
    """An open automation HID. Use as a context manager."""

    def __init__(self, path=None, wait=10.0):
        deadline = time.monotonic() + wait
        while path is None:
            path = find()
            if path is None:
                if time.monotonic() > deadline:
                    raise SystemExit("no VeeTee automation HID: is a "
                                     "debug build (pio run -e pizero-debug) "
                                     "running?")
                time.sleep(0.25)
        self.device = hid.device()
        self.device.open_path(path)
        # cython-hidapi's read(n, 0) blocks for ever unless the device is
        # non-blocking; with it, 0 means "whatever's there now" and a
        # positive timeout still waits that long.
        self.device.set_nonblocking(True)
        self.pending = ""            # log text up to the next newline
        # Lines read but not yet matched by wait_for: a line that came in
        # while something else was reading (sendkeys echoes after every
        # step) must still satisfy the next wait.
        self.seen = []
        self.bootsel_held = False

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        # Never leave the virtual BOOTSEL down: held 5 s, it's a factory
        # reset. (Only a kill -9 gets past this; then press and let go of
        # the real one, or run `sendkeys.py b:up`.)
        if self.bootsel_held:
            self.bootsel(False)
        self.device.close()

    def send(self, kind, payload=b""):
        report = bytes([kind]) + bytes(payload)
        report = report.ljust(REPORT_SIZE, b"\0")[:REPORT_SIZE]
        # hidapi wants the report ID first: 0, as there are none.
        self.device.write(b"\0" + report)
        # The VeeTee queues 32 reports; its main loop takes them far
        # faster than this, except when busy (an SD write, a card scan).
        time.sleep(0.002)

    def type(self, data):
        """Bytes as an xterm would send them for keys."""
        if isinstance(data, str):
            data = data.encode()
        for at in range(0, len(data), TEXT_MAX):
            chunk = data[at:at + TEXT_MAX]
            self.send(TYPE, bytes([len(chunk)]) + chunk)

    def keyboard(self, modifiers=0, *usages):
        """One boot keyboard report: what's held now."""
        self.send(KEYBOARD, bytes([modifiers, 0, *usages[:6]]))

    def press(self, name, hold=0.03):
        """Press and let go of a key on the virtual USB keyboard."""
        modifiers, usage = parse_key(name)
        self.keyboard(modifiers, usage)
        time.sleep(hold)
        self.keyboard()

    def keys(self, text, gap=0.01):
        """Type text key by key on the virtual USB keyboard."""
        for char in text:
            if char not in CHARS:
                raise ValueError(f"no US key for {char!r}")
            usage, shifted = CHARS[char]
            self.keyboard(0x02 if shifted else 0, usage)
            self.keyboard()
            time.sleep(gap)

    def bootsel(self, down):
        self.send(BOOTSEL, bytes([1 if down else 0]))
        self.bootsel_held = down

    def hold_bootsel(self, seconds):
        self.bootsel(True)
        time.sleep(seconds)
        self.bootsel(False)

    def lines(self, timeout=0.1):
        """Debug lines that arrived, waiting up to `timeout` for the first."""
        out = []
        deadline = time.monotonic() + timeout
        while True:
            left = max(0, int((deadline - time.monotonic()) * 1000))
            report = self.device.read(REPORT_SIZE, left if not out else 0)
            if not report:
                break
            if report[0] == LOG:
                self.pending += bytes(report[2:2 + report[1]]).decode(
                    "utf-8", "replace")
                *done, self.pending = self.pending.split("\n")
                out += [line.rstrip("\r") for line in done if line.strip()]
        self.seen = (self.seen + out)[-1000:]
        return out

    def wait_for(self, pattern, timeout=5.0, echo=None):
        """The first debug line since the last match that matches
        `pattern`, reading more for up to `timeout` seconds."""
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while True:
            for at, line in enumerate(self.seen):
                if regex.search(line):
                    del self.seen[:at + 1]
                    return line
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"no debug line matching {pattern!r} "
                                   f"in {timeout} s")
            for line in self.lines(min(0.2, left)):
                if echo:
                    echo(line)
