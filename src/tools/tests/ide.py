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
.ini put beside it, which home centers in the graph's area at zoom 1, what lies outside
it placed among them, the flow layer alone showing its curves, with
Wave.comp open in the editor: a press on a box's name selects it, a drag moves the box
selected, a drag on the ground lets go of the selection and moves the graph, shift with a
press selects the box again, the wheel zooms the graph, and shift with a drag on the
ground draws a select box over the box; the wheel then zooms the Perform panel's view,
which a drag moves, and h, with the focus the press gave the panel, brings home; a double
click on it holds the frame it shows, and space lets it go and holds it again. The .ini
must then hold the box moved. The last run hosts the
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
The terminal run, with nothing presented, first sends mode perform, which must warn and
leave it in edit mode, as the run button pressed then flashes rather than switch.
The terminal run also scrolls the rows of the app's start with the wheel, a fifth of a
step too, and drags their scroll bar to the oldest, before help, sent, shows the newest
again, and scrolls them across with shift and the wheel and back with the wheel turned
sideways; the editor run scrolls the note past its end with the wheel, and back with a
drag on its scroll bar. In both, three presses in a row select a word, then its row.
A run types into a note in the editor, opens the menu of what the terminal does
anywhere, the lines a right press in the graph ends with, by its line, and presses its
write, which the terminal runs as typed, so the note must hold what was typed. Another moves the graph's panel below the Perform panel by panel
move, hides both, so their dock hides too, shows the graph's again, which the Perform
panel must not follow, moves the Perform panel into the graph's, which then holds both,
and out again, the one it shows, into a panel of its own right of the terminal, and drags
the terminal's tab strip onto the editor's right edge, so the ide's docks are edited over
and over, and must rebuild with no line refused. No node of any run may stop, as one
whose menu.ini or keymap names a line no command takes would.
The zoom a person keeps in the library's font folder, and the panels as they left them
beside the ide's manifest, are set aside while it runs, and put back after; every run
starts from the manifest's panels. Where no display exists it is skipped (V07).

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
# The ide's font is the library's part, so it keeps its zoom in the library's folder;
# its docking part keeps the panels as they were left beside the ide's manifest, which
# each run starts without, so every run starts from the manifest's.
ZOOMED = RUNTIME / "recipes/library/parts/font/.vulpen/font.ini"
LAID = RUNTIME / "recipes/ide/.vulpen/layout.log"
TRIANGLE = RUNTIME.parents[1] / "projects" / "triangle"
PRESENT = [f"view load {TRIANGLE}"]  # presented as it comes
PERFORM = [*PRESENT, "input key down f1", "input key up f1"]  # the chord: mode toggle
# Back to edit mode, out of the terminal's focus, and out of the run.
ESCAPE = [*PERFORM, *["input key down escape"] * 3]
RAN = re.compile(r"\{run\} ran (\d+) frames")
LISTED = re.compile(r"^  Triangle\.vert$", re.M)  # a row ls answers for the triangle
# The folder a path begun completes to, which complete answers alone on its line, the
# folders as they were typed.
COMPLETE = f"complete view load {TRIANGLE.parent}/tri"
COMPLETED = f"{TRIANGLE.parent}/{TRIANGLE.name}/"
# Every scripted event reaches the first frame, so the presses are placed for its size.
SIZE = re.compile(r"\{swp\} presents \d+ images of (\d+)x(\d+)")
WAVE = RUNTIME.parents[1] / "projects" / "wave"
# The wave box, probe's and the manifest's in a row, each larger than its rows need, so
# where they reach is known, and home, which centers them in the graph's area, shows them
# at zoom 1 in any window from 1280 by 720 pixels; and what lies outside, the docs the
# copy's comments cite, below the manifest's, within that reach, so it widens nothing
# home centers, though only the flow layer's curves show, as a layer hidden hides its
# curves alone.
ARRANGED = {"place wave": [0, 0], "size wave": [120, 90],
            "place probe": [200, 0], "size probe": [120, 90],
            "place /view.vlp": [400, 0], "size /view.vlp": [120, 30],
            "place outside left": [400, 40], "size outside left": [120, 50]}
REACH = (520, 90)
HOME_MARGIN = (24, 24)  # the least room home leaves round them: a line of the ground
MOVED = ("place wave",)
# The graph's area starts below its panel's tab strip, a text row and 4 pixels either
# side, at the font's height; a box's name is a row a pad below its top.
CELL = 18
CELL_WIDE = 8  # a cell's width at that height, as Roboto Mono's advance makes it
STRIP = CELL + 2 * 4
NAME = STRIP + 4 + CELL // 2
# The menubar's strip, along the window's top, and the divider of 4 pixels under it,
# over the dock's area.
MENUBAR = STRIP + 4
# The close mark of the editor's first tab, the dock's top left: a cell in, the label, a
# blank, then the mark, a cell wide, as wave/Wave.comp's.
MARK = (CELL_WIDE * (1 + len("wave/Wave.comp") + 1) + CELL_WIDE // 2,
        MENUBAR + STRIP // 2)
# A line opens the menu at the graph's top left, its first column past a frame of 4
# pixels: the first words of menu.ini's [all], of which write is the second.
WRITE_ITEM = (4 + CELL_WIDE * 2, 4 + CELL + CELL // 2)
TYPED = "typed"  # input text drops a blank at its end
# A node left out as its view loads, or whose operator stops, names itself in the log.
STOPPED = re.compile(r"\{!!!\} (?:\S+ )?node [\w.-]+: ")
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
UNPERFORMED = "nothing is presented to perform"  # what modes warns, staying in edit mode


def clicks(x: int, y: int) -> list[str]:
    """A press, a double click's second and a third, at a place, as a person's in a row."""
    return [f"input pointer {x} {y}", "input button down left", "input button up left",
            "input button down left 2", "input button up left",
            "input button down left 3", "input button up left"]


def typed(width: int, height: int) -> list[str]:
    # The middle of its 4 pixels, at the dock's ratio of what the menubar leaves.
    seam = MENUBAR + round((height - MENUBAR - 4) * 0.65) + 2
    return [
        "focus ide.terminal.command-line",  # first, so the keys part's param gives none
        "mode perform",  # with nothing presented: a warning, in edit mode still
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


def menu(note: Path, width: int) -> list[str]:
    graph = (round((width - 4) * 0.5) + 4, MENUBAR + STRIP)  # the graph's area's corner
    return [
        f"open {note}",
        "focus ide.editor.text",
        f"input text {TYPED}",  # where the caret starts, as the note opens
        "menu all",  # shown with the first frame, before the press
        f"input pointer {graph[0] + WRITE_ITEM[0]} {graph[1] + WRITE_ITEM[1]}",
        "input button down left",
        "input button up left",
    ]


def docked(width: int, height: int) -> list[str]:
    row = round((height - MENUBAR - 4) * 0.65)  # the dock's top row, under the menubar
    strip = MENUBAR + row + 4 + STRIP // 2  # the terminal's tab strip, past the seam
    editor = round((width - 4) * 0.5)  # the editor's right edge, at the top row's seam
    return [
        "ide: panel move ide.graph-panel ide.perform-panel bottom",  # the two swap
        "ide: panel hide ide.graph-panel",
        "ide: panel hide ide.perform-panel",  # their dock hides in the top row
        "ide: panel show ide.graph-panel",  # their dock shows again, the graph's alone
        "ide: panel move ide.perform-panel ide.graph-panel middle",  # one strip, two tabs
        "ide: panel move ide.graph-panel ide.terminal-panel right",  # Perform, its own
        f"input pointer {width // 4} {strip}",
        "input button down left",
        f"input pointer {editor - 10} {MENUBAR + row // 2}",
        "input button up left",  # the terminal beside the editor, at its right
    ]


def graph(wave: Path, width: int, height: int) -> list[str]:
    left = round((width - 4) * 0.5) + 4  # past the top row's seam: the graph's area
    # The dock's top row, under the menubar, and above the graph's row of buttons, as
    # tall as its tab strip, over the Perform panel.
    row = round((height - MENUBAR - 4) * 0.65)
    bottom = MENUBAR + round((row - 4) * 0.5) - STRIP
    above = MENUBAR + STRIP  # the graph's area's top, below its panel's tab strip
    shown = (left + max((width - left - REACH[0]) // 2, HOME_MARGIN[0]),
             above + max((bottom - above - REACH[1]) // 2, HOME_MARGIN[1]))
    box = (shown[0] + 40, shown[1] + NAME - STRIP)  # on wave's name
    moved = (box[0] + 30, box[1])  # the box dragged right
    ground = (width - 40, 150)  # below and right of both boxes
    panned = (moved[0] + 20, moved[1] + 10)  # where it shows once the graph moved
    # Two thirds down the Perform panel, under the graph's, in the dock's top row.
    top = MENUBAR + row
    below = MENUBAR + round((row - 4) * 0.5) + 4
    perform = (width * 3 // 4, below + (top - below) * 2 // 3)
    return [
        # The flow layer alone, so no curve of the docs ids the copy cites crosses the
        # boxes; the box outside they reach is placed with the others.
        "ide: param set ide.graph.graph relations flow",
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
        f"input pointer {left + 20} {above + 4}",
        "input button up left",  # what it touches joins the selection
        "input key up shift",
        f"input pointer {perform[0]} {perform[1]}",
        "input wheel 0 3",  # the view presented zoomed in, so the panel cuts it
        "input button down left",
        f"input pointer {perform[0] + 60} {perform[1] + 40}",
        "input button up left",  # and moved
        "input key down h",  # home again, as the press gave the panel the focus
        f"input pointer {perform[0]} {perform[1]}",
        "input button down left",
        "input button up left",
        "input button down left 2",  # a double click: the frame it shows held
        "input button up left",
        "input key down space",  # let go
        "input key up space",
        "input key down space",  # and held again
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
    LAID.unlink(missing_ok=True)
    code, output = run(sys.argv[1], [CLI, IDE, "--frames", frames, "--fps", 0, "--log",
                                     "info", "--source", script], headless=False, typed=typed)
    if code != 0 or problems(output) or STOPPED.search(output):
        sys.exit(f"ide: expected the {name} to take every key, every node running, got "
                 f"exit {code}:\n{output}")
    if whole and RAN.findall(output) != [str(frames)]:
        sys.exit(f"ide: expected the {name} to run {frames} frames, got:\n{output}")
    return output


def main() -> None:
    if not display():
        print("skipped: no display to open a window on")
        return
    kept = {path: path.read_bytes() if path.exists() else None for path in (ZOOMED, LAID)}
    ZOOMED.unlink(missing_ok=True)
    try:
        test()
    finally:
        for path, held in kept.items():
            if held is None:
                path.unlink(missing_ok=True)
                if path.parent.exists() and not any(path.parent.iterdir()):
                    path.parent.rmdir()
            else:
                path.write_bytes(held)


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
        if UNPERFORMED not in (said := drive(folder, "terminal", typed(width, height))):
            sys.exit(f"ide: expected mode perform with nothing presented to warn "
                     f"{UNPERFORMED!r}, got:\n{said}")
        note = folder / "note.txt"
        # Its breaks as NOTE gives them, so the bytes the editor writes are alike everywhere.
        note.write_text(NOTE, encoding="utf-8", newline="\n")
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
        typed_into = folder / "typed.txt"
        typed_into.write_text(NOTE, encoding="utf-8")
        drive(folder, "menu", menu(typed_into, width))
        if (held := typed_into.read_text(encoding="utf-8")) != TYPED + NOTE:
            sys.exit(f"ide: expected the file menu's write to write {TYPED + NOTE!r}, "
                     f"got {held!r}")
        if "{!!!}" in (moved := drive(folder, "docked", docked(width, height))):
            sys.exit(f"ide: expected the panels to move with no line refused, got:\n"
                     f"{moved}")
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
                                for key, value in ARRANGED.items()),
                        encoding="utf-8")
        said = drive(folder, "graph", graph(wave, width, height))
        arranged = numbers(kept.read_text(encoding="utf-8"))
        if still := [key for key in MOVED if arranged.get(key) == ARRANGED[key]]:
            sys.exit(f"ide: expected the graph to keep {', '.join(still)} moved, it kept\n"
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
