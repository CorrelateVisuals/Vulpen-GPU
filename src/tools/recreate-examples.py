#!/usr/bin/env python3
"""Make every example project again, as projects/re-<example>, from the terminal alone, as
a person would type it (V06), with mistakes made on purpose and put right; then check
each copy against its example: its manifest, its files, and that it runs.

The run is the cli hosting the ide with no window (V07), from the repository's top, so the
live build compiles each node as it comes, as in a person's run; the lines go to the cli's
terminal on standard input, or are typed into the ide's terminal as keys. Each line is
expected to run, or to be refused naming why: a line that does otherwise, or a problem
that stays once a copy is built, is a finding. The copies go before the run and, without
--keep, after it; what the run rewrites in .vulpen/ folders as panels move and the graph
zooms is put back.

Run it while no other vulpen of this tree runs, as both would build into out/.

Usage: python3 src/tools/recreate-examples.py BINARY [--only NAME,...] [--keep]
                                                     [--said FILE]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROJECTS = ROOT / "projects"
COPY = "re-"           # a copy's name: re-<example>
TIMEOUT_S = 120        # for a frame, or the run's end
POLL_S = 0.01
FRAMES_A_LINE = 3      # at least, from one line to the next
SETTLE_FRAMES = 30     # a window of frames, as the live build goes on beside them
SETTLE_S = 0.5         # between windows, so the live build's watch sees each change
SETTLE_ROUNDS = 240    # at most, before the build counts as never idle
ERROR = "{!!!}"
SETUP = f"""view add ide {ROOT}/src/runtime/recipes/ide/view.vlp
node add cli.snapshot recipe=snapshot
connect snapshot ide: cli.snapshot.picture
"""


@dataclass
class Step:
    """Lines sent together, in one frame, and what they should do: run (refused None) or
    be refused, naming the words given; typed into the ide's terminal when ide."""
    lines: list[str]
    refused: str | None = None
    ide: bool = False


def ok(*lines: str) -> Step:
    return Step(list(lines))


def no(line: str, why: str) -> Step:
    return Step([line], why)


def typed(line: str, refused: str | None = None) -> Step:
    """A line typed into the ide's terminal, pressed into focus first, as a person
    clicks it, since open gives the focus to the editor."""
    return Step([line], refused, ide=True)


@dataclass
class Example:
    """An example made again, or, not compared, a project only to try lines in."""
    name: str
    steps: list[Step] = field(default_factory=list)
    compared: bool = True

    @property
    def copy(self) -> str:
        return COPY + self.name


def at(example: str, *parts: str) -> str:
    """A path from the repository's top, as a line names it."""
    return "/".join(("projects", example, *parts))


def swapped(example: str, node: str, made: list[str], wanted: list[str]) -> list[Step]:
    """The files node new wrote, which the example's node does not hold, removed, and the
    example's copied in: one copied over a file there first, which is refused."""
    copy, folder = COPY + example, node.replace(".", "/")
    steps = []
    clash = next((name for name in wanted if name in made), None)
    if clash:
        steps.append(no(f"file copy {at(example, folder, clash)} {at(copy, folder, clash)}",
                        "is there already"))
    steps.append(ok(*(f"file remove {at(copy, folder, name)}" for name in made)))
    steps.append(ok(*(f"file copy {at(example, folder, name)} {at(copy, folder, name)}"
                      for name in wanted)))
    return steps


def files_of(example: str, node: str) -> list[str]:
    folder = PROJECTS / example / node.replace(".", "/")
    return sorted(path.name for path in folder.iterdir() if path.is_file())


def made_by(kind: str, node: str) -> list[str]:
    """The files node new writes for a node: its class's name, from its own name."""
    own = node.rsplit(".", 1)[-1]
    name = "".join(word.capitalize() for word in own.split("-"))
    ends = (".cpp", ".glsl", ".vert", ".frag") if kind == "draw" else (".cpp", ".comp")
    return [name + end for end in ends]


def made(kind: str, example: str, node: str) -> list[Step]:
    """A node started from the template, its files then swapped for the example's."""
    return [ok(f"node new {kind} {node}"),
            *swapped(example, node, made_by(kind, node), files_of(example, node))]


def examples(scratch: Path) -> list[Example]:
    """Each example as a person might make it again, a different way each, with the
    mistakes a person makes on the way: a line refused at once, or one taken that a
    later line puts right."""
    every = []

    # triangle: the template, its files swapped for the example's, the words set.
    e = Example("triangle")
    e.steps = [
        no("view new", "does not fit the usage"),
        ok(f"view new {e.copy}"),
        no(f"view new {e.copy}", e.copy),
        no("node new draw Triangle", "no name for a new node"),
        no("nod new draw triangle", "unknown command nod"),
        *made("draw", e.name, "triangle"),
        no("node set triangle vertex_count=three", "three is not a whole number"),
        no("node set triangle vertex_count=3 invocations=8", "invocations"),
        ok("node set triangle vertex_count=3 instance_count= param=frames_per_turn=600"),
        ok("info triangle"),
        ok("ls"),
        ok("clear"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-shaders: the example's node folder copied in, which adds the node; first
    # to a folder whose name holds a capital, then that node removed, its folder too.
    # The editor opens the copy's shaders, finds in them, moves a tab and writes one.
    e = Example("triangle-shaders")
    shader = at(e.copy, "triangle", "Triangle.vert")
    e.steps = [
        ok(f"view new {e.copy}"),
        ok(f"file copy {at(e.name, 'triangle')}/ {at(e.copy, 'Triangle')}/"),
        ok("node remove Triangle"),
        ok(f"file copy {at(e.name, 'triangle')}/ {at(e.copy, 'triangle')}/",
           "node set triangle vertex_count=3 param=frames_per_turn=600"),
        no("node set triangle log=loud", "loud"),
        ok(f"open {shader}"),
        ok("find gl_Position"),
        ok("jump main"),
        ok(f"open {at(e.copy, 'triangle', 'Triangle.frag')}"),
        ok(f"tab move {at(e.copy, 'triangle', 'Triangle.frag')} {shader}"),
        ok(f"close {at(e.copy, 'triangle', 'Triangle.frag')}"),
        ok("write"),
        ok("close all"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-params: the template; the params set on the node the graph selects, a line
    # short of it, the selection filling it in, typed in the ide's terminal too; a
    # misspelled key taken back.
    e = Example("triangle-params")
    e.steps = [
        ok(f"view new {e.copy}"),
        *made("draw", e.name, "triangle"),
        ok("node set triangle vertex_count=3 instance_count= param=frames_per_turn=600"),
        no("param set top 0,-0.5", "does not fit the usage"),
        ok("select triangle"),
        ok("selected"),
        ok("param set top 0,-0.5"),
        ok("param set top_color 1,0,0"),
        ok("param set right_colour 0,1,0"),
        ok("param unset right_colour"),
        ok("param set right 0.5,0.5"),
        typed("param set right_color 0,1,0"),
        typed("param set left -0.5,0.5"),
        typed("param set left_color 0,0,1"),
        ok("info"),
        ok("menu node for triangle"),
        ok("menu"),
        ok("select"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-twin: the shared folder, first copied where it makes a node, which goes
    # whole, folder too, and then where the view's shared code is.
    e = Example("triangle-twin")
    e.steps = [
        ok(f"view new {e.copy}"),
        ok(f"file copy {at(e.name, 'shared')}/ {at(e.copy, 'share')}/"),
        ok("node remove share"),
        ok(f"file copy {at(e.name, 'shared')}/ {at(e.copy, 'shared')}/"),
        ok(f"file copy {at(e.name, 'triangle')}/ {at(e.copy, 'triangle')}/",
           "node set triangle vertex_count=3 param=frames_per_turn=600"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-compute: two templates, a connection to a misspelled port, which the line
    # takes and the bind refuses, so it is let go of and made again.
    e = Example("triangle-compute")
    e.steps = [
        ok(f"view new {e.copy}"),
        *made("dispatch", e.name, "raster"),
        *made("draw", e.name, "screen"),
        ok("node set raster invocations=65536 param=frames_per_turn=600",
           "node set screen vertex_count=3 instance_count= param="),
        ok("connect canvas raster.canvas screen.pictur"),
        no("connect canvas raster.canvas screen.picture", "two connections are named canvas"),
        ok("disconnect canvas"),
        ok("connect canvas raster.canvas screen.picture"),
        no("image clear screen.picture", "no image a node's C++ fills"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-cpp-raster: the contracts, a folder made by file new and filled file by
    # file, a connection first made backwards.
    e = Example("triangle-cpp-raster")
    e.steps = [
        ok(f"view new {e.copy}"),
        ok(f"file copy {at(e.name, 'contracts')}/ {at(e.copy, 'contracts')}/"),
        ok(f"file new {at(e.copy, 'raster')}/"),
        no(f"file new {at(e.copy, 'raster')}/", "is there already"),
        ok(f"file copy {at(e.name, 'raster', 'Raster.comp')} {at(e.copy, 'raster', 'Raster.com')}"),
        ok(f"file rename {at(e.copy, 'raster', 'Raster.com')} "
           f"{at(e.copy, 'raster', 'Raster.comp')}"),
        ok(f"file copy {at(e.name, 'raster', 'Raster.cpp')} {at(e.copy, 'raster', 'Raster.cpp')}",
           "node set raster invocations=65536 param=frames_per_turn=600 param=side=256"),
        ok(f"file copy {at(e.name, 'screen')}/ {at(e.copy, 'screen')}/",
           "node set screen vertex_count=6 instance_count=cells"),
        ok("connect cells screen.cells raster.cells"),
        ok("disconnect cells"),
        ok("connect cells raster.cells screen.cells"),
        *(ok(f"open {at(e.copy, *path)}") for path in (("raster", "Raster.comp"),
                                                      ("raster", "Raster.cpp"),
                                                      ("screen", "Screen.frag"))),
        ok(f"close left {at(e.copy, 'raster', 'Raster.cpp')}"),
        ok(f"close right {at(e.copy, 'raster', 'Raster.cpp')}"),
        ok("close force"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-cpp-clock: a node named wrong, renamed with its folder.
    e = Example("triangle-cpp-clock")
    e.steps = [
        ok(f"view new {e.copy}"),
        ok(f"file copy {at(e.name, 'contracts')}/ {at(e.copy, 'contracts')}/"),
        ok(f"file copy {at(e.name, 'raster')}/ {at(e.copy, 'rastr')}/",
           "node set rastr invocations=65536 param=frames_per_turn=600 param=side=256"),
        no("node rename rastr screen.raster", "screen"),
        ok("node rename rastr raster"),
        ok(f"file copy {at(e.name, 'screen')}/ {at(e.copy, 'screen')}/",
           "node set screen vertex_count=6 instance_count=cells"),
        ok("connect cells raster.cells screen.cells"),
        ok("focus"),
        ok("focus ide.terminal.command-line"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-cpp-corners: the nodes made in the wrong order, then moved.
    e = Example("triangle-cpp-corners")
    e.steps = [
        ok(f"view new {e.copy}"),
        ok(f"file copy {at(e.name, 'contracts')}/ {at(e.copy, 'contracts')}/"),
        ok(f"file copy {at(e.name, 'screen')}/ {at(e.copy, 'screen')}/",
           "node set screen vertex_count=6 instance_count=cells"),
        ok(f"file copy {at(e.name, 'raster')}/ {at(e.copy, 'raster')}/",
           "node set raster invocations=65536 param=side=256"),
        ok(f"file copy {at(e.name, 'corners')}/ {at(e.copy, 'corners')}/",
           "node set corners param=frames_per_turn=600"),
        no("node move corners corners", "corners"),
        ok("node move corners screen"),
        ok("node move raster screen"),
        ok("connect corners corners.corners raster.corners"),
        ok("connect cells raster.cells screen.cells"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-files: a draw split over two nodes; a connection named twice, then one
    # misnamed, let go of and made again.
    e = Example("triangle-files")
    e.steps = [
        ok(f"view new {e.copy}"),
        ok(f"file copy {at(e.name, 'contracts')}/ {at(e.copy, 'contracts')}/"),
        *(ok(f"file copy {at(e.name, node)}/ {at(e.copy, node)}/")
          for node in ("clock", "corners", "face", "blend")),
        ok("node set clock param=frames_per_turn=600", "node set face vertex_count=3"),
        ok("connect spin clock.spin corners.spin"),
        no("connect spin corners.corners face.corners", "two connections are named spin"),
        ok("connect corner corners.corners face.corners"),
        ok("disconnect corner"),
        ok("connect corners corners.corners face.corners"),
        ok("connect color face.color blend.color"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-header: a node with a header beside its C++, looked at as the graph goes
    # into it by cd, and through ls, info and probe stats.
    e = Example("triangle-header")
    e.steps = [
        ok(f"view new {e.copy}"),
        ok(f"file copy {at(e.name, 'contracts')}/ {at(e.copy, 'contracts')}/"),
        ok(f"file copy {at(e.name, 'corners')}/ {at(e.copy, 'corners')}/",
           "node set corners param=frames_per_turn=600"),
        ok(f"file copy {at(e.name, 'face')}/ {at(e.copy, 'face')}/",
           "node set face vertex_count=3"),
        ok("connect corners corners.corners face.corners"),
        ok("cd corners"),
        ok("ls"),
        ok("cd .."),
        no("cd nowhere", "nowhere"),
        ok("cd"),
        ok("select find corners"),
        ok("info"),
        ok("probe stats"),
        # A right press on the graph's ground opens its menu, escape shuts it; the wheel
        # zooms the graph in and out.
        ok("input pointer 760 180", "input button down right", "input button up right"),
        ok("input key down escape", "input key up escape"),
        ok("input pointer 900 150", "input wheel 0 1", "input wheel 0 -1"),
        ok("view save"),
    ]
    every.append(e)

    # triangle-points: a node holding nodes, brought by one copy, in the order its
    # folders sort, moved into the example's; one let go of, its folder kept, and taken
    # back by node add.
    e = Example("triangle-points")
    words = {"top": "param=at=0,-0.5 param=color=1,0,0",
             "right": "param=at=0.5,0.5 param=color=0,1,0",
             "left": "param=at=-0.5,0.5 param=color=0,0,1"}
    e.steps = [
        ok(f"view new {e.copy}"),
        ok(f"file copy {at(e.name, 'contracts')}/ {at(e.copy, 'contracts')}/"),
        ok(f"file copy {at(e.name, 'corners')}/ {at(e.copy, 'corners')}/",
           *(f"node set corners.{corner} invocations=1 {set}" for corner, set in words.items())),
        ok("node move corners.top corners.left"),
        ok("node move corners.right corners.left"),
        ok(f"file copy {at(e.name, 'face')}/ {at(e.copy, 'face')}/",
           "node set face vertex_count=3 param=frames_per_turn=600"),
        ok("node remove keep corners.left"),
        no(f"file remove {at(e.copy, 'corners')}/", "node corners's folder"),
        ok(f"node add corners.left invocations=1 {words['left']}"),
        ok("node move corners.left face"),
        *(ok(f"connect {corner} corners.{corner}.corner face.{corner}") for corner in words),
        ok("view save"),
    ]
    every.append(e)

    # wave: two templates; the words from a file of lines, sourced; the panels moved
    # about, the Perform panel into the graph's and a project made while it is there, then
    # out again; the modes switched, the graph zoomed and arranged.
    e = Example("wave")
    lines = scratch / "wave-words.txt"
    # A file's lines go to the view vulpen started with, unless they name one, as a log's.
    lines.write_text(f"{e.copy}: node set wave invocations=1024 param=amplitude=1.0 "
                     "param=speed=0.02\n"
                     f"{e.copy}: node set probe invocations=8 param=step=128 param=every=60 "
                     "log=info\n")
    e.steps = [
        ok(f"view new {e.copy}"),
        *made("dispatch", e.name, "wave"),
        *made("dispatch", e.name, "probe"),
        no("source nowhere.txt", "nowhere.txt"),
        ok(f"source {lines}"),
        ok("connect values wave.values probe.value"),
        ok("disconnect values"),
        ok("connect values wave.values probe.values"),
        ok("ide: panel maximize ide.graph-panel"),
        ok("ide: panel maximize ide.graph-panel"),
        ok("ide: panel hide ide.editor-panel"),
        ok("ide: panel show ide.editor-panel"),
        no("ide: panel move ide.perform-panel ide.side bottom", "not ide.side"),
        ok("ide: panel move ide.perform-panel ide.graph-panel middle"),
        ok(f"view new {e.copy}-two"),
        ok(f"view close {e.copy}-two", f"file remove force projects/{e.copy}-two/"),
        ok(f"view switch {e.copy}"),
        no("ide: panel move ide.perform-panel ide.graph-panel bottom", "ide.perform-panel"),
        ok("ide: panel move ide.graph-panel ide.graph-panel bottom"),
        ok("ide: panel restore"),
        ok("zoom in", "zoom out", "zoom reset"),
        ok("mode toggle"),
        ok("mode edit"),
        ok("mode perform"),
        ok("mode edit"),
        ok("arrange"),
        ok("view list"),
        ok("view save"),
    ]
    every.append(e)

    # life: a project named wrong, renamed; a node name with _, and one misspelled; the
    # copy saved as another project, which goes again.
    e = Example("life")
    e.steps = [
        ok(f"view new {COPY}lif"),
        ok(f"view rename {e.copy}"),
        ok(f"file copy {at(e.name, 'contracts')}/ {at(e.copy, 'contracts')}/"),
        no("node new dispatch seed_cells", "no name for a new node"),
        *made("dispatch", e.name, "seed-cells"),
        ok(f"file copy {at(e.name, 'engine')}/ {at(e.copy, 'enigne')}/"),
        ok("node rename enigne engine"),
        ok(f"file copy {at(e.name, 'cells')}/ {at(e.copy, 'cells')}/"),
        ok("node set seed-cells invocations=65536 param=alive_percent=30 param=seed=1",
           "node set engine invocations=65536 param=grid_xy=256,256 "
           "param=frames_per_generation=3",
           "node set cells vertex_count=6 instance_count=cells"),
        ok("connect seeded seed-cells.seeded engine.seeded"),
        ok("connect cells engine.cells cells.cells"),
        ok("view save"),
        ok(f"view save as {COPY}life-two"),
        ok(f"view load {e.copy}"),
        ok(f"view close {COPY}life-two"),
        ok(f"file remove force projects/{COPY}life-two/"),
        ok(f"view switch {e.copy}"),
    ]
    every.append(e)

    # A project to try the library's lines in: a recipe dropped, synced, renamed; a file
    # added beside what it copied, or one of those taken, is a change a sync refuses; then
    # the node removed, folder and all.
    e = Example("scratch", compared=False)
    shot = at(e.copy, "picture")
    e.steps = [
        ok(f"view new {e.copy}"),
        no("recipe drop nothing shot", "no recipe nothing"),
        ok("recipe drop snapshot shot"),
        ok("recipe sync shot"),
        ok("node rename shot picture"),
        ok(f"file new {shot}/notes.txt"),
        no("recipe sync picture", "changed since it was dropped"),
        ok(f"file rename {shot}/notes.txt {shot}/readme.txt"),
        ok(f"file remove {shot}/readme.txt"),
        ok("recipe sync picture"),
        ok(f"file remove {shot}/Snapshot.comp"),
        no("recipe sync picture", "changed since it was dropped"),
        ok("node remove picture"),
        ok(f"view close {e.copy}", f"file remove force projects/{e.copy}/"),
    ]
    every.append(e)
    return every


def comparable(manifest: Path) -> list[str]:
    """A manifest's lines as they mean: no comments or blank lines, one blank round =."""
    lines = []
    for line in manifest.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            key, eq, value = line.partition("=")
            lines.append(f"{key.strip()} = {value.strip()}" if eq else line)
    return lines


def held(folder: Path) -> dict[str, bytes]:
    """A project's files but its manifest and what .vulpen/ keeps of a run."""
    return {path.relative_to(folder).as_posix(): path.read_bytes()
            for path in sorted(folder.rglob("*"))
            if path.is_file() and ".vulpen" not in path.parts and path.name != "view.vlp"}


@dataclass
class Finding:
    example: str
    what: str
    said: list[str]


# A problem a view's node meets as it binds names where its words come from, the view's
# manifest or a sourced file's line, and the node; a line's refusal names neither.
BOUND = re.compile(r"\{!!!\} \S+ node \S+: ")
STARTED = "a file changed; building"
FINISHED = ("built in", "the build failed")


class Run:
    """The cli hosting the ide, at the debug level, so the live build says when it starts,
    its output in a file read as it grows."""

    def __init__(self, binary: Path, said: Path, scratch: Path):
        self.said, self.scratch, self.read = said, scratch, 0
        self.barriers = 0
        self.building = False
        self.last = ""  # the line sent last, which a run that ends names
        (scratch / "setup.txt").write_text(SETUP)
        env = {key: value for key, value in os.environ.items()
               if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
        self.out = said.open("w")
        self.vulpen = subprocess.Popen(
            [binary, "src/runtime/recipes/cli/view.vlp", "--fps", "0", "--size", "1280x720",
             "--log", "debug", "--source", scratch / "setup.txt"],
            cwd=ROOT, env=env, text=True, stdin=subprocess.PIPE, stdout=self.out,
            stderr=subprocess.STDOUT)

    def frames(self, count: int) -> None:
        for _ in range(count):
            self.barriers += 1
            mark = self.scratch / f"frame-{self.barriers}"
            self.vulpen.stdin.write(f"log save {mark}\n")
            self.vulpen.stdin.flush()
            deadline = time.monotonic() + TIMEOUT_S
            while not mark.exists():
                if self.vulpen.poll() is not None or time.monotonic() > deadline:
                    self.vulpen.kill()
                    sys.exit(f"vulpen ended (exit {self.vulpen.wait()}) or hung after "
                             f"{self.last!r}; what it said is in {self.said}")
                time.sleep(POLL_S)
            mark.unlink()

    def new(self) -> list[str]:
        """What it said since this was last asked, following the live build as it goes."""
        with self.said.open(errors="replace") as said:
            said.seek(self.read)
            text = said.read()
            self.read = said.tell()
        lines = text.splitlines()
        for line in lines:
            if STARTED in line:
                self.building = True
            elif any(word in line for word in FINISHED):
                self.building = False
        return lines

    def send(self, step: Step) -> list[str]:
        self.last = step.lines[-1]
        if step.ide:
            for line in ("focus ide.terminal.command-line", f"input text {step.lines[0]}",
                         "input key down enter", "input key up enter"):
                self.vulpen.stdin.write(line + "\n")
                self.frames(1)
        else:
            self.vulpen.stdin.write("".join(line + "\n" for line in step.lines))
        self.frames(FRAMES_A_LINE)
        return self.new()

    def idle(self) -> bool:
        """Until the live build saw every file the lines changed and built them: two windows
        of frames running with no build; whether that came before the last round."""
        quiet = 0
        for _ in range(SETTLE_ROUNDS):
            time.sleep(SETTLE_S)
            self.frames(SETTLE_FRAMES)
            self.new()
            quiet = 0 if self.building else quiet + 1
            if quiet == 2:
                return True
        return False

    # quit, as a person ends the run, which refuses while a node holds work, then quit
    # force.
    def end(self) -> int:
        self.vulpen.stdin.write("quit\n")
        self.vulpen.stdin.flush()
        try:
            code = self.vulpen.wait(FRAMES_A_LINE)
        except subprocess.TimeoutExpired:
            self.vulpen.stdin.write("quit force\n")
            self.vulpen.stdin.close()
            code = self.vulpen.wait(TIMEOUT_S)
        self.out.close()
        return code


def refusals(lines: list[str]) -> list[str]:
    """The refusals of the lines run: problems no binding node met."""
    return [line.strip() for line in lines if ERROR in line and not BOUND.search(line)]


def bound(lines: list[str], view: str) -> list[str]:
    """The problems a view's nodes met as they bound, each once."""
    found = []
    for line in lines:
        if ERROR in line and f"{view}/view.vlp" in line:
            problem = line[line.index(ERROR):].strip()
            if problem not in found:
                found.append(problem)
    return found


def kept() -> dict[Path, bytes]:
    """What the parts keep beside the views in .vulpen/, which a run rewrites as panels
    move and the graph zooms, but the copies': put back after the run, so it leaves a
    person's ide and graphs as they were."""
    folders = [ROOT / ".vulpen", *ROOT.glob("src/**/.vulpen"), *PROJECTS.glob("*/.vulpen")]
    return {path: path.read_bytes() for folder in folders if folder.is_dir()
            and not folder.parent.name.startswith(COPY)
            for path in folder.iterdir() if path.is_file()}


def put_back(before: dict[Path, bytes]) -> None:
    for path, data in kept().items():
        if path not in before:
            path.unlink()
            if not any(path.parent.iterdir()):
                path.parent.rmdir()
        elif data != before[path]:
            path.write_bytes(before[path])
    for path, data in before.items():
        if not path.exists():
            path.write_bytes(data)


def forget(binary: Path, name: str) -> None:
    """A copy left by a run before, and what its build left, so this run builds anew."""
    for folder in (PROJECTS / name, binary.parent / "views" / name):
        shutil.rmtree(folder, ignore_errors=True)


# Once built, the copy is closed and loaded again from what view save wrote, as a person
# loads it another day: what its nodes meet as they bind then is what stays.
def check(example: Example, run: Run, findings: list[Finding]) -> None:
    if not example.compared:
        return
    if not run.idle():
        findings.append(Finding(example.name, "the live build never went idle", []))
    said = run.send(ok(f"view close {example.copy}"))
    said += run.send(ok(f"view load {example.copy}"))
    run.frames(SETTLE_FRAMES)
    if problems := bound(said + run.new(), example.copy):
        findings.append(Finding(example.name, "problems as the saved copy loads", problems))
    original, copy = PROJECTS / example.name, PROJECTS / example.copy
    if not (copy / "view.vlp").exists():
        findings.append(Finding(example.name, "no manifest saved", []))
        return
    if comparable(original / "view.vlp") != comparable(copy / "view.vlp"):
        findings.append(Finding(example.name, "manifest differs",
                                ["example:", *comparable(original / "view.vlp"),
                                 "copy:", *comparable(copy / "view.vlp")]))
    want, have = held(original), held(copy)
    if want != have:
        findings.append(Finding(example.name, "files differ", sorted(
            [f"missing {name}" for name in want.keys() - have.keys()] +
            [f"extra {name}" for name in have.keys() - want.keys()] +
            [f"changed {name}" for name in want.keys() & have.keys()
             if want[name] != have[name]])))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("binary", type=Path)
    parser.add_argument("--only", help="the examples to make again, by name, comma-separated")
    parser.add_argument("--keep", action="store_true", help="keep the copies after the run")
    parser.add_argument("--said", type=Path, help="where to keep what the run printed")
    arguments = parser.parse_args()
    binary = arguments.binary.resolve()
    findings: list[Finding] = []
    sent = 0
    with tempfile.TemporaryDirectory() as scratch:
        chosen = examples(Path(scratch))
        if arguments.only:
            names = arguments.only.split(",")
            chosen = [example for example in chosen if example.name in names]
        others = [f"{COPY}lif", f"{COPY}life-two", f"{COPY}wave-two"]
        for name in [example.copy for example in chosen] + others:
            forget(binary, name)
        said = arguments.said.resolve() if arguments.said else Path(scratch) / "said.txt"
        before = kept()
        try:
            run = Run(binary, said, Path(scratch))
            run.frames(FRAMES_A_LINE)
            for step in (ok("help"), ok("recipe list"), ok('complete node ""')):
                run.send(step)
            for example in chosen:
                print(f"{example.name}: {len(example.steps)} steps", flush=True)
                for step in example.steps:
                    sent += len(step.lines)
                    errors = refusals(run.send(step))
                    if step.refused is None and errors:
                        findings.append(Finding(
                            example.name, f"refused: {' | '.join(step.lines)}", errors))
                    elif step.refused is not None and not any(step.refused in line
                                                              for line in errors):
                        findings.append(Finding(example.name,
                                                f"not refused for {step.refused!r}: "
                                                f"{step.lines[0]}", errors))
                check(example, run, findings)
            if not arguments.keep:
                for example in filter(lambda example: example.compared, chosen):
                    run.send(ok(f"view close {example.copy}",
                                f"file remove force projects/{example.copy}/"))
            code = run.end()
        finally:
            put_back(before)
    print(f"{len(chosen)} examples, {sent} lines, {len(findings)} findings, exit {code}")
    for finding in findings:
        print(f"\n{finding.example}: {finding.what}")
        for line in finding.said:
            print(f"    {line}")
    sys.exit(1 if findings or code != 0 else 0)


if __name__ == "__main__":
    main()
