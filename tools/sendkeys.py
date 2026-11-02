#!/usr/bin/env python3
"""Send a timed script of keystrokes / line data to the VeeTee.

    python3 sendkeys.py LINE_PORT KBD_PORT 'k:\\x1bOR' 'sleep:2' 'l:hello\\r\\n' ...

Steps run in order: `k:` writes to the keyboard port, `l:` to the line
port (both take Python escapes), `sleep:N` waits N seconds, `rep:N:k:...`
repeats a step N times. Used for scripted SET-UP tests and devlog
captures.
"""
import os
import sys
import time
import tty


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    return fd


def decode(text):
    return text.encode().decode("unicode_escape").encode("latin-1")


def run(step, ports):
    kind, _, rest = step.partition(":")
    if kind == "sleep":
        time.sleep(float(rest))
    elif kind == "rep":
        count, _, inner = rest.partition(":")
        for _ in range(int(count)):
            run(inner, ports)
            time.sleep(0.15)
    elif kind in ports:
        os.write(ports[kind], decode(rest))
    else:
        raise SystemExit(f"bad step {step!r}")


def main():
    ports = {"l": open_port(sys.argv[1]), "k": open_port(sys.argv[2])}
    for step in sys.argv[3:]:
        run(step, ports)


if __name__ == "__main__":
    main()
