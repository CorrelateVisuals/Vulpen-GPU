#!/usr/bin/env python3
"""Check what the wave example prints against its golden (C01, GLSL02): from frame 0, and
from a day, a year and a century of frames in, where time must still be exact (A03).

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

from harness import DEVICE, EXAMPLES, OUT, ROOT, matches, problems, run

GOLDEN = ROOT / "src" / "tests" / "wave.golden"
VIEW = EXAMPLES / "wave" / "view.vlp"
# At 60 fps: now, a day, a year and a century. A 32-bit frame count wraps before the last.
FIRST_FRAMES = (0, 5_184_000, 1_892_160_000, 189_216_000_000)
FRAMES = 121  # the probe prints every 60th frame, so each run prints three lines
# GLSL's sin may be off by 2^-11 within [-pi, pi]; this view's angles go past it.
TOLERANCE = 1e-3
NUMBER = re.compile(r"[-+]?\d[\d.e+-]*")
HEADER = f"""\
# What {VIEW.relative_to(ROOT).as_posix()} prints, bit for bit, on the device below.
# Only a change meant to move it rewrites it: python3 src/tools/tests/golden.py VULPEN --update
"""


def record(vulpen: str, own_gpu: bool) -> tuple[str, dict[int, list[str]]]:
    """The device the runs took, and what each printed, by its first frame."""
    device, printed = "", {}
    for first in FIRST_FRAMES:
        code, output = run(vulpen, [VIEW, "--frames", FRAMES, "--fps", 0,
                                    "--first-frame", first, "--log", "info"],
                           headless=not own_gpu)
        if code != 0 or problems(output):
            sys.exit(f"the run from frame {first} failed with exit {code}:\n{output}")
        device = matches(DEVICE, output)[0]
        printed[first] = matches(OUT, output)
    return device, printed


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
    device, printed = record(options.vulpen, options.own_gpu)
    if options.update:
        write(device, printed)
        return
    golden_device, tolerance, golden = read()
    exact = device == golden_device
    print(f"on {device}: " + ("bit for bit" if exact else
          f"within {tolerance}, since the golden is from {golden_device}"))
    for first in FIRST_FRAMES:
        expected, got = golden.get(first, []), printed[first]
        same = expected == got if exact else (
            len(expected) == len(got) and all(map(close, expected, got,
                                                  [tolerance] * len(got))))
        if not same:
            sys.exit(f"from frame {first}, expected\n  " + "\n  ".join(expected) +
                     "\nbut got\n  " + "\n  ".join(got))


if __name__ == "__main__":
    main()
