#!/usr/bin/env python3
"""Run every gate in src/tools/gates/; any failing gate fails the build.

Usage: python3 src/tools/gates.py
"""
import subprocess
import sys
from pathlib import Path

GATES = Path(__file__).resolve().parent / "gates"


def main() -> None:
    gates = sorted(GATES.glob("*.py"))
    if not gates:  # a moved or emptied folder must not pass as "all green"
        sys.exit(f"no gates in {GATES}")
    # Every gate runs, so one build reports every broken rule, not only the first.
    failed = [gate.stem for gate in gates if subprocess.call([sys.executable, gate]) != 0]
    if failed:
        sys.exit(f"gates failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
