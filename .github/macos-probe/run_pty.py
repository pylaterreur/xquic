#!/usr/bin/env python3
"""Run a command on a pseudo-terminal and copy its output to a log file.

Fork-only probe helper (pylaterreur/xquic, try/macos-unittests). On a
terminal the command's stdout is line-buffered, so a crash keeps every line
printed before it. After --timeout seconds the command is sampled with
sample(1), then killed.

Exit status: the command's own; 128 + N when signal N killed it; 124 on
timeout.
"""

import argparse
import os
import select
import signal
import subprocess
import sys
import termios
import time


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--sample", help="file for sample(1) on timeout")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("cmd", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd

    master, slave = os.openpty()
    attrs = termios.tcgetattr(slave)
    attrs[1] &= ~termios.ONLCR  # keep "\n" line ends in the log
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=slave,
        stderr=slave,
        start_new_session=True,
    )
    os.close(slave)

    deadline = time.monotonic() + args.timeout
    timed_out = False
    with open(args.log, "wb") as log:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                break
            ready, _, _ = select.select([master], [], [], min(remaining, 5.0))
            if not ready:
                continue
            try:
                data = os.read(master, 65536)
            except OSError:  # EIO once no process holds the terminal
                data = b""
            if not data:
                break
            log.write(data)
            log.flush()
            if not args.quiet:
                sys.stdout.buffer.write(data)
                sys.stdout.flush()

        if timed_out:
            note = (
                f"\n*** timeout after {args.timeout:.0f} s: sampling and "
                f"killing pid {proc.pid}\n"
            ).encode()
            log.write(note)
            sys.stdout.buffer.write(note)
            sys.stdout.flush()
            if args.sample:
                subprocess.run(
                    ["sudo", "-n", "sample", str(proc.pid), "10", "-file",
                     args.sample],
                    check=False,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            os.killpg(proc.pid, signal.SIGKILL)

    rc = proc.wait()
    os.close(master)
    if timed_out:
        return 124
    if rc < 0:
        print(f"*** killed by signal {-rc} ({signal.Signals(-rc).name})")
        return 128 - rc
    return rc


if __name__ == "__main__":
    sys.exit(main())
