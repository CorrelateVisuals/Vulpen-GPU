#!/usr/bin/env python3
"""quit restart carries the session into the new vulpen, which a person would not notice
going wrong (V08): an edit lost, or an edit a save already wrote replayed on its file.

A host view hosts a copy of the wave example and edits it; the wave is saved halfway, so
its file holds the edits before the save and the restart replays only those after it. A
second view is hosted, edited and closed again, by a line addressed to itself, so its
edits go with it and its host's lines stay. Then quit restart builds vulpen, which the
test's build finds built, and the new vulpen runs in the old one's place, replays what
the files do not hold, and counts the frames on: the probe prints rising frames across
the restart, and the run ends after the frames asked for in all. A save's edits replayed
on its file would refuse a node added twice; any refusal fails the test.

Usage: python3 src/tools/tests/restart.py VULPEN
"""
import re
import shutil
import sys
import tempfile
from pathlib import Path

from harness import EXAMPLES, matches, problems, run

SCRIPT = """\
view add wave wave/view.vlp
wave: node add group
wave: param set probe every 30
wave: view save
wave: param set wave speed 0.04
view add extra extra/view.vlp
extra: node add inside
extra: view close extra
view switch wave
quit restart
"""
# What the new vulpen replays: the host's lines and the wave's after its save, as typed.
REPLAYED = [re.compile(r"^view add wave .*wave/view\.vlp$"),
            re.compile(r"^wave: param set wave speed 0\.04$"),
            re.compile(r"^view add extra .*extra/view\.vlp$"),
            re.compile(r"^extra: view close extra$"),
            re.compile(r"^view switch wave$")]
FRAMES = 600  # at 60 a second, time for the build before the run ends
STARTED = re.compile(r"\{run\} vulpen ")
FRAME = re.compile(r"probe: frame +(\d+):")


def main() -> None:
    vulpen = sys.argv[1]
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(temporary)
        # Named wave, so it finds what the build made for the example.
        shutil.copytree(EXAMPLES / "wave", folder / "wave",
                        ignore=shutil.ignore_patterns(".vulpen"))
        (folder / "host").mkdir()
        (folder / "host" / "view.vlp").write_text("[manifest]\nversion = 1\n",
                                                  encoding="utf-8")
        (folder / "script.txt").write_text(SCRIPT, encoding="utf-8")
        code, output = run(vulpen, [folder / "host" / "view.vlp", "--source",
                                    folder / "script.txt", "--frames", FRAMES,
                                    "--log", "info"], timeout=300)
        replay = Path(vulpen).parent / "restart.log"
        lines = replay.read_text(encoding="utf-8").splitlines() if replay.exists() else []
    frames = [int(frame) for frame in matches(FRAME, output)]
    failures = []
    if code != 0 or problems(output) or "{!!!}" in output:
        failures.append(f"the run failed with exit {code}")
    if len(STARTED.findall(output)) != 2 or "restarted: replayed" not in output:
        failures.append("no new vulpen ran in the old one's place")
    if len(lines) != len(REPLAYED) or not all(
            pattern.match(line) for pattern, line in zip(REPLAYED, lines)):
        failures.append("the replay is not the edits the files do not hold:\n  " +
                        "\n  ".join(lines))
    if not frames or frames != sorted(set(frames)) or frames[-1] < FRAMES - 60:
        failures.append(f"the frames did not count on across the restart: {frames}")
    if failures:
        sys.exit("restart: " + "; ".join(failures) + f"\n{output}")


if __name__ == "__main__":
    main()
