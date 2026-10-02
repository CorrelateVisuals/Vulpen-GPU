#!/usr/bin/env python3
"""Every broken view ends the run with exit 1 and an error that names its cause (A02,
RV03), before anything wrong reaches the GPU: a manifest that says a thing twice, SPIR-V
cut short, C++ and a shader that disagree on a name or a type, a connection whose ends
disagree, a dispatch that leaves a workgroup part full, a draw that writes, and a
node's command without its help, registered twice or failing when it runs. A view with
more pass blocks than one pool holds is no mistake: it runs, and so does an edit on it.

The views are written into a folder named mistakes, so they find the recipes the build
compiles from mistakes/ beside this script, which get these things wrong on purpose.

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
PASSES = 1025  # one past what a pool of pass blocks in Pipelines.cpp holds


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
    "command-help": (HEAD + operator("NoHelp"), "command `fill help` needs a usage and a help"),
    "command-twice": (HEAD + operator("Twin") + operator("Twin").replace('"fill"', '"twin"'),
                      "node twin: command `fill twin` registers twice"),
    "command-fails": (HEAD + operator("Refuse"), "command-fails.txt:1: refused, as the fixture",
                      "refuse\n"),
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


def check(vulpen: str, folder: Path, name: str, text: str, cause: str,
          script: str = "") -> str | None:
    """Why the case failed, or None. A script runs through --source."""
    view = folder / f"{name}.vlp"
    view.write_text(text, encoding="utf-8")
    arguments = [view, "--frames", 1, "--fps", 0]
    if script:
        (folder / f"{name}.txt").write_text(script, encoding="utf-8")
        arguments += ["--source", folder / f"{name}.txt"]
    code, output = run(vulpen, arguments, headless=name not in WINDOWED)
    if code != 1 or cause not in output or problems(output):
        return f"{name}: expected exit 1 and '{cause}', got exit {code}:\n{output}"
    return None


def no_limit(vulpen: str, folder: Path) -> str | None:
    """Why a view past one pool of pass blocks, or an edit on it, failed, or None."""
    view = folder / "passes.vlp"
    view.write_text(HEAD + "".join(FILL.replace('"fill"', f'"fill{index}"')
                                   for index in range(PASSES)), encoding="utf-8")
    edit = folder / "edit.txt"
    edit.write_text("param set fill0 amount 2\n", encoding="utf-8")
    code, output = run(vulpen, [view, "--frames", 1, "--fps", 0, "--source", edit])
    if code != 0 or problems(output):
        return (f"passes: expected {PASSES} passes and an edit on them to run, got exit "
                f"{code}:\n{output}")
    return None


def main() -> None:
    vulpen = sys.argv[1]
    cut = cut_spirv(vulpen)
    try:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / "mistakes"
            folder.mkdir()
            failed = [problem for name, (text, cause, *script) in CASES.items()
                      if name not in WINDOWED or display()
                      if (problem := check(vulpen, folder, name, text, cause, *script))]
            if problem := no_limit(vulpen, folder):
                failed.append(problem)
    finally:
        for file in cut:
            file.unlink()
    if failed:
        sys.exit("\n".join(failed))


if __name__ == "__main__":
    main()
