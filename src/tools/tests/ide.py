#!/usr/bin/env python3
"""The ide app runs in a window while typed keys drive its terminal down every path: Tab
on a word not begun and on one begun, the completions listed, the edits and the arrows,
history, a line the command port refuses, help, and the keymap's chord that clears, which
waits a frame for the keys before it. The pointer drags the dock's seam, whose param set
rebuilds the running app, and presses a completion. It must end with exit 0 and print no
validation message or sanitizer report (RVK00, A03), so in the asan preset it checks the
memory of the keys part, the terminal, its panel, the dock and the rebuild. Where no
display exists it is skipped (V07).

Usage: python3 src/tools/tests/ide.py VULPEN
"""
import sys
import tempfile
from pathlib import Path

from harness import display, problems, run

IDE = Path(__file__).resolve().parents[2] / "recipes" / "apps" / "ide" / "view.vlp"
# Enough for the refusal to reach the log, and for the list the last Tab opens to stay
# drawn.
FRAMES = 10
TYPED = [
    "focus ide.terminal.command-line",  # a line first, so the keys part's param gives none
    "input key down tab",  # a word not begun: every command's first word
    "input text no",
    "input key down tab",  # one candidate: node, finished
    "input key down tab",  # several: listed
    "input key down home",
    "input key down delete",
    "input key down end",
    "input key down backspace",
    "input key down left",
    "input key down right",
    "input key down enter",  # ode, which the command port refuses
    "input key down up",
    "input key down down",
    "input text help",
    "input key down enter",
    "input key down control",
    "input key down l",  # the chord: clear, once the terminal took help's keys
    "input key up l",
    "input key up control",
    "input text node",
    "input key down tab",
    "input key down tab",  # node's words listed, from the word's column
    # Where a 1280 by 720 window puts them; a window of another size misses, harmlessly.
    "input pointer 640 466",  # the seam, at 0.65 of the height
    "input button down left",
    "input pointer 640 300",
    "input button up left",  # one param set, which rebuilds the app
    "input pointer 70 660",  # remove, the third word listed
    "input button down left",
]


def main() -> None:
    if not display():
        print("skipped: no display to open a window on")
        return
    with tempfile.TemporaryDirectory() as temporary:
        script = Path(temporary) / "typed.txt"
        script.write_text("\n".join(TYPED) + "\n", encoding="utf-8")
        code, output = run(sys.argv[1], [IDE, "--frames", FRAMES, "--fps", 0,
                                         "--source", script], headless=False)
    if code != 0 or problems(output):
        sys.exit(f"ide: expected the terminal to take every key, got exit {code}:\n{output}")


if __name__ == "__main__":
    main()
