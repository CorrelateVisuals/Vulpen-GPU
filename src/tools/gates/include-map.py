#!/usr/bin/env python3
"""Fail when the includes in src/ differ from docs/architecture/include-map.md, or form a cycle.

The map is the one page a person reads to steer the engine's structure: a new engine
file or include edge lands only once someone writes it there, and the graph stays
acyclic, so code never includes from code that builds on it.

Recipe code, any file under a folder named recipes/, is not engine and has no row: a
new recipe file builds with no edit to the map. Each of its includes must be a file in
its own folder, a contract from the contracts/ folder of the nearest recipes/ folder
above it, or what the map's recipe table lists. Engine code never includes recipe code.

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
RECIPES = "recipes"  # the folder name that makes a file recipe code
CONTRACTS = "contracts"  # beside the recipes, the only files more than one includes (RV05)
# Each table on the page, by the first cell of its header.
ENGINE_TABLE = "File"
RECIPE_TABLE = "Recipe code may include"


def recipes_folder(file: Path) -> Path | None:
    """The nearest folder named recipes/ above a file in src/; None for engine code."""
    return next((folder for folder in file.parents
                 if folder.name == RECIPES and SOURCE in folder.parents), None)


def resolve(file: Path, quote: str, name: str) -> str | None:
    """The include as the map names it: a path from src/ for our files, <name> for a
    library, None for the standard library."""
    if quote == "<":
        # Standard headers carry no dot or folder and every file may use them; other
        # libraries (Vulkan, OS headers) are part of the structure.
        return f"<{name}>" if "." in name or "/" in name else None
    # The compiler's search order, so "Log.h" and "baseclasses/Log.h" map alike and no
    # spelling hides a cycle. Recipe code finds its contracts from its recipes/ folder,
    # so a copied recipe includes the view's copy of a contract (V03).
    for base in (file.parent, recipes_folder(file), SOURCE):
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
    """What recipe code may include besides its own folder and the contracts, as
    patterns: `<glm/*>` stands for every glm header."""
    return {span for cells in page.get(RECIPE_TABLE, []) for span in CODE_SPAN.findall(cells[0])}


def row(file: str, names: set[str]) -> str:
    return f"| `{file}` | " + (" ".join(f"`{name}`" for name in sorted(names)) or "—") + " |"


def differences(code: dict[str, set[str]], page: dict[str, set[str]]) -> list[str]:
    engine = {file: names for file, names in code.items() if not recipes_folder(SOURCE / file)}
    problems = [f"{file}: new file; once approved, add the row\n      {row(file, engine[file])}"
                for file in sorted(engine.keys() - page.keys())]
    problems += [f"{file}: recipe code has no row in the map" if file in code
                 else f"{file}: in the map, but not in src/"
                 for file in sorted(page.keys() - engine.keys())]
    for file in sorted(engine.keys() & page.keys()):
        problems += [f"{file} -> {name}: new include; once approved, add it to the map"
                     for name in sorted(engine[file] - page[file])]
        problems += [f"{file} -> {name}: in the map, but not included"
                     for name in sorted(page[file] - engine[file])]
    return problems


def recipe_rule(code: dict[str, set[str]], reach: set[str]) -> list[str]:
    """Recipe code reaches only its own folder, the contracts and what the map lists for
    it (RV00, RV05); engine code never reaches recipe code (RA00)."""
    problems = []
    for file, names in sorted(code.items()):
        folder = recipes_folder(SOURCE / file)
        for name in sorted(names):
            target = SOURCE / name
            if folder is None:
                if recipes_folder(target):
                    problems.append(f"{file} -> {name}: engine code never includes recipe code "
                                    "(RA00)")
            elif not (target.parent == (SOURCE / file).parent
                      or (target.parent == folder / CONTRACTS and target.suffix == ".glsl")
                      or any(fnmatchcase(name, pattern) for pattern in reach)):
                problems.append(f"{file} -> {name}: recipe code includes only its own folder, a "
                                "contract, and what the map lists for recipe code (RV00, RV05)")
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
    problems += recipe_rule(code, reachable(page))
    if loop := cycle(code):
        problems.append("include cycle: " + " -> ".join(loop))
    if problems:
        sys.exit(f"{MAP.relative_to(ROOT).as_posix()}:\n  " + "\n  ".join(problems))


if __name__ == "__main__":
    main()
