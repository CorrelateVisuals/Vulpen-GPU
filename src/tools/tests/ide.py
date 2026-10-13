#!/usr/bin/env python3
"""The ide app runs in a window while typed keys drive its terminal down every path: Tab
on a word not begun and on one begun, the completions listed, the edits and the arrows,
history, a line the command port refuses, help, and the keymap's chord that clears, which
waits a frame for the keys before it. The pointer drags the dock's seam, whose param set
rebuilds the running app, and presses in the terminal. A second run edits a file in its
editor, selecting, stepping over a character past ASCII and deleting it whole, and saves
it with the keymap's chord, so the file must hold just the edits; then the keymap's chords
zoom in and out, so the font bakes its atlas larger and back, and the zoom it keeps must
be back where it started. Two more runs host the
triangle example, presented as it comes: one in edit mode, its window in the Perform panel,
and one in perform mode by the keymap's chord, its window over the whole window; a third
then presses escape three times, back to edit mode, out of the terminal's focus and out of
the run, which must end before its frames do. The last two host a copy of the wave
example, laid out from the .ini put beside it: a press on a box opens its first file in
the editor, and a drag moves the box, one on the ground the graph, as the wheel does, and
the wheel with control held zooms it. The .ini must then hold the box, the graph and the
zoom moved, and the last run must read it back without a word. Each must end with exit 0
and print no validation message or sanitizer report (RVK00, A03), so in the asan preset
it checks the memory of the keys part, the terminal, the editor, modes, the graph, their
panels, the dock, the images a hosted view's window renders into, the atlas made anew
at another size, and the rebuild. The zoom a person keeps in the library's font folder is
set aside while it runs, and put back after. Where no display exists it is skipped (V07).

Usage: python3 src/tools/tests/ide.py VULPEN
"""
import re
import shutil
import sys
import tempfile
from pathlib import Path

from harness import display, problems, run

IDE = Path(__file__).resolve().parents[2] / "recipes" / "apps" / "ide" / "view.vlp"
# The ide's font is the library's part, so it keeps its zoom in the library's folder.
ZOOMED = Path(__file__).resolve().parents[2] / "recipes/parts/font/.vulpen/font.ini"
TRIANGLE = Path(__file__).resolve().parents[2] / "examples" / "triangle"
PRESENT = [f"view load {TRIANGLE}"]  # presented as it comes
PERFORM = [*PRESENT, "input key down f2", "input key up f2"]  # the chord: mode perform
# Back to edit mode, out of the terminal's focus, and out of the run.
ESCAPE = [*PERFORM, *["input key down escape"] * 3]
RAN = re.compile(r"\{run\} ran (\d+) frames")
# Every scripted event reaches the first frame, so the presses are placed for its size.
SIZE = re.compile(r"\{swp\} presents \d+ images of (\d+)x(\d+)")
WAVE = Path(__file__).resolve().parents[2] / "examples" / "wave"
# Four times as near, the wave box at the graph's top left and probe far below it, so the
# presses find the box, and the ground beside it, in any window from 1280 pixels wide.
ARRANGED = {"pan": [0, 0], "zoom": [4], "place wave": [0, 0], "place probe": [0, 200]}
MOVED = ("pan", "zoom", "place wave")
# Enough for the refusal to reach the log, and for the list the last Tab opens to stay
# drawn.
FRAMES = 10
QUIET = 40  # the graph saves once the wheel rests for 30 frames
NOTE = "first line\nsecond — line\nthird\n"
EDITED = "first line!\nScond \n  x line\nthird\n"


def typed(width: int, height: int) -> list[str]:
    seam = round((height - 4) * 0.65) + 2  # the middle of its 4 pixels, at the dock's ratio
    return [
        "focus ide.terminal.command-line",  # first, so the keys part's param gives none
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
        f"input pointer {width // 2} {seam}",
        "input button down left",
        f"input pointer {width // 2} {height * 5 // 12}",
        "input button up left",  # one param set, which rebuilds the app
        # Where the third completion shows once listed; a script's press lands in the first
        # frame, before Tab's complete answers, so it presses the terminal alone.
        f"input pointer 70 {height - 60}",
        "input button down left",
    ]


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
        # After the write, so a press that misses in a small window changes nothing the
        # file holds.
        "input pointer 300 200",
        "input button down left",
        "input pointer 500 260",
        "input button up left",
        "input wheel 0 -1",
        "input key down control",
        "input key down =",  # the chord: zoom in, so the font bakes a larger atlas
        "input key up =",
        "input key down -",  # the chord: zoom out, a frame later, back again
        "input key up -",
        "input key up control",
    ]


def graph(wave: Path, width: int) -> list[str]:
    box = (width // 2 + 100, 90)
    ground = (width * 9 // 10, 150)  # right of the box, after it moved left
    return [
        f"view load {wave}",
        f"input pointer {box[0]} {box[1]}",
        "input button down left",
        "input button up left",  # its first file opens
        "input button down left",
        f"input pointer {box[0] - 40} {box[1] - 20}",
        "input button up left",  # the box moved, so it opens nothing
        f"input pointer {ground[0]} {ground[1]}",
        "input button down left",
        f"input pointer {ground[0] - 20} {ground[1] - 10}",
        "input button up left",
        "input wheel 0 1",
        "input key down control",
        "input wheel 0 -1",  # zoomed out, about the pointer
        "input key up control",
    ]


def numbers(text: str) -> dict[str, list[float]]:
    """Each line's key and numbers, as the graph keeps them."""
    rows = (line.split("#")[0].split("=") for line in text.splitlines())
    return {row[0].strip(): [float(number) for number in row[1].split()]
            for row in rows if len(row) == 2}


def drive(folder: Path, name: str, lines: list[str], frames: int = FRAMES) -> str:
    script = folder / f"{name}.txt"
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    code, output = run(sys.argv[1], [IDE, "--frames", frames, "--fps", 0, "--log", "info",
                                     "--source", script], headless=False)
    if code != 0 or problems(output):
        sys.exit(f"ide: expected the {name} to take every key, got exit {code}:\n{output}")
    return output


def main() -> None:
    if not display():
        print("skipped: no display to open a window on")
        return
    kept = ZOOMED.read_bytes() if ZOOMED.exists() else None
    ZOOMED.unlink(missing_ok=True)
    try:
        test()
    finally:
        if kept is None:
            ZOOMED.unlink(missing_ok=True)
            if ZOOMED.parent.exists() and not any(ZOOMED.parent.iterdir()):
                ZOOMED.parent.rmdir()
        else:
            ZOOMED.write_bytes(kept)


def test() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(temporary)
        size = SIZE.search(drive(folder, "presented view", PRESENT))
        if not size:
            sys.exit("ide: expected the log to name the window's size")
        width, height = int(size.group(1)), int(size.group(2))
        drive(folder, "terminal", typed(width, height))
        note = folder / "note.txt"
        note.write_text(NOTE, encoding="utf-8")
        drive(folder, "editor", edits(note))
        zoomed = ZOOMED.read_text(encoding="utf-8") if ZOOMED.exists() else ""
        if numbers(zoomed) != {"zoom": [1]}:
            sys.exit(f"ide: expected the font to keep zoom = 1 after zooming in and out, "
                     f"it kept\n{zoomed}")
        drive(folder, "performed view", PERFORM)
        ran = RAN.findall(drive(folder, "escapes", ESCAPE))
        if not ran or int(ran[-1]) >= FRAMES:
            sys.exit(f"ide: expected escape to end the run before {FRAMES} frames, "
                     f"it ran {ran}")
        # A copy, as the graph writes beside it; named wave, so it finds the modules the
        # build made for the example.
        wave = folder / "copy" / "wave"
        shutil.copytree(WAVE, wave)
        kept = wave / ".vulpen" / "graph.ini"
        kept.parent.mkdir()
        kept.write_text("".join(f"{key} = {' '.join(map(str, value))}\n"
                                for key, value in ARRANGED.items()), encoding="utf-8")
        said = drive(folder, "graph", graph(wave, width), QUIET)
        arranged = numbers(kept.read_text(encoding="utf-8"))
        if still := [key for key in MOVED if arranged.get(key) == ARRANGED[key]]:
            sys.exit(f"ide: expected the graph to keep {', '.join(still)} moved, it kept\n"
                     f"{kept.read_text(encoding='utf-8')}")
        said += drive(folder, "graph read back", [f"view load {wave}"])
        if kept.name in said:
            sys.exit(f"ide: expected the graph to read its .ini without a word, got:\n"
                     f"{said}")
        # As bytes, so a character split in two shows as what it left.
        if (held := note.read_bytes()) != EDITED.encode("utf-8"):
            sys.exit(f"ide: expected the editor to write {EDITED!r}, got {held!r}")


if __name__ == "__main__":
    main()
