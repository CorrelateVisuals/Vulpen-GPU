#!/usr/bin/env python3
"""Fail when the includes in src/ differ from docs/architecture/include-map.md, or form a cycle.

The map is the one page a person reads to steer the structure: a new file or include
edge lands only once someone writes it there, and the graph stays acyclic, so code
never includes from code that builds on it.

Usage: python3 src/tools/gates/include-map.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src"
MAP = ROOT / "docs" / "architecture" / "include-map.md"
VENDORED = SOURCE / "external-libraries"  # not our code, so not our structure
SUFFIXES = {".h", ".hpp", ".cpp", ".glsl", ".vert", ".tesc", ".tese", ".geom", ".frag", ".comp"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)
CODE_SPAN = re.compile(r"`([^`]+)`")


def resolve(file: Path, quote: str, name: str) -> str | None:
    """The include as the map names it: a path from src/ for our files, <name> for a
    library, None for the standard library."""
    if quote == "<":
        # Standard headers carry no dot or folder and every file may use them; other
        # libraries (Vulkan, OS headers) are part of the structure.
        return f"<{name}>" if "." in name or "/" in name else None
    # The compiler's search order, so "Log.h" and "baseclasses/Log.h" map alike and
    # no spelling hides a cycle.
    for base in (file.parent, SOURCE):
        candidate = (base / name).resolve()
        if candidate.is_file() and SOURCE in candidate.parents:
            return candidate.relative_to(SOURCE).as_posix()
    return name  # generated into the build tree, such as commit.h


def included() -> dict[str, set[str]]:
    edges = {}
    for file in sorted(SOURCE.rglob("*")):
        if file.suffix in SUFFIXES and VENDORED not in file.parents:
            text = file.read_text(encoding="utf-8")
            names = (resolve(file, quote, name) for quote, name in INCLUDE.findall(text))
            edges[file.relative_to(SOURCE).as_posix()] = {name for name in names if name}
    return edges


def mapped(problems: list[str]) -> dict[str, set[str]]:
    edges = {}
    for line in MAP.read_text(encoding="utf-8").splitlines():
        cells = line.split("|")
        if len(cells) == 4 and (spans := CODE_SPAN.findall(cells[1])):
            if spans[0] in edges:
                problems.append(f"{spans[0]}: listed twice in the map")
            edges[spans[0]] = set(CODE_SPAN.findall(cells[2]))
    return edges


def row(file: str, names: set[str]) -> str:
    return f"| `{file}` | " + (" ".join(f"`{name}`" for name in sorted(names)) or "—") + " |"


def differences(code: dict[str, set[str]], page: dict[str, set[str]]) -> list[str]:
    problems = [f"{file}: new file; once approved, add the row\n      {row(file, code[file])}"
                for file in sorted(code.keys() - page.keys())]
    problems += [f"{file}: in the map, but not in src/" for file in sorted(page.keys() - code.keys())]
    for file in sorted(code.keys() & page.keys()):
        problems += [f"{file} -> {name}: new include; once approved, add it to the map"
                     for name in sorted(code[file] - page[file])]
        problems += [f"{file} -> {name}: in the map, but not included"
                     for name in sorted(page[file] - code[file])]
    return problems


def cycle(edges: dict[str, set[str]]) -> list[str]:
    """One include cycle as the path around it, or [] when there is none."""
    done: set[str] = set()

    def walk(path: list[str]) -> list[str]:
        for name in sorted(edges[path[-1]] & edges.keys()):
            if name in path:
                return path[path.index(name):] + [name]
            if name not in done and (found := walk(path + [name])):
                return found
        done.add(path[-1])
        return []

    for file in sorted(edges):
        if file not in done and (found := walk([file])):
            return found
    return []


def main() -> None:
    problems: list[str] = []
    code = included()
    problems += differences(code, mapped(problems))
    if loop := cycle(code):
        problems.append("include cycle: " + " -> ".join(loop))
    if problems:
        sys.exit(f"{MAP.relative_to(ROOT).as_posix()}:\n  " + "\n  ".join(problems))


if __name__ == "__main__":
    main()
