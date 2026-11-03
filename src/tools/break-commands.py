#!/usr/bin/env python3
"""Try to break every command of the cli and the ide (V06), as a person might, and as no
person means to: each usage the command port lists, in each state a person reaches, with
the words its help names, with none, with one too many, and with words no person means
in each place; the words Tab offers for each placeholder; seeded walks of random lines,
each then carried through quit restart, and every second one, of lines that only edit
the graph, replayed from its log in a new vulpen, which must both come back to the same
views (V08); the lines the ide's menus and keys send; and sequences a person was seen
to run.

The states: the cli alone; the ide hosted; a project loaded; two, the first current; the
view vulpen started with current, a project hosted; the graph inside a node; a file open
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

Usage: python3 src/tools/break-commands.py [--only PART,...] [--states NAME,...]
           [--seed N] [--walks N] [--steps N] [--tree DIR] [--keep] [--report FILE]
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
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tests"))
from harness import PROBLEM  # noqa: E402 (the tests' folder is no package)

ROOT = Path(__file__).resolve().parents[2]
CLI = Path("src/runtime/recipes/cli/view.vlp")
IDE = Path("src/runtime/recipes/ide/view.vlp")
MENUS = Path("src/runtime/recipes/library/parts/menus/menu.ini")
KEYMAP = Path("src/runtime/recipes/library/parts/keys/keymap.ini")
GPU_CHOICE = Path("src/runtime/recipes/cli/.vulpen/gpu.ini")
PARTS = ("sweep", "tab", "walk", "gui", "seen")
SIZE = "1280x720"
# The ide's window taken as an image, from the start or the frame it comes back, as no
# display is here to open it on.
SNAPSHOT = "connect snapshot ide: cli.snapshot.picture"
TIMEOUT_S = 90         # for a frame to pass, or a restart's build
POLL_S = 0.005
FRAMES_A_LINE = 2      # at least, from one line to the next
SETTLE_S = 0.4         # between looks at whether the live build is idle
SETTLE_ROUNDS = 150
QUIT_S = 5
TAIL_LINES = 12
QUIET = ("help zz-", "{mem}", "{nod}", "{gpu}", "{dbg}")
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


# A warning a line asked for: the GPU the next launch takes, and the modes part's answer to
# perform mode with nothing presented, a frame on (Q4 of the first report: keep).
EXPECTED_WARNINGS = re.compile(r"the next launch prefers|nothing is presented to perform")
# A byte a terminal obeys rather than shows, as an escape sequence: never in what it says.
CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")
# Memory left on the machine below which, or held by vulpen above which, the run is
# killed: a line asking for more than a GPU holds can take the machine down with it.
LEAST_FREE_KB = 6 * 1024 * 1024
MOST_HELD_KB = 8 * 1024 * 1024


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


SANDBOX = {
    "bf-one": Project("triangles/triangle-points",
                      ("corners", "corners.top", "corners.right", "corners.left", "face"),
                      "corners.left", "corners", ("face", "frames_per_turn", "300"),
                      "top", "corners.top.corner", "face.top",
                      ("face/Face.frag", "face/Face.vert", "corners/top/Point.comp")),
    "bf-two": Project("life", ("seed-cells", "engine", "cells"), "cells", "engine",
                      ("engine", "frames_per_generation", "4"), "cells", "engine.cells",
                      "cells.cells", ("engine/Engine.cpp", "engine/Engine.comp",
                                      "cells/Cells.frag")),
    "bf-three": Project("wave", ("wave", "probe"), "probe", "wave",
                        ("wave", "speed", "0.03"), "values", "wave.values",
                        "probe.values", ("wave/Wave.cpp", "probe/Probe.comp")),
}
SCRAP = "scrap.txt"    # a file at each project's top, which no node holds


@dataclass(frozen=True)
class State:
    """A state a person reaches from a new vulpen: the ide hosted or not, projects loaded,
    the view current (none: the view vulpen started with), lines run after, and whether
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
    the last may take one or more, or be left out."""
    text: str
    words: list[str]
    kinds: list[str]
    more: bool = False
    optional: bool = False

    @property
    def least(self) -> int:
        return len(self.kinds) - self.optional


def usage_of(text: str) -> Usage:
    words, kinds, more, optional = [], [], False, False
    for word in text.split():
        if word.startswith("[") and word.endswith("]"):
            optional, word = True, word[1:-1]
        if word.endswith("..."):
            more, word = True, word[:-3]
        if word.startswith("<") and word.endswith(">"):
            kinds.append(word[1:-1])
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
            mark = f"help zz-{self.marks} does not fit"
            self.write(f"help zz-{self.marks}\n")
            deadline = time.monotonic() + timeout
            while True:
                lines = self.new()
                done = any(mark in line for line in lines)
                said += [line for line in lines if "help zz-" not in line]
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
            elif WARNING in line and not EXPECTED_WARNINGS.search(line):
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

    def name(self, start: str = "bfn") -> str:
        self.fresh += 1
        return f"{start}{self.fresh}"

    def find(self, state: str, what: str, line: str, said: list[str]) -> None:
        self.findings.append(Finding(self.part, state, what, line,
                                     [short(text, self.tree) for text in said]))

    def put_back(self) -> None:
        """The copy as built: its files as committed, the projects and the decoy as they
        were made, nothing a run keeps in .vulpen/, so the next state starts alike."""
        subprocess.run(["git", "checkout", "-q", "--", "."], cwd=self.tree, check=True)
        subprocess.run(["git", "clean", "-fdxq", "-e", "/out", "-e", "/projects/bf-*"],
                       cwd=self.tree, check=True)
        for folder in (self.tree / "projects").glob("bf*"):
            if folder.name not in SANDBOX or held(folder) != held(self.pristine /
                                                                  folder.name):
                shutil.rmtree(folder)
        for name in SANDBOX:
            if not (self.tree / "projects" / name).exists():
                shutil.copytree(self.pristine / name, self.tree / "projects" / name)
        if held(self.decoy) != held(self.pristine / "decoy"):
            shutil.rmtree(self.decoy, ignore_errors=True)
            shutil.copytree(self.pristine / "decoy", self.decoy)

    def changed(self) -> list[str]:
        """The files outside the projects a run changed: what git sees, but what the
        parts keep beside views and the statistics a build rewrites."""
        status = subprocess.run(["git", "status", "--porcelain", "-uall"], cwd=self.tree,
                                capture_output=True, text=True, check=True).stdout
        return [line[3:] for line in status.splitlines()
                if ".vulpen/" not in line and "docs/statistics/" not in line]

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
    """A folder's files, by path, as their contents' hashes, but what .vulpen/ keeps."""
    if not folder.is_dir():
        return {}
    return {path.relative_to(folder).as_posix(): hashlib.sha1(path.read_bytes()).hexdigest()
            for path in sorted(folder.rglob("*"))
            if path.is_file() and ".vulpen" not in path.parts}


def copy_tree(to: Path) -> None:
    """The tree's files as they are, committed or not, but what it ignores, as a git
    repository of their own, which the build stamps its commit from and the graph finds
    the top by; the editor's program a stand-in; the projects of SANDBOX; built."""
    listed = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "-z"],
                            cwd=ROOT, capture_output=True, check=True).stdout
    for name in filter(None, listed.decode().split("\0")):
        source = ROOT / name
        if source.is_file() and not name.startswith("projects/bf"):
            (to / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, to / name)
    editor = to / "src/runtime/recipes/library/parts/editor/view.vlp"
    editor.write_text(editor.read_text().replace("program=code", "program=true"))
    git = ["git", "-c", "user.name=break-commands", "-c", "user.email=break@localhost"]
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
                    ["cmake", "--build", "--preset", "debug", "-j", str(os.cpu_count())]):
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
    "connect <name> <port> <port>...":
        "disconnect {connection} ;; connect {fresh} {writer} {reader}",
    "disconnect <connection>": "disconnect {connection}",
    "file copy <file> <file>": "file copy {scrap} {newfile}",
    "file remove <file>": "file remove {scrap}",
    "file remove force <file>": "file remove force {tree}/../decoy/folder/",
    "file rename <file> <file>": "file rename {scrap} {newfile}",
    "gpu": "gpu",
    "gpu toggle": "gpu toggle",
    "help": "help",
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
    "log save <file>": "log save {tree}/../decoy/{fresh}.log",
    "ls": "ls",
    "node add <name> [<word=value>...]": "node add {fresh} log=info",
    "node move <node> <node>": "node move {leaf} {group}",
    "new <file>...": "new {tree}/projects/{home}/{fresh}/ "
                     "{tree}/projects/{home}/{fresh}/Shape.vert "
                     "{tree}/projects/{home}/{same}/Shape.frag",
    "node remove <node>": "node remove {leaf}",
    "node remove keep <node>": "node remove keep {leaf}",
    "node rename <node> <name>": "node rename {leaf} {fresh}",
    "node set <node> <word=value>...": "node set {leaf} log=info",
    "open [<path>]": "open {file}",
    "param set <node> <key> <value>": "param set {pnode} {key} {value}",
    "param unset <node> <key>": "param unset {pnode} {key}",
    "probe stats [<node>]": "probe stats",
    "quit": "quit",
    "quit force": "quit force",
    "quit restart": "quit restart",
    "recipe drop <recipe> <name>": "recipe drop snapshot {fresh}",
    "recipe list": "recipe list",
    "recipe sync <node>": "recipe drop snapshot {fresh} ;; recipe sync {same}",
    "source <file>": "source {tree}/../decoy/script.txt",
    "view add <name> <file>": "view add {fresh} {tree}/../decoy/{same}/view.vlp",
    "view close <view>": "view close {view}",
    "view list": "view list",
    "view load <project>": "view load {project}",
    "view new <project>": "view new {freshproject}",
    "view remove <project>": "view remove {project}",
    "view rename <name>": "view rename {freshproject}",
    "view save": "view save",
    "view save as <name>": "view save as {freshproject}",
    "view switch [<view>]": "view switch {view}",
    "arrange": "arrange",
    "cd [<path>]": "cd {group}",
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
    "panel hide <node>": "panel hide ide.editor-panel ;; panel show ide.editor-panel",
    "panel maximize <node>": "panel maximize ide.graph-panel ;; panel maximize ide.graph-panel",
    "panel move <node> <node> <value>": "panel move ide.perform-panel ide.graph-panel bottom",
    "panel restore": "panel restore",
    "panel show <node>": "panel show ide.editor-panel",
    "select [<node>...]": "select {leaf}",
    "select find <value>...": "select find {leaf}",
    "selected": "selected",
    "tab move <file> <file>": "open {file} ;; open {file2} ;; tab move {file2} {file}",
    "write": "open {file} ;; focus ide.editor.text ;; input text // ;; write",
    "write force": "open {file} ;; focus ide.editor.text ;; input text // ;; write force",
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
    "value": ["nan", "inf", "-1", "1e30", "-0", "0x10", "1,2,3,4,5", LONG, "ñ", "{}", "#",
              "=", ":"],
    "file": ["{tree}/projects/{home}/nothing/x.txt", "{tree}/projects/{home}/view.vlp/x",
             "{tree}/projects/", "./", "nothing/", "{tree}/projects/{home}/../bf-three/"
             + SCRAP, "{tree}/projects/{home}/abs.txt", "{tree}/../decoy/binary.bin",
             "{tree}/projects/{home}/" + LONG],
    "view": ["nobody", "library", "cli", LONG, ":"],
    "project": ["ide", "cli", "Bad", "nobody", "{tree}/../decoy",
                "{tree}/src/runtime/recipes/ide/", "{tree}/projects/{home}/view.vlp",
                LONG],
    "path": ["nobody.txt", "Point.comp", "{tree}/../decoy/binary.bin",
             "{tree}/../decoy/folder/", LONG],
    "recipe": ["nothing", "../parts", "library", "projects", LONG],
}
SLOT = re.compile(r"\{([a-z0-9]+)\}")
# Kinds whose completion, the command port's own table says, offers words.
COMPLETED = ("node", "port", "connection", "word=value", "key", "file", "view", "project",
             "path", "recipe")


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
            "key": key, "value": value,
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
         "connection": "{connection}", "key": "{key}", "word=value": "log=info",
         "file": "{scrap}", "view": "{view}", "project": "{project}", "path": "{file}",
         "recipe": "snapshot"}


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
             before: list[str], line: str) -> list[str]:
    """The line after those that set up what it works on, judged by its own answer; what
    the lines before said no line asked for is theirs."""
    for first in before:
        vulpen.send(first)
    if before:
        vulpen.frames(1)
        for what, lines in vulpen.problems:
            copy.find(state.name, what, before[-1], lines)
        vulpen.problems.clear()
    said = vulpen.send(line)
    judge(copy, state.name, usage, variant, line, said, vulpen)
    return said


def damage(copy: Copy, state: State, usage: Usage, lines: list[str]) -> bool:
    """Files outside the projects that a line changed, which none of the lines named."""
    named = [Path(word) for line in lines for word in line.split() if "/" in word]
    changed = [path for path in copy.changed() if not path.startswith("projects/") and
               not any((copy.tree / path).is_relative_to((copy.tree / name).resolve())
                       for name in named)]
    if changed:
        copy.find(state.name, f"`{usage.text}` changed files outside the projects",
                  lines[0] if lines else usage.text, changed[:12])
    return bool(changed)


def hosted(vulpen: Vulpen) -> list[str]:
    return [line.split()[0] for line in vulpen.answer(": view list")
            if line.strip() and not line.startswith(ERROR)]


def put_back(copy: Copy, vulpen: Vulpen, state: State) -> Vulpen:
    """The state again after an edit: the projects loaded as built, unless the view
    vulpen started with or the ide was edited, or a view went that none can bring back,
    where a new vulpen starts."""
    if not vulpen.alive():
        vulpen.end()
        return copy.start(state)
    vulpen.idle()
    views = hosted(vulpen)
    if state.current not in SANDBOX or (state.ide and "ide" not in views):
        vulpen.end(force=True)
        return copy.start(state)
    for name in views:
        if name != "ide":
            vulpen.send(f"view close {name}")
    copy.put_back()
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
             "probe stats [<node>]", "cd [<path>]", "recipe drop <recipe> <name>",
             "node set <node> <word=value>...", "view close <view>"}
    offers: set[tuple[str, int]] = set()
    for state in states:
        print(f"tab: {state.name}", flush=True)
        vulpen = copy.start(state)
        if not reached(copy, state, vulpen):
            continue
        words = Words(copy, state)
        for usage in usages(vulpen):
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
                    for word in offered[:3]:
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
# that make it first, and a word for each placeholder a line begins for the person; a path
# whole from the copy's top, {tree}, as the terminal they go to counts a path typed from
# the folder its prompt shows.
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
}


def gui(copy: Copy) -> None:
    """Every line the menus send, on a thing of its kind in a project, and every chord of
    the keymap, which must send its line."""
    state = State("gui", loads=("bf-one", "bf-two"), current="bf-one")
    vulpen = copy.start(state)
    print("gui: menus", flush=True)
    for section, lines in menu_lines(copy.tree).items():
        tree = str(copy.tree)
        name, folder, before = PRESSED.get(section, ("face", "{tree}/projects/bf-one", []))
        name, folder = name.replace("{tree}", tree), folder.replace("{tree}", tree)
        before = [made.replace("{tree}", tree) for made in before]
        for line in lines:
            sent = (line.replace("{/}", folder).replace("{}", name)
                    .replace("<name>", copy.name()).replace("<recipe>", "snapshot")
                    .replace("<file>", f"{tree}/projects/bf-one/{copy.name()}.txt")
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
    vulpen.end(force=True)


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
WRITES = ("new", "node remove <node>", "node rename", "recipe", "view new", "view save",
          "view rename", "view remove", "file", "write")


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
            said = try_line(copy, vulpen, state, usage, variant, before, line)
            for text in said:
                if found := re.search(r"command: view switch ?(\S*)$", text):
                    current = found.group(1)
                elif found := re.search(r"command: view close (\S+)$", text):
                    current = "" if found.group(1) == current else current
            damage(copy, state, usage, [*before, line])
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


# --- the report --------------------------------------------------------------------------

def write_report(copy: Copy, report: Path, took: float, commit: str) -> None:
    by_what: dict[str, list[Finding]] = {}
    for finding in copy.findings:
        by_what.setdefault(finding.what, []).append(finding)
    text = [f"# break-commands at {commit}", "",
            f"{len(copy.tried)} lines tried, {len(copy.findings)} findings, "
            f"in {took / 60:.1f} minutes.", "", "## Findings", ""]
    for what, found in by_what.items():
        text += [f"### {what} ({len(found)})", ""]
        for finding in found:
            text.append(f"- {finding.part}, {finding.state}: `{finding.line[:160]}`")
            text += [f"    - {line[:300]}" for line in finding.said[:8]]
        text.append("")
    states = sorted({tried.state for tried in copy.tried if tried.part == "sweep"},
                    key=lambda name: [state.name for state in STATES].index(name)
                    if name in [state.name for state in STATES] else 99)
    text += ["## The lines help means, by state", "",
             "| usage | " + " | ".join(states) + " |",
             "|---|" + "---|" * len(states)]
    rows: dict[str, dict[str, str]] = {}
    for tried in copy.tried:
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
        for tried in copy.tried:
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
    parser.add_argument("--report", type=Path, default=ROOT / "out" / "break-commands.md")
    arguments = parser.parse_args()
    parts = arguments.only.split(",") if arguments.only else list(PARTS)
    states = [state for state in STATES if not arguments.states
              or state.name in arguments.states.split(",")]
    started = time.monotonic()
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                            capture_output=True, text=True).stdout.strip()
    if arguments.tree:
        tree = arguments.tree.resolve()
    else:
        tree = Path(tempfile.mkdtemp(prefix="break-commands-")) / "Vulpen"
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
        write_report(copy, report, time.monotonic() - started, commit)
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
