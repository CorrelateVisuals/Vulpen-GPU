#!/usr/bin/env python3
"""Run the wave example's manifest, mutated, many times over; meant for a sanitizer
build. Each run either works, or ends with exit 1 and an error that names its cause
(A02). A crash, a hang, a sanitizer report or a validation message fails the test, and so
does a refusal that only repeats what the standard library threw (A03).

With --commands, the manifest stays whole and a script of commands is mutated instead,
which a copy of the wave view runs through the command port before its first frame
(RV04); a copy, since the script saves it.

The mutations follow a seed, so a failure repeats (C01): the output names the seed, the
run and its input. "--seed today" takes the date, so each night tries new input.

Usage: python3 src/tools/tests/fuzz.py VULPEN [--commands] [--count N] [--seed N|today]
"""
import argparse
import random
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from harness import EXAMPLES, environment, problems, run

SEED_VIEW = EXAMPLES / "wave" / "view.vlp"
# A script of every edit, which leaves the wave graph whole, then saves the view and its
# log and replays the log, which leaves the graph as it was. quit comes in through the
# tokens, so most runs go on to cook their frames on the edited graph.
SEED_SCRIPT = """\
param set wave amplitude 0.5
param set wave spare 1
param unset wave spare
node set wave invocations=1024
disconnect values
node remove probe
node add probe recipe=probe operator=Probe shader=Probe.comp invocations=8 param=step=64
param set probe every 30
connect values wave.values probe.values
view save
log save session.log
source session.log
"""
FRAMES = 3
TIMEOUT = 60  # seconds; a sanitizer build of a small view starts in about one
TOKENS = ["", "0", "1", "-1", "64", "65", "4294967295", "4294967296", "1e9", "0.5", "nan",
          "inf", "9" * 40, "x" * 4000, '"', "[", "]", "=", ".", "#", "é", "\t",
          "wave", "probe", "values", "samples", "Wave.comp", "Probe.comp", "Wave.vert",
          "quit", "node", "add", "remove", "set", "unset", "param", "connect", "disconnect",
          "operator", "shader", "invocations", "log", "save", "source", "session.log",
          "script.txt", "view", "deploy", "recipe", "a.wave", "a.wave.values",
          "[node \"wave\"]", "[connection \"values\"]", "[deploy \"a\"]", "[manifest]",
          "version = 1"]
# What std::exception::what() says for the standard library's own throws: a message that
# names no file, node or key of the view.
BARE = re.compile(r"\{!!!\} (?:map::at|unordered_map::at|vector::|basic_string|array::at"
                  r"|std::bad_|bad_optional_access|bad_variant_access|sto[a-z]+)\b")


def mutate(text: str, chance: random.Random) -> str:
    for _ in range(chance.randint(1, 3)):
        lines = text.splitlines(keepends=True) or [""]
        at = chance.randrange(len(lines))
        kind = chance.randrange(6)
        if kind == 0:
            del lines[at]
        elif kind == 1:
            lines.insert(at, lines[chance.randrange(len(lines))])
        elif kind == 2:
            other = chance.randrange(len(lines))
            lines[at], lines[other] = lines[other], lines[at]
        elif kind == 3:
            words = re.split(r"([\s=.\"]+)", lines[at])
            words[chance.randrange(len(words))] = chance.choice(TOKENS)
            lines[at] = "".join(words)
        text = "".join(lines)
        if kind == 4:
            text = text[:chance.randrange(len(text) + 1)]
        elif kind == 5:
            spot = chance.randrange(len(text) + 1)
            text = text[:spot] + chr(chance.randrange(1, 256)) + text[spot:]
    return text


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("vulpen")
    parser.add_argument("--commands", action="store_true")
    parser.add_argument("--count", type=int, default=300)
    parser.add_argument("--seed", default="1")
    options = parser.parse_args()
    if options.seed == "today":
        options.seed = time.strftime("%Y%m%d")
    chance = random.Random(int(options.seed))
    seed = SEED_SCRIPT if options.commands else SEED_VIEW.read_text(encoding="utf-8")
    # A mutation may make a node draw; with no display it fails loud instead of flashing
    # a window.
    env = {key: value for key, value in environment().items()
           if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
    refused = 0
    with tempfile.TemporaryDirectory() as temporary:
        view = Path(temporary) / "wave" / "view.vlp"  # named wave, so it finds its recipes
        view.parent.mkdir()
        script = Path(temporary) / "script.txt"
        mutated, arguments = ((script, [view, "--source", script]) if options.commands
                              else (view, [view]))
        for index in range(options.count):
            text = mutate(seed, chance)
            if options.commands:
                view.write_text(SEED_VIEW.read_text(encoding="utf-8"), encoding="utf-8")
            mutated.write_text(text, encoding="utf-8", errors="replace")
            try:
                code, output = run(options.vulpen,
                                   [*arguments, "--frames", FRAMES, "--fps", 0],
                                   timeout=TIMEOUT, env=env)
            except subprocess.TimeoutExpired:
                code, output = None, f"no end after {TIMEOUT} s"
            refused += code == 1
            if code not in (0, 1) or problems(output) or BARE.search(output) or (
                    code == 1 and "{!!!}" not in output):
                sys.exit(f"run {index} of seed {options.seed} failed with exit {code}.\n"
                         f"The {'script' if options.commands else 'manifest'}:\n{text}\n"
                         f"The output:\n{output}")
    print(f"{options.count} runs: {refused} refused, naming their cause; "
          f"{options.count - refused} ran")


if __name__ == "__main__":
    main()
