#!/usr/bin/env python3
"""Fail when C++ of ours compiles without -ffp-contract=off, or with a flag that breaks
IEEE 754 (RC06): the same input must give the same bits in every build and on every
machine (C01, C10).

Reads the compile commands CMake exports into the build folder. A build without them
(Visual Studio) is not checked: MSVC neither fuses nor relaxes floating point by default.

Usage: python3 src/tools/gates/floating-point.py BUILD_DIR
"""
import json
import shlex
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src"
VENDORED = SOURCE / "external-libraries"
FORBIDDEN = ("-ffast-math", "-Ofast", "-funsafe-math-optimizations", "-ffinite-math-only",
             "-fno-signed-zeros", "-freciprocal-math", "-fassociative-math")
CONTRACT = "-ffp-contract="


def problems(entry: dict) -> list[str]:
    file = Path(entry["file"]).resolve()
    if SOURCE not in file.parents or VENDORED in file.parents:
        return []
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    name = file.relative_to(SOURCE).as_posix()
    found = [f"{name}: compiles with {flag}" for flag in FORBIDDEN if flag in arguments]
    contracts = [argument for argument in arguments if argument.startswith(CONTRACT)]
    if not contracts or contracts[-1] != CONTRACT + "off":  # the last one wins
        found.append(f"{name}: compiles without {CONTRACT}off")
    return found


def main() -> None:
    if len(sys.argv) < 2:
        return  # run by hand, with no build to read
    commands = Path(sys.argv[1]) / "compile_commands.json"
    if not commands.is_file():
        return
    entries = json.loads(commands.read_text(encoding="utf-8"))
    if found := [problem for entry in entries for problem in problems(entry)]:
        sys.exit(f"{commands}:\n  " + "\n  ".join(found) + "\n  (RC06, C01)")


if __name__ == "__main__":
    main()
