#!/usr/bin/env python3
"""Fail on a name that hides another as MSVC refuses it and GCC lets pass (RP01).

GCC's -Wshadow compares a local only with the names of its function's own namespace, so
a name of a file's unnamed namespace, hidden in a member function defined outside it,
builds on Linux and fails on Windows (C4459). This checks a copy of src/ and projects/
whose unnamed namespaces are taken out, every file the build compiles, with -Wshadow and
nothing written, so GCC sees what MSVC sees. A copy that no longer compiles fails too: a
name of an unnamed namespace declared outside it as well is written twice (C02).

check.py quick runs it after the build; Windows has no need, as MSVC refuses these there.

Usage: python3 src/tools/hidden-names.py BUILD_DIR
"""
import argparse
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COPIED = ("src", "projects")
VENDORED = Path("src", "external-libraries")
# The lines that open and close an unnamed namespace, as .clang-format writes them; each
# becomes a comment, so a finding names the line it has in the tree.
UNNAMED = re.compile(r"^(namespace \{|\} // namespace)$", re.MULTILINE)
HIDES = "[-Wshadow"


def copy(into: Path) -> None:
    for top in COPIED:
        shutil.copytree(ROOT / top, into / top, symlinks=True,
                        ignore=shutil.ignore_patterns(".vulpen", VENDORED.name))
    (into / VENDORED).symlink_to(ROOT / VENDORED)
    for top in COPIED:
        for path in (into / top).rglob("*"):
            if path.suffix in (".cpp", ".h") and not path.is_symlink():
                text = path.read_text()
                path.write_text(UNNAMED.sub("//", text))


def command(entry: dict, into: Path) -> list[str]:
    words = entry.get("arguments") or shlex.split(entry["command"])
    kept, skip = [], False
    for word in words:
        if skip or word in ("-o", "-MF", "-MT", "-MQ"):
            skip = not skip
            continue
        if word not in ("-MD", "-MMD"):
            for top in COPIED:
                word = word.replace(str(ROOT / top), str(into / top))
            kept.append(word)
    return [*kept, "-fsyntax-only", "-Wshadow", "-Wno-error"]


def check(entry: dict, into: Path) -> list[str]:
    said = subprocess.run(command(entry, into), cwd=entry["directory"],
                          capture_output=True, text=True)
    lines = said.stderr.replace(f"{into}/", "").splitlines()
    found = []
    for at, line in enumerate(lines):
        if HIDES in line:
            hidden = next((after for after in lines[at + 1:] if "note:" in after), "")
            found.append(f"{line}\n    {hidden.strip()}")
    if said.returncode != 0:
        first = next((line for line in lines if "error" in line), "")
        found.append(f"{Path(entry['file']).relative_to(ROOT)}: with its unnamed "
                     f"namespaces taken out it compiles no more, so a name there is "
                     f"declared outside them too: {first}")
    return found


def main(build: Path) -> int:
    entries = [entry for entry in json.loads((build / "compile_commands.json").read_text())
               if not Path(entry["file"]).is_relative_to(ROOT / VENDORED)]
    with tempfile.TemporaryDirectory(prefix="hidden-names-") as folder:
        into = Path(folder)
        copy(into)
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
            results = list(pool.map(lambda entry: check(entry, into), entries))
    findings = sorted({finding for found in results for finding in found})
    for finding in findings:
        print(finding)
    print(f"{len(entries)} files, {len(findings)} names hidden as MSVC refuses them")
    return 1 if findings else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("build", type=Path, help="a build folder, as CMake exports its "
                        "compile commands there")
    sys.exit(main(parser.parse_args().build))
