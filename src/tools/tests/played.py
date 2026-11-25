#!/usr/bin/env python3
"""Fail when a project run alone cannot be left once its files changed, or loses the change.

A project vulpen started with alone, with no app as the cli or the ide, is played, as a
game is: its files change meanwhile in an editor or by a bake, and its Exit and the
window's close button send quit. quit must end such a run whatever its files hold, and
put none back, where a run with an app refuses it, as that one is edited (RA04).

A project made in a temporary folder holds one node whose C++ sends quit at a set frame,
as a game's Exit does. A first run hosts it from the CLI, which builds it; a second runs
it alone, paced, and a file of the project changes before that frame comes. The run
must end there, saying its files stay, and the file must hold the change.

Usage: python3 src/tools/tests/played.py VULPEN
"""
import os
import sys
import tempfile
import time
from pathlib import Path

from harness import ROOT, problems
from soak import SWAP_TIMEOUT, Run  # a run whose lines are read as they come

CLI = ROOT / "src" / "runtime" / "recipes" / "vulpen" / "cli" / "view.vlp"
AT = 300            # the frame the node sends quit at: 5 s at 60 frames a second
FRAMES = 3 * AT     # past it, a run that refused quit ends anyway
ENDED_S = 60        # for the second run to end
VIEW = f"""[manifest]
version = 1

[node "ender"]
param = at={AT}
"""
ENDER = """#include "runtime/Operator.h"

#include <cstdint>

// Sends quit at the frame its at param names, as a game's Exit does.
class Ender final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _at = node.param<std::uint32_t>("at");
  }
  void cook(VP::Cook &frame) override {
    if (frame.index() == _at)
      frame.commands().send("quit");
  }
  std::uint32_t _at = 0;
};

VP_OPERATOR(Ender);
"""
CHANGE = "changed in an editor while played\n"
PLAYED = "it plays projects alone, so their files stay as they are"


def main() -> None:
    vulpen = sys.argv[1]
    with tempfile.TemporaryDirectory() as temporary:
        view = Path(temporary) / f"played-{os.getpid()}"
        (view / "ender").mkdir(parents=True)
        (view / "view.vlp").write_text(VIEW, encoding="utf-8")
        (view / "ender" / "Ender.cpp").write_text(ENDER, encoding="utf-8")
        notes = view / "notes.txt"
        notes.write_text("as hosted\n", encoding="utf-8")
        # The CLI hosts it once, which builds its module, as run.sh builds a project it
        # starts; that run may end at the node's quit, as nothing changed yet.
        cli = Run(vulpen, [CLI, "--fps", 60, "--log", "info"], headless=True, listen=True,
                  typed=True)
        cli.type(f"view load {view}")
        if "{!!!}" in (line := cli.wait_for("and swapped", SWAP_TIMEOUT)):
            sys.exit(f"hosting the project was refused: {line.strip()}")
        if cli.process.poll() is None:
            cli.process.terminate()
            cli.process.wait()
        alone = Run(vulpen, [view / "view.vlp", "--fps", 60, "--frames", FRAMES, "--log",
                             "info"], headless=True, listen=True)
        alone.wait_for("live code:", SWAP_TIMEOUT)
        with notes.open("a", encoding="utf-8") as file:
            file.write(CHANGE)
        said = alone.wait_for("quit:", ENDED_S)
        end = time.monotonic() + ENDED_S
        while alone.process.poll() is None and time.monotonic() < end:
            time.sleep(0.1)
        kept = notes.read_text(encoding="utf-8")
        if alone.process.poll() is None:
            alone.process.kill()
    failures = [problem for problem in problems("\n".join(alone.problems))]
    if PLAYED not in said:
        failures.append(f"quit did not end the run as played: {said.strip()}")
    if CHANGE not in kept:
        failures.append("the file changed while played was put back")
    if failures:
        sys.exit("\n".join(failures))
    print(f"ended: {said.strip()}")


if __name__ == "__main__":
    main()
