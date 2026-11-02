#!/usr/bin/env python3
"""Run a shell on the Mac whose terminal is the VeeTee 2350.

    uv run hostshell.py /dev/cu.usbmodemXXXX [--type 'ls -G' ...] [--exit-after 20]

The shell runs in a pseudo-terminal sized to the VeeTee grid (80x30 by
default) with TERM=xterm-256color. Bytes flow:

    shell (pty) --> serial --> VeeTee screen
    VeeTee replies (DA, DSR, ...) --> serial --> shell
    this script's stdin (your keyboard) --> shell

so it is a complete terminal session, with the Mac's keyboard standing in
until the USB HID keyboard (Goal 3) exists. `--type` injects keystrokes
for scripted tests. Standard library only (termios/pty/select), so no
pyserial dependency.
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
    parser.add_argument(
        "--type",
        action="append",
        default=[],
        help="line to type into the shell (repeatable; sent 1 s apart)",
    )
    parser.add_argument("--exit-after", type=float, default=0.0)
    args = parser.parse_args()

    serial_fd = open_serial(args.port)
    pid, master = spawn_shell(args.rows, args.cols, args.shell)
    interactive = sys.stdin.isatty() and not args.type
    saved = None
    if interactive:
        saved = termios.tcgetattr(sys.stdin.fileno())
        tty.setraw(sys.stdin.fileno())

    pending = list(args.type)
    next_type = time.monotonic() + 1.5
    deadline = time.monotonic() + args.exit_after if args.exit_after else None
    watched = [master, serial_fd] + ([sys.stdin.fileno()] if interactive else [])

    try:
        while True:
            now = time.monotonic()
            if pending and now >= next_type:
                write_all(master, pending.pop(0).encode() + b"\r")
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
                else:
                    write_all(master, data)
    finally:
        if saved is not None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, saved)
        os.close(serial_fd)
        try:
            os.kill(pid, 9)
        except ProcessLookupError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
