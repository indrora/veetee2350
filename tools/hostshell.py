#!/usr/bin/env python3
"""Run a shell on the Mac whose terminal is the VeeTee 2350.

    uv run hostshell.py LINE_PORT [--keyboard KBD_PORT] [--type 'ls -G' ...]
                        [--keys '\x1bOR'] [--exit-after 20]

The shell runs in a pseudo-terminal sized to the VeeTee grid (80x30 by
default) with TERM=xterm-256color. Bytes flow:

    shell (pty) --> serial --> VeeTee screen
    VeeTee replies (DA, DSR, ...) --> serial --> shell
    this script's stdin (your keyboard) --> shell

so it is a complete terminal session, with the Mac's keyboard standing in
until the USB HID keyboard (Goal 3) exists.

With --keyboard, keys go the way they would on a real terminal: into the
VeeTee's keyboard port, through the device (which can claim them, e.g.
for the SET-UP screen), and back out of the line port to the shell.
Without it, keys go straight to the shell.

--type TEXT (TEXT + Enter) and --keys ESCAPED (Python escapes, no Enter)
inject keystrokes in order, 1 s apart, for scripted tests. Standard
library only (termios/pty/select), so no pyserial dependency.
"""

from __future__ import annotations

import argparse
import fcntl
import os
import pty
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


def spawn_shell(rows: int, cols: int, shell: str) -> tuple[int, int]:
    pid, master = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM="xterm-256color", CLICOLOR="1")
        env.pop("COLUMNS", None)
        env.pop("LINES", None)
        os.execvpe(shell, [shell, "-i"], env)
    winsize = struct.pack("HHHH", rows, cols, 0, 0)
    fcntl.ioctl(master, termios.TIOCSWINSZ, winsize)
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
    pid, master = spawn_shell(args.rows, args.cols, args.shell)
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
                    write_all(master, data)
                else:  # our stdin: the keyboard
                    write_all(key_sink, data)
    finally:
        if saved is not None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, saved)
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
