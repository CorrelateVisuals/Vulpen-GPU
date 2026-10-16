#!/usr/bin/env python3
"""A person types at the terminal the CLI runs at, which takes each key as it comes:
Tab completes hel to help, up recalls the line sent before, left moves the caret, so
lp, then he before it, sends help, and control+d on an empty line ends the run, which
must exit 0. The CLI runs in a pseudo-terminal under a shell, which then prints the
terminal's modes: it must have it back as it was, echoing and taking lines, as a run
that leaves it taking keys leaves a shell no one can type at (A03).

Usage: python3 src/tools/tests/keys.py VULPEN
"""
import os
import pty
import re
import select
import sys
import time
from pathlib import Path

from harness import environment, problems

CLI = Path(__file__).resolve().parents[2] / "runtime" / "recipes" / "cli" / "view.vlp"
HELP = b"lists every command"  # what help answers, once a line runs it
# Each with how many times help has answered once it ran: Tab, up, then left.
TYPED = [(b"hel\t\r", 1), (b"\x1b[A\r", 2), (b"lp\x1b[D\x1b[Dhe\r", 3)]
END = b"\x04"  # control+d
SHELL = '"$0" "$1"; echo "vulpen exited $?"; stty -a'
EXITED = re.compile(r"vulpen exited (\d+)")
MODES = ("icanon", "echo")  # stty -a names a mode that is off with a - before it
TIMEOUT = 60  # seconds; a sanitizer build starts in a few


def main() -> None:
    vulpen = sys.argv[1]
    pid, terminal = pty.fork()
    if pid == 0:
        os.execve("/bin/sh", ["sh", "-c", SHELL, vulpen, str(CLI)], environment())
    said = bytearray()

    def wait(text: bytes, count: int = 1) -> None:
        deadline = time.monotonic() + TIMEOUT
        while said.count(text) < count and time.monotonic() < deadline:
            if select.select([terminal], [], [], 0.1)[0]:
                try:
                    chunk = os.read(terminal, 4096)
                except OSError:  # the shell ended, and the terminal with it
                    return
                if not chunk:
                    return
                said.extend(chunk)

    wait(b"> ")
    for keys, count in TYPED:
        os.write(terminal, keys)
        wait(HELP, count)
    os.write(terminal, END)
    wait(b"\n", said.count(b"\n") + 1_000_000)  # all of it, until the shell ends
    os.waitpid(pid, 0)
    output = said.decode("utf-8", errors="replace")
    exited = EXITED.search(output)
    if said.count(HELP) != len(TYPED) or not exited or exited.group(1) != "0":
        sys.exit(f"keys: expected help {len(TYPED)} times and an exit of 0, got:\n{output}")
    if off := [mode for mode in MODES if not re.search(rf"(?<![-\w]){mode}(?!\w)", output)]:
        sys.exit(f"keys: expected the terminal back with {', '.join(off)} on, got:\n{output}")
    if found := problems(output):
        sys.exit("keys: " + "\n".join(found))


if __name__ == "__main__":
    main()
