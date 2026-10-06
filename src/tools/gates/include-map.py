#!/usr/bin/env python3
"""Fail when the includes in src/ differ from docs/architecture/include-map.md, or form a cycle.

The map is the one page a person reads to steer the engine's structure: a new engine
file or include edge lands only once someone writes it there, and the graph stays
acyclic, so code never includes from code that builds on it.

Node code, any file of the library (src/recipes/) or of a view, under a folder that
holds a view.vlp, is not engine and has no row: a new node's file builds with no edit to
the map. Each of its includes must be a file in its own folder, a contract from the
contracts/ folder at the top of the library or of its view, or what the map's node table
lists. Engine code never includes node code.

Usage: python3 src/tools/gates/include-map.py
"""
import re
import sys
from fnmatch import fnmatchcase
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src"
MAP = ROOT / "docs" / "architecture" / "include-map.md"
VENDORED = SOURCE / "external-libraries"  # not our code, so not our structure
SUFFIXES = {".h", ".hpp", ".cpp", ".glsl", ".vert", ".tesc", ".tese", ".geom", ".frag", ".comp"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)
CODE_SPAN = re.compile(r"`([^`]+)`")
LIBRARY = SOURCE / "recipes"  # every recipe, and the contracts they share
MANIFEST = "view.vlp"  # a folder holding one is a view's
CONTRACTS = "contracts"  # at the top, the only files more than one node includes (RV05)
CONTRACT_SUFFIXES = {".glsl", ".h"}  # a GLSL struct, or a C++ header for C++ nodes
# Each table on the page, by the first cell of its header.
ENGINE_TABLE = "File"
NODE_TABLE = "Node code may include"


def node_root(file: Path) -> Path | None:
    """Where the contracts/ of a node's file is: the library's folder for a file in the
    library, else the nearest folder above the file that holds a view.vlp; None for
    engine code."""
    if LIBRARY in file.parents:
        return LIBRARY
    return next((folder for folder in file.parents
                 if SOURCE in folder.parents and (folder / MANIFEST).is_file()), None)


def resolve(file: Path, quote: str, name: str) -> str | None:
    """The include as the map names it: a path from src/ for our files, <name> for a
    library, None for the standard library."""
    if quote == "<":
        # Standard headers carry no dot or folder and every file may use them; other
        # libraries (Vulkan, OS headers) are part of the structure.
        return f"<{name}>" if "." in name or "/" in name else None
    # The compiler's search order, so "Log.h" and "baseclasses/Log.h" map alike and no
    # spelling hides a cycle. Node code finds its contracts from the top of its view or
    # of the library, so a dropped copy includes the view's copy of a contract (V03).
    for base in (file.parent, node_root(file), SOURCE):
        if base is not None:
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


def tables() -> dict[str, list[list[str]]]:
    """The page's tables, by the first cell of each header, as the cells of each row."""
    found: dict[str, list[list[str]]] = {}
    header, rows = "", None
    for line in MAP.read_text(encoding="utf-8").splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if not line.startswith("|"):
            rows = None
        elif all(set(cell) <= set("-: ") for cell in cells):  # the rule under a header
            rows = found.setdefault(header, [])
        elif rows is None:
            header = cells[0]
        else:
            rows.append(cells)
    return found


def mapped(page: dict[str, list[list[str]]], problems: list[str]) -> dict[str, set[str]]:
    edges = {}
    for cells in page.get(ENGINE_TABLE, []):
        if len(cells) == 2 and (spans := CODE_SPAN.findall(cells[0])):
            if spans[0] in edges:
                problems.append(f"{spans[0]}: listed twice in the map")
            edges[spans[0]] = set(CODE_SPAN.findall(cells[1]))
    return edges


def reachable(page: dict[str, list[list[str]]]) -> set[str]:
    """What node code may include besides its own folder and the contracts, as patterns:
    `<glm/*>` stands for every glm header."""
    return {span for cells in page.get(NODE_TABLE, []) for span in CODE_SPAN.findall(cells[0])}


def row(file: str, names: set[str]) -> str:
    return f"| `{file}` | " + (" ".join(f"`{name}`" for name in sorted(names)) or "—") + " |"


def differences(code: dict[str, set[str]], page: dict[str, set[str]]) -> list[str]:
    engine = {file: names for file, names in code.items() if not node_root(SOURCE / file)}
    problems = [f"{file}: new file; once approved, add the row\n      {row(file, engine[file])}"
                for file in sorted(engine.keys() - page.keys())]
    problems += [f"{file}: node code has no row in the map" if file in code
                 else f"{file}: in the map, but not in src/"
                 for file in sorted(page.keys() - engine.keys())]
    for file in sorted(engine.keys() & page.keys()):
        problems += [f"{file} -> {name}: new include; once approved, add it to the map"
                     for name in sorted(engine[file] - page[file])]
        problems += [f"{file} -> {name}: in the map, but not included"
                     for name in sorted(page[file] - engine[file])]
    return problems


def node_rule(code: dict[str, set[str]], reach: set[str]) -> list[str]:
    """Node code reaches only its own folder, the contracts and what the map lists for it
    (RV00, RV05, RV08); engine code never reaches node code (RA00)."""
    problems = []
    for file, names in sorted(code.items()):
        root = node_root(SOURCE / file)
        for name in sorted(names):
            target = SOURCE / name
            if root is None:
                if node_root(target):
                    problems.append(f"{file} -> {name}: engine code never includes node code "
                                    "(RA00)")
            elif not (target.parent == (SOURCE / file).parent
                      or (target.parent == root / CONTRACTS
                          and target.suffix in CONTRACT_SUFFIXES)
                      or any(fnmatchcase(name, pattern) for pattern in reach)):
                problems.append(f"{file} -> {name}: node code includes only its own folder, a "
                                "contract, and what the map lists for node code (RV00, RV05)")
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
    page = tables()
    problems += differences(code, mapped(page, problems))
    problems += node_rule(code, reachable(page))
    if loop := cycle(code):
        problems.append("include cycle: " + " -> ".join(loop))
    if problems:
        sys.exit(f"{MAP.relative_to(ROOT).as_posix()}:\n  " + "\n  ".join(problems))


if __name__ == "__main__":
    main()
