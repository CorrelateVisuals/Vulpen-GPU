#!/usr/bin/env python3
"""Run every check of a tier, so none that guards a change waits on someone recalling it.

quick: configure, build and test the debug preset, as CI does on every push; the build
runs the gates, the mutable-globals check and the statistics. Then, but on Windows, the
names hidden as MSVC refuses them and GCC lets pass (hidden-names.py). About a minute.
full: quick, then every example made again by typing (examples-replay.py), every command
swept in each state with the menus and keys (command-sweep.py's sweep and gui), every
engine function's rows against the bench's records (bench.py compare), the address
sanitizer's build and tests, and ctest's window tests under an Xvfb of the run's own.
Once the debug build is made, three chains run side by side: its checks, the examples and
the sanitizer's tests, as each runs vulpen in this tree, then the bench's compare, whose
benches build with every thread once the others are about done; the sweep, in copies of
its own (--jobs); and the sanitizer's build, with a share of the threads. About
twenty-five minutes on 16 threads; a change to the engine runs it before it lands.
nightly: every preset built and tested, GPU-assisted validation, the nightly tests (the
soaks, the long fuzz, examples-replay), the whole command sweep, every engine function's
rows in each context and build (bench.py run), with a canary per context that must move, a
fault planted for each of the bench's guards (bench.py planted), the engine's lines no row
or test runs against the bench's record (bench.py coverage), and thirty mutants in a tree
of their own of a file whose functions wait for a verdict, else of the day's engine file
(bench.py mutants, survivors in mutants.md), and, where the review's private notes are,
the engine functions waiting for a verdict (unjudged.md). Once every preset is built,
alone, the sweep runs beside the rest, in a copy of its own, as full's does. Some hours;
its report also goes to docs/reports/.
release: before main moves to this commit, run in a tree that holds the commit alone, as
git worktree add makes one, with main behind it, so main fast-forwards to exactly what
ran: nightly, then LOC.md as the builds count it and the README's pictures as the release
build takes them (both committed), the commit built as GitHub's source download holds it,
and main's own lines run on main's build and this one (compare-main.py). Its report adds
what they found and what to try by hand, and goes to docs/reports/ under the commit's
name. Some hours. Before it, run the sweep's tab, walks and seen at the commit
(command-sweep.py --only tab,walk,seen), about eight minutes: no other tier runs them, and
a tool bug they find moves the commit, so the whole release would run again.
install-hook: a git pre-push hook running quick, which stops a push it fails (opt-in).

Each step runs unless the step it needs did not pass, once that step ended, and the run
fails unless every step passed: one that raised failed, and one never reached is skipped.
What each said is kept in out/check/<tier>-<time>/, with a report of each step's result
and, for one that failed, its errors and its last lines.
full, nightly and release run the Linux presets (asan, tsan) and Linux's tools.

Usage: python3 src/tools/check.py quick|full|nightly|release|install-hook
"""
import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import traceback
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "src" / "tools"
REPORTS = ROOT / "docs" / "reports"
MAIN = "main"  # the branch a release moves, only ever fast-forwarded
LINES = "docs/statistics/LOC.md"  # every build counts them again
PICTURES = "docs/images"  # the README's, which a release build takes again
# The review's notes, ledger and pages.py, which git ignores: where they are, nightly says
# which engine functions wait for a verdict; a clone has none.
PRIVATE = ROOT / "docs" / "logs" / "code-review"
STAMP = re.compile(r"\{run\} vulpen (\S+)")  # the commit a vulpen says it was built from
# A build's count of jobs: with none, CMake hands Unix Makefiles make -j, which starts
# every compile at once, as a fresh tree's or every module's after an engine header
# changed. The machine's threads, near what Ninja starts by default.
JOBS = str(os.cpu_count() or 1)
# full's sweep: copies of the tree side by side (command-sweep.py --jobs), which runs
# fewer where the machine's memory holds fewer.
COPIES = "6"
# A build beside the sweep's copies, which share the machine's threads among them: a
# quarter more compiles at once, rather than twice one build's.
BESIDE = str(max(1, (os.cpu_count() or 1) // 4))
# Lavapipe's threads for nightly's sweep, one copy drawing beside the runs of vulpen in
# this tree: as many as each of full's copies draws with, so the runs beside it keep the
# rest. A sweep's rows are the same on any count.
SWEEP_THREADS = str(max(1, (os.cpu_count() or 1) // int(COPIES)))
TAIL = 30  # lines of a failed step's log the report shows, after those naming an error
MUTANTS = 30  # of the day's engine file, nightly
HOOK = """#!/bin/sh
# Written by src/tools/check.py install-hook: each push runs its quick checks first, and
# one failing stops the push; git push --no-verify pushes anyway.
exec python3 "$(git rev-parse --show-toplevel)/src/tools/check.py" quick
"""


class Step:
    """A command, or a function given the log, run once the step it needs passed; env: the
    command's environment, if not this one's; chain: the steps that run one after another
    beside the other chains, once every step of no chain ran."""

    def __init__(self, name, run, needs=None, env=None, chain=None):
        self.name, self.run, self.needs, self.env = name, run, needs, env
        self.chain = chain
        self.result, self.seconds = "skipped", 0.0
        self.ended = threading.Event()


def preset(name: str, window: dict[str, str] | None = None, jobs: str = JOBS,
           chain: str | None = None) -> list[Step]:
    return [Step(f"configure {name}", ["cmake", "--preset", name], chain=chain),
            Step(f"build {name}",
                 ["cmake", "--build", "--preset", name, "--parallel", jobs],
                 f"configure {name}", chain=chain),
            Step(f"test {name}", ["ctest", "--preset", name], f"build {name}", window,
                 chain)]


def test_driver(preset: str = "debug") -> str:
    """The Vulkan driver a build's tests take, and a release build's pictures: lavapipe,
    which the records and the goldens were made on; none before a configure."""
    cache = ROOT / "out" / "build" / preset / "CMakeCache.txt"
    return next((line.split("=", 1)[1] for line in cache.read_text().splitlines()
                 if line.startswith("VULPEN_TEST_DRIVER:")), "") if cache.is_file() else ""


@contextmanager
def screen(wanted: bool):
    """The environment ctest's window tests (triangle, ide) run in: for full and nightly, a
    display of the run's own, an Xvfb server no person sees, and no Wayland display, so
    nothing opens on the screen beside the run, on the tests' driver, as a window would
    take the machine's own GPU; None for quick, as CI runs it, and where Xvfb is not
    installed, where those tests say they skipped."""
    xvfb = shutil.which("Xvfb")
    if not wanted or xvfb is None or sys.platform == "win32":
        yield None
        return
    read, write = os.pipe()
    # Over twice vulpen's window of 1280 by 720, which no window manager moves from the
    # top left here: X starts the pointer in the screen's middle, so it lies outside the
    # window, and only the pointer a test scripts moves in it, as the file menu's place.
    server = subprocess.Popen([xvfb, "-displayfd", str(write), "-screen", "0", "2600x1500x24",
                               "-nolisten", "tcp", "-noreset"], pass_fds=(write,),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write)
    with os.fdopen(read) as ready:  # the server writes its number once it listens
        number = ready.readline().strip()
    window = {key: value for key, value in os.environ.items()
              if key != "WAYLAND_DISPLAY"}
    window["DISPLAY"] = f":{number}"
    if driver := test_driver():
        window["VK_DRIVER_FILES"] = driver
    try:
        yield window if number else None
    finally:
        server.terminate()
        server.wait()


def ran(log, command, cwd=ROOT) -> bool:
    said = subprocess.run([str(word) for word in command], cwd=cwd, stdout=log,
                          stderr=subprocess.STDOUT)
    return said.returncode == 0


def waiting() -> list[dict[str, str]]:
    """The engine's functions new or changed since the review's verdict, as its ledger holds
    them once bench.py inventory ran; none without a ledger."""
    ledger = PRIVATE / "ledger.tsv"
    if not ledger.is_file():
        return []
    with ledger.open(encoding="utf-8") as rows:
        return [row for row in csv.DictReader(rows, delimiter="\t")
                if not row["file"].startswith("tools/") and (row["status"] == "-" or (
                    row["status"] in ("reviewed", "finding") and row["reviewed_hash"]
                    and row["hash"] != row["reviewed_hash"]))]


def unjudged(log, out: Path) -> bool:
    """bench.py inventory, then unjudged.md beside the report: each engine function new or
    changed since its verdict, with the commit that last changed its lines, for the round
    the lead asks for by hand (the plan's 5.3). A function waiting fails nothing."""
    if not ran(log, [sys.executable, TOOLS / "bench" / "bench.py", "inventory"]):
        return False
    inventory = json.loads((ROOT / "out" / "bench" / "inventory.json").read_text())
    ends = {(each["file"], each["line"]): each["end"]
            for each in inventory["functions"].values()}
    lines = ["# Engine functions waiting for a verdict", ""]
    for row in waiting():
        line = int(row["line"])
        said = subprocess.run(["git", "log", "-1", "--format=commit %h %s", "-L",
                               f"{line},{ends.get((row['file'], line), line)}:"
                               f"src/{row['file']}"], cwd=ROOT, capture_output=True,
                              text=True).stdout
        last = next((each[len("commit "):] for each in said.splitlines()
                     if each.startswith("commit ")), "no commit found")
        new = "new" if row["status"] == "-" else "changed"
        lines.append(f"- `{row['function']}` ({new}), last by {last}")
    if len(lines) == 2:
        lines.append("None: every engine function has its verdict.")
    (out / "unjudged.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    log.write(f"{len(lines) - 2} engine functions wait for a verdict: unjudged.md\n")
    return True


def mutate(log, out: Path) -> bool:
    """MUTANTS mutants of one engine file, the day's pick (bench.py mutants, the plan's
    7.3), in a tree of their own at this commit, as they rewrite the file while they run,
    on the driver this tree's debug build takes, which the records were made on. What
    survives is in mutants.md, for a round to give inputs: a survivor fails nothing. A
    file whose functions wait for a verdict goes first, as its new code is what the next
    round reads; else the day's file of the rotation."""
    tree = out / "mutants-tree"
    cased = sorted(case.stem for case in (TOOLS / "bench" / "cases").glob("*.cpp")
                   if any((ROOT / "src" / folder / case.name).is_file()
                          for folder in ("runtime", "baseclasses")))
    # A file whose functions another bench runs too, as Views' sessions run Edits' and
    # Commands', Schedule's bind runs Checks' and reflects every project's shaders, is run
    # by both, its own bench named first.
    also = {"Checks": "Schedule", "Commands": "Views", "Edits": "Views",
            "Shader": "Schedule"}
    picks = [(f"{stem},{also[stem]}" if stem in also else stem, None) for stem in cased]
    picks.append(("Schedule", "runtime/Binder.cpp"))
    day = datetime.now(timezone.utc).date().toordinal()
    pending = {Path(row["file"]).stem for row in waiting()}
    first = [pick for pick in picks
             if Path(pick[1] or pick[0].split(",")[0]).stem in pending] or picks
    bench, source = first[day % len(first)]
    driver = test_driver()
    passed = (ran(log, ["git", "worktree", "add", "--detach", tree, "HEAD"])
              and ran(log, ["cmake", "--preset", "debug", f"-DVULPEN_TEST_DRIVER={driver}"],
                      tree)
              and ran(log, ["cmake", "--build", "--preset", "debug", "--parallel", JOBS], tree)
              and ran(log, [sys.executable, tree / "src/tools/bench/bench.py", "mutants",
                            bench, "--limit", MUTANTS, "--seed", day,
                            *(["--source", source] if source else [])], tree))
    found = tree / "out" / "bench" / "mutants" / f"{Path(source or bench.split(',')[0]).stem}.md"
    if found.is_file():
        shutil.copy(found, out / "mutants.md")
    ran(log, ["git", "worktree", "remove", "--force", tree])
    return passed


def counted(log) -> bool:
    """LOC.md as the builds counted the lines again: the commit's own, else a change
    landed without the statistics its build wrote."""
    said = subprocess.run(["git", "diff", "--stat", "HEAD", "--", LINES], cwd=ROOT,
                          capture_output=True, text=True).stdout
    log.write(said or f"{LINES} is as committed\n")
    return not said


def pictured(log) -> bool:
    """The README's pictures as the release build took them again: the commit's own, else
    a change moved what a scene shows and its pictures were not taken again. The build
    takes them on the tests' driver, so a difference is never another driver's."""
    if not test_driver("release"):
        log.write("the release build names no VULPEN_TEST_DRIVER, so its pictures are the "
                  "machine's own GPU's\n")
        return False
    said = subprocess.run(["git", "status", "--porcelain", "--", PICTURES], cwd=ROOT,
                          capture_output=True, text=True).stdout
    log.write(said or f"{PICTURES} is as committed\n")
    return not said


def download(log, out: Path) -> bool:
    """The commit as GitHub's source download holds it, git archive's, in a folder git
    finds no repository above: it configures and builds as the README's quick start does,
    and its vulpen names the commit (RC07), as v0.1.0's download first did not."""
    folder, archive = out / "download", out / "download.tar"
    env = {**os.environ, "GIT_CEILING_DIRECTORIES": str(out)}

    def run(command, cwd=folder) -> subprocess.CompletedProcess:
        log.write(f"$ {' '.join(map(str, command))}\n")
        log.flush()
        return subprocess.run(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    try:
        if run(["git", "archive", "--format=tar", "-o", archive, "HEAD"], ROOT).returncode:
            return False
        folder.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(folder, filter="data")
        for command in (["cmake", "--preset", "debug"],
                        ["cmake", "--build", "--preset", "debug", "--parallel", JOBS]):
            said = run(command)
            log.write(said.stdout)
            if said.returncode:
                return False
        # The cli, which draws no window, a few frames on the tests' driver: the download's
        # own modules load and run.
        env |= {"DISPLAY": "", "WAYLAND_DISPLAY": "", "VK_DRIVER_FILES": test_driver()}
        said = run([folder / "out" / "build" / "debug" / "vulpen",
                    folder / "src" / "runtime" / "recipes" / "vulpen" / "cli" / "view.vlp",
                    "--frames", "3"])
        log.write(said.stdout)
        stamp = STAMP.search(said.stdout)
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True,
                              text=True).stdout.strip()
        log.write(f"the download's vulpen exited {said.returncode}, naming "
                  f"{stamp[1] if stamp else 'no commit'}; HEAD is {head}\n")
        return said.returncode == 0 and bool(stamp) and head.startswith(stamp[1])
    finally:
        archive.unlink(missing_ok=True)
        shutil.rmtree(folder, ignore_errors=True)  # a whole build, which the log holds


def unreleasable() -> str | None:
    """Why main could not move to what a release run checks: the tree differs from its
    commit, as an untracked file reaches a build and the README's pictures, so what ran
    would not be the commit; or main is not behind it, so it could not fast-forward."""
    said = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True,
                          text=True).stdout
    if said:
        return f"the tree differs from its commit:\n{said}run release in a worktree of it"
    if subprocess.run(["git", "merge-base", "--is-ancestor", MAIN, "HEAD"],
                      cwd=ROOT).returncode:
        return f"{MAIN} is not behind HEAD, so it could not fast-forward to it"
    return None


def beside_sweep(steps: list[Step]) -> list[Step]:
    """nightly's and release's steps, run as full runs its own: every configure and build
    first, alone, as the release build times wave and takes the README's pictures; then
    the runs of vulpen in this tree one at a time, in their order, beside the whole sweep,
    whose copy lies outside out/, where no check for another vulpen of this tree looks."""
    built = [step for step in steps if step.name.split()[0] in ("configure", "build")]
    for step in steps:
        step.chain = (None if step in built else
                      "sweep" if step.name == "commands" else "tree")
    return built + [step for step in steps if step not in built]


def steps(tier: str, out: Path, window: dict[str, str] | None) -> list[Step]:
    python = sys.executable
    quick = preset("debug", window)
    if sys.platform != "win32":
        quick.insert(2, Step("hidden names", [python, TOOLS / "hidden-names.py",
                                              ROOT / "out/build/debug"], "build debug"))
    if tier == "quick":
        return quick
    if tier == "full":
        # After the debug build, one chain of what runs vulpen in this tree, whose ide
        # keeps its panels' layout there for the next run: so one at a time.
        asan = preset("asan", window, BESIDE, "asan")
        for step in [*quick[2:], asan[2]]:
            step.chain = "tree"
        return [*quick,
                Step("examples", [python, TOOLS / "examples-replay.py",
                                  ROOT / "out/build/debug/vulpen", "--said",
                                  out / "examples-replay.log"], "build debug", chain="tree"),
                Step("commands", [python, TOOLS / "command-sweep.py", "--only",
                                  "sweep,gui", "--jobs", COPIES, "--report",
                                  out / "command-sweep.md"], chain="sweep"),
                *asan,
                # Last in the tree's chain: its benches build with every thread, once
                # the sweep's copies and the sanitizer's build are about done.
                Step("bench", [python, TOOLS / "bench" / "bench.py", "compare"],
                     "build debug", chain="tree")]
    if tier == "release":
        return beside_sweep([*steps("nightly", out, window),
                             Step("lines counted", counted, "build debug"),
                             Step("pictures taken", pictured, "build release"),
                             Step("download", lambda log: download(log, out)),
                             Step("compare main", [python, TOOLS / "compare-main.py",
                                                   "--base", MAIN, "--out",
                                                   out / "compare-main"], "build debug")])
    presets = ("debug", "release", "asan", "tsan")
    nightly = [*quick, *(step for name in presets[1:] for step in preset(name, window))]
    nightly += [Step("test gpu-validation", ["ctest", "--preset", "gpu-validation"],
                     "build debug"),
                # On the run's own display, as the other test steps, so window-soak and
                # the soak's windowed half run rather than skip.
                Step("test nightly", ["ctest", "--preset", "nightly"], "build debug",
                     window),
                Step("test asan-nightly", ["ctest", "--preset", "asan-nightly"],
                     "build asan"),
                Step("commands", [python, TOOLS / "command-sweep.py", "--report",
                                  out / "command-sweep.md"],
                     env={**os.environ, "LP_NUM_THREADS": SWEEP_THREADS}),
                Step("bench", [python, TOOLS / "bench" / "bench.py", "run", "--presets",
                                "release,asan,odd"], "build debug"),
                Step("planted", [python, TOOLS / "bench" / "bench.py", "planted"],
                     "build debug"),
                Step("coverage", [python, TOOLS / "bench" / "bench.py", "coverage"]),
                *([Step("unjudged", lambda log: unjudged(log, out), "build debug")]
                  if (PRIVATE / "pages.py").is_file() else []),
                Step("mutants", lambda log: mutate(log, out))]
    return beside_sweep(nightly)


def log_of(step: Step, out: Path) -> Path:
    return out / f"{step.name.replace(' ', '-')}.log"


def take(step: Step, out: Path, by_name: dict[str, Step], alone: bool) -> None:
    """The step, once the step it needs ended, unless that did not pass; an error in it
    fails it, its traceback in its log. A step alone says how it went on its own line, as
    it runs; one beside others, a line as it starts and one as it ends. A step of no chain
    needs only steps before it, which ended already, so it never waits on a chain."""
    try:
        needed = by_name.get(step.needs) if step.needs else None
        if needed and not alone:
            needed.ended.wait()
        if step.needs and (needed is None or needed.result != "passed"):
            print(f"{step.name}: skipped, as {step.needs} did not pass", flush=True)
            return
        print(f"{step.name}: " + ("" if alone else "started\n"), end="", flush=True)
        try:
            run(step, out)
        except Exception:
            step.result = "failed"
            with log_of(step, out).open("a") as log:
                log.write(traceback.format_exc())
        print(("" if alone else f"{step.name}: ") +
              f"{step.result} in {step.seconds:.0f} s", flush=True)
    finally:
        step.ended.set()


def chained(chain: list[Step], out: Path, by_name: dict[str, Step]) -> None:
    for step in chain:
        take(step, out, by_name, alone=False)


def run(step: Step, out: Path) -> None:
    started = time.monotonic()
    with log_of(step, out).open("w") as log:
        if callable(step.run):
            passed = step.run(log)
        else:
            command = [str(word) for word in step.run]
            log.write(f"$ {' '.join(command)}\n")
            log.flush()
            passed = subprocess.run(command, cwd=ROOT, env=step.env, stdout=log,
                                    stderr=subprocess.STDOUT).returncode == 0
    step.result = "passed" if passed else "failed"
    step.seconds = time.monotonic() - started


def commit() -> str:
    said = subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT,
                          capture_output=True, text=True)
    return said.stdout.strip() or "an unknown commit"


def released(out: Path, named: str) -> list[str]:
    """What a release run's report adds: the README pictures its release build took
    otherwise than the commit holds them, compare-main.py's report, and what no machine
    here runs, to try by hand before main moves."""
    pictures = subprocess.run(["git", "status", "--porcelain", "--", PICTURES], cwd=ROOT,
                              capture_output=True, text=True).stdout.splitlines()
    driver = test_driver("release") or "the machine's own GPU"
    lines = ["", "## The README's pictures", "",
             f"The release build took them on {driver}, each as the commit holds it."]
    if pictures:
        lines[-1] = (f"The release build took these on {driver} otherwise than the commit "
                     "holds them, so they are stale: take them again with this commit's "
                     "release build, and commit them:")
        lines += ["", *(f"- {line[3:]}" for line in pictures)]
    compared = out / "compare-main" / "compare-main.md"
    said = compared.read_text().splitlines() if compared.exists() else [
        "# compare-main.py wrote no report: what it said is in compare-main.log"]
    lines += ["", *(f"#{line}" if line.startswith("#") else line for line in said)]
    return lines + ["", "## By hand", "",
                    f"- [ ] GitHub CI passed on {named}, on Linux and Windows",
                    "- [ ] `./run.sh` on the machine's own GPU: the ide opens, and a project "
                    "draws as it should",
                    "- [ ] `.\\run.ps1` on Windows: the same",
                    f"- [ ] Each difference above is one a commit since {MAIN} names", "",
                    f"Then {MAIN} fast-forwards to {named}, never by a merge commit or a "
                    "squash, so what people get is what ran."]


def report(tier: str, done: list[Step], out: Path, began: datetime, named: str,
           took: float) -> str:
    lines = [f"# check {tier} at {named}, {began:%Y-%m-%d %H:%M} UTC", "",
             "| Step | Result | Time |", "| --- | --- | --: |"]
    lines += [f"| {step.name} | {step.result} | {step.seconds:.0f} s |"
              for step in done]
    lines += ["", f"In all: {took / 60:.1f} minutes."]
    for step in done:
        if step.result == "failed":
            said = log_of(step, out).read_text(errors="replace").splitlines()
            # A parallel build goes on past its first error, which the tail may not hold.
            errors = [line for line in said[:-TAIL] if "error" in line.lower()][:TAIL]
            lines += ["", f"## {step.name}", "", "```", *errors,
                      *(["..."] if errors else []), *said[-TAIL:], "```"]
    if tier == "release":
        lines += released(out, named)
    lines += ["", f"What each step said: {out.relative_to(ROOT)}/", ""]
    return "\n".join(lines)


def check(tier: str) -> int:
    if tier == "release" and (refused := unreleasable()):
        sys.exit(refused)
    began = datetime.now(timezone.utc)
    named = commit()  # before the builds, which rewrite the statistics
    out = ROOT / "out" / "check" / f"{tier}-{began:%Y%m%dT%H%M%SZ}"
    out.mkdir(parents=True)
    started = time.monotonic()
    with screen(tier != "quick") as window:
        done = steps(tier, out, window)
        by_name = {step.name: step for step in done}
        chains: dict[str, list[Step]] = {}
        for step in done:
            if step.chain:
                chains.setdefault(step.chain, []).append(step)
            else:
                take(step, out, by_name, alone=True)
        beside = [threading.Thread(target=chained, args=(chain, out, by_name), daemon=True)
                  for chain in chains.values()]
        for thread in beside:
            thread.start()
        for thread in beside:
            thread.join()
    text = report(tier, done, out, began, named, time.monotonic() - started)
    (out / "report.md").write_text(text)
    if tier in ("nightly", "release"):
        REPORTS.mkdir(parents=True, exist_ok=True)
        kept = f"{began:%Y%m%d}" if tier == "nightly" else named
        shutil.copy(out / "report.md", REPORTS / f"check-{tier}-{kept}.md")
    failed = [step.name for step in done if step.result != "passed"]
    said = "all passed" if not failed else "not passed: " + ", ".join(failed)
    print(f"{tier}: {said}; {(out / 'report.md').relative_to(ROOT)}")
    return 1 if failed else 0


def install_hook() -> int:
    said = subprocess.run(["git", "rev-parse", "--git-path", "hooks"], cwd=ROOT,
                          capture_output=True, text=True, check=True)
    hooks = Path(said.stdout.strip())
    hook = (hooks if hooks.is_absolute() else ROOT / hooks) / "pre-push"
    if hook.exists() and hook.read_text() != HOOK:
        sys.exit(f"{hook} is another's hook, which check.py leaves as it is")
    hook.parent.mkdir(parents=True, exist_ok=True)
    hook.write_text(HOOK)
    hook.chmod(0o755)
    print(f"wrote {hook}: each git push runs check.py quick first")
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("tier",
                        choices=("quick", "full", "nightly", "release", "install-hook"))
    tier = parser.parse_args().tier
    sys.exit(install_hook() if tier == "install-hook" else check(tier))


if __name__ == "__main__":
    main()
