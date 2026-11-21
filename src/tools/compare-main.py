#!/usr/bin/env python3
"""Run main's own lines on main's build and on this tree's, and list where they differ.

A regression is what main did that this tree no longer does, where no test looks, or
where a test's expectation changed along with it. A session replays to the same output
(V08, C01), so the same lines run on both builds, on the same driver, and each difference
is listed for a person to read before main moves to this commit (A00): one a commit since
main names, or a regression.

- usages: help on each build, as a log or a script made on main names its commands: each
  usage gone, new, or with its help reworded.
- scenes: the README's pictures as main takes them, its scenes run by each build's own
  update-readme-images.py, compared byte for byte, and what stopped each run.
- projects: main's projects, each copied under main-<name> and loaded in each build, as
  a person's project made on main would be; the live build builds it there. What each met
  as it loaded, built and ran.

main builds in a worktree of its own in --out, removed after, unless --main-tree names a
built one. Fails where a project or a scene ran clean on main and meets a problem here,
or a run ends; differences alone are listed, in compare-main.md in --out.

Usage: python3 src/tools/compare-main.py --out DIR [--base REF] [--main-tree DIR]
                                         [--only usages,scenes,projects]
"""
import argparse
import importlib.util
import os
import re
import shutil
import signal
import subprocess
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import JOBS, test_driver  # noqa: E402 (beside it, but not on the path when -I)

ROOT = Path(__file__).resolve().parents[2]
PARTS = ("usages", "scenes", "projects")
COPY = "main-"  # a main project's copy: a name neither build holds a view of
SETTLE_FRAMES = 60  # a project runs, once built, before what it met is read
SHOWN = 3  # of a project's problems, in the table
# A line vulpen logs: its time on the first, then the tag of who says it; an answer has
# neither.
TAGGED = re.compile(r"^\s*(\d\d\.\d\d\.\d\d \d\d:\d\d:\d\d\s+)?\{")
GAP = re.compile(r"\s{2,}")  # between a usage and its help, as help aligns them


@dataclass
class Build:
    """A tree's name in the report, the tree, and the tools of its own commit that drive
    its debug vulpen: examples-replay.py's runs, update-readme-images.py's scenes."""
    name: str
    tree: Path
    replay: ModuleType
    images: ModuleType

    @property
    def binary(self) -> Path:
        return self.tree / "out" / "build" / "debug" / "vulpen"


def module(path: Path, name: str) -> ModuleType:
    """A tool of a tree under a name of its own, so main's and this tree's sit side by
    side; each imports its own tree's tests harness."""
    sys.modules.pop("harness", None)
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    sys.modules[name] = loaded  # a dataclass looks its module up by name
    spec.loader.exec_module(loaded)
    return loaded


def build_of(name: str, tree: Path) -> Build:
    tools = tree / "src" / "tools"
    return Build(name, tree, module(tools / "examples-replay.py", f"{name}_replay"),
                 module(tools / "update-readme-images.py", f"{name}_images"))


def built_main(base: str, out: Path, driver: str) -> Path:
    """main in a worktree of its own, configured on the driver this tree's tests take."""
    tree, said = out / "main", out / "main-build.log"
    commands = [(ROOT, ["git", "worktree", "add", "--detach", tree, base]),
                (tree, ["cmake", "--preset", "debug", f"-DVULPEN_TEST_DRIVER={driver}"]),
                (tree, ["cmake", "--build", "--preset", "debug", "--parallel", JOBS])]
    with said.open("w") as log:
        for cwd, command in commands:
            said = subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
            if said.returncode:
                sys.exit(f"{' '.join(map(str, command))} failed; what it said: {said}")
    return tree


def session(build: Build, scratch: Path, lines: Callable) -> None:
    """A cli hosting the ide headless in the build's own tree, as examples-replay runs
    one, given lines(run); what its parts keep in .vulpen/ put back after, as there."""
    replay = build.replay
    scratch.mkdir(parents=True)
    before = replay.kept()
    for path in replay.SESSION:  # the panels a person left, which the run would restore
        path.unlink(missing_ok=True)
    try:
        run = replay.Run(build.binary, scratch / "said.txt", scratch)
        run.frames(replay.FRAMES_A_LINE)
        lines(run)
        run.end()
    finally:
        replay.put_back(before)


def usages(build: Build, out: Path) -> dict[str, str]:
    """Each command's whole usage, and its help, as help lists them under the words they
    share; with the ide hosted, its commands too."""
    said = []
    session(build, out / build.name / "usages",
            lambda run: said.extend(run.send(build.replay.ok("help"))))
    found, words, answer = {}, [], False
    for line in said:
        if not answer:
            answer = line.rstrip().endswith("{run} command: help")
        elif TAGGED.match(line):
            if found and "{run} command:" in line:  # the frames' barriers, after it
                break
        elif line.strip():
            depth = (len(line) - len(line.lstrip(" "))) // 2
            usage = GAP.split(line.strip(), maxsplit=1)
            words[depth:] = [usage[0]]
            if len(usage) == 2:
                found[" ".join(words)] = usage[1]
    return found


def stop_strays(folder: Path) -> None:
    """A vulpen left running in a scene's copy, as main's take() leaves one that will not
    end: it would go on drawing on the CPU, with no window to close."""
    for cwd in Path("/proc").glob("[0-9]*/cwd"):
        try:
            if folder in Path(os.readlink(cwd)).parents:
                os.kill(int(cwd.parent.name), signal.SIGKILL)
        except OSError:  # another person's, or one that ended
            continue


def scenes(build: Build, runs: tuple, out: Path) -> tuple[dict[str, Path], dict[int, str]]:
    """main's README scenes, each run taken by the build's own update-readme-images.py:
    the pictures, and by run, what stopped one, as take() says it exiting."""
    pictures, stopped = {}, {}
    for index, run in enumerate(runs):
        into = out / build.name / f"scenes-{index}"
        into.mkdir(parents=True)
        try:
            build.images.take(build.binary, build.images.Run(run.scenes, run.size, run.theme),
                              into)
        except (SystemExit, Exception) as said:  # take() exits naming why
            stopped[index] = str(said).strip()
            stop_strays(into)
        pictures |= {png.stem: png for png in into.glob("*.png")}
    return pictures, stopped


def projects_of(tree: Path) -> list[Path]:
    """A tree's projects as its build finds them (CMakeLists.txt): each folder in projects/
    with a view.vlp, and each in a folder there that holds none, as triangles/."""
    found = []
    for folder in sorted(path for path in (tree / "projects").iterdir() if path.is_dir()):
        if (folder / "view.vlp").exists():
            found.append(folder)
        else:
            found += sorted(inner for inner in folder.iterdir()
                            if (inner / "view.vlp").exists())
    return found


def forget(build: Build, copies: list[Path]) -> None:
    """The copies, and what their builds left in the build's tree: their modules, and
    their lines in mounted-views, which its next build would otherwise build again."""
    for copy in copies:
        shutil.rmtree(build.binary.parent / "views" / copy.name, ignore_errors=True)
        shutil.rmtree(copy, ignore_errors=True)
    mounts = build.binary.parent / "mounted-views"
    if mounts.exists():
        lines = mounts.read_text().splitlines()
        kept = [line for line in lines if not any(line == str(copy) for copy in copies)]
        mounts.write_text("".join(f"{line}\n" for line in kept))


def met(build: Build, run, copy: Path) -> list[str]:
    """What a project met: lines refused as it loads; once the live build is idle, loaded
    again, as examples-replay checks a copy, its nodes' errors and warnings as they bind
    built; and what no line asked for, as a failed build or a validation message."""
    ok, replay = build.replay.ok, build.replay
    first = run.send(ok(f"view load {copy}"))
    idle = run.idle()
    again = run.send(ok(f"view close {copy.name}")) + run.send(ok(f"view load {copy}"))
    run.frames(SETTLE_FRAMES)
    again += run.new()
    found = [*replay.refusals(first), *replay.refusals(again),
             *replay.bound(again, copy.name),
             *(f"{what}: {' / '.join(said)}" for what, said in run.problems)]
    run.problems.clear()
    run.send(ok(f"view close {copy.name}"))
    return found if idle else [*found, "the live build never went idle"]


def projects(build: Build, originals: list[Path], out: Path) -> dict[str, list[str]]:
    """main's projects, each copied under main-<name>, loaded in the build and closed once
    it ran: what each met, by its copy's name, and "the run" for a run that ended."""
    folder = out / build.name / "projects"
    copies = [folder / f"{COPY}{original.name}" for original in originals]
    for original, copy in zip(originals, copies):
        shutil.copytree(original, copy, ignore=shutil.ignore_patterns(".vulpen"))
    found: dict[str, list[str]] = {}

    def load(run) -> None:
        for copy in copies:
            found[copy.name] = met(build, run, copy)

    try:
        session(build, out / build.name / "projects-run", load)
    except build.replay.Ended as ended:
        found["the run"] = [str(ended)]
    finally:
        forget(build, copies)
    return found


def usages_part(main: dict[str, str], here: dict[str, str]) -> list[str]:
    """Each usage gone, new or reworded. None fails: a usage may go on purpose, which only
    a person reading the commits can tell."""
    gone, new = sorted(main.keys() - here.keys()), sorted(here.keys() - main.keys())
    reworded = sorted(usage for usage in main.keys() & here.keys()
                      if main[usage] != here[usage])
    lines = ["## Usages", "", f"{len(main)} on main, {len(here)} here: help on each."]
    for title, found in (("Gone: a line of main's naming one is refused here", gone),
                         ("New", new)):
        if found:
            lines += ["", f"{title}:", "", *(f"- `{usage}`" for usage in found)]
    if reworded:
        lines += ["", "Help reworded:", ""]
        lines += [f"- `{usage}`\n  - main: {main[usage]}\n  - here: {here[usage]}"
                  for usage in reworded]
    return lines


def scenes_part(main: tuple, here: tuple, out: Path) -> tuple[list[str], bool]:
    """The pictures side by side; failed where a run stopped here that ran through on main."""
    (main_pictures, main_stopped), (here_pictures, here_stopped) = main, here
    lines = ["## The README's scenes, main's", "", "| Picture | Here |", "| --- | --- |"]
    for name in sorted(main_pictures.keys() | here_pictures.keys()):
        before, after = main_pictures.get(name), here_pictures.get(name)
        verdict = ("not taken here" if not after else "not taken on main" if not before
                   else "same" if before.read_bytes() == after.read_bytes()
                   else f"differs: {before.relative_to(out)}, {after.relative_to(out)}")
        lines.append(f"| {name} | {verdict} |")
    for side, stopped in (("main", main_stopped), ("here", here_stopped)):
        lines += [f"\nRun {index} stopped {side}:\n\n```\n{said}\n```" for index, said in
                  sorted(stopped.items())]
    return lines, any(index not in main_stopped for index in here_stopped)


def projects_part(main: dict[str, list[str]],
                  here: dict[str, list[str]]) -> tuple[list[str], bool]:
    """Each project's problems on each build; failed where one ran clean on main and meets
    a problem here."""
    lines = ["## main's projects", "", "| Project | main | here |", "| --- | --- | --- |"]
    failed = "the run" in here and "the run" not in main
    for name in sorted(main.keys() | here.keys()):
        before, after = main.get(name), here.get(name)
        failed |= before == [] and bool(after)
        shown = ["not run" if found is None else " / ".join(found[:SHOWN]) or "clean"
                 for found in (before, after)]
        lines.append(f"| {name} | {shown[0]} | {shown[1]} |")
    return lines, failed


def sha(ref: str) -> str:
    return subprocess.run(["git", "rev-parse", "--short", ref], cwd=ROOT, check=True,
                          capture_output=True, text=True).stdout.strip()


def report(base: str, driver: str, parts: list[list[str]], out: Path) -> None:
    commits = subprocess.run(["git", "log", "--format=- %h %s", f"{base}..HEAD"], cwd=ROOT,
                             check=True, capture_output=True, text=True).stdout.splitlines()
    lines = [f"# {base} {sha(base)} and this tree {sha('HEAD')}, compared", "",
             f"Both builds ran on {driver}. {len(commits)} commits since {base}: each "
             "difference below is one of them, or a regression.", "", *commits]
    for part in parts:
        lines += ["", *part]
    (out / "compare-main.md").write_text("\n".join(lines) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--base", default="main", help="the commit compared with")
    parser.add_argument("--main-tree", type=Path, help="a built tree of that commit")
    parser.add_argument("--only", default=",".join(PARTS), help="the parts, comma-separated")
    arguments = parser.parse_args()
    out, only = arguments.out.resolve(), arguments.only.split(",")
    if not (driver := test_driver()):
        sys.exit("out/build/debug names no VULPEN_TEST_DRIVER: both builds run on it")
    os.environ["VK_DRIVER_FILES"] = driver  # every run inherits it
    out.mkdir(parents=True)
    built = None if arguments.main_tree else out / "main"  # removed after, as made here
    try:
        tree = arguments.main_tree.resolve() if arguments.main_tree else built_main(
            arguments.base, out, driver)
        compared = [build_of("main", tree), build_of("here", ROOT)]
        if others := [found for build in compared for found in build.replay.running()]:
            sys.exit("another vulpen of a tree compared runs:\n" + "\n".join(others))
        parts, failed = [], False
        if "usages" in only:
            parts.append(usages_part(*(usages(build, out) for build in compared)))
        if "scenes" in only:
            runs = compared[0].images.README.runs
            taken = (scenes(build, runs, out) for build in compared)
            found, broke = scenes_part(*taken, out)
            parts.append(found)
            failed |= broke
        if "projects" in only:
            originals = projects_of(tree)
            found, broke = projects_part(*(projects(build, originals, out)
                                           for build in compared))
            parts.append(found)
            failed |= broke
        report(arguments.base, driver, parts, out)
    finally:
        if built and built.exists():
            subprocess.run(["git", "worktree", "remove", "--force", built], cwd=ROOT)
    print(f"{'failed' if failed else 'compared'}: {out / 'compare-main.md'}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
