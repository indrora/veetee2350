#!/usr/bin/env python3
"""Drive the VeeTee from a script, through its automation HID (debug builds).

    uv run sendkeys.py [--line PORT] [--log] STEP ...

    uv run sendkeys.py 'key:ctrl+F3' 'key:enter' 'wait:set-up' 'key:F3'
    uv run sendkeys.py 'k:ls -G\\r' 'sleep:1'
    uv run sendkeys.py 'hold:6' 'wait:Factory defaults saved'
    uv run sendkeys.py --log            # just show the debug text

Steps run in order:

    k:TEXT        what an xterm sends for keys, Python escapes
                  ("k:\\x1b[1;5R" is Ctrl+F3), as the old keyboard port took
    key:NAME      press and let go of a key on the virtual USB keyboard:
                  "key:ctrl+F3", "key:enter", "key:right", "key:shift+a"
    keys:TEXT     type TEXT key by key on the virtual USB keyboard
    b:down, b:up  BOOTSEL; "tap" is a short press, "hold:N" holds N seconds
    l:TEXT        write TEXT (Python escapes) to the line port (--line)
    sleep:N       wait N seconds
    wait:REGEX[@SECONDS]  wait for a debug line matching REGEX (default 5 s)
    rep:N:STEP    STEP N times

--log prints every debug line as it arrives, and keeps reading after the
steps until Ctrl+C. Release builds have no automation HID; flash
env:pizero-debug (or any debug env) first. Mirrors tools/vthid.py and
include/automation_hid.h.
"""
import argparse
import os
import sys
import time
import tty

from vthid import VeeTee


def decode(text):
    return text.encode().decode("unicode_escape").encode("latin-1")


def run(step, veetee, line_fd, echo):
    kind, _, rest = step.partition(":")
    if kind == "sleep":
        time.sleep(float(rest))
    elif kind == "rep":
        count, _, inner = rest.partition(":")
        for _ in range(int(count)):
            run(inner, veetee, line_fd, echo)
            time.sleep(0.15)
    elif kind == "k":
        veetee.type(decode(rest))
    elif kind == "key":
        veetee.press(rest)
    elif kind == "keys":
        veetee.keys(decode(rest).decode("latin-1"))
    elif kind == "b":
        veetee.bootsel(rest == "down")
    elif kind == "tap":
        veetee.hold_bootsel(0.2)
    elif kind == "hold":
        veetee.hold_bootsel(float(rest))
    elif kind == "wait":
        pattern, _, timeout = rest.rpartition("@")
        if not pattern:
            pattern, timeout = rest, "5"
        veetee.wait_for(pattern, float(timeout), echo)
    elif kind == "l":
        if line_fd is None:
            raise SystemExit("l: steps need --line PORT")
        os.write(line_fd, decode(rest))
    else:
        raise SystemExit(f"bad step {step!r}")
    for line in veetee.lines(0):
        if echo:
            echo(line)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--line", help="the VeeTee line port, for l: steps")
    parser.add_argument("--log", action="store_true",
                        help="print the debug text (and keep reading)")
    parser.add_argument("steps", nargs="*")
    args = parser.parse_args()

    line_fd = None
    if args.line:
        line_fd = os.open(args.line, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(line_fd)
    echo = (lambda line: print(line, flush=True)) if args.log else None
    with VeeTee() as veetee:
        for step in args.steps:
            run(step, veetee, line_fd, echo)
        try:
            while args.log:
                for line in veetee.lines(0.5):
                    echo(line)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    sys.exit(main())
