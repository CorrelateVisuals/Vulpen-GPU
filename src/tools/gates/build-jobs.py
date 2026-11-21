#!/usr/bin/env python3
"""Fail when a build of ours starts with no count of jobs (A03).

cmake --build --parallel with no count hands Unix Makefiles, the generator a Linux
configure takes by default, make -j: no limit and no jobserver, so a fresh tree, or every
module once an engine header changed, starts every compile at once. So each --parallel in
the scripts, the tools, CI and the engine's C++ names its count, as a number, a variable or
a format's placeholder, and so does each -j of a cmake --build. Windows' builds are left as
they are: Visual Studio bounds its own jobs, so run.ps1 and CI's jobs that run on Windows
may name none. A job whose runs-on is a matrix is checked: a count works on every system.

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
# --parallel, or -j as an argument list's word ("-j") or on a cmake --build line, then the
# word after it, past quotes, commas and blanks.
FLAG = re.compile(r"(?<![\w-])(--parallel|[\"']-j[\"']|-j)(?![\w-])[\"',\s]*([^\s\"',]*)")
WORD = re.compile(r"[\"',\s]*([^\s\"',]*)")
QUOTES = "\"'"
# A count starts with a digit, $ (a shell's), { (a format's) or a name; the end of the
# command, another flag, a bracket closing or a redirect (2>&1 too) means none.
COUNT = re.compile(r"[\w${]")
COMMENT = {".py": "#", ".sh": "#", ".cmake": "#", ".yml": "#", ".cpp": "//", ".h": "//"}
# Where a call goes on past a line's end, its count may stand first on the next line; a
# shell's command ends with its line, but after a \.
BRACKETED = {".py", ".cmake", ".cpp", ".h"}


def code_of(line: str, mark: str) -> str:
    """A line but its comment: the mark where no quote is open."""
    quote, index = "", 0
    while index < len(line):
        if quote:
            if line[index] == "\\":
                index += 1
            elif line[index] == quote:
                quote = ""
        elif line[index] in "\"'":
            quote = line[index]
        elif line.startswith(mark, index):
            return line[:index]
        index += 1
    return line


def counted(word: str) -> bool:
    return bool(COUNT.match(word)) and not any(sign in word for sign in "<>")


def problems(file: Path, skipped=lambda line: False) -> list[str]:
    found = []
    mark = COMMENT.get(file.suffix, "#")
    # Only an ASCII flag is looked for, so a file in another encoding is read all the same.
    lines = file.read_text(encoding="utf-8", errors="replace").splitlines()
    for index, line in enumerate(lines):
        if skipped(line) or line.lstrip().startswith(mark):
            continue
        code = code_of(line, mark)
        for match in FLAG.finditer(code):
            flag, word = match.groups()
            if flag == "-j" and "--build" not in code:
                continue  # make -j in a sentence, no build of ours
            if word == "\\" or (not word and file.suffix in BRACKETED):
                following = next((later for later in lines[index + 1:] if later.strip()), "")
                word = WORD.match(code_of(following, mark)).group(1)
            if not counted(word):
                found.append(f"{file.relative_to(ROOT).as_posix()}:{index + 1}: "
                             f"{flag.strip(QUOTES)} with no count, which Unix "
                             "Makefiles run as make -j with no limit (A03)")
    return found


def workflow(file: Path) -> list[str]:
    """A workflow's jobs, but those that run on Windows: a job starts at the indent of
    the first line under jobs:, whatever the file's indent is."""
    windows, in_jobs, job_indent = False, False, None

    def skipped(line: str) -> bool:
        nonlocal windows, in_jobs, job_indent
        text = line.strip()
        if not text or text.startswith("#"):
            return windows
        indent = len(line) - len(line.lstrip())
        if indent == 0:
            in_jobs, windows, job_indent = text == "jobs:", False, None
        elif in_jobs:
            job_indent = indent if job_indent is None else job_indent
            if indent <= job_indent:
                windows = False
            elif text.startswith("runs-on:") and "windows" in text:
                windows = True
        return windows
    return problems(file, skipped)


def main() -> None:
    if "--view" in sys.argv:
        return
    files = [ROOT / "run.sh", ROOT / "CMakeLists.txt",
             *(file for suffix in SUFFIXES for file in SOURCE.rglob(f"*{suffix}")
               # nor this gate, whose patterns name the flags it looks for
               if VENDORED not in file.parents and file != Path(__file__).resolve())]
    found = [problem for file in sorted(files) if file.is_file()
             for problem in problems(file)]
    found += [problem for file in sorted(WORKFLOWS.glob("*.yml")) for problem in workflow(file)]
    if found:
        sys.exit("\n".join(found))


if __name__ == "__main__":
    main()
