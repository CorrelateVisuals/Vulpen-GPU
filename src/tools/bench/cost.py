"""Cost against a base commit, for bench.py cost --base: instructions per call of each
function's rows, in this tree's build and in the base's, both built and measured on this
machine, so the two are measured alike (C03: measure before and after). A function more
than 5% slower is listed; one on the hot path (bench.py hot) fails the run (the plan's
7.3). One faster is listed too.

The base is checked out in a worktree of its own, out/bench/base, and measured by its
own bench.py, so each side runs its own cases; an id only one side has is left out.
"""
import json
import re
import subprocess
import sys

SLOWER = 1.05  # how much more a hot function may cost a call than at the base
QUALIFIERS = re.compile(r"^(?:VP::|\(anonymous namespace\)::|\(unnamed\)::)+")


def bare(name: str) -> str:
    """A function's name as a bench id names it: no namespace, no parameters."""
    return QUALIFIERS.sub("", name.split("(")[0].split("<")[0])


def hot_names(runner) -> set[str]:
    path = runner.OUT / "hot.json"
    if not path.is_file():
        sys.exit("no out/bench/hot.json: run bench.py hot first, so the hot path is known")
    return {bare(function["name"])
            for function in json.loads(path.read_text(encoding="utf-8"))["functions"]}


def worktree(ref: str, runner):
    tree = runner.OUT / "base"
    if (tree / ".git").exists():
        subprocess.run(["git", "-C", str(tree), "checkout", "--quiet", "--force", "--detach",
                        ref], check=True)
    else:
        subprocess.run(["git", "worktree", "add", "--quiet", "--detach", str(tree), ref],
                       cwd=runner.ROOT, check=True)
    return tree


def against(arguments, runner) -> int:
    hot = hot_names(runner)
    runner.command_cost(arguments)
    tree = worktree(arguments.base, runner)
    configure = ["cmake", "--preset", arguments.preset]
    if lavapipe := runner.driver(arguments.preset):  # found beside this tree, not the base's
        configure.append(f"-DVULPEN_TEST_DRIVER={lavapipe}")
    subprocess.run(configure, cwd=tree, check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, str(tree / "src" / "tools" / "bench" / "bench.py"),
                    "cost", "--preset", arguments.preset, *arguments.benches],
                   cwd=tree, check=True, stdout=subprocess.DEVNULL)
    failed, slower, faster = [], [], []
    for path in sorted((runner.OUT / arguments.preset / "cost").glob("*.json")):
        theirs = tree / "out" / "bench" / arguments.preset / "cost" / path.name
        if not theirs.is_file():
            continue
        now = json.loads(path.read_text(encoding="utf-8"))
        before = json.loads(theirs.read_text(encoding="utf-8"))
        for ident in sorted(now.keys() & before.keys()):
            mine, base = now[ident]["instructions"], before[ident]["instructions"]
            if not mine or not base:
                continue
            said = f"{path.stem}/{ident}: {base} to {mine} instructions a call ({mine / base - 1:+.1%})"
            if mine > base * SLOWER:
                (failed if bare(ident.split("/", 1)[1].split(" ")[0]) in hot else slower).append(said)
            elif mine * SLOWER < base:
                faster.append(said)
    for title, lines in (("hot, slower", failed), ("slower", slower), ("faster", faster)):
        print(f"{title} than {arguments.base}: {len(lines)}")
        for line in lines:
            print(f"  {line}")
    return 1 if failed else 0
