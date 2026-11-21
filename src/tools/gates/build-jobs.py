#!/usr/bin/env python3
"""Fail when a build of ours starts with no count of jobs (A03).

cmake --build --parallel with no count hands Unix Makefiles, the generator a Linux
configure takes by default, make -j: no limit and no jobserver, so a fresh tree, or every
module once an engine header changed, starts every compile at once. So each --parallel in
the scripts, the tools, CI and the engine's C++ names its count, as a number, a variable or
a format's placeholder. Windows' builds are left as they are: Visual Studio bounds its own
jobs, so run.ps1 and CI's jobs that run on Windows may name none.

No view holds a build command, so with --view it checks nothing.

Usage: python3 src/tools/gates/build-jobs.py [BUILD_DIR] [--view FOLDER]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src"
VENDORED = SOURCE / "external-libraries"  # not our code, so not our rules
WORKFLOWS = ROOT / ".github" / "workflows"
SUFFIXES = (".py", ".sh", ".cmake", ".cpp", ".h")
# --parallel, then what comes after it past quotes, commas and blanks: a count starts with
# a digit, $ (a shell's), { (a format's) or a name; the end of the command, another flag
# or a redirect means none.
PARALLEL = re.compile(r"--parallel[\"',\s]*(.?)")
NONE = set("])-|&;>\n") | {""}
COMMENT = {".py": "#", ".sh": "#", ".cmake": "#", ".yml": "#", ".cpp": "//", ".h": "//"}
JOB = re.compile(r"^  [\w-]+:\s*$")  # a job of a workflow, at its first indent


def problems(file: Path, skipped=lambda line: False) -> list[str]:
    found = []
    mark = COMMENT.get(file.suffix, "#")
    for number, line in enumerate(file.read_text(encoding="utf-8").splitlines(), 1):
        code = line.split(mark, 1)[0] if not line.lstrip().startswith(mark) else ""
        if skipped(line):
            continue
        for match in PARALLEL.finditer(code + "\n"):
            if match.group(1) in NONE:
                found.append(f"{file.relative_to(ROOT).as_posix()}:{number}: "
                             "--parallel with no count, which Unix Makefiles run as make -j "
                             "with no limit (A03)")
    return found


def workflow(file: Path) -> list[str]:
    """A workflow's jobs, but those that run on Windows."""
    windows = False

    def skipped(line: str) -> bool:
        nonlocal windows
        if JOB.match(line):
            windows = False
        elif line.strip().startswith("runs-on:") and "windows" in line:
            windows = True
        return windows
    return problems(file, skipped)


def main() -> None:
    if "--view" in sys.argv:
        return
    files = [ROOT / "run.sh", ROOT / "CMakeLists.txt",
             *(file for suffix in SUFFIXES for file in SOURCE.rglob(f"*{suffix}")
               if VENDORED not in file.parents)]
    found = [problem for file in sorted(files) if file.is_file()
             for problem in problems(file)]
    found += [problem for file in sorted(WORKFLOWS.glob("*.yml")) for problem in workflow(file)]
    if found:
        sys.exit("\n".join(found))


if __name__ == "__main__":
    main()
