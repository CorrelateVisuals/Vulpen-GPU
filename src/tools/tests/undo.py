#!/usr/bin/env python3
"""undo takes back the last line a person ran, and redo runs it again: the graph's edits
and the files the line made, removed, moved or copied. Gone wrong, a person finds out only
when the file they wanted back is not there, or a node comes back without its folder.

The CLI takes the lines on standard input, as typed, in a project made in a temporary
folder: a node holding a file of known text is removed with it and brought back; a copy
of it, moved into a group, is taken back step by step and run again; a param set is taken
back. Each undo and redo must answer, nothing may be refused, and once the project is
saved its folder must hold each file with its text, and its manifest each node, but the
param taken back.

Usage: python3 src/tools/tests/undo.py VULPEN
"""
import sys
import tempfile
from pathlib import Path

from harness import ROOT, problems, run

CLI = ROOT / "src" / "runtime" / "recipes" / "vulpen" / "cli" / "view.vlp"
SEED = "the text undo must bring back\n"
SCRIPT = """\
view new {project}
new a/ g/
copy {seed} a/seed.txt
remove force a/
undo
copy a/ b/
rename b/ g/b/
undo
undo
redo
param set a speed 2
undo
undo
redo
view save
"""
UNDID, REDID = 5, 2  # the answers the script's lines give


def main() -> None:
    vulpen = sys.argv[1]
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(temporary)
        project = folder / "undone"
        (folder / "seed.txt").write_text(SEED, encoding="utf-8")
        script = SCRIPT.format(project=project, seed=folder / "seed.txt")
        code, output = run(vulpen, [CLI, "--fps", "0"], typed=script, timeout=120)
        failures = []
        if code != 0 or problems(output) or "{!!!}" in output:
            failures.append(f"the run failed or refused a line, exit {code}")
        undid, redid = output.count("undid "), output.count("redid ")
        if (undid, redid) != (UNDID, REDID):
            failures.append(f"{undid} undid and {redid} redid answered, where the lines "
                            f"ask {UNDID} and {REDID}")
        for kept in ("a/seed.txt", "b/seed.txt"):
            file = project / kept
            if not file.is_file() or file.read_text(encoding="utf-8") != SEED:
                failures.append(f"{kept} is not there with its text")
        if (project / "g" / "b").exists() or not (project / "g").is_dir():
            failures.append("the group's folder is not as the last undo left it")
        manifest = (project / "view.vlp").read_text(encoding="utf-8") \
            if (project / "view.vlp").is_file() else ""
        if any(f'[node "{node}"]' not in manifest for node in ("a", "b", "g")) or \
                "speed" in manifest:
            failures.append(f"the manifest is not the graph undo left:\n{manifest}")
    if failures:
        sys.exit("undo: " + "; ".join(failures) + f"\n{output}")


if __name__ == "__main__":
    main()
