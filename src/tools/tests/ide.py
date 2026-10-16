#!/usr/bin/env python3
"""The ide app runs in a window, hosted by the cli as ./run.sh --ide runs it, while typed
keys drive its terminal down every path: Tab on a word not begun and on one begun, the
completions listed, the edits and the arrows, history, a line the command port refuses,
help, which the cli answers, and control with l, which sends clear from the terminal, so
what the cli answers clears it. The pointer drags the dock's seam, whose param set
rebuilds the running app, and drags over the rows the terminal shows, selecting them; no
run copies or pastes, as that would write over the clipboard of whoever runs it. A second
run edits a file in its editor, selecting, stepping over a character past ASCII and
deleting it whole, and saves it with the keymap's chord, so the file must hold just the
edits; then the keymap's chords zoom in and out, so the font bakes its atlas larger and
back, and the zoom it keeps must be back where it started; then it types once more and
escapes until modes sends quit, which the editor's hold must refuse, naming the file, so
the run runs all its frames and the file keeps only what was written. Two more runs host
the triangle example, presented as it comes: one in edit mode, its window in the Perform
panel, where the cli takes ls on standard input, the terminal vulpen was started from, and
prints what it answers for the triangle, the view both terminals follow, and completes a
path begun to the one folder it fits from the folders the file port lists, and one in
perform mode by the keymap's chord in the first frame, its window over the whole window,
which nothing may refuse; a third then presses escape three times, back to edit mode, out
of the terminal's focus and out of the run, which must end before its frames do; every
other run must run all of them, so the end of standard input, which every run finds at
once, ends none. The last two host a copy of the wave example, its boxes placed by the
.ini put beside it where both show whole, so the graph starts at home unmoved, with
Wave.comp open in the editor: a press on a box's name selects it, a drag moves the box
selected, a drag on the ground lets go of the selection and moves the graph, shift with a
press selects the box again, the wheel zooms the graph, and shift with a drag on the
ground draws a select box over the box; the wheel then zooms the Perform panel's view,
which a drag moves, and h, with the focus the press gave the panel, brings home. The .ini
must then hold the box moved, and the color it gave dispatches. The last run hosts the
triangle after the copy, which it keeps presented, and the copy hosts a view of its own,
inner; it must read the .ini back without a word, select probe and inner as a script does,
the graph following the copy presented, not the triangle hosted last, show both with info
under their headers, and delete them with the key, so the .ini no longer places probe and
inner is no longer hosted, with this file open in the editor, in Python's colors; ls on
standard input must answer for the copy, as the terminal follows it too. Each must end
with exit 0 and print no validation message or sanitizer report (RVK00, A03), so in the
asan preset it checks the memory of the keys part, the terminal, the editor, modes, the
graph, their panels, the dock, the images a hosted view's window renders into, the atlas
made anew at another size, and the rebuild.
The terminal run also scrolls the rows of the app's start with the wheel, a fifth of a
step too, and drags their scroll bar to the oldest, before help, sent, shows the newest
again, and scrolls them across with shift and the wheel and back with the wheel turned
sideways; the editor run scrolls the note past its end with the wheel, and back with a
drag on its scroll bar. In both, three presses in a row select a word, then its row.
The zoom a person keeps in the library's font folder is set aside while it runs, and put
back after. Where no display exists it is skipped (V07).

Usage: python3 src/tools/tests/ide.py VULPEN
"""
import re
import shutil
import sys
import tempfile
from pathlib import Path

from harness import display, problems, run

RUNTIME = Path(__file__).resolve().parents[2] / "runtime"
CLI = RUNTIME / "recipes" / "cli" / "view.vlp"
IDE = RUNTIME / "recipes" / "ide" / "view.vlp"
# The ide's font is the library's part, so it keeps its zoom in the library's folder.
ZOOMED = RUNTIME / "recipes/library/parts/font/.vulpen/font.ini"
TRIANGLE = RUNTIME.parents[1] / "projects" / "triangle"
PRESENT = [f"view load {TRIANGLE}"]  # presented as it comes
PERFORM = [*PRESENT, "input key down f1", "input key up f1"]  # the chord: mode toggle
# Back to edit mode, out of the terminal's focus, and out of the run.
ESCAPE = [*PERFORM, *["input key down escape"] * 3]
RAN = re.compile(r"\{run\} ran (\d+) frames")
LISTED = re.compile(r"^  Triangle\.vert$", re.M)  # a row ls answers for the triangle
# The folder a path begun completes to, which complete answers alone on its line.
COMPLETE = f"complete view load {TRIANGLE.parent}/tri"
COMPLETED = f"{TRIANGLE}/"
# Every scripted event reaches the first frame, so the presses are placed for its size.
SIZE = re.compile(r"\{swp\} presents \d+ images of (\d+)x(\d+)")
WAVE = RUNTIME.parents[1] / "projects" / "wave"
# The wave box at the graph's top left and probe's beside it, so both show whole in any
# window from 1280 pixels wide, and the graph starts unmoved at zoom 1.
ARRANGED = {"place wave": [0, 0], "place probe": [200, 0]}
MOVED = ("place wave",)
COLORED = "color dispatch = number"  # a person's line, which the graph's saves keep
# The graph's area starts below its panel's tab strip, a text row and 4 pixels either
# side, at the font's height; a box's name is a row a pad below its top.
CELL = 18
CELL_WIDE = 8  # a cell's width at that height, as Roboto Mono's advance makes it
STRIP = CELL + 2 * 4
NAME = STRIP + 4 + CELL // 2
# The close mark of the editor's first tab, the window's top left: a cell in, the label,
# a blank, then the mark, a cell wide, as wave/Wave.comp's.
MARK = (CELL_WIDE * (1 + len("wave/Wave.comp") + 1) + CELL_WIDE // 2, STRIP // 2)
INFO = ['[node "probe"]', '[view "inner"]']  # info's headers for the boxes selected
DELETED = "place probe"
NESTED = '\n[view "inner"]\n'  # a view the copy hosts, empty until a save writes it
CLOSED = "child inner: no longer hosted"
FOLLOWED = re.compile(r"^  Wave\.comp$", re.M)  # a row ls answers for the copy
# Enough for the refusal to reach the log, and for the list the last Tab opens to stay
# drawn.
FRAMES = 10
NOTE = "first line\nsecond — line\nthird\n"
EDITED = "first line!\nScond \n  x line\nthird\n"
# What quit answers while the editor holds the note, named by its folder and name.
REFUSED = "quit would lose what nodes hold"
HELD = re.compile(r"ide\.editor\.text: \S+/note\.txt\* has changes")


def clicks(x: int, y: int) -> list[str]:
    """A press, a double click's second and a third, at a place, as a person's in a row."""
    return [f"input pointer {x} {y}", "input button down left", "input button up left",
            "input button down left 2", "input button up left",
            "input button down left 3", "input button up left"]


def typed(width: int, height: int) -> list[str]:
    seam = round((height - 4) * 0.65) + 2  # the middle of its 4 pixels, at the dock's ratio
    return [
        "focus ide.terminal.command-line",  # first, so the keys part's param gives none
        "input key down tab",  # a word not begun: every command's first word
        "input text no",
        "input key down tab",  # one candidate: node, finished
        "input key down tab",  # several: printed among the rows
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
        "input key down l",  # clear, sent by the terminal, which the cli answers
        "input key up l",
        "input key up control",
        "input text node",
        "input key down tab",
        "input key down tab",  # node's words printed
        f"input pointer {width // 2} {seam}",
        "input button down left",
        f"input pointer {width // 2} {height * 5 // 12}",
        "input button up left",  # one param set, which rebuilds the app
        # Over the rows the terminal shows, Tab's lists among them, selecting them.
        f"input pointer 70 {height - 60}",
        "input button down left",
        f"input pointer 300 {height - 100}",
        "input button up left",
        # The log of the app's start fills more rows than show, so they scroll.
        "input wheel 0 1",
        "input wheel 0 0.2",
        f"input pointer {width - 4} {height - 100}",  # the scroll bar, at the right edge
        "input button down left",
        f"input pointer {width - 4} {height // 2}",
        "input button up left",
        *clicks(70, height - 60),  # a word of the oldest rows, and then its row
        "input key down shift",
        "input wheel 0 -1",  # across, as the log's rows are wider than the terminal
        "input key up shift",
        "input wheel 1 0",  # and back, turned sideways, as a touchpad turns it
    ]


def edits(width: int) -> list[str]:
    bar = round((width - 4) * 0.5) - 4  # the editor's scroll bar, left of the dock's seam
    return [
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
        "input wheel 0 -1",  # past the note's end, so the bar shows
        f"input pointer {bar} 260",
        "input button down left",
        f"input pointer {bar} 100",
        "input button up left",
        *clicks(100, 70),  # a word of the note's top rows, then its row
        "input key down control",
        "input key down =",  # the chord: zoom in, so the font bakes a larger atlas
        "input key up =",
        "input key down -",  # the chord: zoom out, a frame later, back again
        "input key up -",
        "input key up control",
        "input text ?",  # a change after the write, so the editor holds the run
        "input key down escape",  # the editor lets go of the focus
        "input key down escape",  # so modes sends quit, which the hold refuses
    ]


def graph(wave: Path, width: int, height: int) -> list[str]:
    left = round((width - 4) * 0.5) + 4  # past the top row's seam: the graph's area
    box = (left + 40, NAME)
    moved = (box[0] + 30, box[1])  # the box dragged right
    ground = (width - 40, 150)  # below and right of both boxes
    panned = (moved[0] + 20, moved[1] + 10)  # where it shows once the graph moved
    # Two thirds down the Perform panel, under the graph's, in the dock's top row.
    top = round((height - 4) * 0.65)
    below = round((top - 4) * 0.5) + 4
    perform = (width * 3 // 4, below + (top - below) * 2 // 3)
    return [
        f"view load {wave}",
        f"open {wave / 'wave' / 'Wave.comp'}",  # GLSL in the editor, in color
        f"input pointer {box[0]} {box[1]}",
        "input button down left",
        "input button up left",  # on its name's row: selected, and nothing opens
        "input button down left",
        f"input pointer {moved[0]} {moved[1]}",
        "input button up left",  # the box selected moved
        f"input pointer {ground[0]} {ground[1]}",
        "input button down left",  # the selection goes
        f"input pointer {ground[0] + 20} {ground[1] + 10}",
        "input button up left",  # and the graph moved
        f"input pointer {panned[0]} {panned[1]}",
        "input key down shift",
        "input button down left",
        "input button up left",  # shift adds the box to the selection
        "input key up shift",
        "input wheel 0 -1",  # zoomed out, about the pointer
        "input key down shift",
        f"input pointer {ground[0]} {ground[1]}",
        "input button down left",  # a select box, not a move
        f"input pointer {left + 20} {STRIP + 4}",
        "input button up left",  # what it touches joins the selection
        "input key up shift",
        f"input pointer {perform[0]} {perform[1]}",
        "input wheel 0 3",  # the view presented zoomed in, so the panel cuts it
        "input button down left",
        f"input pointer {perform[0] + 60} {perform[1] + 40}",
        "input button up left",  # and moved
        "input key down h",  # home again, as the press gave the panel the focus
        f"input pointer {MARK[0]} {MARK[1]}",
        "input button down left",  # on Wave.comp's close mark, which closes it, unchanged
        "input button up left",
    ]


def deletes(wave: Path) -> list[str]:
    return [
        f"view load {wave}",
        f"view load {TRIANGLE}",  # hosted last, presented as it comes
        "view switch wave",  # so the panels follow the copy, not the view hosted last
        f"open {Path(__file__).resolve()}",  # Python in color, a docstring over many rows
        "wave: select probe inner",  # as a script selects, before the first frame
        "wave: info",  # the node and the view selected, under their headers
        "focus ide.graph.graph",
        "input key down delete",  # probe goes, and the connection it reads; inner closes
    ]


def numbers(text: str) -> dict[str, list[float]]:
    """Each line's key and numbers, as the graph and the font keep them; a line of a word,
    as a color's, is left out."""
    rows = (line.split("#")[0].split("=") for line in text.splitlines())
    return {row[0].strip(): [float(number) for number in row[1].split()]
            for row in rows if len(row) == 2
            and all(re.fullmatch(r"-?[\d.]+", number) for number in row[1].split())}


def drive(folder: Path, name: str, lines: list[str], frames: int = FRAMES,
          typed: str = "", whole: bool = True) -> str:
    script = folder / f"{name}.txt"
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    code, output = run(sys.argv[1], [CLI, IDE, "--frames", frames, "--fps", 0, "--log",
                                     "info", "--source", script], headless=False, typed=typed)
    if code != 0 or problems(output):
        sys.exit(f"ide: expected the {name} to take every key, got exit {code}:\n{output}")
    if whole and RAN.findall(output) != [str(frames)]:
        sys.exit(f"ide: expected the {name} to run {frames} frames, got:\n{output}")
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
        presented = drive(folder, "presented view", [*PRESENT, COMPLETE], typed="ls\n")
        if COMPLETED not in presented.splitlines():
            sys.exit(f"ide: expected {COMPLETE} to answer {COMPLETED}, got:\n{presented}")
        if not (size := SIZE.search(presented)) or not LISTED.search(presented):
            sys.exit(f"ide: expected the log to name the window's size, and ls typed on "
                     f"standard input to print {LISTED.pattern}, got:\n{presented}")
        width, height = int(size.group(1)), int(size.group(2))
        drive(folder, "terminal", typed(width, height))
        note = folder / "note.txt"
        note.write_text(NOTE, encoding="utf-8")
        # open names the note in the editor's file param, which opens it in the first
        # frame, so the edits come in a script that standard input sources then.
        later = folder / "edits.txt"
        later.write_text("\n".join(edits(width)) + "\n", encoding="utf-8")
        edited = drive(folder, "editor", [f"open {note}"], typed=f"source {later}\n")
        if REFUSED not in edited or not HELD.search(edited):
            sys.exit(f"ide: expected quit to be refused while the note has changes, "
                     f"naming it, got:\n{edited}")
        zoomed = ZOOMED.read_text(encoding="utf-8") if ZOOMED.exists() else ""
        if numbers(zoomed) != {"zoom": [1]}:
            sys.exit(f"ide: expected the font to keep zoom = 1 after zooming in and out, "
                     f"it kept\n{zoomed}")
        performed = drive(folder, "performed view", PERFORM)
        if "{!!!}" in performed:
            sys.exit(f"ide: expected the chord to give the triangle the window, got:\n"
                     f"{performed}")
        ran = RAN.findall(drive(folder, "escapes", ESCAPE, whole=False))
        if not ran or int(ran[-1]) >= FRAMES:
            sys.exit(f"ide: expected escape to end the run before {FRAMES} frames, "
                     f"it ran {ran}")
        # A copy, as the graph writes beside it; named wave, so it finds the modules the
        # build made for the example. Without the .vulpen a person's run left there, so
        # the graph reads only the layout put beside it.
        wave = folder / "copy" / "wave"
        shutil.copytree(WAVE, wave, ignore=shutil.ignore_patterns(".vulpen"))
        kept = wave / ".vulpen" / "graph.ini"
        kept.parent.mkdir()
        kept.write_text("".join(f"{key} = {' '.join(map(str, value))}\n"
                                for key, value in ARRANGED.items()) + COLORED + "\n",
                        encoding="utf-8")
        said = drive(folder, "graph", graph(wave, width, height))
        arranged = numbers(kept.read_text(encoding="utf-8"))
        if still := [key for key in MOVED if arranged.get(key) == ARRANGED[key]]:
            sys.exit(f"ide: expected the graph to keep {', '.join(still)} moved, it kept\n"
                     f"{kept.read_text(encoding='utf-8')}")
        if COLORED not in kept.read_text(encoding="utf-8").splitlines():
            sys.exit(f"ide: expected the graph's save to keep {COLORED}, it kept\n"
                     f"{kept.read_text(encoding='utf-8')}")
        manifest = wave / "view.vlp"
        nested = manifest.read_text(encoding="utf-8") + NESTED
        manifest.write_text(nested, encoding="utf-8")
        deleted = drive(folder, "graph read back", deletes(wave), typed="ls\n")
        if kept.name in said + deleted:
            sys.exit(f"ide: expected the graph to read its .ini without a word, got:\n"
                     f"{said}{deleted}")
        placed = numbers(kept.read_text(encoding="utf-8"))
        if (any(header not in deleted.splitlines() for header in INFO)
                or DELETED in placed or CLOSED not in deleted):
            sys.exit(f"ide: expected info to show probe and inner selected under {INFO}, "
                     f"and delete to take probe's place out of the .ini and close inner, "
                     f"got:\n{deleted}\n{kept.read_text(encoding='utf-8')}")
        if not FOLLOWED.search(deleted):
            sys.exit(f"ide: expected ls on standard input to answer for the copy "
                     f"presented, {FOLLOWED.pattern}, got:\n{deleted}")
        # As bytes, so a character split in two shows as what it left.
        if (held := note.read_bytes()) != EDITED.encode("utf-8"):
            sys.exit(f"ide: expected the editor to write {EDITED!r}, got {held!r}")


if __name__ == "__main__":
    main()
