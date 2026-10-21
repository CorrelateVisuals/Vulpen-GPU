#!/usr/bin/env python3
"""Take the README's pictures of the ide into docs/images/, as commands replayed headless.

Every build runs it after the link, as it runs fetch-statistics.py, so the pictures show
the code as built. It runs the cli hosting the ide with no window (V07), from a copy of
the recipes and example projects, so no person's .vulpen/ arrangement shows and nothing
the run saves reaches the tree. Each scene is lines a person could type (V06), sent on
standard input; then the snapshot part saves the ide's window as a PNG. A picture is
written only when its pixels changed, and nothing runs while what the pictures follow
from is as it was at the last build that took them.

Usage: python3 src/tools/update-readme-images.py --binary PATH
"""
import argparse
import hashlib
import itertools
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGES = ROOT / "docs" / "images"
COPIED = ("src/runtime/recipes", "projects")  # what the run reads; git ignores the rest
STAMP = "readme-images.stamp"  # beside the binary, a build's own
SIZE = "1920x1080"  # the snapshot part reads this many pixels at most
TIMEOUT_S = 120     # for a line, a picture, or the run's end
POLL_S = 0.01
FRAMES_A_LINE = 3   # at least, from one line of a scene to the next
ERROR = "{!!!}"
# Before the first frame, so the ide draws into the snapshot's picture from its first and
# never opens a window. Paths count from the script's folder.
SETUP = """view add ide src/runtime/recipes/ide/view.vlp
node add cli.snapshot recipe=snapshot
connect snapshot ide: cli.snapshot.picture
"""


def typed(line: str) -> list[str]:
    """A line typed into the ide's terminal, so the terminal shows it as a person's."""
    return [f"input text {line}", "input key down enter", "input key up enter"]


CLEAR = ["input key down control", "input key down l", "input key up l",
         "input key up control"]
# Each picture, by its file's name, and the lines that make it, from where the scene
# before left the ide. Whatever turns is held still, and each ends with no focus, so no
# caret blinks: the picture is the same at whatever frame it is taken. No line shows an
# absolute path, which names the copy, a new folder each run.
SCENES = {
    # The whole ide: a node's C++, the project's graph, what it draws, the lines typed.
    "ide": [*CLEAR, *typed("view load triangle-files"),
            *typed("param set clock frames_per_turn 0"), *typed("open Corners.cpp"),
            *typed("info corners"), "ide: zoom in", "ide: zoom in", "ide: focus"],
    # The manifest beside the graph it makes.
    "manifest": ["ide: open projects/triangle-files/view.vlp",
                 "ide: panel hide ide.perform-panel", "ide: panel hide ide.terminal-panel",
                 "ide: focus"],
    # One view's C++ and GLSL, in two editors side by side.
    "languages": ["ide: close all projects/triangle-files/view.vlp",
                  "view load triangle-twin", "param set triangle frames_per_turn 0",
                  "open Triangle.cpp", "open Triangle.vert",
                  "ide: panel move ide.editor-panel ide.graph-panel left",
                  "ide: panel hide ide.graph-panel", "ide: panel hide ide.perform-panel",
                  "ide: focus"],
    # The command line, as the cli runs it in a terminal.
    "cli": ["ide: panel maximize ide.terminal-panel", "ide: focus ide.terminal.command-line",
            *CLEAR, *typed("help"), *typed("ls"), *typed("info triangle"), "ide: focus"],
    # The ide is a view too: its own graph, its recipes and the wires between them.
    "graph": ["view switch ide", "ide: panel maximize ide.graph-panel",
              "ide: param set ide.graph.graph inside ide", "input pointer 800 520",
              *["input wheel 0 1"] * 6, "ide: focus"],
}


def copy_tree(into: Path) -> None:
    listed = subprocess.run(["git", "ls-files", "-z", "--cached", "--others",
                             "--exclude-standard", *COPIED], cwd=ROOT, check=True,
                            capture_output=True).stdout.decode().split("\0")
    for path in filter(None, listed):
        if (ROOT / path).is_file():
            (into / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, into / path)


def digest(base: Path, files) -> str:
    hashed = hashlib.sha256()
    for file in sorted(file for file in files if file.is_file()):
        hashed.update(file.relative_to(base).as_posix().encode())
        hashed.update(file.read_bytes())
    return hashed.hexdigest()


def fingerprint(binary: Path, tree: Path) -> str:
    """What the pictures follow from, read before the run writes into the tree: this
    script, the binary and the nodes the build made beside it, and the tree the run reads;
    then the pictures as they are."""
    return "\n".join([digest(ROOT, [Path(__file__)]),
                      digest(binary.parent, [binary, *(binary.parent / "views").rglob("*")]),
                      digest(tree, tree.rglob("*")), ""])


def take(binary: Path, into: Path) -> dict[str, Path]:
    said = into / "said.txt"
    # No display, so a scene that would open a window fails loud instead of covering the
    # person's screen.
    env = {key: value for key, value in os.environ.items()
           if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
    files = (into / f"{count}" for count in itertools.count())
    with said.open("w") as out:
        run = subprocess.Popen([binary, "src/runtime/recipes/cli/view.vlp", "--size", SIZE,
                                "--fps", "0", "--source", "setup.txt"], cwd=into, env=env,
                               text=True, stdin=subprocess.PIPE, stdout=out,
                               stderr=subprocess.STDOUT)

        def wait_for(file: Path, after: str) -> None:
            deadline = time.monotonic() + TIMEOUT_S
            while not file.exists():
                if run.poll() is not None or time.monotonic() > deadline:
                    run.kill()
                    sys.exit(f"vulpen saved no {file.name} after {after}:\n"
                             f"{said.read_text(errors='replace')}")
                time.sleep(POLL_S)

        # Each line saving a file is written once the one before saved its own, so it runs
        # a frame later at least, and the next line of the scene once what this one set off
        # has passed from node to node, as a person types.
        def send(line: str) -> None:
            run.stdin.write(f"{line}\n")
            for _ in range(FRAMES_A_LINE):
                file = next(files)
                run.stdin.write(f"log save {file}\n")
                run.stdin.flush()
                wait_for(file, line)

        pictures = {}
        for name, lines in SCENES.items():
            for line in lines:
                send(line)
            pictures[name] = into / f"{name}.png"
            run.stdin.write(f": param set cli.snapshot file {pictures[name]}\n")
            run.stdin.flush()
            wait_for(pictures[name], name)
        run.stdin.write("quit\n")
        run.stdin.close()
        code = run.wait(TIMEOUT_S)
    errors = [line for line in said.read_text(errors="replace").splitlines() if ERROR in line]
    if code != 0 or errors:
        sys.exit(f"vulpen exited {code}:\n" + "\n".join(errors))
    return pictures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--binary", type=Path, required=True, help="the vulpen to run")
    binary = parser.parse_args().binary.resolve()
    stamp = binary.parent / STAMP
    IMAGES.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vulpen-images-") as into:
        tree = Path(into)
        copy_tree(tree)
        inputs = fingerprint(binary, tree)
        if stamp.exists() and stamp.read_text() == inputs + digest(ROOT, IMAGES.glob("*.png")):
            return
        (tree / "setup.txt").write_text(SETUP, encoding="utf-8")
        for name, picture in take(binary, tree).items():
            image = IMAGES / f"{name}.png"
            if image.exists() and image.read_bytes() == picture.read_bytes():
                continue
            temporary = image.with_name(f"{image.name}.tmp")
            shutil.copyfile(picture, temporary)
            temporary.replace(image)  # a killed run never leaves half a file
            print(f"wrote {image.relative_to(ROOT).as_posix()}")
    stamp.write_text(inputs + digest(ROOT, IMAGES.glob("*.png")))


if __name__ == "__main__":
    main()
