#!/usr/bin/env python3
"""The ide app runs in a window while typed keys drive its terminal down every path: Tab
on a word not begun and on one begun, the completions listed, the edits and the arrows,
history, a line the command port refuses, help, and the keymap's chord that clears, which
waits a frame for the keys before it. The pointer drags the dock's seam, whose param set
rebuilds the running app, and presses a completion. A second run edits a file in its
editor, selecting, stepping over a character past ASCII and deleting it whole, and saves
it with the keymap's chord, so the file must hold just the edits. Two more runs host the
triangle example and present it: one in edit mode, its window in the Perform panel, and
one in perform mode by the keymap's chord, its window over the whole window. The last
hosts the wave example, whose graph the graph panel lays out, a press on a box opening its
first file in the editor, and a drag and the wheel moving the graph. Each must end with
exit 0 and print no validation message or sanitizer report (RVK00, A03), so in
the asan preset it checks the memory of the keys part, the terminal, the editor, modes,
the graph, their panels, the dock, the images a hosted view's window renders into and
the rebuild.
Where no display exists it is skipped (V07).

Usage: python3 src/tools/tests/ide.py VULPEN
"""
import sys
import tempfile
from pathlib import Path

from harness import display, problems, run

IDE = Path(__file__).resolve().parents[2] / "recipes" / "apps" / "ide" / "view.vlp"
TRIANGLE = Path(__file__).resolve().parents[2] / "examples" / "triangle"
PRESENT = [f"view load {TRIANGLE}", "present triangle"]
PERFORM = [*PRESENT, "input key down f2", "input key up f2"]  # the chord: mode perform
WAVE = Path(__file__).resolve().parents[2] / "examples" / "wave"
GRAPH = [
    f"view load {WAVE}",
    # Where a 1280 by 720 window puts them; a window of another size misses, harmlessly.
    "input pointer 680 57",  # the wave box, whose first file opens
    "input button down left",
    "input button up left",
    "input pointer 900 150",  # the ground, dragged
    "input button down left",
    "input pointer 1000 200",
    "input button up left",
    "input wheel 0 1",
]
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
NOTE = "first line\nsecond — line\nthird\n"
EDITED = "first line!\nScond \n  x line\nthird\n"


def edits(note: Path) -> list[str]:
    return [
        f"open {note}",
        "find line",  # selects it, which the end key then leaves
        "focus ide.editor.text",
        "input key down end",
        "input text !",
        "input key down down",
        "input key down home",
        "input key down shift",
        "input key down right",
        "input key down right",
        "input key up shift",
        "input text S",  # in place of se, selected
        "input key down end",
        *["input key down left"] * 5,  # over line and a blank, to just past the dash
        "input key down backspace",  # the dash, all three of its bytes
        "input key down enter",  # what follows goes down a row
        "input key down tab",
        "input text x",
        "input key down control",
        "input key down s",  # the chord: write, once the editor took the keys before it
        "input key up s",
        "input key up control",
        # After the write, so a window of another size, which puts them elsewhere, changes
        # nothing the file holds.
        "input pointer 300 200",
        "input button down left",
        "input pointer 500 260",
        "input button up left",
        "input wheel 0 -1",
    ]


def drive(folder: Path, name: str, lines: list[str]) -> None:
    script = folder / f"{name}.txt"
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    code, output = run(sys.argv[1], [IDE, "--frames", FRAMES, "--fps", 0,
                                     "--source", script], headless=False)
    if code != 0 or problems(output):
        sys.exit(f"ide: expected the {name} to take every key, got exit {code}:\n{output}")


def main() -> None:
    if not display():
        print("skipped: no display to open a window on")
        return
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(temporary)
        drive(folder, "terminal", TYPED)
        note = folder / "note.txt"
        note.write_text(NOTE, encoding="utf-8")
        drive(folder, "editor", edits(note))
        drive(folder, "presented view", PRESENT)
        drive(folder, "performed view", PERFORM)
        drive(folder, "graph", GRAPH)
        # As bytes, so a character split in two shows as what it left.
        if (held := note.read_bytes()) != EDITED.encode("utf-8"):
            sys.exit(f"ide: expected the editor to write {EDITED!r}, got {held!r}")


if __name__ == "__main__":
    main()
