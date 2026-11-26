#!/usr/bin/env python3
"""The bench's runner: every engine function, and every function of the library's shared
code, run on its own, in every context, against the records committed beside it.

A bench per case file in cases/ (bench.cmake builds them; bench/Bench.h says what a row
holds) prints a row per call. This script runs each function's rows in a process of its
own, in each context, and compares the rows: with the baseline, as a context must not
move them, and with records/, the behaviour approved so far, so a change of behaviour
shows as a diff a person reads in the commit (A00) however the code is reshaped.

  build      the benches of a preset
  run        each function in each context, and with --presets in other builds; a table
             of the rows that moved, kept in out/bench/contexts.json for the report
  compare    the debug baseline against records/; fails on any difference, showing it,
             as check.py full runs it
  approve    a function's rows of the last run into records/, the why kept for the commit
  cost       instructions, time and allocations per call, under perf stat; with --base
             REF, against that commit built and measured here too, a hot function more
             than 5% slower failing (cost.py)
  coverage   the lines of each engine file the benches' rows and the tests run, as gcov
             counts them in the coverage preset's build; fails where a file's lines run by
             nothing grew past records/coverage.tsv, which --approve WHY writes (coverage.py)
  hot        the engine functions every frame calls, as gcov counts wave's frames in the
             coverage build (coverage.py), for cost --base and the report
  inventory  every engine function as libclang reads it (pages.py, which the review keeps
             private in docs/logs/code-review/ beside its notes; without it, no inventory)
  next       the next function to review, callees first, as a card
  report     the review's pages, in docs/reports/code-review/, which git ignores
  mutants    small changes to one engine or bench file, each built into the benches named
             and run against their records: one no row notices marks inputs that cannot
             tell right from wrong there (mutants.py); in a worktree of its own, as it
             rewrites the file
  planted    a fault planted for each of these guards, which must fail on it: a row
             moved, a record missing, a device not made, each context broken in turn
             (planted.py); fails where a guard passed its fault

Each process runs in a sandbox of its own under out/bench/, from an empty folder, with
address randomization off and a fixed environment, so what differs between contexts is
only what each context sets. What a run makes anew each time shows as a name: <root> for
the sandbox, <cwd>, <home>, <build>, <repo>, and the log's clock as <clock>.

Usage: python3 src/tools/bench/bench.py build [--preset P]
       python3 src/tools/bench/bench.py run [BENCH...] [--contexts C,...] [--presets P,...]
       python3 src/tools/bench/bench.py compare [BENCH...] [--preset P]
       python3 src/tools/bench/bench.py approve ID|BENCH... --why TEXT
       python3 src/tools/bench/bench.py cost [BENCH...] [--preset P] [--base REF]
       python3 src/tools/bench/bench.py coverage [BENCH...] [--approve WHY]
       python3 src/tools/bench/bench.py hot
       python3 src/tools/bench/bench.py inventory|next|report [BENCH...]
       python3 src/tools/bench/bench.py mutants BENCH[,BENCH...] [--limit N] [--seed S]
                       [--lines L,L] [--source FOLDER/FILE]
       python3 src/tools/bench/bench.py planted
"""
import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE = ROOT / "src"
CASES = HERE / "cases"
RECORDS = HERE / "records"
OUT = ROOT / "out" / "bench"
ENGINE = ("runtime", "baseclasses")
# The library's shared code (RV09): each part that includes a header holds its own copy,
# so a fault there is in every one of them, and in every view a part was dropped into.
SHARED = "runtime/recipes/vulpen/library/parts/shared"
# The folders a case may name a file of: the engine's, the shared code's, and the bench's
# own core, whose Shown.h every row goes through, so a fault there, which would hide a
# change in every record, shows in a record of its own.
CASED = (*ENGINE, SHARED, "tools/bench")
TIMEOUT = 300  # seconds a function's rows may take in one process
# A build's count of jobs, as check.py's JOBS: with none, Unix Makefiles start every
# compile at once.
JOBS = str(os.cpu_count() or 1)
LOCALE = "de_DE.UTF-8"  # a decimal comma
# The bench's marks (bench/Bench.cpp): a row ends with ROW, a dropped one with SKIP; a
# cost line starts with COST.
ROW, SKIP, COST = "\x1e", "\x1f", "\x1d"
# A function's one row where this machine lacks what it needs, as lavapipe.
NOT_HERE = "?? "
FRAME = re.compile(r"([\x1e\x1f])([^\n]*)\n")
COSTED = re.compile(r"\x1dcost rows=(\d+) calls=(\d+) nanoseconds=(\d+) allocations=(\d+) "
                    r"bytes=(\d+)")
# The log's time column: the date and time at a line that starts a new second, blank at
# the others, so it shows the same either way.
CLOCK = re.compile(r"^((?:\r\x1b\[K)?)(?: \d\d\.\d\d\.\d\d \d\d:\d\d:\d\d |[ ]{19})",
                   re.MULTILINE)
CLOCK_SHOWN = "\\1<clock>" + " " * 12
# A Vulkan handle as the validation layer prints it in a message, numbered by when its
# object was made, which a context that runs the rows in another order changes.
HANDLE = re.compile(r"0x[0-9a-fA-F]{6,}")
HANDLE_SHOWN = "0x<handle>"
# Past this many characters, a text in a row shows its ends and a hash, so a row stays a
# line.
QUOTED = re.compile(r'"((?:\\.|[^"\\])*)"')
QUOTED_WHOLE, QUOTED_HEAD, QUOTED_TAIL = 160, 64, 16
# What a bench process may take, so a damaged input that asks for gigabytes fails in the
# process, never on the machine: the memory it writes, and under a sanitizer, which maps
# terabytes of shadow, the sanitizer's own limits.
MEMORY = 4 << 30
SANITIZER = {"LSAN_OPTIONS": f"suppressions={SOURCE}/tools/tests/lsan.supp"
                             ":print_suppressions=0",
             "UBSAN_OPTIONS": "print_stacktrace=1",
             "ASAN_OPTIONS": "detect_stack_use_after_return=1"
                             f":max_allocation_size_mb={MEMORY >> 20}"
                             f":hard_rss_limit_mb={MEMORY >> 20}"}


def capped(preset: str):
    """What a bench process runs before it starts: the memory it writes capped, but
    under a sanitizer, whose options cap it instead, and on Windows, which has no such
    limit. Not its address space: a lavapipe device reserves near 3 GB of it, a malloc
    arena per thread, and a driver whose malloc fails crashes rather than refusing."""
    if os.name == "nt" or preset in ("asan", "tsan"):
        return None

    def cap() -> None:
        import resource
        resource.setrlimit(resource.RLIMIT_DATA, (MEMORY, MEMORY))
    return cap


@dataclass(frozen=True)
class Context:
    """What one run varies from the baseline's."""
    name: str
    bench: str = "baseline"  # what the bench varies in its own process
    env: tuple = ()
    locale: str | None = None
    aslr: bool = False  # address randomization, which the baseline turns off
    elsewhere: bool = False  # a working folder of canary files, not an empty one
    home: bool = False  # HOME an empty folder of its own
    printed: bool = True  # whether its rows keep what each call printed


CONTEXTS = (
    Context("baseline"),
    Context("again", bench="again"),
    Context("thread", bench="thread"),
    Context("threads", bench="threads", printed=False),
    Context("heap", bench="heap", env=(("MALLOC_PERTURB_", "165"),), aslr=True),
    Context("cwd", elsewhere=True),
    Context("locale", locale=LOCALE, env=(("LC_ALL", LOCALE), ("LANG", LOCALE))),
    Context("environment", home=True,
            env=(("TZ", "Pacific/Kiritimati"), ("TERM", "dumb"), ("NO_COLOR", "1"),
                 ("COLUMNS", "7"), ("LINES", "3"))),
)
BASELINE = CONTEXTS[0]
# Builds whose baseline must give the debug build's rows: the optimizer's, the
# sanitizers', and the odd build's, which fills every uninitialized variable with a
# pattern, makes char unsigned, as ARM's is, and checks every iterator.
PRESETS = ("release", "asan", "odd")
# Files a function reading paths against the working folder finds in the cwd context.
CANARIES = ("a", "view.vlp", "README.md", ".vulpen/kept.ini", "projects/a/view.vlp")
# An id under canary/ names the context or build its rows must move in, before any comma,
# as canary/again and canary/again, last to first (cases/Bench.cpp plants them). One that
# stays the same there fails the run: that context no longer does what it is for, and
# would call every function the same.
CANARY = "canary/"


@dataclass
class Bench:
    """A case file's bench, and the engine file its rows are recorded under."""
    name: str
    folder: str

    @property
    def record(self) -> Path:
        return RECORDS / self.folder / f"{self.name}.rec"

    def binary(self, preset: str) -> Path:
        suffix = ".exe" if os.name == "nt" else ""
        return ROOT / "out" / "build" / preset / "bench" / f"bench-{self.name}{suffix}"

    def ran(self, preset: str, context: str) -> Path:
        return OUT / preset / context / self.folder / f"{self.name}.rec"


def benches(names: list[str]) -> list[Bench]:
    found = []
    for case in sorted(CASES.glob("*.cpp")):
        folders = [folder for folder in CASED
                   if any((SOURCE / folder / f"{case.stem}{suffix}").is_file()
                          for suffix in (".h", ".cpp"))]
        if len(folders) != 1:
            sys.exit(f"{case.name}: names no one file in src/runtime, src/baseclasses, "
                     f"src/{SHARED} or src/tools/bench")
        found.append(Bench(case.stem, folders[0]))
    unknown = set(names) - {bench.name for bench in found}
    if unknown:
        sys.exit(f"no case for {', '.join(sorted(unknown))} in {CASES}")
    return [bench for bench in found if not names or bench.name in names]


def build(preset: str, chosen: list[Bench] | None = None) -> None:
    """The benches chosen, or all of them: a bench named alone builds alone, as each case
    file is a compile of its own, half a gigabyte for the larger ones."""
    folder = ROOT / "out" / "build" / preset
    if not (folder / "CMakeCache.txt").is_file():
        subprocess.run(["cmake", "--preset", preset], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
    def built(targets: list[str]) -> subprocess.CompletedProcess:
        return subprocess.run(["cmake", "--build", str(folder), "--target", *targets,
                               "--parallel", JOBS], cwd=ROOT, capture_output=True, text=True)

    done = built([f"bench-{bench.name}" for bench in chosen] if chosen else ["bench"])
    if done.returncode == 0:
        return
    # A build stops at the first bench that fails, so each then builds alone, and one run
    # names every bench that fails, by its first error or warning (warnings are errors
    # here): CI's Windows build is the only MSVC compile, and GitHub annotates only a
    # step's first ten.
    failed = [f"bench-{bench.name}:\n{first_problem(alone)}"
              for bench in chosen or benches([])
              if (alone := built([f"bench-{bench.name}"])).returncode != 0]
    sys.exit(f"the {preset} benches did not build:\n" + "\n".join(
        failed or [f"{done.stdout[-4000:]}{done.stderr[-4000:]}"]))


def first_problem(done: subprocess.CompletedProcess) -> str:
    said = done.stdout + done.stderr
    return next((line.strip() for line in said.splitlines()
                 if re.search(r":\s*(fatal )?(error|warning)\b", line)), said[-4000:])


def ids_of(bench: Bench, preset: str) -> list[str]:
    return list(listed(bench, preset))


def listed(bench: Bench, preset: str) -> dict[str, dict[str, str]]:
    """Each id the bench runs, with the contexts its case leaves out for it, and why. In
    the time a function's rows may take, as a bench that hangs before listing would hang
    every run that lists it, a compare in check.py full too."""
    try:
        done = subprocess.run([bench.binary(preset), "list"], capture_output=True, text=True,
                              timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        sys.exit(f"bench-{bench.name} list: still running after {TIMEOUT} s")
    if done.returncode != 0:
        sys.exit(f"bench-{bench.name} list: {done.stderr.strip() or done.returncode}")
    found: dict[str, dict[str, str]] = {}
    for line in done.stdout.splitlines():
        if line.startswith("\t"):
            context, why = line[1:].split("\t", 1)
            found[last][context] = why
        else:
            last = line
            found[last] = {}
    return found


def driver(preset: str) -> str | None:
    """The Vulkan driver the build's tests take, lavapipe where it is installed, so a
    function that makes a device never makes it on this machine's own GPU."""
    cache = ROOT / "out" / "build" / preset / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            if line.startswith("VULPEN_TEST_DRIVER:") and "=" in line:
                return line.split("=", 1)[1] or None
    return None


def escaped(text: str) -> str:
    """As Bench.cpp's escaped(), for what a call printed: in quotes, the bytes a terminal
    would obey escaped."""
    out = []
    for letter in text:
        code = ord(letter)
        if letter in "\n\r\t\\\"":
            out.append({"\n": "\\n", "\r": "\\r", "\t": "\\t", "\\": "\\\\",
                        "\"": "\\\""}[letter])
        elif code < 0x20 or code > 0x7e:
            out.append(f"\\x{code:02x}")
        else:
            out.append(letter)
    return f"\"{''.join(out)}\""


def fnv(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) % (1 << 64)
    return value


@dataclass
class Sandbox:
    """The folders one process runs in, which the rows name by what they are."""
    root: Path
    cwd: Path
    home: Path

    def names(self, preset: str) -> list[tuple[str, str]]:
        """Each folder as the rows may spell it, the longest first, so a folder inside
        another is named as itself."""
        folders = [(self.root, "<root>"), (self.cwd, "<cwd>"),
                   (self.root.parent, "<sandbox>"),
                   (self.root.parent.parent, "<sandboxes>"),  # each preset's own
                   (ROOT / "out" / "build" / preset, "<build>"), (ROOT, "<repo>"),
                   (self.home, "<home>")]
        spelled = []
        for folder, name in folders:
            for form in {str(folder), folder.as_posix()}:
                spelled += [(form, name), (form.replace("\\", "\\\\"), name)]
        return sorted(set(spelled), key=lambda pair: -len(pair[0]))


def normal(text: str, sandbox: Sandbox, preset: str) -> str:
    """A row as every run gives it: the folders named, then each long text cut."""
    for spelled, name in sandbox.names(preset):
        text = text.replace(spelled, name)

    def cut(found: re.Match) -> str:
        inner = found.group(1)
        if len(inner) <= QUOTED_WHOLE:
            return found.group(0)
        return (f'"{inner[:QUOTED_HEAD]}...{inner[-QUOTED_TAIL:]}" '
                f'({len(inner)} characters, hash {fnv(inner.encode("latin-1")):016x})')
    return QUOTED.sub(cut, text)


def rows_of(out: str, err: str, context: Context, sandbox: Sandbox,
            preset: str) -> list[str]:
    """The rows a bench printed, each with what its call printed before it, on standard
    output and standard error, which pair by the marks the bench writes to both."""
    pieces = FRAME.split(out)
    errors = err.split(ROW + "\n")
    rows = []
    for index in range(len(pieces) // 3):
        printed, mark, text = pieces[3 * index:3 * index + 3]
        said = errors[index] if index < len(errors) else ""
        if mark == SKIP:
            continue
        if context.printed:
            if printed:
                text += "; printed " + escaped(
                    HANDLE.sub(HANDLE_SHOWN, CLOCK.sub(CLOCK_SHOWN, printed)))
            if said:
                text += "; to stderr " + escaped(HANDLE.sub(HANDLE_SHOWN, said))
        rows.append(normal(text, sandbox, preset))
    after = pieces[-1] + "".join(errors[len(pieces) // 3:])
    if after.strip():
        rows.append("!! printed after the last row: " +
                    normal(escaped(CLOCK.sub(CLOCK_SHOWN, after)), sandbox, preset))
    return rows


@contextmanager
def display(size: str = "1280x720"):
    """A display of the run's own for the benches that open a window: an Xvfb server,
    which no person sees, so a bench never opens a window on the screen it runs beside;
    None where Xvfb is not installed (Windows, CI), where those benches say they did not
    run. size is the screen's, in pixels."""
    xvfb = shutil.which("Xvfb")
    if xvfb is None or os.name == "nt":
        yield None
        return
    read, write = os.pipe()
    server = subprocess.Popen([xvfb, "-displayfd", str(write), "-screen", "0", f"{size}x24",
                               "-nolisten", "tcp", "-noreset"], pass_fds=(write,),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write)
    with os.fdopen(read) as ready:  # the server writes its number once it listens
        number = ready.readline().strip()
    try:
        yield f":{number}" if number else None
    finally:
        server.terminate()
        server.wait()


def environment(preset: str, context: Context, sandbox: Sandbox,
                screen: str | None = None) -> dict[str, str]:
    env = {"PATH": os.environ.get("PATH", ""), "HOME": str(sandbox.home),
           "TZ": "UTC", "LC_ALL": "C", "LANG": "C"}
    for key in ("SYSTEMROOT", "TEMP", "TMP"):  # Windows needs them to start a process
        if key in os.environ:
            env[key] = os.environ[key]
    if lavapipe := driver(preset):
        env["VK_DRIVER_FILES"] = lavapipe
    # Synchronization validation, for every device a GPU case makes.
    env["VK_LAYER_SETTINGS_PATH"] = str(HERE / "vk_layer_settings.txt")
    if screen:
        env["DISPLAY"] = screen
    if preset in ("asan", "tsan"):
        env.update(SANITIZER)
    env.update(dict(context.env))
    return env


def sandbox_in(scratch: Path, context: Context) -> Sandbox:
    """The folders one process runs in, made afresh: its root with the fixtures, its
    working folder, and its HOME where the context gives it one."""
    if scratch.exists():
        shutil.rmtree(scratch)
    sandbox = Sandbox(scratch / "root", scratch / "cwd",
                      scratch / "home" if context.home else Path.home())
    for folder in ("one", "two"):  # the folders of two threads at once
        (sandbox.root / folder).mkdir(parents=True)
    # A folder holding a file and a link to the folder above, which no bench file makes
    # (RA04); where no link can be made, as on Windows without the right, none is.
    loop = sandbox.root / "fixtures" / "loop"
    loop.mkdir(parents=True)
    (loop / "a").write_text("1", encoding="utf-8")
    try:
        os.symlink("..", loop / "up", target_is_directory=True)
    except OSError:
        pass
    # A project holding a link out of it, to a folder whose files are none of its own.
    linked = sandbox.root / "fixtures" / "linked"
    (linked / "project").mkdir(parents=True)
    (linked / "outside").mkdir()
    (linked / "project" / "view.vlp").write_text("version = 1", encoding="utf-8")
    (linked / "project" / "a").write_text("1", encoding="utf-8")
    (linked / "outside" / "x").write_text("outside", encoding="utf-8")
    try:
        os.symlink("../outside", linked / "project" / "out", target_is_directory=True)
    except OSError:
        pass
    sandbox.cwd.mkdir()
    if context.home:
        sandbox.home.mkdir()
    if context.elsewhere:
        for name in CANARIES:
            (sandbox.cwd / name).parent.mkdir(parents=True, exist_ok=True)
            (sandbox.cwd / name).write_text("canary\n", encoding="utf-8")
    return sandbox


def run_one(bench: Bench, ident: str, context: Context, preset: str,
            scratch: Path, screen: str | None = None) -> list[str]:
    """One function's rows in one context, in a process and a sandbox of its own."""
    sandbox = sandbox_in(scratch, context)
    command = [str(bench.binary(preset)), "run", ident, "--context", context.bench,
               "--root", str(sandbox.root), "--repo", str(ROOT)]
    if context.locale:
        command += ["--locale", context.locale]
    if not context.aslr and sys.platform.startswith("linux") and shutil.which("setarch"):
        command = ["setarch", "-R", *command]
    try:
        done = subprocess.run(command, cwd=sandbox.cwd,
                              env=environment(preset, context, sandbox, screen),
                              capture_output=True, timeout=TIMEOUT,
                              preexec_fn=capped(preset))
    except subprocess.TimeoutExpired:
        return [f"!! still running after {TIMEOUT} s"]
    out, err = done.stdout.decode("latin-1"), done.stderr.decode("latin-1")
    rows = rows_of(out, err, context, sandbox, preset)
    if done.returncode != 0:
        rows.append(f"!! exited {done.returncode}")
    shutil.rmtree(scratch, ignore_errors=True)
    return rows


def record_text(bench: Bench, sections: dict[str, list[str]]) -> str:
    files = " and ".join(f"src/{bench.folder}/{bench.name}{suffix}"
                         for suffix in (".h", ".cpp")
                         if (SOURCE / bench.folder / f"{bench.name}{suffix}").is_file())
    lines = [f"# The rows of {files}, a call each (src/tools/bench/bench.py)."]
    for ident in sorted(sections):
        lines += ["", f"## {ident}", *sorted(sections[ident])]
    return "\n".join(lines) + "\n"


def sections_of(text: str) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    rows = None
    for line in text.splitlines():
        if line.startswith("## "):
            rows = sections.setdefault(line[3:], [])
        elif rows is not None and line:
            rows.append(line)
    return sections


def save(path: Path, text: str) -> None:
    """Whole or not at all, as every save is (RA04)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    temporary.replace(path)


def run_all(chosen: list[Bench], preset: str,
            contexts: tuple[Context, ...]) -> dict[str, dict[str, dict[str, list[str]]]]:
    """Every function of the benches in every context: rows by bench, context and id,
    each context's also kept in out/bench/<preset>/<context>/."""
    jobs = [(bench, ident, context) for bench in chosen
            for ident, left_out in listed(bench, preset).items()
            for context in contexts if context.name not in left_out]
    scratch = OUT / "sandbox" / preset
    with display() as screen, ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(
            lambda pair: run_one(*pair[1], preset, scratch / str(pair[0]), screen),
            enumerate(jobs)))
    found: dict[str, dict[str, dict[str, list[str]]]] = {}
    for (bench, ident, context), rows in zip(jobs, results):
        found.setdefault(bench.name, {}).setdefault(context.name, {})[ident] = rows
    for bench in chosen:
        for context in contexts:
            save(bench.ran(preset, context.name),
                 record_text(bench, found.get(bench.name, {}).get(context.name, {})))
    return found


def not_here(rows: list[str]) -> str | None:
    """Why a function did not run on this machine, as a GPU case says where it can make no
    device on lavapipe (bench/Gpu.h): its one row, which starts with ??."""
    if len(rows) == 1 and rows[0].startswith(NOT_HERE):
        return rows[0][len(NOT_HERE):]
    return None


def could_run(why: str, preset: str) -> bool:
    """Whether this machine has what a function that did not run says it needs: lavapipe,
    where the build names it, or a display, where Xvfb is installed for bench.py to start.
    Here the function failed, which compare counts as a difference; a machine without
    them, as Windows CI, only lacks the means."""
    if why.startswith("needs lavapipe"):
        return driver(preset) is not None
    if why.startswith("needs a display"):
        return os.name != "nt" and shutil.which("Xvfb") is not None
    return False


def unprinted(rows: list[str]) -> list[str]:
    """Rows without what their calls printed, as a context that runs two threads at once
    keeps them."""
    return sorted(re.sub(r"; (?:printed|to stderr) \".*$", "", row) for row in rows)


def first_moved(rows: list[str], baseline: list[str], printed: bool) -> str | None:
    mine, theirs = (sorted(rows), sorted(baseline)) if printed else (
        unprinted(rows), unprinted(baseline))
    if mine == theirs:
        return None
    for line in difflib.unified_diff(theirs, mine, lineterm="", n=0):
        if line.startswith(("+", "-")) and not line.startswith(("+++", "---")):
            return line
    return "the order"


def canary_of(ident: str) -> str | None:
    """The context a canary's rows must move in, or None for an id that is no canary."""
    return ident[len(CANARY):].split(",")[0] if ident.startswith(CANARY) else None


def command_run(arguments) -> int:
    chosen = benches(arguments.benches)
    contexts = tuple(context for context in CONTEXTS
                     if not arguments.contexts or context.name in arguments.contexts)
    if BASELINE not in contexts:
        contexts = (BASELINE, *contexts)
    named = chosen if arguments.benches else None
    build("debug", named)
    found = run_all(chosen, "debug", contexts)
    for preset in arguments.presets:
        build(preset, named)
        for name, by_context in run_all(chosen, preset, (BASELINE,)).items():
            found[name][preset] = by_context["baseline"]
    table, moved, canaries, missed = {}, 0, 0, 0
    for bench in chosen:
        baseline = found[bench.name]["baseline"]
        left = listed(bench, "debug")
        for ident in sorted(baseline):
            row = table.setdefault(f"{bench.name}/{ident}", {})
            for context, why in left.get(ident, {}).items():
                row[context] = f"left out: {why}"
            for context, by_id in found[bench.name].items():
                if context == "baseline":
                    row[context] = f"{len(baseline[ident])} rows"
                    continue
                if ident not in by_id or context in left.get(ident, {}):
                    continue
                printed = next((c.printed for c in CONTEXTS if c.name == context), True)
                difference = first_moved(by_id.get(ident, []), baseline[ident], printed)
                row[context] = "same" if difference is None else f"moved: {difference}"
                if canary_of(ident) == context:
                    canaries += 1
                    row[context] = (f"caught: {difference}" if difference is not None
                                    else "missed")
                    if difference is None:
                        missed += 1
                        print(f"{ident}: the same in {context}, so that context no longer "
                              "reaches the process")
                elif difference is not None and canary_of(ident) is None:
                    moved += 1
                    print(f"{ident} in {context}: {difference[:160]}")
    previous = (json.loads((OUT / "contexts.json").read_text(encoding="utf-8"))
                if (OUT / "contexts.json").is_file() else {})
    previous.update(table)
    save(OUT / "contexts.json", json.dumps(previous, indent=1, sort_keys=True))
    print(f"{len(table)} functions, {len(contexts) - 1 + len(arguments.presets)} contexts "
          f"each: {moved} moved"
          + (f"; canaries: {canaries - missed} of {canaries} caught" if canaries else ""))
    return 1 if moved or missed else 0


def command_compare(arguments) -> int:
    chosen = benches(arguments.benches)
    build(arguments.preset, chosen if arguments.benches else None)
    found = run_all(chosen, arguments.preset, (BASELINE,))
    problems, unrun = [], []
    for bench in chosen:
        ran = found[bench.name]["baseline"]
        approved = (sections_of(bench.record.read_text(encoding="utf-8"))
                    if bench.record.is_file() else {})
        for ident in sorted(ran.keys() | approved.keys()):
            if ident in ran and (why := not_here(ran[ident])):
                if could_run(why, arguments.preset):
                    problems.append(f"{ident}: did not run, though this machine has what "
                                    f"it needs: {why}")
                else:
                    unrun.append(f"{ident}: not run here, {why}")
            elif ident not in approved:
                problems.append(f"{ident}: no record yet; approve it")
            elif ident not in ran:
                problems.append(f"{ident}: in records/ but no case runs it; approve its "
                                "removal with the why")
            elif sorted(ran[ident]) != sorted(approved[ident]):
                diff = list(difflib.unified_diff(sorted(approved[ident]), sorted(ran[ident]),
                                                 "approved", "now", lineterm="", n=0))
                shown = "\n    ".join(diff[2:22])
                more = f"\n    ... {len(diff) - 22} more" if len(diff) > 22 else ""
                problems.append(f"{ident}: rows moved\n    {shown}{more}")
    for problem in problems + unrun:
        print(problem)
    functions = sum(len(found[bench.name]["baseline"]) for bench in chosen) - len(unrun)
    print(f"{functions} functions compared with records/: "
          f"{'all the same' if not problems else f'{len(problems)} differ'}"
          + (f"; {len(unrun)} not run here" if unrun else ""))
    return 1 if problems else 0


def command_approve(arguments) -> int:
    every = benches([])
    by_name = {bench.name: bench for bench in every}
    chosen: dict[str, set[str] | None] = {}
    for name in arguments.names:
        if name in by_name:
            chosen[name] = None  # every id of the bench
        else:
            bench = next((bench for bench in every
                          if bench.ran("debug", "baseline").is_file()
                          and name in sections_of(bench.ran("debug", "baseline")
                                                  .read_text(encoding="utf-8"))
                          or bench.record.is_file()
                          and name in sections_of(bench.record.read_text(encoding="utf-8"))),
                         None)
            if bench is None:
                sys.exit(f"{name}: no bench or function of that name ran; run compare first")
            if chosen.get(bench.name, set()) is not None:
                chosen.setdefault(bench.name, set()).add(name)
    notes = []
    for name, idents in chosen.items():
        bench = by_name[name]
        if not bench.ran("debug", "baseline").is_file():
            sys.exit(f"{name}: no run to approve; run compare first")
        ran = sections_of(bench.ran("debug", "baseline").read_text(encoding="utf-8"))
        approved = (sections_of(bench.record.read_text(encoding="utf-8"))
                    if bench.record.is_file() else {})
        for ident in (idents if idents is not None else ran.keys() | approved.keys()):
            if ident in ran and not_here(ran[ident]):
                print(f"{ident}: not approved, as it did not run here")
                continue
            if ident in ran:
                approved[ident] = ran[ident]
            else:
                approved.pop(ident, None)
            notes.append(f"- {ident}: {arguments.why}")
        save(bench.record, record_text(bench, approved))
    log = OUT / "approved.md"
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("a", encoding="utf-8") as kept:
        kept.write("\n".join(notes) + "\n")
    print("\n".join(notes))
    print(f"the whys are kept in {log.relative_to(ROOT).as_posix()} for the commit body")
    return 0


def command_cost(arguments) -> int:
    """Per call of a sweep's function, past each row's first: user-space instructions,
    which are steadier than time (C03), as perf counts a run calling each row N times more
    less a run calling none more; the time and what the bench's operator new counted, as
    the bench measures around those calls alone. N fills about a third of a second. A
    claim's or a scenario's rows are no one function's calls, so they cost none."""
    chosen = benches(arguments.benches)
    build(arguments.preset, chosen if arguments.benches else None)
    perf = shutil.which("perf")
    scratch = OUT / "sandbox" / arguments.preset / "cost"
    # Each measured run as a baseline run is made, so its calls take the paths the records
    # hold, in a sandbox of their own, on lavapipe and Xvfb's display, never this machine's.
    with display() as screen:
        return costed(arguments, chosen, perf, scratch, screen)


def costed(arguments, chosen: list[Bench], perf: str | None, scratch: Path,
           screen: str | None) -> int:
    for bench in chosen:
        costs = {}
        for ident in ids_of(bench, arguments.preset):
            def measured(repeat: int) -> tuple[dict, int | None]:
                sandbox = sandbox_in(scratch, BASELINE)
                command = [str(bench.binary(arguments.preset)), "run", ident, "--repeat",
                           str(repeat), "--root", str(sandbox.root), "--repo", str(ROOT)]
                if perf:
                    command = [perf, "stat", "-x,", "-e", "instructions:u", "--", *command]
                done = subprocess.run(command, capture_output=True, text=True,
                                      cwd=sandbox.cwd, timeout=TIMEOUT, errors="replace",
                                      env=environment(arguments.preset, BASELINE, sandbox,
                                                      screen),
                                      preexec_fn=capped(arguments.preset))
                shutil.rmtree(scratch, ignore_errors=True)
                found = COSTED.search(done.stdout)
                if not found:
                    sys.exit(f"{ident}: no cost line\n{done.stdout[-2000:]}{done.stderr[-2000:]}")
                counted = re.search(r"^(\d+),,instructions:u", done.stderr, re.MULTILINE)
                keys = ("rows", "calls", "nanoseconds", "allocations", "bytes")
                return (dict(zip(keys, map(int, found.groups()))),
                        int(counted.group(1)) if counted else None)
            once, _ = measured(1)
            if once["calls"] == 0:
                continue
            per_call = max(once["nanoseconds"], 1) / once["calls"]
            repeat = max(1, min(100000, int(0.3e9 / (per_call * once["calls"]))))
            many, instructions = measured(repeat)
            _, base = measured(0)
            calls = max(1, many["calls"])
            costs[ident] = {
                "rows": many["rows"],
                "instructions": (round((instructions - base) / calls)
                                 if instructions is not None and base is not None else None),
                "nanoseconds": round(many["nanoseconds"] / calls, 1),
                "allocations": round(many["allocations"] / calls, 2),
                "bytes": round(many["bytes"] / calls, 1)}
            print(f"{ident}: {costs[ident]}")
        save(OUT / arguments.preset / "cost" / f"{bench.name}.json",
             json.dumps(costs, indent=1, sort_keys=True))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    built = commands.add_parser("build")
    built.add_argument("--preset", default="debug")
    ran = commands.add_parser("run")
    ran.add_argument("benches", nargs="*")
    ran.add_argument("--contexts", type=lambda text: text.split(","), default=[])
    ran.add_argument("--presets", type=lambda text: text.split(","), default=[])
    compared = commands.add_parser("compare")
    compared.add_argument("benches", nargs="*")
    compared.add_argument("--preset", default="debug")
    approved = commands.add_parser("approve")
    approved.add_argument("names", nargs="+")
    approved.add_argument("--why", required=True)
    costed = commands.add_parser("cost")
    costed.add_argument("benches", nargs="*")
    costed.add_argument("--preset", default="debug")
    costed.add_argument("--base", metavar="REF")
    commands.add_parser("hot")
    for name in ("inventory", "next", "report"):
        commands.add_parser(name).add_argument("benches", nargs="*")
    commands.add_parser("planted")
    covered = commands.add_parser("coverage")
    covered.add_argument("benches", nargs="*")
    covered.add_argument("--approve", metavar="WHY")
    mutated = commands.add_parser("mutants")
    mutated.add_argument("bench", metavar="BENCH[,BENCH...]", help="the file's own bench, "
                         "and others that run it too, as Edits,Views")
    mutated.add_argument("--limit", type=int, default=0)
    mutated.add_argument("--seed", type=int, default=1)
    mutated.add_argument("--lines", metavar="L,L", help="every site on these lines, as a "
                         "survivor's, once inputs were added for it")
    mutated.add_argument("--source", metavar="FOLDER/FILE",
                         help="another file than the bench's own: an engine file every "
                         "bench links, as runtime/Binder.cpp, or a header the benches "
                         "named include, as tools/bench/Bench.h")
    arguments = parser.parse_args()
    if arguments.command == "build":
        build(arguments.preset)
        return 0
    if arguments.command in ("inventory", "next", "report"):
        # The review's own pages, beside its notes and ledger, which git ignores: a clone
        # has the net, not the review's process (the plan's 5.2).
        private = ROOT / "docs" / "logs" / "code-review"
        if not (private / "pages.py").is_file():
            sys.exit(f"{arguments.command} needs {(private / 'pages.py').relative_to(ROOT)}, "
                     "the review's own, which the repository does not hold")
        sys.path.insert(0, str(private))
        import pages  # noqa: E402 (in the review's folder, which is no package)
        return {"inventory": pages.inventory, "next": pages.next_function,
                "report": pages.report}[arguments.command](arguments)
    if arguments.command in ("mutants", "coverage", "hot", "planted") or (
            arguments.command == "cost" and arguments.base):
        sys.path.insert(0, str(HERE))
        import cost  # noqa: E402 (beside this script, which is no package)
        import coverage  # noqa: E402
        import mutants  # noqa: E402
        import planted  # noqa: E402
        return {"mutants": mutants.mutants, "coverage": coverage.coverage,
                "hot": coverage.hot, "cost": cost.against, "planted": planted.planted}[
            arguments.command](arguments, sys.modules[__name__])
    return {"run": command_run, "compare": command_compare, "approve": command_approve,
            "cost": command_cost}[arguments.command](arguments)


if __name__ == "__main__":
    sys.exit(main())
