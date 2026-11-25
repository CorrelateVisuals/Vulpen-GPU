#!/usr/bin/env python3
"""Fail when a live build's failure goes unsaid while a file beside the view changes (A02).

A view hosted from a folder outside projects/ makes the folder around it one the build
reads views from. Other files there may change all the time, as a log beside a project
in a repository of its own, and none is the view's. A broken shader saved meanwhile must
still be named once its build fails, not built again and again in silence, as if each
failure had met a file gone mid-build.

The first run hosts a copy of the wave example, which needs no window, from a temporary
folder. That puts the copy into the build's mounted views and the folder around it into
those it reads views from. A vulpen reads those as it starts, so a second run hosts the
copy again. Then a file beside the copy grows, and the copy's compute shader gains a line
that is no GLSL.

Usage: python3 src/tools/tests/live.py VULPEN
"""
import os
import shutil
import sys
import tempfile
import threading
from pathlib import Path

from harness import EXAMPLES, ROOT
from soak import SWAP_TIMEOUT, Run  # a run whose lines are read as they come

CLI = ROOT / "src" / "runtime" / "recipes" / "vulpen" / "cli" / "view.vlp"
SAID_TIMEOUT = 60  # seconds for the failed build to be named
GROW_S = 0.02      # between two lines written into the file beside the copy
FAILED = "live: the build of"


def hosted(vulpen: str, view: Path) -> Run:
    """A vulpen at the CLI that hosts the view and has swapped in its first build."""
    one = Run(vulpen, [CLI, "--fps", 60, "--log", "info"], headless=True, listen=True,
              typed=True)
    one.type(f"view load {view}")
    if "{!!!}" in (line := one.wait_for("and swapped", SWAP_TIMEOUT)):
        sys.exit(f"hosting the copy was refused: {line.strip()}")
    return one


def grow(beside: Path, done: threading.Event) -> None:
    with beside.open("a", encoding="utf-8") as file:
        while not done.wait(GROW_S):
            file.write("a line\n")
            file.flush()


def main() -> None:
    vulpen = sys.argv[1]
    with tempfile.TemporaryDirectory() as temporary:
        view = Path(temporary) / f"live-{os.getpid()}"
        shutil.copytree(EXAMPLES / "wave", view,
                        ignore=shutil.ignore_patterns(".vulpen"))
        # Made before the second run reads the folder's time, as writing into it later
        # changes only the file's.
        beside = Path(temporary) / "beside.log"
        beside.write_text("", encoding="utf-8")
        hosted(vulpen, view).stop()
        one = hosted(vulpen, view)
        done = threading.Event()
        writer = threading.Thread(target=grow, args=(beside, done), daemon=True)
        writer.start()
        with (view / "wave" / "Wave.comp").open("a", encoding="utf-8") as shader:
            shader.write("no glsl\n")
        line = one.wait_for(FAILED, SAID_TIMEOUT)
        done.set()
        writer.join()
        one.stop()
    if FAILED not in line:
        sys.exit(f"a broken shader's build was not named; vulpen said: {line.strip()}")
    print(f"named: {line.strip()}")


if __name__ == "__main__":
    main()
