#!/usr/bin/env python3
"""Show what a command's output SHOULD look like on an 80x30 terminal.

    uv run refscreen.py [--rows 30] [--cols 80] [--wait 8] -- fastfetch

Runs the command in a pty of the VeeTee's size with TERM=xterm-256color,
feeds its output to pyte (an independent terminal emulator) and prints
the final screen as text. Compare against a Genki capture of the same
command run through tools/hostshell.py: matching layouts mean VeeTee
behaves like a reference emulator, and differences point at bugs (or at
pyte's own gaps; it doesn't do everything xterm does).
"""

from __future__ import annotations

import argparse
import fcntl
import os
import pty
import select
import struct
import termios
import time

import pyte


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rows", type=int, default=30)
    parser.add_argument("--cols", type=int, default=80)
    parser.add_argument("--wait", type=float, default=8.0)
    parser.add_argument("command", nargs="+")
    args = parser.parse_args()

    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM="xterm-256color")
        os.execvpe(args.command[0], args.command, env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", args.rows, args.cols, 0, 0))

    output = b""
    deadline = time.monotonic() + args.wait
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break  # child exited and the pty closed
        if not chunk:
            break
        output += chunk

    screen = pyte.Screen(args.cols, args.rows)
    pyte.ByteStream(screen).feed(output)
    for row, line in enumerate(screen.display):
        print(f"{row:2}|{line.rstrip()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
