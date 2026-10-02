#!/usr/bin/env python3
"""Fail when the C++ in src/ breaks a rule a text search can check:

- RC03: ownership is std::unique_ptr or by value, so no std::shared_ptr, and no naked new
  or delete;
- RA01: OS APIs and OS conditionals live only in the platform files, so no other file
  includes a header beyond the standard library, Vulkan, glm, VMA and stb_truetype, or
  tests for an OS;
- RA04: only the platform files write or move a file, so every save goes through
  Files::save, which writes a temp file and renames it over the old one. A run killed
  mid-save then leaves the old file or the new one; a kill rarely lands inside a small
  file's write, so a test that kills runs would pass an unsafe save.

Comments and string literals are left out, so prose and log text never trip a rule.

Usage: python3 src/tools/gates/code-rules.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src"
VENDORED = SOURCE / "external-libraries"  # not our code, so not our rules
PLATFORM = {"baseclasses/Platform.h", "baseclasses/Platform.cpp"}
# What any file may include; the include map says which files do.
LIBRARIES = ("vulkan/", "glm/", "vk_mem_alloc.h", "stb_truetype.h")

# Raw strings first: their text may hold quotes.
LITERALS = re.compile(r'R"([^(\s]*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\''
                      r"|//[^\n]*|/\*.*?\*/", re.DOTALL)
SHARED = re.compile(r"\b(?:shared_ptr|make_shared)\b")
NEW_OR_DELETE = re.compile(r"\b(?:new|delete)\b")
INCLUDE = re.compile(r"^\s*#\s*include\s*<([^>]+)>", re.MULTILINE)
OS_TESTS = re.compile(r"\b(?:_WIN32|_WIN64|__linux__|__APPLE__|__unix__|__ANDROID__)\b")
WRITES = re.compile(r"\bstd::(?:ofstream|fstream|fopen|filesystem::(?:rename|remove"
                    r"|remove_all|copy|copy_file|create_directory|create_directories"
                    r"|resize_file))\b")


def blank(match: re.Match) -> str:
    """The literal as spaces, keeping its newlines so line numbers stay true."""
    return re.sub(r"[^\n]", " ", match.group(0))


def ownership(line: str) -> list[str]:
    found = ["std::shared_ptr"] if SHARED.search(line) else []
    for match in NEW_OR_DELETE.finditer(line):
        before = line[:match.start()].rstrip()
        word = match.group(0)
        # operator new and a deleted function (= delete) own nothing.
        if not before.endswith("operator") and not (word == "delete" and before.endswith("=")):
            found.append(f"a naked {word}")
    return found


def problems(file: Path) -> list[str]:
    name = file.relative_to(SOURCE).as_posix()
    code = LITERALS.sub(blank, file.read_text(encoding="utf-8"))
    found = []
    for number, line in enumerate(code.splitlines(), 1):
        found += [f"{name}:{number}: {what} (RC03)" for what in ownership(line)]
    if name not in PLATFORM:
        for match in INCLUDE.finditer(code):
            header = match.group(1)
            if ("." in header or "/" in header) and not header.startswith(LIBRARIES):
                line = code.count("\n", 0, match.start()) + 1
                found.append(f"{name}:{line}: <{header}> outside the platform files (RA01)")
        for match in OS_TESTS.finditer(code):
            line = code.count("\n", 0, match.start()) + 1
            found.append(f"{name}:{line}: {match.group(0)} outside the platform files (RA01)")
        for match in WRITES.finditer(code):
            line = code.count("\n", 0, match.start()) + 1
            found.append(f"{name}:{line}: {match.group(0)} writes outside the platform "
                         "files; save through Files::save (RA04)")
    return found


def main() -> None:
    files = [file for suffix in ("*.h", "*.cpp") for file in SOURCE.rglob(suffix)
             if VENDORED not in file.parents]
    if found := [problem for file in sorted(files) for problem in problems(file)]:
        sys.exit("\n".join(found))


if __name__ == "__main__":
    main()
