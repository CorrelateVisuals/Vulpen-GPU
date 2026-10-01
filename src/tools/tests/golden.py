#!/usr/bin/env python3
"""Check what the wave example prints against its golden (C01, GLSL02): from frame 0, and
from a day, a year and a century of frames in, where time must still be exact (A03).
Edits that take the graph apart and put it back before the first frame must print the
golden too: every buffer the rebuilds make starts zeroed, whatever memory it reuses.

On the device that made the golden, every value matches bit for bit, in every build: the
probe prints each value as the shortest text that reads back to the same bits. On any
other device, each value lies within the golden's tolerance, since every driver computes
sin to its own precision.

Usage: python3 src/tools/tests/golden.py VULPEN [--own-gpu] [--update]
  --own-gpu  run on the machine's own GPU instead of the driver the build chose for tests
  --update   write the golden from this run, after a change meant to move it
"""
import argparse
import re
import sys
import tempfile
from pathlib import Path

from harness import DEVICE, EXAMPLES, OUT, ROOT, matches, problems, run

GOLDEN = Path(__file__).resolve().parent / "wave.golden"
VIEW = EXAMPLES / "wave" / "view.vlp"
# At 60 fps: now, a day, a year and a century. A 32-bit frame count wraps before the last.
FIRST_FRAMES = (0, 5_184_000, 1_892_160_000, 189_216_000_000)
FRAMES = 121  # the probe prints every 60th frame, so each run prints three lines
# GLSL's sin may be off by 2^-11 within [-pi, pi]; this view's angles go past it.
TOLERANCE = 1e-3
NUMBER = re.compile(r"[-+]?\d[\d.e+-]*")
# Each line reruns the schedule, and the graph ends as the manifest has it.
EDITS = """\
disconnect values
node remove probe
node add probe recipe=probe operator=Probe shader=Probe.comp invocations=8 param=step=128 \
param=every=60 log=info
connect values wave.values probe.values
param set wave amplitude 1.0
"""
HEADER = f"""\
# What {VIEW.relative_to(ROOT).as_posix()} prints, bit for bit, on the device below.
# Only a change meant to move it rewrites it: python3 src/tools/tests/golden.py VULPEN --update
"""


def record(vulpen: str, own_gpu: bool, arguments: list) -> tuple[str, list[str]]:
    """The device the run took, and what it printed."""
    code, output = run(vulpen, [VIEW, "--frames", FRAMES, "--fps", 0, "--log", "info",
                                *arguments], headless=not own_gpu)
    if code != 0 or problems(output):
        sys.exit(f"the run with {arguments} failed with exit {code}:\n{output}")
    return matches(DEVICE, output)[0], matches(OUT, output)


def read() -> tuple[str, float, dict[int, list[str]]]:
    device, tolerance, printed, first = "", TOLERANCE, {}, None
    for line in GOLDEN.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line:
            continue
        key, _, value = line.partition(" ")
        if key == "device":
            device = value
        elif key == "tolerance":
            tolerance = float(value)
        elif key == "from":
            first = int(value)
            printed[first] = []
        else:
            printed[first].append(line)
    return device, tolerance, printed


def write(device: str, printed: dict[int, list[str]]) -> None:
    lines = [HEADER + f"device {device}", f"tolerance {TOLERANCE}"]
    for first, output in printed.items():
        lines += [f"from {first}", *output]
    GOLDEN.write_text("\n".join(lines) + "\n", encoding="utf-8")


def close(expected: str, got: str, tolerance: float) -> bool:
    """Equal but for numbers, and each number within the tolerance."""
    if NUMBER.sub("#", expected) != NUMBER.sub("#", got):
        return False
    return all(abs(float(a) - float(b)) <= tolerance
               for a, b in zip(NUMBER.findall(expected), NUMBER.findall(got)))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("vulpen")
    parser.add_argument("--own-gpu", action="store_true")
    parser.add_argument("--update", action="store_true")
    options = parser.parse_args()
    printed = {}
    for first in FIRST_FRAMES:
        device, printed[first] = record(options.vulpen, options.own_gpu,
                                        ["--first-frame", first])
    if options.update:
        write(device, printed)
        return
    with tempfile.TemporaryDirectory() as temporary:
        script = Path(temporary) / "edits.txt"
        script.write_text(EDITS, encoding="utf-8")
        _, edited = record(options.vulpen, options.own_gpu, ["--source", script])
    golden_device, tolerance, golden = read()
    exact = device == golden_device
    print(f"on {device}: " + ("bit for bit" if exact else
          f"within {tolerance}, since the golden is from {golden_device}"))
    for what, first, got in [*((f"from frame {first}", first, printed[first])
                               for first in FIRST_FRAMES),
                             ("after the edits", 0, edited)]:
        expected = golden.get(first, [])
        same = expected == got if exact else (
            len(expected) == len(got) and all(map(close, expected, got,
                                                  [tolerance] * len(got))))
        if not same:
            sys.exit(f"{what}, expected\n  " + "\n  ".join(expected) +
                     "\nbut got\n  " + "\n  ".join(got))


if __name__ == "__main__":
    main()
