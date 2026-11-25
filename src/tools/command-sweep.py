#!/usr/bin/env python3
"""Sweep every command of the cli and the ide, in each state, for what breaks (V06).

Try to break every command of the cli and the ide (V06), as a person might, and as no
person means to: each usage the command port lists, in each state a person reaches, with
the words its help names, with none, with one too many, and with words no person means
in each place; the words Tab offers for each placeholder; seeded walks of random lines,
each then carried through quit restart, and every second one, of lines that only edit
the graph, replayed from its log in a new vulpen, which must both come back to the same
views (V08); the lines the ide's menus and keys send; and sequences a person was seen
to run.

The states: the cli alone; the ide hosted; a project loaded; two, the first current; the
start view current, a project hosted; the graph inside a node; a file open
with changes; and a project carried through quit restart.

A finding is vulpen ending or hanging; a validation message or sanitizer report (RVK00,
A03); a refusal that only repeats what the standard library threw (A02, RC04); a warning
no line asked for; a live build failing; a file changed outside the projects by a line
that names nothing there (RA04); a view gone that no line closed; a restart or a replay
that does not come back to the same views; a line its own usage refuses; Tab offering
nothing where its kind promises words, or words the command then refuses as naming
nothing; a sequence a person ran not doing what they meant. Every line and its answer
are kept in the report, by command and state, for a person to read what each refusal
says.

It runs in a copy of the tree it is in, built there, with projects of its own beside the
copy's examples, so no line reaches this tree's files or build, and a vulpen of this
tree may run meanwhile. In the copy, open starts a stand-in for the person's editor; no
word sent names a path outside the copy's folder, and no count a GPU could not hold
(no-memory-bombs); vulpen is killed if the machine's memory runs low. It takes the
machine's own GPU, with no window.

With --jobs N, the sweep and the gui run as pieces in up to N copies side by side,
fewer where the machine's memory holds fewer: the sweep a piece a state, the menus in
MENU_PIECES pieces of whole sections, the keys one. Copy k takes every N-th piece from
the k-th, so each piece runs in the same copy every run (C01). A piece that did not run
to its end is a finding, so a copy that failed fails the run rather than passing it.

Usage: python3 src/tools/command-sweep.py [--only PART,...] [--states NAME,...]
           [--seed N] [--walks N] [--steps N] [--tree DIR] [--keep] [--report FILE]
           [--jobs N]
"""
import argparse
import hashlib
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tests"))
from harness import PROBLEM  # noqa: E402 (the tests' folder is no package)

ROOT = Path(__file__).resolve().parents[2]
CLI = Path("src/runtime/recipes/vulpen/cli/view.vlp")
IDE = Path("src/runtime/recipes/vulpen/ide/view.vlp")
MENUS = Path("src/runtime/recipes/vulpen/library/parts/menus/menu.ini")
RECIPES = Path("src/runtime/recipes")
KEYMAP = Path("src/runtime/recipes/vulpen/library/parts/keys/keymap.ini")
GPU_CHOICE = Path("src/runtime/recipes/vulpen/cli/.vulpen/gpu.ini")
PARTS = ("sweep", "tab", "walk", "gui", "seen")
SIZE = "1280x720"
# The ide's window taken as an image, from the start or the frame it comes back, as no
# display is here to open it on.
SNAPSHOT = "connect snapshot ide: cli.snapshot.picture"
TIMEOUT_S = 90         # for a frame to pass, or a restart's build
JOBS = str(os.cpu_count() or 1)  # a build's count of jobs, as check.py's
POLL_S = 0.005
FRAMES_A_LINE = 2      # at least, from one line to the next
# What a walk leaves after each line, as a person typing does: vulpen counts a part's
# line of the 3 frames after a person's (answer_frames, in Commands.cpp) as that line's
# answer, which undo takes back with it, or keeps none of after an undo; a line sooner
# would have the parts' answers to the one before counted as its, and the log would
# replay to other views (V08).
WALK_FRAMES = 4
SETTLE_S = 0.4         # between looks at whether the live build is idle
SETTLE_ROUNDS = 150
QUIT_S = 5
TAIL_LINES = 12
# A frame's mark: help given a word no command starts with, which it refuses so.
MARK = "help zz-"
MARKED = "no command starts with zz-"
QUIET = (MARK, MARKED, "{mem}", "{nod}", "{gpu}", "{dbg}")
ERROR = "{!!!}"
WARNING = "{ ! }"
# A line of the log, by its tag, after the time or the blanks in its place; anything else
# is a command's answer.
LOGGED = re.compile(r"^(?: \d\d\.\d\d\.\d\d \d\d:\d\d:\d\d | {19})\{([^}]*)\} ?(.*)$")
STARTED = "a file changed; building"
BUILT = re.compile(r"built in [\d.]+ s and swapped|live: the build of .+ failed")
FAILED = re.compile(r"live: the build of .+ failed")
# What a compiler says of a file a line removed while the build read it: the build failed
# on the line, not on the code.
VANISHED = re.compile(r"No such file|unable to open input file|No rule to make target")
# A problem a node meets as it binds, naming the line that last gave it words and itself:
# the state's, which the views' comparison judges, not a line refused.
BOUND = re.compile(r"(?:\S+:\d+ )?node \S+: ")
RESTARTED = re.compile(r"restarted: replayed|restart: .*the views go on as replayed")
# What std::exception::what() says for the standard library's own throws, as fuzz.py
# reads them: a message naming no file, node or key.
BARE = re.compile(r"\{!!!\} (?:map::at|unordered_map::at|vector::|basic_string|array::at"
                  r"|std::|bad_optional_access|bad_variant_access|stoi|stod|stoul"
                  r"|filesystem error|Invalid argument$|invalid argument$)")
# A refusal of words as naming nothing, which Tab should not have offered.
NOTHING = re.compile(r"no node|no view is named|is no node|unknown|holds no|no connection"
                     r"|no file|no recipe|is no project's name|no param|is not open"
                     r"|is no panel")


# Lines that may work and answer nothing (RV10): those a front end sends at a high rate,
# and those that hand on a line of their own, which answers itself.
SILENT = ("input ", "log save", "focus", "panel move", "zoom ", "type ")
# A warning a line asked for: the GPU the next launch takes, or keeps where no GPU of the
# other kind meets the floor, as where lavapipe is the only one, a stuck button's answer,
# which warns rather than refuses.
EXPECTED_WARNINGS = re.compile(r"the next launch (?:prefers|takes the \S+ GPU still)")
# A node's warning as it binds, as a Texture a disconnect left unfilled or a value a param
# unset left unset: what the line made of the node, as its errors are.
BOUND_WARNING = re.compile(r"\{ ! \} \S+ node \S+: ")
# A byte a terminal obeys rather than shows, as an escape sequence: never in what it says.
CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")
# Memory left on the machine below which, or held by vulpen above which, the run is
# killed: a line asking for more than a GPU holds can take the machine down with it.
LEAST_FREE_KB = 6 * 1024 * 1024
MOST_HELD_KB = 8 * 1024 * 1024
# What a copy at work takes of the machine's memory at most, its vulpen and a live build:
# --jobs runs no more copies than fit above LEAST_FREE_KB.
COPY_KB = 2 * 1024 * 1024
# The menus' pieces for --jobs, of whole sections, each about as long as a state's sweep.
MENU_PIECES = 3


# The copy's own projects, each a copy of an example: a group of nodes and a draw, C++
# beside compute, and a node that prints; with what a line may name in each.
@dataclass(frozen=True)
class Project:
    example: str
    nodes: tuple[str, ...]
    leaf: str              # a node with nothing inside, which goes or moves alone
    group: str             # a node holding others, or one alone
    params: tuple[str, str, str]   # a node, one of its params, a value it takes
    connection: str
    writer: str            # the port connection writes, and a port it reads
    reader: str
    files: tuple[str, ...]  # the project's own, from its folder
    shader: str            # one whose pass block may hold a field reading writer


SANDBOX = {
    "bf-one": Project("triangles/triangle-points",
                      ("corners", "corners.top", "corners.right", "corners.left", "face"),
                      "corners.left", "corners", ("face", "frames_per_turn", "300"),
                      "top", "corners.top.corner", "face.top",
                      ("face/Face.frag", "face/Face.vert", "corners/top/Point.comp"),
                      "face/Face.vert"),
    "bf-two": Project("life", ("seed-cells", "engine", "cells"), "cells", "engine",
                      ("engine", "frames_per_generation", "4"), "cells", "engine.cells",
                      "cells.cells", ("engine/Engine.cpp", "engine/Engine.comp",
                                      "cells/Cells.frag"), "cells/Cells.vert"),
    "bf-three": Project("wave", ("wave", "probe"), "probe", "wave",
                        ("wave", "speed", "0.03"), "values", "wave.values",
                        "probe.values", ("wave/Wave.cpp", "probe/Probe.comp"),
                        "probe/Probe.comp"),
}
SCRAP = "scrap.txt"    # a file at each project's top, which no node holds


@dataclass(frozen=True)
class State:
    """A state a person reaches from a new vulpen: the ide hosted or not, projects loaded,
    the view current (none: the start view), lines run after, and whether
    quit restart then carries it on."""
    name: str
    ide: bool = True
    loads: tuple[str, ...] = ()
    current: str = ""
    then: tuple[str, ...] = ()
    restarted: bool = False

    @property
    def home(self) -> str:
        """The project lines name: the current one, else one on disk."""
        return self.current if self.current in SANDBOX else "bf-one"


TYPING = ("input text // typed", "input key down enter", "input key up enter")
STATES = [
    State("cli", ide=False),
    State("ide"),
    State("project", loads=("bf-one",), current="bf-one"),
    State("two", loads=("bf-one", "bf-two"), current="bf-one"),
    State("top", loads=("bf-one",)),
    State("ide current", loads=("bf-one",), current="ide"),
    State("inside", loads=("bf-one",), current="bf-one", then=("cd corners",)),
    State("editing", loads=("bf-two",), current="bf-two",
          then=("open engine/Engine.cpp", "focus ide.editor.text",
                *TYPING)),
    State("restarted", loads=("bf-one", "bf-three"), current="bf-one",
          then=("param set face frames_per_turn 400",), restarted=True),
]


@dataclass
class Usage:
    """A command as the port names it: its words, then the kind of each placeholder;
    the last may take one or more, and those last in brackets, optional, may be left out,
    as node add [<name>] [<word=value>...] runs with neither."""
    text: str
    words: list[str]
    kinds: list[str]
    more: bool = False
    optional: int = 0  # how many placeholders may be left out

    @property
    def least(self) -> int:
        return len(self.kinds) - self.optional


def usage_of(text: str) -> Usage:
    words, kinds, more, optional = [], [], False, 0
    for word in text.split():
        bracketed = word.startswith("[") and word.endswith("]")
        if bracketed:
            word = word[1:-1]
        if word.endswith("..."):
            more, word = True, word[:-3]
        if word.startswith("<") and word.endswith(">"):
            kinds.append(word[1:-1])
            optional += bracketed
        else:
            words.append(word)
    return Usage(text, words, kinds, more, optional)


class Ended(Exception):
    """vulpen ended, or stopped answering, while lines were still to come."""


def free_kb() -> int:
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1])
    return LEAST_FREE_KB * 2


def held_kb(pid: int) -> int:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except OSError:
        pass
    return 0


class Vulpen:
    """The copy's vulpen, the cli at its top and no display, at the debug level, so the
    live build says when it starts. Lines go to its terminal on standard input; a frame
    has passed once a refusal sent after them comes back, as each frame reads what came
    since the one before. What it says goes to a file, read as it grows."""

    def __init__(self, tree: Path, said: Path, setup: list[str], top: Path = CLI):
        self.tree, self.said = tree, said
        self.marks = 0
        self.last = "the start"
        self.building = False
        self.problems: list[tuple[str, list[str]]] = []
        # Warnings an earlier line asked for, by what they say, as a walk keeps them.
        self.asked: list[str] = []
        script = said.with_suffix(".setup")
        script.write_text("".join(line + "\n" for line in setup))
        env = {key: value for key, value in os.environ.items()
               if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
        self.out = said.open("a")
        self.start = self.read = self.out.tell()
        self.process = subprocess.Popen(
            [tree / "out/build/debug/vulpen", top, "--fps", "0", "--size", SIZE,
             "--log", "debug", "--source", script],
            cwd=tree, env=env, text=True, stdin=subprocess.PIPE, stdout=self.out,
            stderr=subprocess.STDOUT)
        self.killed = ""
        self.watching = True
        threading.Thread(target=self.watch, daemon=True).start()

    def watch(self) -> None:
        while self.watching and self.process.poll() is None:
            if free_kb() < LEAST_FREE_KB or held_kb(self.process.pid) > MOST_HELD_KB:
                self.killed = (f"memory ran low: {free_kb() // 1024} MiB free, vulpen "
                               f"holding {held_kb(self.process.pid) // 1024} MiB")
                self.process.kill()
                return
            time.sleep(0.05)

    def alive(self) -> bool:
        return self.process.poll() is None

    def write(self, text: str) -> None:
        try:
            self.process.stdin.write(text)
            self.process.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            raise Ended(self.why_ended())

    def why_ended(self) -> str:
        if self.killed:
            return f"{self.killed}, after {self.last!r}"
        code = self.process.poll()
        if code is None:
            return f"vulpen stopped answering after {self.last!r}"
        return f"vulpen ended, exit {code}, after {self.last!r}"

    def new(self) -> list[str]:
        with self.said.open("rb") as said:
            said.seek(self.read)
            data = said.read()
        cut = data.rfind(b"\n") + 1  # a line half written waits for its end
        self.read += cut
        return data[:cut].decode(errors="replace").splitlines()

    def frames(self, count: int = 1, timeout: float = TIMEOUT_S) -> list[str]:
        """What it said while count frames passed, but the marks."""
        said = []
        for _ in range(count):
            self.marks += 1
            mark = f"{MARKED}{self.marks};"
            self.write(f"{MARK}{self.marks}\n")
            deadline = time.monotonic() + timeout
            while True:
                lines = self.new()
                done = any(mark in line for line in lines)
                said += [line for line in lines if MARK not in line and MARKED not in line]
                if done:
                    break
                if not self.alive() or time.monotonic() > deadline:
                    if self.alive():
                        self.process.kill()
                        self.process.wait()
                    self.notice(said)
                    raise Ended(self.why_ended())
                time.sleep(POLL_S)
        self.notice(said)
        return said

    def notice(self, said: list[str]) -> None:
        """Follows the live build, and gathers what no line asked for."""
        for at, line in enumerate(said):
            if STARTED in line:
                self.building = True
            elif BUILT.search(line):
                self.building = False
            if FAILED.search(line):
                failed(said[at + 1:], self.problems)
            elif (WARNING in line and not EXPECTED_WARNINGS.search(line) and
                  not BOUND_WARNING.search(line) and
                  not any(asked in line for asked in self.asked)):
                self.problems.append(("a warning", [line.strip()]))
            elif PROBLEM.search(line):
                self.problems.append(("a validation message or sanitizer report",
                                      [line.strip()]))

    def send(self, *lines: str, frames: int = FRAMES_A_LINE) -> list[str]:
        """The lines in one frame, and what was said until frames had passed."""
        self.last = lines[-1] if lines else self.last
        self.write("".join(line + "\n" for line in lines))
        return self.frames(frames)

    def answer(self, line: str) -> list[str]:
        """A line's answer alone: what the command replied, and its refusals."""
        return answers(self.send(line))

    def idle(self) -> bool:
        """Once the live build saw every change and built it: two looks with no build."""
        quiet = 0
        for _ in range(SETTLE_ROUNDS):
            time.sleep(SETTLE_S)
            self.frames(3)
            quiet = 0 if self.building else quiet + 1
            if quiet == 2:
                return True
        return False

    def restart(self) -> list[str]:
        """quit restart, until the new vulpen replayed what it carries, or refused."""
        said = self.send("quit restart")
        deadline = time.monotonic() + TIMEOUT_S
        while not any(RESTARTED.search(line) or ERROR in line for line in said):
            if time.monotonic() > deadline:
                raise Ended(f"no new vulpen after {self.last!r}")
            time.sleep(0.05)
            said += self.frames(1)
        return said + self.frames(3)

    def end(self, force: bool = False) -> int | None:
        self.watching = False
        if self.alive():
            try:
                self.write("quit force\n" if force else "quit\n")
                self.process.wait(QUIT_S)
            except (Ended, subprocess.TimeoutExpired):
                try:
                    self.write("quit force\n")
                    self.process.wait(QUIT_S)
                except (Ended, subprocess.TimeoutExpired):
                    self.process.kill()
                    self.process.wait()
        try:
            self.process.stdin.close()
        except OSError:
            pass
        self.out.close()
        return self.process.returncode

    def tail(self) -> list[str]:
        """What it said last, but the marks and what nodes and the GPU say as they start,
        as a run that ends or hangs says why there."""
        with self.said.open(errors="replace") as said:
            said.seek(self.start)
            lines = [line.rstrip() for line in said.read().splitlines()
                     if not any(noise in line for noise in QUIET)]
        return lines[-TAIL_LINES:]


def failed(after: list[str], problems: list) -> None:
    """A live build that failed, by what the compilers said, which follows untagged."""
    said = []
    for line in after:
        if LOGGED.match(line):
            break
        said.append(line.strip())
    errors = [line for line in said if "error" in line.lower() and "***" not in line]
    if not errors or not all(VANISHED.search(line) for line in errors):
        problems.append(("the live build failed", errors[:8] or said[-8:]))


def answers(said: list[str]) -> list[str]:
    """What commands replied, and every refusal, without the time; not what the engine
    or a node logs on its own."""
    kept = []
    for line in said:
        logged = LOGGED.match(line)
        if not logged:
            kept.append(line.rstrip())
        elif logged.group(1) == "!!!":
            kept.append(f"{ERROR} {logged.group(2)}")
    return kept


def refusals(said: list[str]) -> list[str]:
    return [line for line in answers(said) if line.startswith(ERROR)]


# What the run said that names the copy's folder, named from the copy's top instead, as
# a person reads it.
def short(line: str, tree: Path) -> str:
    return line.replace(str(tree) + "/", "").replace(str(tree.parent) + "/", "<scratch>/")


@dataclass
class Finding:
    part: str
    state: str
    what: str
    line: str
    said: list[str] = field(default_factory=list)


@dataclass
class Tried:
    """A line tried, and how it went: ran, refused, or ended."""
    part: str
    state: str
    usage: str
    variant: str
    line: str
    outcome: str
    said: list[str]


# What the build makes something of: a node's code and its shaders.
CODE = (".cpp", ".h", ".hpp", ".comp", ".vert", ".frag", ".tesc", ".tese", ".geom", ".glsl")

class Copy:
    """The tree copied and built, its projects, a decoy folder beside it for paths that
    lead out of the copy, and what each part found and tried."""

    def __init__(self, tree: Path, report: Path):
        self.tree = tree
        self.scratch = tree.parent
        self.decoy = self.scratch / "decoy"
        self.pristine = self.scratch / "pristine"
        self.said = report.with_suffix(".log")
        self.said.write_text("")
        self.findings: list[Finding] = []
        self.tried: list[Tried] = []
        self.fresh = 0
        self.part = ""
        self.stale = False  # code put back that the build has not made yet

    def name(self, start: str = "bfn") -> str:
        self.fresh += 1
        return f"{start}{self.fresh}"

    def find(self, state: str, what: str, line: str, said: list[str]) -> None:
        self.findings.append(Finding(self.part, state, what, line,
                                     [short(text, self.tree) for text in said]))

    def put_back(self, build: bool = True) -> bool:
        """The copy as built: its files as committed, the projects and the decoy as they
        were made, nothing a run keeps in .vulpen/, so the next state starts alike. Where
        a line changed code or a shader, as connect declare does, or took a project of
        SANDBOX away, the copy is built again before the next vulpen loads it: what the
        build made of the change is newer than the file put back, so neither make nor the
        live build would make it again, and the next state would start on it. With build
        False, as while a vulpen runs, whose own build would race this one, the build
        waits for the next put_back; says whether one waits."""
        # Every file a line left, also inside a folder git has never seen.
        changed = subprocess.run(["git", "status", "--porcelain", "-uall"], cwd=self.tree,
                                 capture_output=True, text=True, check=True).stdout
        if any(line[3:].strip('"').endswith(CODE) for line in changed.splitlines()):
            self.stale = True
        subprocess.run(["git", "checkout", "-q", "--", "."], cwd=self.tree, check=True)
        # Not a save's temp file: a vulpen still running renames it away mid-clean, and
        # git stops at the name it listed.
        subprocess.run(["git", "clean", "-fdxq", "-e", "/out", "-e", "/projects/bf-*",
                        "-e", "*.vulpen-tmp"], cwd=self.tree, check=True)
        # A project a line renamed or removed may have had its code changed first: what
        # the build made of that is newer than its files put back, as for one changed.
        coded = {name for name in SANDBOX if not (self.tree / "projects" / name).exists()}
        for folder in (self.tree / "projects").glob("bf*"):
            now, was = held(folder), held(self.pristine / folder.name)
            if folder.name not in SANDBOX or now != was:
                if any(path.endswith(CODE) and now.get(path) != was.get(path)
                       for path in now.keys() | was.keys()):
                    coded.add(folder.name)
                shutil.rmtree(folder)
        for name in SANDBOX:
            if not (self.tree / "projects" / name).exists():
                # A project whose code changed comes back with its files' times now, as
                # git checkout gives them, so the build sees them newer than what it made.
                shutil.copytree(self.pristine / name, self.tree / "projects" / name,
                                copy_function=shutil.copy if name in coded else shutil.copy2)
        if held(self.decoy) != held(self.pristine / "decoy"):
            shutil.rmtree(self.decoy, ignore_errors=True)
            shutil.copytree(self.pristine / "decoy", self.decoy)
        if coded & set(SANDBOX):
            self.stale = True
        if self.stale and build:
            done = subprocess.run(["cmake", "--build", "--preset", "debug", "-j", JOBS],
                                  cwd=self.tree, capture_output=True, text=True)
            if done.returncode != 0:  # the next put_back tries again
                self.find("put back", "the copy did not build as put back", "cmake --build",
                          (done.stdout[-3000:] + done.stderr[-3000:]).splitlines())
            else:
                self.stale = False
        return self.stale

    def changed(self) -> list[str]:
        """The files outside the projects a run changed: what git sees, but what the
        parts keep beside views and the statistics a build rewrites."""
        status = subprocess.run(["git", "status", "--porcelain", "-uall"], cwd=self.tree,
                                capture_output=True, text=True, check=True).stdout
        return [line[3:] for line in status.splitlines()
                if ".vulpen/" not in line and "docs/statistics/" not in line]

    def held(self) -> dict[str, str]:
        """What each file changed outside the projects holds now, by its hash, empty for
        one gone; so a walk, which puts nothing back between its lines, judges a line by
        what it changed, not by the recipe an earlier recipe make wrote."""
        return {path: hashlib.sha1((self.tree / path).read_bytes()).hexdigest()
                if (self.tree / path).is_file() else "" for path in self.changed()}

    def start(self, state: State) -> Vulpen:
        """A new vulpen in the state, the copy put back first."""
        self.put_back()
        setup = [f"view add ide {self.tree / IDE}", "node add cli.snapshot recipe=snapshot",
                 SNAPSHOT] if state.ide else []
        vulpen = Vulpen(self.tree, self.said, setup)
        said = vulpen.frames(3)
        for name in state.loads:
            said += vulpen.send(f"view load {name}")
        said += vulpen.send(f"view switch {state.current}".rstrip())
        for line in state.then:
            said += vulpen.send(line)
        if state.restarted:
            said += vulpen.restart()
        if errors := refusals(said):
            self.find(state.name, "the state's own lines were refused", "the start", errors)
        return vulpen


def held(folder: Path) -> dict[str, str]:
    """A folder's files, by path, as their contents' hashes, and its folders, but what
    .vulpen/ keeps: a folder a line left, empty or holding only what .vulpen/ keeps, as
    one a project brought into another, is no node's, which the next load names."""
    if not folder.is_dir():
        return {}
    return {path.relative_to(folder).as_posix():
            hashlib.sha1(path.read_bytes()).hexdigest() if path.is_file() else "/"
            for path in sorted(folder.rglob("*"))
            if (path.is_file() or path.is_dir()) and ".vulpen" not in path.parts}


def copy_tree(to: Path, jobs: str = JOBS) -> None:
    """The tree's files as they are, committed or not, but what it ignores, as a git
    repository of their own, which the build stamps its commit from and the graph finds
    the top by; the editor's program a stand-in; the projects of SANDBOX; built, with
    jobs compiles at once, fewer where copies build side by side."""
    listed = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "-z"],
                            cwd=ROOT, capture_output=True, check=True).stdout
    for name in filter(None, listed.decode().split("\0")):
        source = ROOT / name
        if source.is_file() and not name.startswith("projects/bf"):
            (to / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, to / name)
    editor = to / "src/runtime/recipes/vulpen/library/parts/editor/view.vlp"
    editor.write_text(editor.read_text().replace("program=code", "program=true"))
    git = ["git", "-c", "user.name=command-sweep", "-c", "user.email=sweep@localhost"]
    for command in (["init", "-q"], ["add", "-A"], ["commit", "-qm", "copy"]):
        subprocess.run(git + command, cwd=to, check=True)
    pristine = to.parent / "pristine"
    for name, project in SANDBOX.items():
        shutil.copytree(to / "projects" / project.example, pristine / name,
                        ignore=shutil.ignore_patterns(".vulpen"))
        (pristine / name / SCRAP).write_text("scrap\n")
    (pristine / "decoy" / "folder").mkdir(parents=True)
    (pristine / "decoy" / "folder" / "a.txt").write_text("a\n")
    (pristine / "decoy" / "script.txt").write_text("ls\nview list\n")
    (pristine / "decoy" / "binary.bin").write_bytes(random.Random(1).randbytes(1 << 20))
    for name in [*SANDBOX, "decoy"]:
        shutil.copytree(pristine / name, (to / "projects" if name != "decoy"
                                          else to.parent) / name)
    for command in (["cmake", "--preset", "debug"],
                    ["cmake", "--build", "--preset", "debug", "-j", jobs]):
        done = subprocess.run(command, cwd=to, capture_output=True, text=True)
        if done.returncode != 0:
            sys.exit(f"{' '.join(command)} failed in {to}:\n{done.stdout[-3000:]}"
                     f"{done.stderr[-3000:]}")


# --- the words a line gives ------------------------------------------------------------

# A line each usage runs as help means it, in a state: {slots} filled from the state's
# project; lines before ;; run first, each in a frame of its own, as a person sets up
# what the command works on.
VALID = {
    "clear": "clear",
    "complete <value>...": 'complete node ""',
    "connect [<name>] <port> <port>...":
        "disconnect {connection} ;; connect {fresh} {writer} {reader}",
    "disconnect <connection>": "disconnect {connection}",
    "copy [<file>...]": "copy {scrap} {newfile}",
    "gpu": "gpu",
    "gpu toggle": "gpu toggle",
    "help [<value>...]": "help",
    "image clear <port>": "image clear {reader}",
    "info [<node>]": "info {leaf}",
    "input button down <value> [<value>]": "input button down left ;; input button up left",
    "input button up <value>": "input button up left",
    "input focus off": "input focus off",
    "input focus on": "input focus on",
    "input key down <value>": "input key down a ;; input key up a",
    "input key up <value>": "input key up a",
    "input pointer <value> <value>": "input pointer 640 360",
    "input text <value>...": "input text hello",
    "input wheel <value> <value>": "input wheel 0 1",
    "input axis <value> <value> <value>": "input axis gamepad_left 0.5 -0.5",
    "window title <value>...": "window title sweep",
    # The window fills its monitor only while vsync is on, which stays on meanwhile.
    "window fullscreen on": "window vsync on ;; window fullscreen on",
    "window fullscreen off": "window fullscreen off",
    "window pointer lock": "window pointer lock ;; window pointer free",
    "window pointer free": "window pointer free",
    # 4 samples a pixel, which Vulkan promises every GPU draws.
    "window samples <value>": "window samples 4 ;; window samples 1",
    "window vsync on": "window vsync on ;; window vsync off",
    "window vsync off": "window fullscreen off ;; window vsync off",
    "log save <file>": "log save {tree}/../decoy/{fresh}.log",
    "ls": "ls",
    "node add [<name>] [<word=value>...]": "node add {fresh} log=info",
    "node move <node> <node>": "node move {leaf} {group}",
    "new [<file>...]": "new {tree}/projects/{home}/{fresh}/ "
                       "{tree}/projects/{home}/{fresh}/Shape.vert "
                       "{tree}/projects/{home}/{same}/Shape.frag",
    "remove <file>": "remove {scrap}",
    "remove force <file>": "remove force {tree}/../decoy/folder/",
    "node remove keep <node>": "node remove keep {leaf}",
    "rename <file> <file>": "rename {scrap} {newfile}",
    "node set <node> <word=value>...": "node set {leaf} log=info",
    "open [<path>]": "open {file}",
    "param set <node> <key> <value>": "param set {pnode} {key} {value}",
    "param declare <file> <type> [<name>]": "param declare {shader} float {fresh}",
    "connect declare <port> <file> <name>": "connect declare {writer} {shader} {fresh}",
    "param unset <node> <key>": "param unset {pnode} {key}",
    "probe stats [<node>]": "probe stats",
    "quit": "quit",
    "quit force": "quit force",
    "quit restart": "quit restart",
    "recipe drop <recipe> [<name>]": "recipe drop snapshot {fresh}",
    "recipe list": "recipe list",
    "recipe sync <node>": "recipe drop snapshot {fresh} ;; recipe sync {same}",
    "source <file>": "source {tree}/../decoy/script.txt",
    "view add [<name>] <file>": "view add {fresh} {tree}/../decoy/{same}/view.vlp",
    "view close [<view>]": "view close {view}",
    "view close force [<view>]": "view close force {view}",
    "view list": "view list",
    "view list projects": "view list projects",
    "view load <project>": "view load {project}",
    "view new [<project>]": "view new {freshproject}",
    "view remove <project>": "view remove {project}",
    "view rename <name>": "view rename {freshproject}",
    "view save": "view save",
    "view save as [<name>]": "view save as {freshproject}",
    "view switch [<view>]": "view switch {view}",
    "arrange": "arrange",
    "cd [<folder>]": "cd {group}",
    "close [<file>]": "open {file} ;; close {file}",
    "close all [<file>]": "open {file} ;; close all",
    "close force [<file>]": "open {file} ;; close force {file}",
    "close left [<file>]": "open {file} ;; open {file2} ;; close left {file2}",
    "close right [<file>]": "open {file} ;; open {file2} ;; close right {file}",
    "find <value>...": "open {file} ;; find main",
    "focus [<node>]": "focus ide.terminal.command-line",
    "jump [<value>]": "open {file} ;; jump main",
    "menu [<value>...]": "menu node for {leaf}",
    "mode edit": "mode edit",
    "mode perform": "mode perform ;; mode edit",
    "mode toggle": "mode toggle ;; mode toggle",
    "panel focus <value>": "panel focus graph",
    "panel hide <node>": "panel hide ide.editor-panel ;; panel show ide.editor-panel",
    "panel maximize <node>": "panel maximize ide.graph-panel ;; panel maximize ide.graph-panel",
    "panel move <node> <node> <value>": "panel move ide.perform-panel ide.graph-panel bottom",
    "panel restore": "panel restore",
    "panel show <node>": "panel show ide.editor-panel",
    "select [<node>...]": "select {leaf}",
    "select find <value>...": "select find {leaf}",
    "selected": "selected",
    "tab move <file> <file>": "open {file} ;; open {file2} ;; tab move {file2} {file}",
    # A comment line of its own, as TYPING's: // alone, at the caret the file opens with,
    # makes its first line a comment, whose build then fails on what the line declared,
    # which a walk, putting nothing back, would take as the next line's finding.
    "write": "open {file} ;; focus ide.editor.text ;; input text // typed ;; "
             "input key down enter ;; input key up enter ;; write",
    "write force": "open {file} ;; focus ide.editor.text ;; input text // typed ;; "
                   "input key down enter ;; input key up enter ;; write force",
    "zoom in": "zoom in",
    "zoom out": "zoom out",
    "zoom reset": "zoom reset",
}
# Words no person means, by the kind of the placeholder they stand in: wrong case and
# form, a name the engine or a view takes already, too long, punctuation the port reads
# as its own, numbers past what fits; every path in the copy's folder, every count one a
# GPU holds (no-memory-bombs).
LONG = "x" * 300
NASTY = {
    "name": ["Bad", "9lives", "-x", "a..b", "a.", ".a", "a/b", "contracts", "shared",
             "cli", "ide", LONG, "ñame", "a:b", "{}", ".."],
    "node": ["nobody", "cli", "ide", "{group}.nobody", ".", "..", "a..b", LONG],
    "port": ["nobody.out", "{leaf}.", ".top", "{leaf}", "ide:", "nobody:", "{leaf}.nothing",
             "{writer}.x"],
    "connection": ["nobody", LONG, "ide:"],
    "word=value": ["=", "x=", "=1", "invocations=-1", "invocations=0", "invocations=1.5",
                   "invocations=nan", "vertex_count=99999999999", "recipe=nothing",
                   "recipe=snapshot", "operator=X", "file=x", "log=loud", "param=a=b=c",
                   "param==1", "instance_count=nobody", "image=x", LONG],
    "key": ["nokey", "a=b", "invocations", LONG],
    "type": ["nothing", "Float", "float;", "Texture[]", "{}", "image", "vec5", "Name",
             LONG],
    "value": ["nan", "inf", "-1", "1e30", "-0", "0x10", "1,2,3,4,5", LONG, "ñ", "{}", "#",
              "=", ":"],
    "file": ["{tree}/projects/{home}/nothing/x.txt", "{tree}/projects/{home}/view.vlp/x",
             "{tree}/projects/", "./", "nothing/", "{tree}/projects/{home}/../bf-three/"
             + SCRAP, "{tree}/projects/{home}/abs.txt", "{tree}/../decoy/binary.bin",
             "{tree}/projects/{home}/" + LONG],
    "view": ["nobody", "library", "cli", LONG, ":"],
    "project": ["ide", "cli", "Bad", "nobody", "{tree}/../decoy",
                "{tree}/src/runtime/recipes/vulpen/ide/", "{tree}/projects/{home}/view.vlp",
                LONG],
    "path": ["nobody.txt", "Point.comp", "{tree}/../decoy/binary.bin",
             "{tree}/../decoy/folder/", LONG],
    # Only what cd refuses: a folder it goes into would move the graph for every line
    # after it in the state.
    "folder": ["nobody", "{leaf}.", ":", "{}", "{tree}/projects/{home}/nothing/",
               "{tree}/projects/{home}/view.vlp", "{tree}/../decoy/binary.bin",
               "{tree}/projects/{home}/" + LONG, LONG],
    "recipe": ["nothing", "../parts", "library", "projects", LONG],
}
SLOT = re.compile(r"\{([a-z0-9]+)\}")
# Kinds whose completion, the command port's own table says, offers words.
COMPLETED = ("node", "port", "connection", "word=value", "key", "type", "file", "view",
             "project", "path", "folder", "recipe")


# What a line names where an app's view is current: the cli's own nodes, or the ide's, as
# their manifests have them; a node, one of its params and a value it takes.
APP_WORDS = {
    "": {"leaf": "cli.library", "group": "cli", "connection": "snapshot",
         "pnode": "cli.library", "key": "spare", "value": "1"},
    "ide": {"leaf": "ide.buttons", "group": "ide", "connection": "dock",
            "writer": "ide.side.split.second", "reader": "ide.dock.split.area",
            "pnode": "ide.dock", "key": "split.ratio", "value": "0.6"},
}


@dataclass
class Words:
    """A state's slots: what a line names there, fresh names counted through the copy."""
    copy: Copy
    state: State
    same: str = ""

    def fill(self, text: str) -> str:
        project = SANDBOX[self.state.home]
        node, key, value = project.params
        # Whole, as a path typed counts from the folder the prompt shows, which the
        # state's current view moves.
        home = self.copy.tree / "projects" / self.state.home
        files = [str(home / path) for path in project.files]
        slots = {
            "home": self.state.home, "tree": str(self.copy.tree),
            "scrap": str(home / SCRAP), "file": files[0], "file2": files[1],
            "leaf": project.leaf, "group": project.group, "connection": project.connection,
            "writer": project.writer, "reader": project.reader, "pnode": node,
            "key": key, "value": value, "shader": str(home / project.shader),
            "view": self.state.loads[-1] if self.state.loads else "ide",
            "project": next((name for name in SANDBOX if name not in self.state.loads),
                            "bf-three"),
        } | APP_WORDS.get(self.state.current, {})
        while "{fresh}" in text or "{freshproject}" in text or "{newfile}" in text:
            self.same = self.copy.name()
            text = (text.replace("{fresh}", self.same, 1)
                    .replace("{freshproject}", f"bf-{self.same}", 1)
                    .replace("{newfile}", str(home / f"{self.same}.txt"), 1))
        slots["same"] = self.same
        # Only the slots: a word no person means may hold braces of its own.
        return SLOT.sub(lambda found: slots.get(found.group(1), found.group(0)), text)


PICKS = {"name": "{fresh}", "node": "{leaf}", "port": "{reader}", "value": "1",
         "type": "float",
         "connection": "{connection}", "key": "{key}", "word=value": "log=info",
         "file": "{scrap}", "view": "{view}", "project": "{project}", "path": "{file}",
         "folder": "{group}", "recipe": "snapshot"}


def default_line(usage: Usage) -> str:
    """A usage no table names, as a node registers it: its words, and a word a kind."""
    return " ".join(usage.words + [PICKS[kind] for kind in usage.kinds])


def split_line(text: str) -> tuple[list[str], str]:
    lines = [line.strip() for line in text.split(";;")]
    return lines[:-1], lines[-1]


def placed(usage: Usage, line: str) -> list[str]:
    """The line's arguments, after the command's words."""
    return line.split()[len(usage.words):]


def variants(usage: Usage, words: Words) -> list[tuple[str, list[str], str]]:
    """A usage's lines to try, each with its lines before it: the line help means, first,
    so what the others leave behind never decides it; one short of the words, one word
    too many, each placeholder in turn given words no person means. Each takes names of
    its own, so none collides with a name a line before took."""
    template = VALID.get(usage.text) or default_line(usage)
    head = " ".join(usage.words)

    def filled() -> tuple[list[str], list[str]]:
        before, line = split_line(words.fill(template))
        return before, placed(usage, line)

    before, arguments = filled()
    tries = [("valid", before, " ".join([head, *arguments]))]
    if usage.least > 0 and len(arguments) >= usage.least:
        tries.append(("short", [], " ".join([head, *filled()[1][:usage.least - 1]])))
    if not usage.more:
        # Past the last placeholder, an optional one given too.
        full = filled()[1]
        full += [words.fill(PICKS[kind]) for kind in usage.kinds[len(full):]]
        tries.append(("extra", [], " ".join([head, *full, "zz"])))
    for at, kind in enumerate(usage.kinds):
        for token in NASTY[kind]:
            before, given = filled()
            if at >= len(given):
                break
            given[at] = words.fill(token)
            tries.append((f"{kind}:{token[:12]}", before, " ".join([head, *given])))
    return tries


# --- what a part judges ----------------------------------------------------------------

# Usages that change nothing a later line reads, but the ide's look: the state stays.
LOOKS = ("help", "ls", "info", "complete", "view list", "recipe list", "selected", "probe",
         "gpu", "clear", "menu", "find", "jump", "focus", "zoom", "mode", "select", "cd",
         "panel", "input", "image clear", "tab move", "arrange")


def names_nothing(error: str, word: str) -> bool:
    """Whether a refusal says the word itself names nothing, as no node of that name."""
    w = re.escape(word.rstrip("/"))
    return bool(re.search(rf"no \w+ is named {w}\b|no \w+ named {w}\b|\b{w} is no "
                          rf"|no recipe {w}\b|{w}/? holds no|unknown command {w}\b", error))


def looks(usage: Usage) -> bool:
    return any(usage.text == word or usage.text.startswith(word + " ") for word in LOOKS)


def usages(vulpen: Vulpen) -> list[Usage]:
    """Every usage the port knows, as a word it does not know lists them."""
    for line in vulpen.send("zz-usages"):
        if "the commands are " in line:
            return [usage_of(text) for text in
                    line.split("the commands are ", 1)[1].strip().split(", ")]
    raise Ended("no list of the commands")


def judge(copy: Copy, state: str, usage: Usage, variant: str, line: str,
          said: list[str], vulpen: Vulpen) -> None:
    errors = refusals(said)
    outcome = "refused" if errors else "ran"
    copy.tried.append(Tried(copy.part, state, usage.text, variant, line, outcome,
                            [short(text, copy.tree) for text in answers(said)]))
    fits = any("does not fit the usage" in error for error in errors)
    # A line short of a node may take the graph's selection for it, and is then refused,
    # if at all, for what it names.
    if variant in ("short", "extra") and not errors:
        copy.find(state, f"`{usage.text}` runs a line {'short of' if variant == 'short'
                  else 'past'} its usage", line, answers(said))
    if variant == "valid" and fits:
        copy.find(state, f"`{usage.text}` refuses the line its help means", line, errors)
    if variant == "valid" and not errors and not answers(said) and \
            not usage.text.startswith(SILENT):
        copy.find(state, f"`{usage.text}` works and answers nothing (RV10)", line, [])
    for error in errors:
        if BARE.search(error):
            copy.find(state, "a refusal only repeats what the standard library threw",
                      line, [error])
    # In what the log says: a command's answer may be one on purpose, as clear's.
    if raw := [text for text in said if LOGGED.match(text) and CONTROL.search(text)]:
        copy.find(state, "a control character reaches the terminal, which obeys it", line,
                  [repr(text)[:200] for text in raw[:3]])
    for what, lines in vulpen.problems:
        copy.find(state, what, line, lines)
    vulpen.problems.clear()


def try_line(copy: Copy, vulpen: Vulpen, state: State, usage: Usage, variant: str,
             before: list[str], line: str, frames: int = FRAMES_A_LINE) -> list[str]:
    """The line after those that set up what it works on, judged by its own answer; what
    the lines before said no line asked for is theirs. Each takes frames at least."""
    for first in before:
        vulpen.send(first, frames=frames)
    if before:
        vulpen.frames(1)
        for what, lines in vulpen.problems:
            copy.find(state.name, what, before[-1], lines)
        vulpen.problems.clear()
    said = vulpen.send(line, frames=frames)
    judge(copy, state.name, usage, variant, line, said, vulpen)
    return said


def damage(copy: Copy, state: State, usage: Usage, lines: list[str],
           held: dict[str, str] | None = None) -> bool:
    """Files outside the projects that a line changed, which none of the lines named; a
    recipe make writes the recipe it names into the recipes' folder, and a view save force
    an app's view.vlp there, as their help says. held is what they held before the lines,
    as Copy.held gives it, where the copy was not put back since."""
    named = [Path(word) for line in lines for word in line.split() if "/" in word]
    if any(line.startswith(("recipe make ", "view save force")) for line in lines):
        named.append(RECIPES)
    held = held or {}
    changed = [path for path, holds in copy.held().items()
               if not path.startswith("projects/") and held.get(path) != holds and
               not any((copy.tree / path).is_relative_to((copy.tree / name).resolve())
                       for name in named)]
    if changed:
        copy.find(state.name, f"`{usage.text}` changed files outside the projects",
                  lines[0] if lines else usage.text, changed[:12])
    return bool(changed)


def hosted(vulpen: Vulpen) -> list[str]:
    return [line.split()[0] for line in vulpen.answer(": view list")
            if line.strip() and not line.startswith(ERROR) and line.split()[0] != ":"]


def put_back(copy: Copy, vulpen: Vulpen, state: State) -> Vulpen:
    """The state again after an edit: the projects loaded as built, unless the start
    view or the ide was edited, or a view went that none can bring back,
    where a new vulpen starts."""
    if not vulpen.alive():
        vulpen.end()
        return copy.start(state)
    vulpen.idle()
    views = hosted(vulpen)
    if state.current not in SANDBOX or (state.ide and "ide" not in views):
        vulpen.end(force=True)
        return copy.start(state)
    for name in views:  # with force, as the lines wrote what no view save kept
        if name != "ide":
            vulpen.send(f"view close force {name}")
    if copy.put_back(build=False):
        # Code to build again: built while no vulpen runs, whose own build would race it.
        vulpen.end(force=True)
        return copy.start(state)
    said = []
    for name in state.loads:
        said += vulpen.send(f"view load {name}")
    said += vulpen.send(f"view switch {state.current}")
    for line in state.then:
        said += vulpen.send(line)
    if refusals(said):
        vulpen.end(force=True)
        return copy.start(state)
    vulpen.problems.clear()
    return vulpen


def ended(copy: Copy, state: State, vulpen: Vulpen, why: Ended) -> Vulpen:
    copy.find(state.name, "vulpen ended or hung", vulpen.last,
              [str(why), *vulpen.tail()])
    vulpen.end(force=True)
    return copy.start(state)


# --- the parts ---------------------------------------------------------------------------

def sweep(copy: Copy, states: list[State]) -> None:
    """Every usage in every state, each in its variants; the quit lines last, each in a
    vulpen of its own, quit restart carrying the state on."""
    for state in states:
        print(f"sweep: {state.name}", flush=True)
        vulpen = copy.start(state)
        if not reached(copy, state, vulpen):
            continue
        listed = usages(vulpen)
        words = Words(copy, state)
        for usage in listed:
            tried = []
            try:
                for variant, before, line in variants(usage, words):
                    if variant == "valid" and usage.words[0] == "quit":
                        continue  # each in a vulpen of its own, after the rest
                    try_line(copy, vulpen, state, usage, variant, before, line)
                    tried += [*before, line]
                hurt = damage(copy, state, usage, tried)
                if hurt or not looks(usage):
                    vulpen = put_back(copy, vulpen, state)
            except Ended as why:
                vulpen = ended(copy, state, vulpen, why)
        vulpen.end(force=True)
        for line in ("quit", "quit force", "quit restart"):
            quit_line(copy, state, line)


def reached(copy: Copy, state: State, vulpen: Vulpen) -> bool:
    """Whether a new vulpen hosts what the state names; else the part leaves it out."""
    try:
        views = hosted(vulpen)
    except Ended:
        views = []
    wanted = {*state.loads, *(["ide"] if state.ide else [])}
    if wanted <= set(views):
        return True
    copy.find(state.name, "the state could not be reached, so its lines were not tried",
              "the start", [f"hosts {', '.join(views) or 'nothing'}", *vulpen.tail()[-8:]])
    vulpen.end(force=True)
    return False


def quit_line(copy: Copy, state: State, line: str) -> None:
    usage = usage_of(line)
    vulpen = copy.start(state)
    try:
        if line == "quit restart":
            before = signature(vulpen)
            said = vulpen.restart()
            judge(copy, state.name, usage, "valid", line, said, vulpen)
            if not refusals(said):
                compare(copy, state.name, "quit restart", before, signature(vulpen))
            vulpen.end(force=True)
            return
        # It ends before its next frame, so no frame passes to wait for.
        vulpen.last = line
        vulpen.write(line + "\n")
        try:
            vulpen.process.wait(QUIT_S)
            said = vulpen.new()
        except subprocess.TimeoutExpired:
            said = vulpen.frames(FRAMES_A_LINE)
        judge(copy, state.name, usage, "valid", line, said, vulpen)
        held_work = bool(refusals(said))
        if vulpen.alive() == (line == "quit force" or not held_work):
            copy.find(state.name, f"{line} {'did not end' if vulpen.alive() else 'ended'}"
                      f" the run", line, answers(said))
        elif not vulpen.alive() and vulpen.process.returncode != 0:
            copy.find(state.name, f"{line} ended with exit {vulpen.process.returncode}",
                      line, vulpen.tail())
    except Ended as why:
        copy.find(state.name, "vulpen ended or hung", line, [str(why), *vulpen.tail()])
    vulpen.end(force=True)


def tab(copy: Copy, states: list[State]) -> None:
    """For each placeholder of each usage, what Tab offers, in each state: a kind the
    port's table says completes must offer words, and a word offered that a command
    takes as it is must not be refused as naming nothing."""
    takes = {"info [<node>]", "select [<node>...]", "view switch [<view>]", "open [<path>]",
             "view load <project>", "disconnect <connection>", "param unset <node> <key>",
             "probe stats [<node>]", "cd [<folder>]", "recipe drop <recipe> [<name>]",
             "node set <node> <word=value>...", "view close [<view>]"}
    offers: set[tuple[str, int]] = set()
    for state in states:
        print(f"tab: {state.name}", flush=True)
        vulpen = copy.start(state)
        if not reached(copy, state, vulpen):
            continue
        words = Words(copy, state)
        listed = usages(vulpen)
        commands = {tuple(each.words) for each in listed}
        for usage in listed:
            if not usage.kinds:
                continue
            try:
                before, line = split_line(words.fill(VALID.get(usage.text) or
                                                     default_line(usage)))
                arguments = placed(usage, line)
                for at, kind in enumerate(usage.kinds):
                    if kind not in COMPLETED or at > len(arguments):
                        continue
                    asked = " ".join(["complete", *usage.words, *arguments[:at], '""'])
                    offered = [word.strip() for word in vulpen.answer(asked)
                               if word.strip() and not word.startswith(ERROR)]
                    if offered and all(word.strip("[].") == f"<{kind}>"
                                       for word in offered):
                        if (usage.text, at) not in offers:
                            offers.add((usage.text, at))
                            copy.find(state.name, f"Tab offers no words for <{kind}>",
                                      asked, offered)
                        continue
                    if usage.text not in takes:
                        continue
                    # A word that makes the line another command, as force makes view
                    # close view close force, is none of the kind: tried as one, it ran
                    # that command, and the next word met what it changed.
                    values = [word for word in offered
                              if (*usage.words, *arguments[:at], word) not in commands]
                    for word in values[:3]:
                        given = [*arguments[:at], word, *arguments[at + 1:]]
                        tried = " ".join([*usage.words, *given])
                        said = try_line(copy, vulpen, state, usage, f"tab:{word}",
                                        before, tried)
                        if nothing := [error for error in refusals(said)
                                       if names_nothing(error, word)]:
                            copy.find(state.name, f"Tab offers {word!r} for <{kind}>, "
                                      f"which `{usage.text}` refuses", tried, nothing)
                    if not looks(usage):
                        vulpen = put_back(copy, vulpen, state)
            except Ended as why:
                vulpen = ended(copy, state, vulpen, why)
        vulpen.end(force=True)


def menu_lines(tree: Path) -> dict[str, list[str]]:
    sections, section = {}, ""
    for line in (tree / MENUS).read_text().splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            sections[section] = []
        elif line and not line.startswith("#") and section:
            sections[section].append(line)
    return sections


# What a right press puts for {} and {/} on each kind of thing in bf-one, with the lines
# that make it first, and a word for each placeholder a line begins for the person or a
# column lists; a path whole from the copy's top, {tree}, as the terminal they go to counts
# a path typed from the folder its prompt shows. Two nodes pressed are face and corners,
# {1} and {2}; a port is face.top, {node} its node.
PRESSED = {
    "ground": ("{tree}/projects/bf-one", "{tree}/projects/bf-one", []),
    "inside": ("corners", "{tree}/projects/bf-one/corners", ["cd corners"]),
    "level": ("{tree}/projects/bf-one/face", "{tree}/projects/bf-one", []),
    "node": ("face", "{tree}/projects/bf-one/face", []),
    "dropped": ("shot", "{tree}/projects/bf-one/shot", ["recipe drop snapshot shot"]),
    "file": ("{tree}/projects/bf-one/face/Face.frag", "{tree}/projects/bf-one/face", []),
    "folder": ("{tree}/projects/bf-one/face", "{tree}/projects/bf-one", []),
    "view": ("bf-two", "{tree}/projects/bf-two", ["view switch"]),
    "stray": ("corners.left", "{tree}/projects/bf-one/corners/left",
              ["node remove keep corners.left"]),
    "connection": ("top", "{tree}/projects/bf-one", []),
    "relation": ("{tree}/projects/bf-one/face/Face.frag", "{tree}/projects/bf-one/face",
                 []),
    "tab": ("{tree}/projects/bf-one/face/Face.frag", "{tree}/projects/bf-one/face",
            ["open {tree}/projects/bf-one/face/Face.frag"]),
    "project": ("", "", []),
    "port": ("face.top", "{tree}/projects/bf-one/face", []),
    "joined": ("face.top", "{tree}/projects/bf-one/face", []),
    "manifest": ("{tree}/projects/bf-one/view.vlp", "{tree}/projects/bf-one", []),
    "project-folder": ("{tree}/projects/bf-three", "{tree}/projects/bf-three", []),
    "pair": ("face", "{tree}/projects/bf-one/face", []),
    "nodes": ("face", "{tree}/projects/bf-one/face", []),
    "folders": ("{tree}/projects/bf-one/face", "{tree}/projects/bf-one", []),
}
# What a column lists for each placeholder the menus find themselves, in bf-one: a line
# joining two ports, whose end is joined already, so it is refused naming why; a
# connection; a value of face and one it takes; a file of face's folder, and one new
# starts there; a type a shader of face may declare a field of, and an image or a buffer
# another node writes, which such a field may read; a field a .cpp may fill, which a
# shader pressed refuses, as its field takes a type and a name; a node. Of a thing
# pressed whose node reads other params, as the snapshot dropped, the param its column
# lists, as a line naming one it reads none of is refused.
FOUND = {"connect <link>": "connect link corners.top.corner face.top",
         "<joined>": "top", "<key>": "frames_per_turn", "<value>": "300",
         "<path>": "Face.frag", "<start>": "Face.cpp", "<type>": "float",
         "<field>": "frames_per_turn", "<writer>": "corners.top.corner",
         "<node>": "corners",
         "{1}": "face", "{2}": "corners", "{node}": "face", "{view}": "bf-one"}
FOUND_ON = {"dropped": {"<key>": "wait", "<value>": "3"}}
# The kinds whose {} or {/} is a folder, which a rename or a copy moves to a folder's
# name, as a node's is, not a file's.
FOLDERS = {"node", "dropped", "folder", "stray", "project-folder", "pair", "nodes",
           "folders"}


def gui(copy: Copy, sections: list[str] | None = None, keys: bool = True) -> None:
    """Every line the menus send, on a thing of its kind in a project, and every chord of
    the keymap, which must send its line; only the menus' sections named, where named, and
    no chord without keys, as --jobs runs them as pieces."""
    state = State("gui", loads=("bf-one", "bf-two"), current="bf-one")
    vulpen = copy.start(state)
    print("gui: menus", flush=True)
    for section, lines in menu_lines(copy.tree).items():
        if sections is not None and section not in sections:
            continue
        tree = str(copy.tree)
        name, folder, before = PRESSED.get(section, ("face", "{tree}/projects/bf-one", []))
        name, folder = name.replace("{tree}", tree), folder.replace("{tree}", tree)
        before = [made.replace("{tree}", tree) for made in before]
        for line in lines:
            # A line naming {image} shows on an image a node writes alone, and bf-one's
            # ports pressed are buffers, so no menu offers it there.
            if "{image}" in line:
                continue
            sent = line
            for found, word in (FOUND | FOUND_ON.get(section, {})).items():
                sent = sent.replace(found, word)
            sent = (sent.replace("{/}", folder).replace("{}", name)
                    .replace("<name>", copy.name()).replace("<recipe>", "snapshot")
                    .replace("<file>", f"{tree}/projects/bf-one/{copy.name()}"
                             + ("" if section in FOLDERS else ".txt"))
                    .replace("<project>", "bf-three").replace("<view>", "bf-two"))
            usage = usage_of(f"menu [{section}] {line}")
            try:
                said = try_line(copy, vulpen, state, usage, f"menu:{section}", before, sent)
                for error in refusals(said):
                    if "does not fit" in error or "unknown command" in error or \
                            NOTHING.search(error):
                        copy.find(state.name, f"a [{section}] menu line refused", sent,
                                  [error])
                damage(copy, state, usage, [*before, sent])
                vulpen = put_back(copy, vulpen, state)
            except Ended as why:
                vulpen = ended(copy, state, vulpen, why)
    if keys:
        vulpen = chords(copy, state, vulpen)
    vulpen.end(force=True)


def chords(copy: Copy, state: State, vulpen: Vulpen) -> Vulpen:
    """Every chord of the keymap, which must send its line."""
    print("gui: keys", flush=True)
    for line in (copy.tree / KEYMAP).read_text().splitlines():
        if "=" not in line or line.lstrip().startswith("#"):
            continue
        chord, sends = (part.strip() for part in line.rsplit(" = ", 1))
        keys = chord.split("+") if chord != "control++" else ["control", "+"]
        if chord.endswith("+="):
            keys = chord[:-2].split("+") + ["="]
        down = [f"input key down {key}" for key in keys]
        up = [f"input key up {key}" for key in reversed(keys)]
        try:
            said = vulpen.send(*down) + vulpen.send(*up)
            usage = usage_of(f"chord {chord}")
            judge(copy, state.name, usage, "chord", chord, said, vulpen)
            if not any(f"command: {sends}" in text for text in said):
                copy.find(state.name, f"the chord {chord} did not send {sends!r}", chord,
                          answers(said))
        except Ended as why:
            vulpen = ended(copy, state, vulpen, why)
    return vulpen


def signature(vulpen: Vulpen) -> dict[str, list[str]]:
    """The views hosted, and each one's folders, nodes and their words and connections,
    as the lines that read them answer: what a replay must come back to."""
    views = hosted(vulpen)
    found = {"views": views}
    for view in ["", *views]:
        found[f"{view}: ls"] = vulpen.answer(f"{view}: ls")
        nodes = [word.strip() for word in vulpen.answer(f'{view}: complete info ""')
                 if word.strip() and not word.startswith(ERROR)]
        for node in nodes:
            found[f"{view}: info {node}"] = vulpen.answer(f"{view}: info {node}")
    return found


def compare(copy: Copy, state: str, what: str, before: dict, after: dict) -> None:
    differs = [key for key in sorted(before.keys() | after.keys())
               if before.get(key) != after.get(key)]
    if differs:
        said = []
        for key in differs[:6]:
            said += [f"{key}:", *(f"  before: {line}" for line in before.get(key, ["-"])[:6]),
                     *(f"  after:  {line}" for line in after.get(key, ["-"])[:6])]
        copy.find(state, f"after {what} the views are not as they were "
                  f"({len(differs)} answers differ)", what, said)


# Usages a walk leaves out: those that end the run, input a frame's timing decides, and
# those that only look.
UNWALKED = ("quit", "input", "help", "clear", "complete", "probe", "menu", "focus", "zoom",
            "panel", "mode", "gpu", "log save", "source", "jump", "find", "arrange")
# Usages whose lines write files: the log keeps the graph edits they make, as recipe drop's
# node add, not the files, so a log replayed where they are not rebuilds the graph alone.
# A declare writes its field into a shader or a .cpp, as the file's own line.
WRITES = ("new", "remove", "rename", "recipe", "view new", "view save",
          "view rename", "view remove", "copy", "write", "param declare",
          "connect declare")


def walk(copy: Copy, seed: int, steps: int) -> None:
    """Random lines, from the seed, in a project with two more hosted, then quit restart,
    which must bring the same views back. Every second walk, of lines that only edit the
    graph, has its log replayed in a new vulpen too, which must come back to the same
    views and log the same lines (V08)."""
    chance = random.Random(seed)
    graph_only = seed % 2 == 0
    state = State(f"walk {seed}", loads=("bf-one", "bf-two", "bf-three"), current="bf-one")
    print(f"walk: seed {seed}, {steps} lines{', the graph only' if graph_only else ''}",
          flush=True)
    vulpen = copy.start(state)
    walked = [usage for usage in usages(vulpen)
              if not any(usage.text.startswith(word) for word in UNWALKED)
              and not (graph_only and any(usage.text.startswith(word) for word in WRITES))]
    current = state.current
    try:
        for _ in range(steps):
            usage = chance.choice(walked)
            words = Words(copy, State(state.name, loads=state.loads,
                                      current=current if current in SANDBOX else ""))
            tries = variants(usage, words)
            variant, before, line = (tries[0] if chance.random() < 0.8
                                     else chance.choice(tries))
            # The connection that takes the ide's window as an image stands in for a
            # display, which no person's run has; the sweep tries it.
            if any("snapshot" in text for text in [*before, line]):
                continue
            held = copy.held()
            said = try_line(copy, vulpen, state, usage, variant, before, line,
                            WALK_FRAMES)
            # node remove keep leaves the node's folder, as its help says, which every
            # read of the view since warns is no node's (RV08): what the line asked for.
            if (words := line.split())[:3] == ["node", "remove", "keep"] and \
                    len(words) == 4 and not refusals(said):
                vulpen.asked.append(f"folder {words[3].replace('.', '/')}/ is no node's")
            for text in said:
                if found := re.search(r"command: view switch ?(\S*)$", text):
                    current = found.group(1)
                elif found := re.search(r"command: view close (\S+)$", text):
                    current = "" if found.group(1) == current else current
            damage(copy, state, usage, [*before, line], held)
        vulpen.idle()
        before = signature(vulpen)
        log = copy.said.parent / f"walk-{seed}.log"
        vulpen.send(f"log save {log}")
        said = vulpen.restart()
        # A node's own problem, as a port a line disconnected, came with the views; only
        # a line the restart refused is its.
        if errors := [error for error in refusals(said)
                      if "restart" in error and not BOUND.search(error)]:
            copy.find(state.name, "quit restart refused a line it carried", "quit restart",
                      errors)
        vulpen.idle()
        compare(copy, state.name, "quit restart", before, signature(vulpen))
        vulpen.end(force=True)
    except Ended as why:
        copy.find(state.name, "vulpen ended or hung", vulpen.last, [str(why), *vulpen.tail()])
        vulpen.end(force=True)
        return
    if not graph_only:
        return
    copy.put_back()
    replay = Vulpen(copy.tree, copy.said, log.read_text().splitlines())
    try:
        said = replay.frames(3)
        if errors := [error for error in refusals(said) if not BOUND.search(error)]:
            copy.find(state.name, "the walk's log refused a line as it replayed",
                      f"--source {log.name}", errors)
        replay.idle()
        compare(copy, state.name, "the log's replay", before, signature(replay))
        again = copy.said.parent / f"walk-{seed}-again.log"
        replay.send(f"log save {again}")
        if again.read_text() != log.read_text():
            copy.find(state.name, "the replay's log differs from the walk's", log.name,
                      [line for line in again.read_text().splitlines()
                       if line not in log.read_text().splitlines()][:10])
    except Ended as why:
        copy.find(state.name, "vulpen ended or hung replaying the walk's log", log.name,
                  [str(why), *replay.tail()])
    replay.end(force=True)


@dataclass
class Seen:
    """A sequence a person ran, in a state, and the views it should leave hosted, with
    the current one; a line @restart is quit restart, carried through."""
    what: str
    state: State
    lines: list[str]
    views: tuple[str, ...]
    refused: str | None = None   # what a line of it should be refused for


PROJECT = State("project", loads=("bf-one",), current="bf-one")
SEEN = [
    Seen("gpu toggle, then quit restart, as in your run 154552Z", PROJECT,
         ["gpu toggle", "@restart"], ("ide", "bf-one")),
    Seen("the ide closed, then view load ide", State("ide"),
         ["view close ide", f"view load ide ;; {SNAPSHOT}"], ("ide",)),
    Seen("quit restart twice", PROJECT, ["@restart", "@restart"], ("ide", "bf-one")),
    Seen("view rename, then quit restart", PROJECT,
         ["view rename bf-renamed", "@restart"], ("ide", "bf-renamed")),
    Seen("view save as, then quit restart", PROJECT,
         ["view save as bf-saved", "@restart"], ("ide", "bf-saved")),
    Seen("quit restart inside a node", State("inside", loads=("bf-one",),
                                             current="bf-one", then=("cd corners",)),
         ["@restart"], ("ide", "bf-one")),
    Seen("quit restart while the editor holds changes",
         next(state for state in STATES if state.name == "editing"), ["@restart"],
         ("ide", "bf-two"), refused="would lose"),
    Seen("a project hosted twice, under another name", PROJECT,
         ["view add other {tree}/projects/bf-one/view.vlp"], ("ide", "bf-one"),
         refused="bf-one"),
    Seen("a project hosted from a script by ../, beside the script", State("ide"),
         ["source ../decoy/host.txt"], ("ide", "bf-x")),
    Seen("the ide closed, then quit restart", State("ide"),
         ["view close ide", "@restart"], ()),
    Seen("view close of the current view, then view load", PROJECT,
         ["view close bf-one", "view load bf-one"], ("ide", "bf-one")),
    Seen("view rename with the ide current", State("ide", then=("view switch ide",)),
         ["view rename bf-ide"], ("ide",), refused="app"),
    Seen("view save as with the ide current", State("ide", then=("view switch ide",)),
         ["view save as bf-ide"], ("ide",), refused="app"),
    Seen("view load cli, the view vulpen started with", State("ide"), ["view load cli"],
         ("ide",), refused="cli"),
]


def seen(copy: Copy) -> None:
    (copy.pristine / "decoy" / "host.txt").write_text(
        "view add bf-x ../Vulpen/projects/bf-three/view.vlp\n")
    for sequence in SEEN:
        print(f"seen: {sequence.what}", flush=True)
        vulpen = copy.start(sequence.state)
        said = []
        try:
            for line in sequence.lines:
                said += (vulpen.restart() if line == "@restart" else
                         vulpen.send(*(part.strip().replace("{tree}", str(copy.tree))
                                       for part in line.split(";;"))))
                usage = usage_of("quit restart" if line == "@restart" else line)
                copy.tried.append(Tried("seen", sequence.what, usage.text, "seen", line,
                                        "refused" if refusals(said) else "ran",
                                        [short(text, copy.tree) for text in answers(said)]))
            vulpen.idle()
            views = tuple(hosted(vulpen))
            errors = refusals(said)
            if sequence.refused is None and errors:
                copy.find(sequence.what, "a line was refused", " ;; ".join(sequence.lines),
                          errors)
            elif sequence.refused and not any(sequence.refused in error for error in errors):
                copy.find(sequence.what, f"no refusal naming {sequence.refused!r}",
                          " ;; ".join(sequence.lines), errors or answers(said)[-6:])
            if set(views) != set(sequence.views):
                copy.find(sequence.what, f"hosts {', '.join(views) or 'nothing'}, not "
                          f"{', '.join(sequence.views) or 'nothing'}",
                          " ;; ".join(sequence.lines), errors)
            for what, lines in vulpen.problems:
                copy.find(sequence.what, what, " ;; ".join(sequence.lines), lines)
        except Ended as why:
            copy.find(sequence.what, "vulpen ended or hung", vulpen.last,
                      [str(why), *vulpen.tail()])
        vulpen.end(force=True)


# --- pieces side by side (--jobs) --------------------------------------------------------

@dataclass
class Piece:
    """What --jobs hands one copy whole; once it ran to its end, what it found and tried,
    and until then why it did not."""
    name: str
    part: str
    run: Callable[[Copy], None]
    copy: int = 0
    findings: list[Finding] = field(default_factory=list)
    tried: list[Tried] | None = None
    lost: str = "no copy reached it"


def split(sections: dict[str, list[str]], count: int) -> list[list[str]]:
    """The sections in at most count runs of about as many lines each, in their order."""
    total = sum(len(lines) for lines in sections.values())
    runs, run, held = [], [], 0
    for section, lines in sections.items():
        run.append(section)
        held += len(lines)
        if len(runs) < count - 1 and held >= total * (len(runs) + 1) / count:
            runs, run = runs + [run], []
    return runs + [run] if run else runs


def pieces(parts: list[str], states: list[State]) -> list[Piece]:
    """The parts as pieces, in the order one copy would run them: a state's sweep, a run
    of the menus' sections, the keys."""
    made = []
    for part in parts:
        if part == "sweep":
            made += [Piece(f"sweep {state.name}", part,
                           lambda copy, state=state: sweep(copy, [state]))
                     for state in states]
        elif part == "gui":
            sections = menu_lines(ROOT)
            runs = split(sections, MENU_PIECES)
            if [section for run in runs for section in run] != list(sections):
                sys.exit("the menus' pieces do not hold each section once")
            made += [Piece(f"gui menus {at}", part,
                           lambda copy, run=run: gui(copy, run, keys=False))
                     for at, run in enumerate(runs, 1)]
            made.append(Piece("gui keys", part, lambda copy: gui(copy, [], keys=True)))
        else:
            sys.exit(f"--jobs runs the sweep and the gui as pieces, not {part}: run {part} "
                     "with --jobs 1")
    return made


def side_by_side(made: list[Piece], copies: int, report: Path, keep: bool) -> None:
    """Each piece in a copy, copy k taking every copies-th from the k-th, so each runs in
    the same copy every run (C01); the copies built at once, sharing the machine's
    threads. A copy that fails leaves the pieces it had not ended lost, saying why."""
    jobs = str(max(1, (os.cpu_count() or 1) // copies))

    def work(k: int) -> None:
        mine = made[k::copies]
        tree = Path(tempfile.mkdtemp(prefix="command-sweep-")) / "Vulpen"
        print(f"copy {k + 1}: copying and building the tree in {tree}", flush=True)
        try:
            copy_tree(tree, jobs)
            copy = Copy(tree, report.with_name(f"{report.stem}-{k + 1}{report.suffix}"))
            for piece in mine:
                print(f"copy {k + 1}: {piece.name}", flush=True)
                piece.copy, found, tried = k + 1, len(copy.findings), len(copy.tried)
                copy.part = piece.part
                piece.run(copy)
                piece.findings, piece.tried = copy.findings[found:], copy.tried[tried:]
        except BaseException as error:  # SystemExit too, as a copy that did not build
            why = "".join(traceback.format_exception_only(error)).strip()
            for piece in mine:
                piece.lost = f"copy {k + 1} ended: {why}"
        finally:
            if keep:
                print(f"copy {k + 1} stays in {tree}", flush=True)
            else:
                shutil.rmtree(tree.parent, ignore_errors=True)

    workers = [threading.Thread(target=work, args=(k,), daemon=True) for k in range(copies)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()


def merged(made: list[Piece]) -> tuple[list[Finding], list[Tried]]:
    """What the pieces found and tried, in their order; a piece that did not run to its
    end, or tried no line, is a finding, so no piece passes by being lost."""
    findings = [finding for piece in made for finding in piece.findings]
    tried = [line for piece in made for line in piece.tried or []]
    for piece in made:
        if piece.tried is None:
            findings.append(Finding(piece.part, piece.name, "a piece did not run to its end, "
                                    "so the lines it holds were not all tried", piece.name,
                                    piece.lost.splitlines()[-TAIL_LINES:]))
        elif not piece.tried:
            findings.append(Finding(piece.part, piece.name, "a piece tried no line",
                                    piece.name))
    return findings, tried


def room() -> int:
    """The copies the machine's free memory holds above LEAST_FREE_KB, at least one."""
    return max(1, (free_kb() - LEAST_FREE_KB) // COPY_KB)


def pieced(arguments: argparse.Namespace, parts: list[str], states: list[State],
           started: float, commit: str) -> int:
    if arguments.tree:
        sys.exit("--tree reuses one copy, which --jobs 1 runs")
    made = pieces(parts, states)
    copies = min(arguments.jobs, len(made))
    if (fits := room()) < copies:
        print(f"{fits} copies, not {copies}: the machine has {free_kb() // 1024} MiB free",
              flush=True)
        copies = fits
    # The threads lavapipe draws a vulpen with: each copy's shares the machine's.
    os.environ.setdefault("LP_NUM_THREADS", str(max(1, (os.cpu_count() or 1) // copies)))
    report = arguments.report.resolve()
    report.parent.mkdir(parents=True, exist_ok=True)
    print(f"{len(made)} pieces in {copies} copies side by side", flush=True)
    side_by_side(made, copies, report, arguments.keep)
    findings, tried = merged(made)
    write_report(findings, tried, report, time.monotonic() - started, commit, made)
    print(f"{len(tried)} lines, {len(findings)} findings; the report is {report}, every "
          f"line in {report.with_suffix('.tsv')}, what each copy's vulpen said in "
          f"{report.with_name(report.stem + '-<copy>.log')}")
    for finding in findings[:40]:
        print(f"  {finding.part}, {finding.state}: {finding.what}: {finding.line[:120]}")
    return 1 if findings else 0


# --- the report --------------------------------------------------------------------------

def write_report(findings: list[Finding], tried_all: list[Tried], report: Path,
                 took: float, commit: str, made: list[Piece] | None = None) -> None:
    by_what: dict[str, list[Finding]] = {}
    for finding in findings:
        by_what.setdefault(finding.what, []).append(finding)
    text = [f"# command-sweep at {commit}", "",
            f"{len(tried_all)} lines tried, {len(findings)} findings, "
            f"in {took / 60:.1f} minutes.", "", "## Findings", ""]
    for what, found in by_what.items():
        text += [f"### {what} ({len(found)})", ""]
        for finding in found:
            text.append(f"- {finding.part}, {finding.state}: `{finding.line[:160]}`")
            text += [f"    - {line[:300]}" for line in finding.said[:8]]
        text.append("")
    if made:
        text += ["## Pieces", "", "| piece | copy | lines tried |", "|---|--:|--:|"]
        text += [f"| {piece.name} | {piece.copy or '-'} | "
                 f"{len(piece.tried) if piece.tried is not None else 'did not run'} |"
                 for piece in made]
        text.append("")
    states = sorted({tried.state for tried in tried_all if tried.part == "sweep"},
                    key=lambda name: [state.name for state in STATES].index(name)
                    if name in [state.name for state in STATES] else 99)
    text += ["## The lines help means, by state", "",
             "| usage | " + " | ".join(states) + " |",
             "|---|" + "---|" * len(states)]
    rows: dict[str, dict[str, str]] = {}
    for tried in tried_all:
        if tried.part == "sweep" and tried.variant == "valid":
            why = next((line for line in tried.said if line.startswith(ERROR)), "")
            rows.setdefault(tried.usage, {})[tried.state] = (
                "ran" if tried.outcome == "ran" else
                "refused: " + why.removeprefix(ERROR).strip()[:70].replace("|", "/"))
    for usage, cells in rows.items():
        text.append(f"| `{usage}` | " + " | ".join(cells.get(state, "")
                                                   for state in states) + " |")
    report.write_text("\n".join(text) + "\n")
    with report.with_suffix(".tsv").open("w") as table:
        table.write("part\tstate\tusage\tvariant\tline\toutcome\tsaid\n")
        for tried in tried_all:
            table.write("\t".join([tried.part, tried.state, tried.usage, tried.variant,
                                   tried.line[:300], tried.outcome,
                                   " / ".join(tried.said)[:600]]).replace("\n", " ") + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--only", help="the parts to run: " + ", ".join(PARTS))
    parser.add_argument("--states", help="the states the sweep and tab run in: " +
                        ", ".join(state.name for state in STATES))
    parser.add_argument("--seed", type=int, default=1, help="the first walk's seed")
    parser.add_argument("--walks", type=int, default=3)
    parser.add_argument("--steps", type=int, default=150, help="lines a walk")
    parser.add_argument("--tree", type=Path, help="a copy a run before kept, to reuse")
    parser.add_argument("--keep", action="store_true", help="keep the copy, to reuse")
    parser.add_argument("--report", type=Path, default=ROOT / "out" / "command-sweep.md")
    parser.add_argument("--jobs", type=int, default=1,
                        help="copies side by side, running the sweep and the gui as pieces")
    arguments = parser.parse_args()
    parts = arguments.only.split(",") if arguments.only else list(PARTS)
    states = [state for state in STATES if not arguments.states
              or state.name in arguments.states.split(",")]
    started = time.monotonic()
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                            capture_output=True, text=True).stdout.strip()
    if arguments.jobs > 1:
        sys.exit(pieced(arguments, parts, states, started, commit))
    if arguments.tree:
        tree = arguments.tree.resolve()
    else:
        tree = Path(tempfile.mkdtemp(prefix="command-sweep-")) / "Vulpen"
        print(f"copying and building the tree in {tree}", flush=True)
        copy_tree(tree)
    report = arguments.report.resolve()
    report.parent.mkdir(parents=True, exist_ok=True)
    copy = Copy(tree, report)
    try:
        for part in parts:
            copy.part = part
            if part == "sweep":
                sweep(copy, states)
            elif part == "tab":
                tab(copy, states)
            elif part == "gui":
                gui(copy)
            elif part == "seen":
                seen(copy)
            elif part == "walk":
                for seed in range(arguments.seed, arguments.seed + arguments.walks):
                    walk(copy, seed, arguments.steps)
    finally:
        write_report(copy.findings, copy.tried, report, time.monotonic() - started, commit)
        if arguments.keep or arguments.tree:
            print(f"the copy stays in {tree}")
        else:
            shutil.rmtree(tree.parent, ignore_errors=True)
    print(f"{len(copy.tried)} lines, {len(copy.findings)} findings; the report is "
          f"{report}, every line in {report.with_suffix('.tsv')}, what vulpen said in "
          f"{copy.said}")
    for finding in copy.findings[:40]:
        print(f"  {finding.part}, {finding.state}: {finding.what}: {finding.line[:120]}")
    sys.exit(1 if copy.findings else 0)


if __name__ == "__main__":
    main()
