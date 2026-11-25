"""Coverage for bench.py: the lines of each engine file, and of the bench's own
(tools/bench, as the review runs on itself), that the benches' rows and the
tests run, as gcov counts them in a build of its own (the coverage preset). A line
nothing runs shows on its function's card in the report: an input no case gives yet, or
code nothing needs (C00). The counts go to out/bench/coverage.json, by engine file and
line, for the report. With benches named, only their rows run, without the tests.

A whole run is also the net's guard (the plan's 7.3): records/coverage.tsv holds each
file's lines with code and how many of them no row or test runs, and a run fails where a
file's lines run by nothing grew, as code came without the inputs that reach it. A
refactor that moves lines moves no count; --approve WHY writes this run's counts there,
the why kept for the commit as approve keeps a function's.

The tests run under the run's own Xvfb, on lavapipe, so their windows open on no screen
and their devices on no GPU of this machine.

hot, from the same build: the engine functions a frame calls (CPP10's hot path, item 9g),
as the calls of wave's frames counted twice, a few and many, headless on lavapipe; what
the extra frames called at least once each is hot. Kept in out/bench/hot.json, which
bench.py cost --base reads, and shown on each card.
"""
import json
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

PRESET = "coverage"
BATCH = 200  # .gcda files per gcov call, well under a command line's length
RECORD = "coverage.tsv"
HEAD = ("# Each engine and bench file's lines holding code, and how many of them no bench's\n"
        "# row and no test runs, as bench.py coverage counts them; a run fails where the\n"
        "# second grows.\n")
HOT_VIEW = Path("projects") / "wave" / "view.vlp"
HOT_FRAMES = (20, 120)  # a run of a few frames and one of many: the difference is per frame


def kept(runner) -> dict[str, tuple[int, int]]:
    path = runner.RECORDS / RECORD
    if not path.is_file():
        return {}
    rows = [line.split("\t") for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")]
    return {file: (int(lines), int(never)) for file, lines, never in rows}


def guarded(now: dict[str, tuple[int, int]], runner) -> list[str]:
    """What grew past the record: a file's lines run by nothing, or a file it lacks."""
    before = kept(runner)
    problems = [f"{file}: no record yet; approve it" for file in sorted(now.keys() - before.keys())]
    problems += [f"{file}: {never} lines no row or test runs, {never - before[file][1]} more "
                 f"than records/{RECORD} holds; give them inputs, or approve with the why"
                 for file, (_, never) in sorted(now.items())
                 if file in before and never > before[file][1]]
    return problems


def entries(build: Path, runner):
    """gcov's entry for each engine or bench file in each object of the build that
    compiled it: the engine's own, a case's including its .cpp, and each includer of a
    header's inline code; with the file as runtime/Text.h or tools/bench/Bench.h."""
    cased = {runner.SOURCE / folder for folder in runner.CASED}
    data = sorted(str(path) for path in build.rglob("*.gcda"))
    for start in range(0, len(data), BATCH):
        done = subprocess.run(["gcov", "--json-format", "--stdout",
                               *data[start:start + BATCH]],
                              cwd=build, capture_output=True, text=True)
        if done.returncode != 0:
            print(f"gcov: {done.stderr.strip()[:2000]}", file=sys.stderr)
        for document in map(json.loads, done.stdout.splitlines()):
            folder = Path(document["current_working_directory"])
            for entry in document["files"]:
                path = (folder / entry["file"]).resolve()
                if path.parent in cased:
                    yield path.relative_to(runner.SOURCE).as_posix(), entry


def counted(build: Path, runner) -> dict[str, dict[int, int]]:
    """Each engine file's lines that hold code and how often they ran, summed."""
    found: dict[str, dict[int, int]] = defaultdict(lambda: defaultdict(int))
    for file, entry in entries(build, runner):
        for line in entry["lines"]:
            found[file][line["line_number"]] += line["count"]
    return found


def called(build: Path, runner) -> dict[tuple[str, int], list]:
    """How often each engine function ran, by its file and first line, summed over its
    objects and instantiations; with its name."""
    found: dict[tuple[str, int], list] = {}
    for file, entry in entries(build, runner):
        for function in entry["functions"]:
            found.setdefault((file, function["start_line"]),
                             [function["demangled_name"], 0])[1] += function["execution_count"]
    return found


def tested(runner) -> str:
    """The tests' summary line, run as the debug preset runs them."""
    env = {key: value for key, value in os.environ.items()
           if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
    if lavapipe := runner.driver(PRESET):
        env["VK_DRIVER_FILES"] = lavapipe
    # Wider and taller than a window of the tests, so X's pointer, at the screen's middle,
    # starts outside it, as check.py's: the ide test's menus open where the pointer is.
    with runner.display("2600x1500") as screen:
        if screen:
            env["DISPLAY"] = screen
        done = subprocess.run(["ctest", "--preset", PRESET], cwd=runner.ROOT, env=env,
                              capture_output=True, text=True)
    return next((line for line in done.stdout.splitlines() if "tests passed" in line),
                "the tests gave no summary")


def built(runner, benches: bool, whole: bool, chosen=None) -> Path:
    """The coverage build: its benches, those chosen or all, and vulpen and every view."""
    build = runner.ROOT / "out" / "build" / PRESET
    if benches:
        runner.build(PRESET, chosen)
    elif not (build / "CMakeCache.txt").is_file():
        subprocess.run(["cmake", "--preset", PRESET], cwd=runner.ROOT, check=True,
                       stdout=subprocess.DEVNULL)
    if whole:
        done = subprocess.run(["cmake", "--build", str(build), "--parallel", runner.JOBS],
                              cwd=runner.ROOT, capture_output=True, text=True)
        if done.returncode != 0:
            sys.exit(f"the {PRESET} build failed:\n{done.stdout[-4000:]}{done.stderr[-4000:]}")
    return build


def hot(arguments, runner) -> int:
    build = built(runner, benches=False, whole=True)
    sandbox = runner.OUT / "sandbox" / "hot"  # its HOME, where the driver keeps its caches
    sandbox.mkdir(parents=True, exist_ok=True)
    env = {"PATH": os.environ.get("PATH", ""), "HOME": str(sandbox)}
    if lavapipe := runner.driver(PRESET):
        env["VK_DRIVER_FILES"] = lavapipe
    runs = []
    for frames in HOT_FRAMES:
        for stale in build.rglob("*.gcda"):
            stale.unlink()
        ran = subprocess.run([str(build / "vulpen"), str(runner.ROOT / HOT_VIEW), "--frames",
                              str(frames), "--fps", "0"], cwd=sandbox, env=env,
                             capture_output=True, text=True, timeout=runner.TIMEOUT)
        if ran.returncode != 0:
            sys.exit(f"{HOT_VIEW} did not run {frames} frames:\n{ran.stdout[-3000:]}"
                     f"{ran.stderr[-3000:]}")
        runs.append(called(build, runner))
    few, many = runs
    extra = HOT_FRAMES[1] - HOT_FRAMES[0]
    found = [{"file": file, "line": line, "name": name,
              "per_frame": round((count - few.get((file, line), [name, 0])[1]) / extra, 2)}
             for (file, line), (name, count) in sorted(many.items())]
    found = [function for function in found if function["per_frame"] >= 1]
    runner.save(runner.OUT / "hot.json", json.dumps(
        {"view": HOT_VIEW.as_posix(), "functions": found}, indent=1))
    for function in found:
        print(f"{function['per_frame']:9.2f} a frame  {function['file']}:{function['line']} "
              f"{function['name'][:90]}")
    print(f"{len(found)} engine functions run in every frame of {HOT_VIEW.as_posix()}; "
          "out/bench/hot.json")
    return 0


def coverage(arguments, runner) -> int:
    chosen = runner.benches(arguments.benches)
    tests = not arguments.benches
    build = built(runner, benches=True, whole=tests,  # what the tests run too
                  chosen=None if tests else chosen)
    for stale in build.rglob("*.gcda"):
        stale.unlink()
    runner.run_all(chosen, PRESET, (runner.BASELINE,))
    said = tested(runner) if tests else ""
    found = counted(build, runner)
    runner.save(runner.OUT / "coverage.json", json.dumps(
        {"benches": [bench.name for bench in chosen], "tests": said,
         "lines": {file: {str(line): count for line, count in sorted(lines.items())}
                   for file, lines in sorted(found.items())}}, indent=1))
    now = {file: (len(lines), sum(1 for count in lines.values() if not count))
           for file, lines in sorted(found.items())}
    for file, (lines, never) in now.items():
        print(f"{file}: {lines - never} of {lines} lines run")
    total, never = sum(lines for lines, _ in now.values()), sum(n for _, n in now.values())
    print(f"{total - never} of {total} lines run ({100 * (total - never) / max(total, 1):.1f}%)"
          + (f"; tests: {said}" if said else "")
          + f"; {(runner.OUT / 'coverage.json').relative_to(runner.ROOT).as_posix()}")
    if not tests:
        return 0
    if arguments.approve:
        runner.save(runner.RECORDS / RECORD, HEAD + "".join(
            f"{file}\t{lines}\t{never}\n" for file, (lines, never) in now.items()))
        with (runner.OUT / "approved.md").open("a", encoding="utf-8") as whys:
            whys.write(f"- coverage: {arguments.approve}\n")
        print(f"records/{RECORD} written; the why is kept in out/bench/approved.md")
        return 0
    problems = guarded(now, runner)
    for problem in problems:
        print(problem)
    return 1 if problems else 0
