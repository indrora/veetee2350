#!/usr/bin/env python3
"""A poor man's getty: a shell on a serial port, for the VeeTee's UART.

    uv run getty.py PORT [--baud 115200] [--format 8N1] [--rtscts]
                    [--shell /bin/zsh] [--once] [--log FILE]

    uv run getty.py /dev/tty.usbserial-BG040N11 --baud 9600 --format 7E1

It sets the port up the way the VeeTee's SET-UP > Communications page
says (speed, data bits, parity, stop bits, RTS/CTS), starts a login-style
shell in a pseudo-terminal, and copies bytes both ways until the shell
exits. Then, like a getty, it starts a fresh one (--once: it stops).

Window size: the same trick as hostshell.py. It turns on the VeeTee's
in-band resize reports (DECSET 2048); the first report sizes the pty,
later ones (font or size changed in SET-UP) are taken out of the stream
and applied with TIOCSWINSZ, so the shell gets SIGWINCH.

Signals, as a getty's would be handled:

    SIGTERM, SIGINT  hang up: SIGHUP to the shell's process group, a
                     moment to go, then SIGKILL; restore the port; exit 0
    SIGHUP           the same: our "line" was dropped
    shell exits      restart it (unless --once), with a line of
                     "-- session ended --" on the terminal
    port vanishes    (adapter unplugged) hang up the shell, exit 1

--log FILE appends what goes each way, escaped, for working out which
end is quiet: lines start "<<" (from the VeeTee) or ">>" (to it). If
nothing ever arrives from the VeeTee, the TX/RX wires are likely
swapped: the adapter's RX goes to header pin 8, its TX to pin 10.

Standard library only (termios/pty/select).
"""

from __future__ import annotations

import argparse
import errno
import os
import pty
import select
import signal
import sys
import termios
import time
import tty

from hostshell import SizeTracker, set_winsize, write_all

SPEEDS = {300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200}
DATA_BITS = {5: termios.CS5, 6: termios.CS6, 7: termios.CS7, 8: termios.CS8}
# macOS calls hardware flow control CCTS_OFLOW | CRTS_IFLOW; Linux CRTSCTS.
RTSCTS = getattr(termios, "CRTSCTS", 0) or (
    getattr(termios, "CCTS_OFLOW", 0) | getattr(termios, "CRTS_IFLOW", 0))


def parse_format(text: str) -> tuple[int, str, int]:
    """'8N1' -> (8, 'N', 1)."""
    text = text.upper()
    if len(text) != 3 or not text[0].isdigit() or text[1] not in "NEO" \
            or text[2] not in "12" or int(text[0]) not in DATA_BITS:
        raise argparse.ArgumentTypeError(
            f"format like 8N1 / 7E1 / 8O2, not {text!r}")
    return int(text[0]), text[1], int(text[2])


def open_port(path: str, baud: int, fmt: tuple[int, str, int],
              rtscts: bool) -> tuple[int, list]:
    """Open and set up the serial port; returns (fd, its old attributes)."""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    saved = termios.tcgetattr(fd)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    bits, parity, stop = fmt
    cflag = attrs[2] & ~(termios.CSIZE | termios.PARENB | termios.PARODD
                         | termios.CSTOPB | RTSCTS)
    cflag |= DATA_BITS[bits] | termios.CLOCAL | termios.CREAD
    if parity != "N":
        cflag |= termios.PARENB
        if parity == "O":
            cflag |= termios.PARODD
    if stop == 2:
        cflag |= termios.CSTOPB
    if rtscts:
        cflag |= RTSCTS
    attrs[2] = cflag
    speed = getattr(termios, f"B{baud}")
    attrs[4] = attrs[5] = speed      # ispeed, ospeed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd, saved


def spawn_shell(shell: str, rows: int, cols: int) -> tuple[int, int]:
    """A login-style shell in a new pty (it gets its own session)."""
    pid, master = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM="xterm-256color", CLICOLOR="1")
        env.pop("COLUMNS", None)
        env.pop("LINES", None)
        os.chdir(os.path.expanduser("~"))
        # argv[0] with a leading '-' is how login asks for a login shell.
        os.execvpe(shell, ["-" + os.path.basename(shell)], env)
    set_winsize(master, rows, cols)
    return pid, master


def reaped(pid: int, within: float) -> bool:
    """Wait up to `within` seconds for `pid` to finish; True if it did."""
    deadline = time.monotonic() + within
    while True:
        try:
            done, _ = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            return True
        if done:
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.05)


def hang_up(pid: int, master: int | None, grace: float = 1.0) -> None:
    """Hang up the shell's session the way a modem drop would.

    Close our end of the pty first. A process exiting on macOS waits for
    its terminal's output to drain; with nobody reading the master, the
    shell hangs half-exited (state E) and no signal shifts it. Closing
    the master is the hangup itself, too: the slave side gets EIO/SIGHUP.
    """
    if master is not None:
        try:
            os.close(master)
        except OSError:
            pass
    for sig in (signal.SIGHUP, signal.SIGCONT):
        try:
            os.killpg(pid, sig)
        except ProcessLookupError:
            return
    if reaped(pid, grace):
        return
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    if not reaped(pid, grace):
        print(f"getty: shell {pid} won't go; leaving it", file=sys.stderr)


class Stop(Exception):
    """Raised from the signal handler to unwind the copy loop."""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--format", type=parse_format, default="8N1",
                        help="data bits, parity (N/E/O), stop bits: 8N1")
    parser.add_argument("--rtscts", action="store_true",
                        help="hardware flow control")
    parser.add_argument("--shell", default=os.environ.get("SHELL", "/bin/sh"))
    parser.add_argument("--once", action="store_true",
                        help="stop when the shell exits, don't respawn")
    parser.add_argument("--rows", type=int, default=24,
                        help="size if the terminal never reports one")
    parser.add_argument("--cols", type=int, default=80)
    parser.add_argument("--log", help="append traffic, escaped, to FILE")
    args = parser.parse_args()
    if isinstance(args.format, str):
        args.format = parse_format(args.format)
    if args.baud not in SPEEDS:
        parser.error(f"--baud: one of {sorted(SPEEDS)} (the VeeTee's list)")

    log = open(args.log, "a", buffering=1) if args.log else None

    def note(direction: str, data: bytes) -> None:
        if log:
            log.write(f"{direction} {data!r}\n")

    def stop(signum, _frame):
        raise Stop(signal.Signals(signum).name)

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, stop)

    fd, saved = open_port(args.port, args.baud, args.format, args.rtscts)
    bits, parity, stop_bits = args.format
    print(f"getty: {args.port} {args.baud} {bits}{parity}{stop_bits}"
          f"{' RTS/CTS' if args.rtscts else ''}, shell {args.shell}",
          file=sys.stderr)
    tracker = SizeTracker()
    status = 0
    pid = master = None
    try:
        write_all(fd, b"\x1b[?2048h")       # resize reports on
        while True:
            # Size from the first report if it comes quickly.
            deadline = time.monotonic() + 1.0
            while tracker.size is None and time.monotonic() < deadline:
                ready, _, _ = select.select([fd], [], [], 0.1)
                if ready:
                    data = os.read(fd, 4096)
                    note("<<", data)
                    tracker.feed(data)
            rows, cols = tracker.size or (args.rows, args.cols)
            pid, master = spawn_shell(args.shell, rows, cols)
            applied = (rows, cols)
            print(f"getty: shell {pid} at {cols}x{rows}", file=sys.stderr)
            while True:
                ready, _, _ = select.select([fd, master], [], [])
                if fd in ready:
                    try:
                        data = os.read(fd, 4096)
                    except OSError as error:
                        if error.errno in (errno.ENXIO, errno.EIO,
                                           errno.ENODEV):
                            print("getty: port gone", file=sys.stderr)
                            status = 1
                            raise Stop("port gone") from error
                        raise
                    note("<<", data)
                    data = tracker.feed(data)
                    if tracker.size and tracker.size != applied:
                        applied = tracker.size
                        set_winsize(master, *applied)
                    if data:
                        write_all(master, data)
                if master in ready:
                    try:
                        data = os.read(master, 4096)
                    except OSError:         # EIO: the shell has gone
                        data = b""
                    if not data:
                        break
                    note(">>", data)
                    write_all(fd, data)
            os.close(master)
            master = None
            reaped(pid, 2.0)
            pid = None
            print("getty: shell exited", file=sys.stderr)
            if args.once:
                break
            write_all(fd, b"\r\n-- session ended --\r\n")
    except Stop as why:
        print(f"getty: {why}: hanging up", file=sys.stderr)
    finally:
        # No more signals mid-cleanup: a second Ctrl+C mustn't leave the
        # shell or the port half-handled.
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(sig, signal.SIG_IGN)
        if pid is not None:
            hang_up(pid, master)
        elif master is not None:
            os.close(master)
        try:
            os.write(fd, b"\x1b[?2048l")    # resize reports off
        except OSError:
            pass                            # full or gone: don't wait on it
        try:
            # TCSANOW, not TCSADRAIN: with the far end stalled (flow
            # control, a crossed wire) draining could wait for ever.
            termios.tcflush(fd, termios.TCOFLUSH)
            termios.tcsetattr(fd, termios.TCSANOW, saved)
        except OSError:
            pass
        os.close(fd)
        if log:
            log.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
