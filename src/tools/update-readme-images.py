#!/usr/bin/env python3
"""Take the README's pictures of the ide into docs/images/, as commands replayed headless.

Every build runs it after the link, as it runs fetch-statistics.py; a release build passes
its binary, so the pictures show the code as it ships, and a debug build passes none and
takes none. Each run is the cli hosting the ide with no window (V07), at a window size and
in a theme of its own, from a copy of the repository, so no person's .vulpen/ arrangement
shows and nothing the run saves reaches the tree. Each scene is lines a person could type
(V06), sent on standard input; then the snapshot part saves the ide's window as a PNG. An
animation joins pictures a run took into one animated PNG. A picture is written only when
its bytes changed.

Usage: python3 src/tools/update-readme-images.py [--binary PATH]
"""
import argparse
import itertools
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGES = ROOT / "docs" / "images"
NAME = "Vulpen"  # the copy's folder, as the repository's .vulpen/graph.ini names its top
# The theme the ide's palette reads, in the copy; a run's own theme goes over it.
THEME = Path("src/runtime/recipes/library/parts/palette/theme.ini")
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
PNG = b"\x89PNG\r\n\x1a\n"


@dataclass(frozen=True)
class Run:
    """A run of the cli hosting the ide: the window's size, of which the snapshot part reads
    1920 by 1080 pixels at most in all, so 1440 by 1440 or 2400 by 860 too; a theme.ini in
    place of the palette's; and the pictures it takes, by their files' names, in order, each
    scene's lines from where the scene before left the ide."""
    scenes: dict[str, list[str]]
    size: str = "1920x1080"
    theme: Path | None = None  # from ROOT


@dataclass(frozen=True)
class Animation:
    """Pictures one run took, joined into an animated PNG that loops, each shown delay_ms;
    they show there alone, as no file of their own."""
    frames: tuple[str, ...]
    delay_ms: int


@dataclass(frozen=True)
class Pictures:
    """What a README shows: the runs that take its pictures, the animations joined from
    them, and the folder both are written into."""
    folder: Path
    runs: tuple[Run, ...]
    animations: dict[str, Animation] = field(default_factory=dict)


def typed(line: str) -> list[str]:
    """A line typed into the ide's terminal, so the terminal shows it as a person's."""
    return [f"input text {line}", "input key down enter", "input key up enter"]


def graph(folder: str) -> list[str]:
    """A folder of the copy in the graph, as open shows it."""
    return [f"open {folder}", "ide: focus"]


def zoom(steps: int) -> list[str]:
    """The text a step larger each, 1.1 times, as control and = draw it."""
    return ["ide: zoom in"] * steps


def ratio(split: str, value: float) -> list[str]:
    """The share of a dock its first half takes: ide.dock's the panels over the terminal,
    ide.top's the editor beside the rest, ide.side's the graph over the Perform panel."""
    return [f"ide: param set ide.{split}.split ratio {value}"]


def panels(verb: str, *names: str) -> list[str]:
    """panel hide, show or maximize, which a second maximize gives back, of each panel."""
    return [f"ide: panel {verb} ide.{name}-panel" for name in names]


def park(width: int) -> list[str]:
    """The pointer at the menubar's empty end, so no tooltip shows for what it was over."""
    return [f"input pointer {width - 4} 2"]


def scroll(x: int, y: int, rows: int, width: int) -> list[str]:
    """The wheel turned over a point of a window width pixels wide: a text scrolls down
    by rows below 0."""
    return [f"input pointer {x} {y}", f"input wheel 0 {rows}", *park(width)]


CLEAR = ["input key down control", "input key down l", "input key up l",
         "input key up control"]
INSIDE_IDE = ["ide: param unset ide.graph.relations folder", "view switch ide",
              "ide: param set ide.graph.graph inside ide", "ide: focus"]
# Each picture, by its file's name, and the lines that make it, from where the scene
# before left the ide. Whatever turns is held still, and each ends with no focus, so no
# caret blinks: the picture is the same at whatever frame it is taken. No line shows an
# absolute path, which names the copy, a new folder each run. The sizes and the text's
# zoom are for the README's column, about 880 pixels wide, so the text reads there.
# The cover: a node's C++ beside the project's graph and what it draws, wide.
HERO = {
    "hero": [*CLEAR, *typed("view load triangle-files"),
             *typed("param set clock frames_per_turn 0"), *typed("open Corners.cpp"),
             *typed("info corners"), *zoom(7), *panels("hide", "terminal"),
             *ratio("top", 0.5), *ratio("side", 0.46), *scroll(500, 400, -6, 2400),
             "ide: focus"],
}
# Three squares side by side: the graph, a shader and the terminal, each given the window.
TILES = {
    "tile-graph": [*CLEAR, "view load triangle-files", "param set clock frames_per_turn 0",
                   *zoom(9), *panels("maximize", "graph"), "ide: focus"],
    "tile-code": [*panels("maximize", "graph"), "open Face.vert",
                  *panels("maximize", "editor"), "ide: focus"],
    "tile-terminal": [*panels("maximize", "editor"), *panels("maximize", "terminal"),
                      "ide: focus ide.terminal.command-line", *typed("ls"),
                      *typed("info face"), "ide: focus"],
}
# The sections' pictures: the manifest beside its graph, the project in the whole window,
# the command line, the repository's top in the graph, and the ide inside its own graph.
DETAILS = {
    "manifest": [*CLEAR, "view load triangle-files", "param set clock frames_per_turn 0",
                 *zoom(5), "open", *panels("hide", "perform", "terminal"), "ide: focus"],
    "perform": ["ide: mode perform"],
    "cli": ["ide: mode edit", *panels("maximize", "terminal"),
            "ide: focus ide.terminal.command-line", *CLEAR, *typed("help"), *typed("ls"),
            *typed("info corners"), "ide: focus"],
    "graph-repository": [*panels("maximize", "terminal", "graph"), *graph("./")],
    "graph-ide": INSIDE_IDE,
}
# A dive through the repository in the graph, a level a frame, down to the part that draws
# the graph, and the ide inside its own graph.
DIVE = {
    "level-0": [*zoom(4), *panels("maximize", "graph"), *graph("./")],
    "level-1": graph("src/"),
    "level-2": graph("src/runtime/"),
    "level-3": graph("src/runtime/recipes/"),
    "level-4": graph("src/runtime/recipes/library/"),
    "level-5": graph("src/runtime/recipes/library/parts/"),
    "level-6": graph("src/runtime/recipes/library/parts/graph/"),
    "level-7": INSIDE_IDE,
}
README = Pictures(IMAGES, (Run(HERO, size="2400x860"), Run(TILES, size="1200x1200"),
                           Run(DETAILS), Run(DIVE, size="2400x860")),
                  {"dive": Animation(tuple(DIVE), 2200)})


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
def take(binary: Path, run: Run, into: Path) -> dict[str, Path]:
    tree = into / NAME
    copy_tree(tree)
    if run.theme:
        shutil.copyfile(ROOT / run.theme, tree / THEME)
    (into / "setup.txt").write_text(SETUP, encoding="utf-8")
    said = into / "said.txt"
    # No display, so a scene that would open a window fails loud instead of covering the
    # person's screen.
    env = {key: value for key, value in os.environ.items()
           if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
    files = (into / f"{count}" for count in itertools.count())
    with said.open("w") as out:
        vulpen = subprocess.Popen([binary, "src/runtime/recipes/cli/view.vlp", "--size",
                                   run.size, "--fps", "0", "--source", into / "setup.txt"],
                                  cwd=tree, env=env, text=True, stdin=subprocess.PIPE,
                                  stdout=out, stderr=subprocess.STDOUT)

        def wait_for(file: Path, after: str) -> None:
            deadline = time.monotonic() + TIMEOUT_S
            while not file.exists():
                if vulpen.poll() is not None or time.monotonic() > deadline:
                    vulpen.kill()
                    sys.exit(f"vulpen saved no {file.name} after {after}:\n"
                             f"{said.read_text(errors='replace')}")
                time.sleep(POLL_S)

        # Each line saving a file is written once the one before saved its own, so it runs
        # a frame later at least, and the next line of the scene once what this one set off
        # has passed from node to node, as a person types.
        def send(line: str) -> None:
            vulpen.stdin.write(f"{line}\n")
            for _ in range(FRAMES_A_LINE):
                file = next(files)
                vulpen.stdin.write(f"log save {file}\n")
                vulpen.stdin.flush()
                wait_for(file, line)

        pictures = {}
        for name, lines in run.scenes.items():
            for line in lines:
                send(line)
            pictures[name] = into / f"{name}.png"
            vulpen.stdin.write(f": param set cli.snapshot file {pictures[name]}\n")
            vulpen.stdin.flush()
            wait_for(pictures[name], name)
        vulpen.stdin.write("quit\n")
        vulpen.stdin.close()
        code = vulpen.wait(TIMEOUT_S)
    errors = [line for line in said.read_text(errors="replace").splitlines() if ERROR in line]
    if code != 0 or errors:
        sys.exit(f"vulpen exited {code}:\n" + "\n".join(errors))
    return pictures


def chunks(png: bytes) -> list[tuple[bytes, bytes]]:
    """A PNG's chunks, each its type and its data."""
    found, at = [], len(PNG)
    while at < len(png):
        (length,) = struct.unpack(">I", png[at:at + 4])
        found.append((png[at + 4:at + 8], png[at + 8:at + 8 + length]))
        at += 12 + length
    return found


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def animated(pictures: list[bytes], delay_ms: int) -> bytes:
    """The pictures as the frames of an animated PNG that loops: each frame's image data is
    its picture's own, copied, so nothing is decoded again and the bytes follow from the
    pictures' alone (C01). One run takes them, so they share a size and a format."""
    read = [chunks(picture) for picture in pictures]
    header = read[0][0][1]  # IHDR, which a PNG holds first
    if any(found[0][1] != header for found in read):
        sys.exit("an animation's pictures differ in size or format")
    width, height = struct.unpack(">II", header[:8])
    out = [PNG, chunk(b"IHDR", header), chunk(b"acTL", struct.pack(">II", len(read), 0))]
    sequence = itertools.count()  # fcTL and fdAT count in one sequence
    for index, found in enumerate(read):
        out.append(chunk(b"fcTL", struct.pack(">IIIIIHHBB", next(sequence), width, height,
                                              0, 0, delay_ms, 1000, 0, 0)))
        for kind, data in found:
            if kind == b"IDAT":
                out.append(chunk(b"IDAT", data) if index == 0 else
                           chunk(b"fdAT", struct.pack(">I", next(sequence)) + data))
    out.append(chunk(b"IEND", b""))
    return b"".join(out)


def write(binary: Path, pictures: Pictures) -> None:
    taken: dict[str, bytes] = {}
    with tempfile.TemporaryDirectory(prefix="vulpen-images-") as folder:
        for count, run in enumerate(pictures.runs):
            into = Path(folder) / f"{count}"
            into.mkdir()
            for name, picture in take(binary, run, into).items():
                taken[name] = picture.read_bytes()
    for name, animation in pictures.animations.items():
        taken[name] = animated([taken[frame] for frame in animation.frames],
                               animation.delay_ms)
    frames = {frame for animation in pictures.animations.values() for frame in animation.frames}
    pictures.folder.mkdir(parents=True, exist_ok=True)
    for name, data in taken.items():
        if name in frames:
            continue  # it shows in its animation alone
        image = pictures.folder / f"{name}.png"
        if image.exists() and image.read_bytes() == data:
            continue
        temporary = image.with_name(f"{image.name}.tmp")
        temporary.write_bytes(data)
        temporary.replace(image)  # a killed run never leaves half a file
        print(f"wrote {image.relative_to(ROOT).as_posix()}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--binary", type=Path, help="the release build's vulpen, to run")
    binary = parser.parse_args().binary
    if binary:
        write(binary.resolve(), README)


if __name__ == "__main__":
    main()
