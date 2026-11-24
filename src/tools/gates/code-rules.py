#!/usr/bin/env python3
"""Fail when the code in src/ or projects/ breaks a rule a text search can check:

- RC03: ownership is std::unique_ptr or by value, so no std::shared_ptr, and no naked new
  or delete;
- RA01: OS APIs and OS conditionals live only in the platform files, so no other file
  includes a header beyond the standard library, Vulkan, glm, VMA, stb_truetype,
  stb_image and stb_image_write, or tests for an OS;
- RA04: only the platform files write or move a file, so every save goes through
  Files::save, which writes a temp file and renames it over the old one. A run killed
  mid-save then leaves the old file or the new one; a kill rarely lands inside a small
  file's write, so a test that kills runs would pass an unsafe save;
- RV05: a C++ contract, a header in a contracts/ folder, opens namespace VP_VIEW, which
  the build names per view, so two views' copies of it stay two types in one release
  binary; without it the linker keeps one definition for both (live code, rule 1);
- RV09: shared code, a header or its .cpp in a shared/ folder, opens the unnamed
  namespace, so each part that includes it holds its own copy, and two views' copies
  never become one definition in a release binary;
- RC05: a comment cites principles and requirements, never a plan's row or step, which
  points at nothing once the plan is done. Each ID in parentheses in a comment of the
  C++, the shaders or the node template is one principles.md or requirements.md defines.
- RC08: no comment there runs past 15 lines, so a class's comment stays what the class is
  for, and what the code does is said where it does it, rather than growing a clause
  with each feature (C07).
- CPP12: a C++ file past 800 lines says why at its top, naming CPP12 in its first
  comment, so a file that grows past what one reads whole is split or kept so on
  purpose, when it crosses the line rather than when someone counts.

Comments and string literals are left out of every other rule, so prose and log text
never trip one.

With --view FOLDER it checks that view's folder alone, as the view's own build runs it,
so a project's mistake stops that project's build; without, src/.

Usage: python3 src/tools/gates/code-rules.py [BUILD_DIR] [--view FOLDER]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src"
VENDORED = SOURCE / "external-libraries"  # not our code, so not our rules
PLATFORM = {"baseclasses/Platform.h", "baseclasses/Platform.cpp"}
# What any file may include; the include map says which files do.
LIBRARIES = ("vulkan/", "glm/", "vk_mem_alloc.h", "stb_truetype.h", "stb_image.h",
             "stb_image_write.h", "tiny_obj_loader.h", "sokol_audio.h")

# Raw strings first: their text may hold quotes.
LITERALS = re.compile(r'R"([^(\s]*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\''
                      r"|//[^\n]*|/\*.*?\*/", re.DOTALL)
SHARED = re.compile(r"\b(?:shared_ptr|make_shared)\b")
NEW_OR_DELETE = re.compile(r"\b(?:new|delete)\b")
INCLUDE = re.compile(r"^\s*#\s*include\s*<([^>]+)>", re.MULTILINE)
OS_TESTS = re.compile(r"\b(?:_WIN32|_WIN64|__linux__|__APPLE__|__unix__|__ANDROID__)\b")
CONTRACTS = "contracts"
VIEW_NAMESPACE = re.compile(r"\bnamespace\s+VP_VIEW\b")
SHARED_CODE = "shared"
UNNAMED_NAMESPACE = re.compile(r"\bnamespace\s*\{")
DOCS = ROOT / "docs" / "architecture"
# RC05 and RC08 read the comments of these; the other rules hold for C++ alone.
COMMENTED = ("*.h", "*.cpp", "*.in", "*.glsl", "*.comp", "*.vert", "*.frag")
LONGEST_COMMENT = 15  # lines a comment may run (RC08)
LONGEST_FILE = 800  # lines a C++ file runs without saying why (CPP12)
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


def long_comments(name: str, text: str) -> list[str]:
    """Each run of more than LONGEST_COMMENT lines that hold only a comment. String
    literals are blanked first, so a // inside one starts no comment."""
    def keep_comments(match: re.Match) -> str:
        return match.group(0) if match.group(0).startswith(("//", "/*")) else blank(match)

    lines = LITERALS.sub(keep_comments, text).splitlines()
    found, run = [], 0
    for number, line in enumerate(lines + [""], 1):
        if line.lstrip().startswith("//"):
            run += 1
            continue
        if run > LONGEST_COMMENT:
            found.append(f"{name}:{number - run}: a comment of {run} lines, past "
                         f"{LONGEST_COMMENT}: keep what the code is for, and say the "
                         "rest where the code does it (RC08)")
        run = 0
    return found


def long_file(name: str, text: str) -> list[str]:
    """A file past LONGEST_FILE lines whose first comment, the first run of lines that
    hold only one, does not name CPP12."""
    lines = text.splitlines()
    if len(lines) <= LONGEST_FILE:
        return []
    first = []
    for line in lines:
        if line.lstrip().startswith("//"):
            first.append(line)
        elif first:
            break
    if any("CPP12" in line for line in first):
        return []
    return [f"{name}: {len(lines)} lines, past {LONGEST_FILE}: split it, or say why at its "
            "top, naming CPP12 in its first comment (CPP12)"]


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


def name_of(file: Path) -> str:
    """From src/, as the platform files are named, a project's from the top, and one
    outside the tree, as a project run.sh mounts, whole."""
    for top in (SOURCE, ROOT):
        if top in file.parents:
            return file.relative_to(top).as_posix()
    return file.as_posix()


def problems(file: Path) -> list[str]:
    name = name_of(file)
    code = LITERALS.sub(blank, file.read_text(encoding="utf-8"))
    found = []
    for number, line in enumerate(code.splitlines(), 1):
        found += [f"{name}:{number}: {what} (RC03)" for what in ownership(line)]
    if file.parent.name == CONTRACTS and file.suffix == ".h" and not VIEW_NAMESPACE.search(code):
        found.append(f"{name}: a C++ contract opens namespace VP_VIEW, so two views' copies "
                     "of it stay two types (RV05)")
    if file.parent.name == SHARED_CODE and not UNNAMED_NAMESPACE.search(code):
        found.append(f"{name}: shared code opens the unnamed namespace, so each part holds its "
                     "own copy (RV09)")
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
    folder = (Path(sys.argv[sys.argv.index("--view") + 1]).resolve() if "--view" in sys.argv
              else SOURCE)
    files = sorted(file for suffix in COMMENTED for file in folder.rglob(suffix)
                   if VENDORED not in file.parents)
    found = []
    for file in files:
        text = file.read_text(encoding="utf-8")
        found += citations(name_of(file), text, ids)
        found += long_comments(name_of(file), text)
        if file.suffix in (".h", ".cpp"):
            found += problems(file)
            found += long_file(name_of(file), text)
    if found:
        sys.exit("\n".join(found))


if __name__ == "__main__":
    main()
