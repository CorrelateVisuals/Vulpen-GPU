#!/usr/bin/env python3
"""Take the README's pictures of the ide into docs/images/, as commands replayed headless.

Every build runs it after the link, as it runs fetch-statistics.py; a release build passes
its binary, so the pictures show the code as it ships, and a debug build passes none and
takes none. It runs the cli hosting the ide with no window (V07), from a copy of the
repository, so no person's .vulpen/ arrangement shows and nothing the run saves reaches
the tree. Each scene is lines a person could type (V06), sent on standard input; then the
snapshot part saves the ide's window as a PNG. A picture is written only when its pixels
changed.

Usage: python3 src/tools/update-readme-images.py [--binary PATH]
"""
import argparse
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
NAME = "Vulpen"  # the copy's folder, as the repository's .vulpen/graph.ini names its top
SIZE = "1920x1080"  # the snapshot part reads this many pixels at most
TIMEOUT_S = 120     # for a line, a picture, or the run's end
POLL_S = 0.01
FRAMES_A_LINE = 3   # at least, from one line of a scene to the next
# From a scene's last line to its picture: the relations part reads 64 KB a frame, so the
# repository's top takes about 160 to show its curves.
SETTLE_FRAMES = 300
ERROR = "{!!!}"
# Before the first frame, from the folder holding the copy, so the ide draws into the
# snapshot's picture from its first and never opens a window.
SETUP = f"""view add ide {NAME}/src/runtime/recipes/ide/view.vlp
node add cli.snapshot recipe=snapshot param=wait={SETTLE_FRAMES}
connect snapshot ide: cli.snapshot.picture
"""


def typed(line: str) -> list[str]:
    """A line typed into the ide's terminal, so the terminal shows it as a person's."""
    return [f"input text {line}", "input key down enter", "input key up enter"]


def graph(folder: str) -> list[str]:
    """A folder of the copy in the graph, as open shows it."""
    return [f"open {folder}", "ide: focus"]


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
    # The project presented, in the whole window.
    "perform": ["ide: mode perform"],
    # The manifest beside the graph it makes.
    "manifest": ["ide: mode edit", "ide: open projects/triangle-files/view.vlp",
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
    # The graph of each part of the repository, given the window, which a second
    # maximize would give back.
    "graph-repository": ["ide: panel maximize ide.graph-panel", *graph("./")],
    "graph-src": graph("src/"),
    "graph-baseclasses": graph("src/baseclasses/"),
    "graph-runtime": graph("src/runtime/"),
    "graph-library": graph("src/runtime/recipes/library/"),
    "graph-parts": graph("src/runtime/recipes/library/parts/"),
    "graph-components": graph("src/runtime/recipes/library/components/"),
    "graph-tools": graph("src/tools/"),
    "graph-projects": graph("projects/"),
    # The apps are views too: the cli's nodes, and the ide's inside its own graph.
    "graph-cli": ["ide: param unset ide.graph.relations folder", "view switch",
                  "ide: param set ide.graph.graph inside cli", "ide: focus"],
    "graph-ide": ["view switch ide", "ide: param set ide.graph.graph inside ide",
                  "input pointer 800 520", *["input wheel 0 1"] * 6, "ide: focus"],
}


def copy_tree(into: Path) -> None:
    """The files git lists, as fetch-statistics.py counts them, and an empty .git, where
    the graph finds the repository's top."""
    listed = subprocess.run(["git", "ls-files", "-z", "--cached", "--others",
                             "--exclude-standard"], cwd=ROOT, check=True,
                            capture_output=True).stdout.decode().split("\0")
    for path in filter(None, listed):
        if (ROOT / path).is_file():
            (into / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, into / path)
    (into / ".git").mkdir()


# The copy is the run's working folder, and what the run writes for this script stays in
# the folder holding it, out of the graph of the repository.
def take(binary: Path, into: Path) -> dict[str, Path]:
    tree = into / NAME
    copy_tree(tree)
    (into / "setup.txt").write_text(SETUP, encoding="utf-8")
    said = into / "said.txt"
    # No display, so a scene that would open a window fails loud instead of covering the
    # person's screen.
    env = {key: value for key, value in os.environ.items()
           if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
    files = (into / f"{count}" for count in itertools.count())
    with said.open("w") as out:
        run = subprocess.Popen([binary, "src/runtime/recipes/cli/view.vlp", "--size", SIZE,
                                "--fps", "0", "--source", into / "setup.txt"], cwd=tree,
                               env=env, text=True, stdin=subprocess.PIPE, stdout=out,
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
    parser.add_argument("--binary", type=Path, help="the release build's vulpen, to run")
    binary = parser.parse_args().binary
    if not binary:
        return
    IMAGES.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vulpen-images-") as into:
        for name, picture in take(binary.resolve(), Path(into)).items():
            image = IMAGES / f"{name}.png"
            if image.exists() and image.read_bytes() == picture.read_bytes():
                continue
            temporary = image.with_name(f"{image.name}.tmp")
            shutil.copyfile(picture, temporary)
            temporary.replace(image)  # a killed run never leaves half a file
            print(f"wrote {image.relative_to(ROOT).as_posix()}")


if __name__ == "__main__":
    main()
