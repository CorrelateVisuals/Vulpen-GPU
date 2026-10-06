#!/usr/bin/env python3
"""Fail when the code in src/ breaks a rule a text search can check:

- RC03: ownership is std::unique_ptr or by value, so no std::shared_ptr, and no naked new
  or delete;
- RA01: OS APIs and OS conditionals live only in the platform files, so no other file
  includes a header beyond the standard library, Vulkan, glm, VMA and stb_truetype, or
  tests for an OS;
- RA04: only the platform files write or move a file, so every save goes through
  Files::save, which writes a temp file and renames it over the old one. A run killed
  mid-save then leaves the old file or the new one; a kill rarely lands inside a small
  file's write, so a test that kills runs would pass an unsafe save;
- RV05: a C++ contract, a header in a contracts/ folder, opens namespace VP_VIEW, which
  the build names per view, so two views' copies of it stay two types in one release
  binary; without it the linker keeps one definition for both (live code, rule 1);
- RC05: a comment cites principles and requirements, never a plan's row or step, which
  points at nothing once the plan is done. Each ID in parentheses in a comment of the
  C++, the shaders or the node template is one principles.md or requirements.md defines.

Comments and string literals are left out of every other rule, so prose and log text
never trip one.

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
CONTRACTS = "contracts"
VIEW_NAMESPACE = re.compile(r"\bnamespace\s+VP_VIEW\b")
DOCS = ROOT / "docs" / "architecture"
# RC05 reads the comments of these; the other rules hold for C++ alone.
COMMENTED = ("*.h", "*.cpp", "*.in", "*.glsl", "*.comp", "*.vert", "*.frag")
DEFINED = re.compile(r"\*\*([A-Z]+\d+)\b")
CITED = re.compile(r"\(([^()\n]*)\)")
ID = re.compile(r"\b[A-Z]+\d+\b")
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


def citations(name: str, text: str, ids: set[str]) -> list[str]:
    found = []
    for comment in LITERALS.finditer(text):
        if not comment.group(0).startswith(("//", "/*")):
            continue
        for cited in CITED.finditer(comment.group(0)):
            line = text.count("\n", 0, comment.start() + cited.start()) + 1
            found += [f"{name}:{line}: {tag} is no principle or requirement, which are all "
                      "a comment cites (RC05)"
                      for tag in ID.findall(cited.group(1)) if tag not in ids]
    return found


def problems(file: Path) -> list[str]:
    name = file.relative_to(SOURCE).as_posix()
    code = LITERALS.sub(blank, file.read_text(encoding="utf-8"))
    found = []
    for number, line in enumerate(code.splitlines(), 1):
        found += [f"{name}:{number}: {what} (RC03)" for what in ownership(line)]
    if file.parent.name == CONTRACTS and file.suffix == ".h" and not VIEW_NAMESPACE.search(code):
        found.append(f"{name}: a C++ contract opens namespace VP_VIEW, so two views' copies "
                     "of it stay two types (RV05)")
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
    ids = set(DEFINED.findall("".join((DOCS / rules).read_text(encoding="utf-8")
                                      for rules in ("principles.md", "requirements.md"))))
    files = sorted(file for suffix in COMMENTED for file in SOURCE.rglob(suffix)
                   if VENDORED not in file.parents)
    found = []
    for file in files:
        found += citations(file.relative_to(SOURCE).as_posix(),
                           file.read_text(encoding="utf-8"), ids)
        if file.suffix in (".h", ".cpp"):
            found += problems(file)
    if found:
        sys.exit("\n".join(found))


if __name__ == "__main__":
    main()
