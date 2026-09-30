#!/usr/bin/env python3
"""Every broken view ends the run with exit 1 and an error that names its cause (A02,
RV03), before anything wrong reaches the GPU: a manifest that says a thing twice, SPIR-V
cut short, C++ and a shader that disagree on a name or a type, a connection whose ends
disagree, a dispatch that leaves a workgroup part full, a draw that writes, and more
passes than the build holds.

The views are written into a folder named mistakes, so they find the recipes the build
compiles from src/tests/mistakes/, which get these things wrong on purpose.

Usage: python3 src/tools/tests/fail-loud.py VULPEN
"""
import sys
import tempfile
from pathlib import Path

from harness import display, problems, run

HEAD = "[manifest]\nversion = 1\n"
FILL = '[node "fill"]\nrecipe = fill\nshader = Fill.comp\ninvocations = 64\nparam = amount=1\n'
PAIRS = '[node "pairs"]\nrecipe = connect\nshader = Pairs.comp\ninvocations = 64\n'
SUM = '[node "sum"]\nrecipe = connect\nshader = Sum.comp\ninvocations = 64\n'
DRAW = ('[node "draw"]\nrecipe = draw\nshader = Draw.vert\nshader = Draw.frag\n'
        'invocations = 3\n')
PASSES = 1025  # one past what Pipelines.cpp holds


def connection(name: str, source: str, *targets: str) -> str:
    return f'[connection "{name}"]\nfrom = {source}\n' + "".join(f"to = {t}\n" for t in targets)


def operator(name: str) -> str:
    return FILL.replace("recipe = fill\n", f"recipe = fill\noperator = {name}\n")


def shader(name: str) -> str:
    return FILL.replace("Fill.comp", name)


# What each view gets wrong, and what its error must say.
CASES = {
    "param-twice": (HEAD + FILL + "param = amount=2\n", "param amount is set twice"),
    "manifest-twice": (HEAD + "[manifest]\n" + FILL, "[manifest] is given twice"),
    "version-twice": (HEAD + "version = 1\n" + FILL, "version is set twice"),
    "connection-twice": (HEAD + PAIRS + SUM + connection("a", "pairs.pairs", "sum.values")
                         + connection("a", "pairs.pairs", "sum.values"),
                         "two connections are named a"),
    "port-twice": (HEAD + PAIRS + SUM
                   + connection("a", "pairs.pairs", "sum.values", "sum.values"),
                   "sum.values joins two connections"),
    "port-in-two": (HEAD + PAIRS + SUM + connection("a", "pairs.pairs", "sum.values")
                    + connection("b", "pairs.pairs", "sum.values"),
                    "pairs.pairs joins two connections"),
    "truncated-spirv": (HEAD + shader("Truncated.comp"), "Truncated.comp.spv is truncated"),
    "empty-spirv": (HEAD + shader("Empty.comp"), "Empty.comp.spv is not SPIR-V"),
    "cpp-name": (HEAD + operator("WrongName"), "the operator sets level, which the shader"),
    "cpp-type": (HEAD + operator("WrongType"), "sets amount as a uint, but the shader"),
    "elements": (HEAD + PAIRS + SUM + connection("a", "pairs.pairs", "sum.values"),
                 "pairs writes 8-byte vec2, values reads 4-byte float"),
    "not-read": (HEAD + PAIRS + SUM + connection("a", "pairs.pairs", "sum.sums"),
                 "sums is not a buffer its shader reads"),
    "workgroups": (HEAD + FILL.replace("= 64", "= 100"),
                   "invocations = 100 is not a multiple of local_size_x = 64"),
    "draw-writes": (HEAD + DRAW, "a draw's shaders only read buffers"),
    "passes": (HEAD + "".join(FILL.replace('"fill"', f'"fill{index}"')
                              for index in range(PASSES)),
               "more than 1024 pass blocks"),
}
WINDOWED = {"draw-writes"}


def cut_spirv(vulpen: str) -> list[Path]:
    """SPIR-V no build writes: one cut inside its first instruction, one empty."""
    fill = Path(vulpen).parent / "views" / "mistakes" / "recipes" / "fill"
    whole = (fill / "Fill.comp.spv").read_bytes()
    header_and_a_word = 24
    written = {fill / "Truncated.comp.spv": whole[:header_and_a_word],
               fill / "Empty.comp.spv": b""}
    for file, content in written.items():
        file.write_bytes(content)
    return list(written)


def check(vulpen: str, folder: Path, name: str, text: str, cause: str) -> str | None:
    """Why the case failed, or None."""
    view = folder / f"{name}.vlp"
    view.write_text(text, encoding="utf-8")
    code, output = run(vulpen, [view, "--frames", 1, "--fps", 0],
                       headless=name not in WINDOWED)
    if code != 1 or cause not in output or problems(output):
        return f"{name}: expected exit 1 and '{cause}', got exit {code}:\n{output}"
    return None


def main() -> None:
    vulpen = sys.argv[1]
    cut = cut_spirv(vulpen)
    try:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / "mistakes"
            folder.mkdir()
            failed = [problem for name, (text, cause) in CASES.items()
                      if name not in WINDOWED or display()
                      if (problem := check(vulpen, folder, name, text, cause))]
    finally:
        for file in cut:
            file.unlink()
    if failed:
        sys.exit("\n".join(failed))


if __name__ == "__main__":
    main()
