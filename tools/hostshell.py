#!/usr/bin/env python3
"""Run a shell on the Mac whose terminal is the VeeTee 2350.

    uv run hostshell.py LINE_PORT [--keyboard KBD_PORT] [--type 'ls -G' ...]
                        [--keys '\x1bOR'] [--exit-after 20]

The shell runs in a pseudo-terminal sized to the VeeTee's grid (see
"Size" below) with TERM=xterm-256color. Bytes flow:

    shell (pty) --> serial --> VeeTee screen
    VeeTee replies (DA, DSR, ...) --> serial --> shell
    this script's stdin (your keyboard) --> shell

so it is a complete terminal session, with the Mac's keyboard standing in
until the USB HID keyboard (Goal 3) exists.

With --keyboard, keys go the way they would on a real terminal: into the
VeeTee's keyboard port, through the device (which can claim them, e.g.
for the SET-UP screen), and back out of the line port to the shell.
Without it, keys go straight to the shell.

Size: the bridge enables the VeeTee's in-band resize reports (DECSET
2048) and starts the shell at the size the terminal reports. Later
reports (font, size or resolution changed in SET-UP) are removed from the
stream and applied to the pty with TIOCSWINSZ, so the kernel sends
SIGWINCH and full-screen programs (htop, micro, vim) redraw at the real
size. --rows/--cols only matter if the terminal never answers.

--type TEXT (TEXT + Enter) and --keys ESCAPED (Python escapes, no Enter)
inject keystrokes in order, 1 s apart, for scripted tests. Standard
library only (termios/pty/select), so no pyserial dependency.
"""

from __future__ import annotations

import argparse
import fcntl
import os
import pty
import re
import select
import struct
import sys
import termios
import time
import tty


def open_serial(path: str) -> int:
    """Open a CDC tty in raw mode. Baud rate is irrelevant for USB CDC."""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.CLOCAL | termios.CREAD  # cflag: ignore modem lines
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


RESIZE_REPORT = re.compile(rb"\x1b\[48;(\d+);(\d+);(\d+);(\d+)t")
REPORT_PREFIX = b"\x1b[48;"


class SizeTracker:
    """Pull CSI 48;rows;cols;h;w t reports out of the device's byte stream.

    A report can be split across reads, so a trailing fragment that could
    still become one is held back until the next chunk.
    """

    def __init__(self):
        self.pending = b""
        self.size = None  # (rows, cols)

    def feed(self, data: bytes) -> bytes:
        data = self.pending + data
        self.pending = b""
        out = bytearray()
        position = 0
        for match in RESIZE_REPORT.finditer(data):
            out += data[position : match.start()]
            self.size = (int(match.group(1)), int(match.group(2)))
            position = match.end()
        tail = data[position:]
        start = tail.rfind(b"\x1b")
        if start != -1 and tail[start:].startswith(REPORT_PREFIX[: len(tail) - start]) and not tail[start:].endswith(b"t"):
            self.pending = tail[start:]
            tail = tail[:start]
        out += tail
        return bytes(out)


def set_winsize(fd: int, rows: int, cols: int) -> None:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def query_size(serial_fd: int, tracker: SizeTracker, timeout: float = 1.5):
    """Enable resize reports; the terminal answers with its current size."""
    write_all(serial_fd, b"\x1b[?2048h")
    deadline = time.monotonic() + timeout
    while tracker.size is None and time.monotonic() < deadline:
        ready, _, _ = select.select([serial_fd], [], [], 0.1)
        if ready:
            try:
                tracker.feed(os.read(serial_fd, 4096))
            except BlockingIOError:
                pass
    return tracker.size


def spawn_shell(rows: int, cols: int, shell: str) -> tuple[int, int]:
    pid, master = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM="xterm-256color", CLICOLOR="1")
        env.pop("COLUMNS", None)
        env.pop("LINES", None)
        os.execvpe(shell, [shell, "-i"], env)
    set_winsize(master, rows, cols)
    return pid, master


def write_all(fd: int, data: bytes) -> None:
    while data:
        try:
            written = os.write(fd, data)
            data = data[written:]
        except BlockingIOError:
            select.select([], [fd], [], 1.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port")
    parser.add_argument("--rows", type=int, default=30)
    parser.add_argument("--cols", type=int, default=80)
    parser.add_argument("--shell", default="/bin/bash")
    parser.add_argument("--keyboard", help="VeeTee keyboard port (CDC 1)")
    parser.add_argument(
        "--type",
        dest="actions",
        action="append",
        type=lambda text: text.encode() + b"\r",
        default=[],
        help="line to type (repeatable; actions run 1 s apart)",
    )
    parser.add_argument(
        "--keys",
        dest="actions",
        action="append",
        type=lambda text: text.encode().decode("unicode_escape").encode("latin-1"),
        help="raw keys with Python escapes, e.g. '\\x1bOR' for F3",
    )
    parser.add_argument("--exit-after", type=float, default=0.0)
    args = parser.parse_args()

    serial_fd = open_serial(args.port)
    keyboard_fd = open_serial(args.keyboard) if args.keyboard else None
    tracker = SizeTracker()
    rows, cols = query_size(serial_fd, tracker) or (args.rows, args.cols)
    applied = (rows, cols)
    pid, master = spawn_shell(rows, cols, args.shell)
    key_sink = keyboard_fd if keyboard_fd is not None else master
    interactive = sys.stdin.isatty() and not args.actions
    saved = None
    if interactive:
        saved = termios.tcgetattr(sys.stdin.fileno())
        tty.setraw(sys.stdin.fileno())

    pending = list(args.actions)
    next_type = time.monotonic() + 1.5
    deadline = time.monotonic() + args.exit_after if args.exit_after else None
    watched = [master, serial_fd] + ([sys.stdin.fileno()] if interactive else [])

    try:
        while True:
            now = time.monotonic()
            if pending and now >= next_type:
                write_all(key_sink, pending.pop(0))
                next_type = now + 1.0
            if deadline and now >= deadline:
                break
            ready, _, _ = select.select(watched, [], [], 0.1)
            for fd in ready:
                try:
                    data = os.read(fd, 4096)
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError:
                    return 0  # shell exited (pty closed)
                if not data:
                    return 0
                if fd == master:
                    write_all(serial_fd, data)
                elif fd == serial_fd:
                    data = tracker.feed(data)
                    if tracker.size and tracker.size != applied:
                        applied = tracker.size
                        set_winsize(master, *applied)  # kernel sends SIGWINCH
                    if data:
                        write_all(master, data)
                else:  # our stdin: the keyboard
                    write_all(key_sink, data)
    finally:
        if saved is not None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, saved)
        write_all(serial_fd, b"\x1b[?2048l")
        os.close(serial_fd)
        if keyboard_fd is not None:
            os.close(keyboard_fd)
        try:
            os.kill(pid, 9)
        except ProcessLookupError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
