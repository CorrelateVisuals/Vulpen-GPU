#!/usr/bin/env python3
"""Run every check of a tier, so none that guards a change waits on someone recalling it.

quick: configure, build and test the debug preset, as CI does on every push; the build
runs the gates, the mutable-globals check and the statistics. Then, but on Windows, the
names hidden as MSVC refuses them and GCC lets pass (hidden-names.py). About a minute.
full: quick, then every example made again by typing (examples-replay.py), every command
swept in each state with the menus and keys (command-sweep.py's sweep and gui), and the
address sanitizer's build and tests. About half an hour; a round runs it before it
lands.
nightly: every preset built and tested, GPU-assisted validation, the nightly tests (the
soaks, the long fuzz, examples-replay), the whole command sweep, and the simulated
testers where docs/logs/beta-sim/ holds them, in a tree of their own. Some hours; its
report also goes to docs/reports/.
install-hook: a git pre-push hook running quick, which stops a push it fails (opt-in).

Each step runs unless the step it needs did not pass, and the run fails unless every step
passed. What each said is kept in out/check/<tier>-<time>/, with a report of each step's
result and, for one that failed, its errors and its last lines.
full and nightly run the Linux presets (asan, tsan) and Linux's tools.

Usage: python3 src/tools/check.py quick|full|nightly|install-hook
"""
import argparse
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "src" / "tools"
# The simulated testers are private, beside the handoffs, which git ignores.
SIMULATION = ROOT / "docs" / "logs" / "beta-sim" / "sim.py"
REPORTS = ROOT / "docs" / "reports"
TAIL = 30  # lines of a failed step's log the report shows, after those naming an error
HOOK = """#!/bin/sh
# Written by src/tools/check.py install-hook: each push runs its quick checks first, and
# one failing stops the push; git push --no-verify pushes anyway.
exec python3 "$(git rev-parse --show-toplevel)/src/tools/check.py" quick
"""


class Step:
    """A command, or a function given the log, run once the step it needs passed."""

    def __init__(self, name, run, needs=None):
        self.name, self.run, self.needs = name, run, needs
        self.result, self.seconds = "skipped", 0.0


def preset(name: str) -> list[Step]:
    return [Step(f"configure {name}", ["cmake", "--preset", name]),
            Step(f"build {name}", ["cmake", "--build", "--preset", name, "--parallel"],
                 f"configure {name}"),
            Step(f"test {name}", ["ctest", "--preset", name], f"build {name}")]


def simulate(log, out: Path) -> bool:
    """The testers make and remove a project and the ide's kept session, so they run in
    a tree of their own, at this commit."""
    tree = out / "simulation-tree"

    def run(command, cwd=ROOT) -> bool:
        said = subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
        return said.returncode == 0

    passed = (run(["git", "worktree", "add", "--detach", str(tree), "HEAD"])
              and run(["cmake", "--preset", "debug"], tree)
              and run(["cmake", "--build", "--preset", "debug", "--parallel"], tree)
              and run([sys.executable, SIMULATION, tree, out / "simulation"]))
    run(["git", "worktree", "remove", "--force", str(tree)])
    return passed


def steps(tier: str, out: Path) -> list[Step]:
    python = sys.executable
    quick = preset("debug")
    if sys.platform != "win32":
        quick.insert(2, Step("hidden names", [python, TOOLS / "hidden-names.py",
                                              ROOT / "out/build/debug"], "build debug"))
    if tier == "quick":
        return quick
    if tier == "full":
        return [*quick,
                Step("examples", [python, TOOLS / "examples-replay.py",
                                  ROOT / "out/build/debug/vulpen", "--said",
                                  out / "examples-replay.log"], "build debug"),
                Step("commands", [python, TOOLS / "command-sweep.py", "--only",
                                  "sweep,gui", "--report", out / "command-sweep.md"]),
                *preset("asan")]
    presets = ("debug", "release", "asan", "tsan")
    nightly = [*quick, *(step for name in presets[1:] for step in preset(name))]
    nightly += [Step("test gpu-validation", ["ctest", "--preset", "gpu-validation"],
                     "build debug"),
                Step("test nightly", ["ctest", "--preset", "nightly"], "build debug"),
                Step("test asan-nightly", ["ctest", "--preset", "asan-nightly"],
                     "build asan"),
                Step("commands", [python, TOOLS / "command-sweep.py", "--report",
                                  out / "command-sweep.md"])]
    if SIMULATION.exists():
        nightly.append(Step("simulation", lambda log: simulate(log, out)))
    return nightly


def log_of(step: Step, out: Path) -> Path:
    return out / f"{step.name.replace(' ', '-')}.log"


def run(step: Step, out: Path) -> None:
    started = time.monotonic()
    with log_of(step, out).open("w") as log:
        if callable(step.run):
            passed = step.run(log)
        else:
            command = [str(word) for word in step.run]
            log.write(f"$ {' '.join(command)}\n")
            log.flush()
            passed = subprocess.run(command, cwd=ROOT, stdout=log,
                                    stderr=subprocess.STDOUT).returncode == 0
    step.result = "passed" if passed else "failed"
    step.seconds = time.monotonic() - started


def commit() -> str:
    said = subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT,
                          capture_output=True, text=True)
    return said.stdout.strip() or "an unknown commit"


def report(tier: str, done: list[Step], out: Path, began: datetime) -> str:
    lines = [f"# check {tier} at {commit()}, {began:%Y-%m-%d %H:%M} UTC", "",
             "| Step | Result | Time |", "| --- | --- | --: |"]
    lines += [f"| {step.name} | {step.result} | {step.seconds:.0f} s |"
              for step in done]
    for step in done:
        if step.result == "failed":
            said = log_of(step, out).read_text(errors="replace").splitlines()
            # A parallel build goes on past its first error, which the tail may not hold.
            errors = [line for line in said[:-TAIL] if "error" in line.lower()][:TAIL]
            lines += ["", f"## {step.name}", "", "```", *errors,
                      *(["..."] if errors else []), *said[-TAIL:], "```"]
    lines += ["", f"What each step said: {out.relative_to(ROOT)}/", ""]
    return "\n".join(lines)


def check(tier: str) -> int:
    began = datetime.now(timezone.utc)
    out = ROOT / "out" / "check" / f"{tier}-{began:%Y%m%dT%H%M%SZ}"
    out.mkdir(parents=True)
    done = steps(tier, out)
    passed = set()
    for step in done:
        if step.needs and step.needs not in passed:
            print(f"{step.name}: skipped, as {step.needs} did not pass", flush=True)
            continue
        print(f"{step.name}: ", end="", flush=True)
        run(step, out)
        print(f"{step.result} in {step.seconds:.0f} s", flush=True)
        if step.result == "passed":
            passed.add(step.name)
    text = report(tier, done, out, began)
    (out / "report.md").write_text(text)
    if tier == "nightly":
        REPORTS.mkdir(parents=True, exist_ok=True)
        shutil.copy(out / "report.md", REPORTS / f"check-nightly-{began:%Y%m%d}.md")
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
    parser.add_argument("tier", choices=("quick", "full", "nightly", "install-hook"))
    tier = parser.parse_args().tier
    sys.exit(install_hook() if tier == "install-hook" else check(tier))


if __name__ == "__main__":
    main()
