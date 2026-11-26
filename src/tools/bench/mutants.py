"""Mutants of an engine file, for bench.py: small changes to its code, each built into
the file's own bench and run against its records, so a change no row notices marks a
place whose inputs cannot tell right from wrong (the plan's 7.3). A file's case includes
its .cpp, which its bench alone compiles, so each mutant rebuilds one bench. A header's
inline code is in every object that includes it, but only the benches named are built
and run, so they are those that include it, as Bench,Shown,Worlds,Text for
tools/bench/Bench.h; a bench with no .cpp of its own, as Shown, has its header
mutated. A file no case includes, as Binder.cpp, is in the engine's objects every bench
links: --source names it and the bench that runs it, whose build makes its object again.

It rewrites the engine file while it runs, so it runs in a worktree of its own, never in
one a person edits or a vulpen runs from: the file must be as committed, and goes back
after each mutant and at the end, and the bench's rows are run again so the next approve
reads the file's own. A run killed outright leaves the last mutant in place, which
git checkout -- <file> takes back.

A mutant whose bench builds byte for byte as before is in code this build leaves out, as
another system's branch of a platform file: it is counted apart, not run, as no row could
notice it here.
"""
import hashlib
import random
import re
import subprocess
import sys
from dataclasses import dataclass

# Each change: a pattern and what it becomes. An operator flips only where clang-format
# sets it apart with blanks, so a template's <> or a shift is never taken for one; then a
# literal true or false, a + 1 or - 1, and a ! dropped.
CHANGES = (
    (r"(?<= )<(?= )", "<="), (r"(?<= )<=(?= )", "<"),
    (r"(?<= )>(?= )", ">="), (r"(?<= )>=(?= )", ">"),
    (r"(?<= )==(?= )", "!="), (r"(?<= )!=(?= )", "=="),
    (r"(?<= )&&(?= )", "||"), (r"(?<= )\|\|(?= )", "&&"),
    (r"\btrue\b", "false"), (r"\bfalse\b", "true"),
    (r"(?<= )\+ 1\b", "- 1"), (r"(?<= )- 1\b", "+ 1"),
    (r"(?<=[( ])!(?=[\w(])", ""),
)
# A mutant may loop for ever where the code it changed bounded a loop; its rows then end
# as still running, which counts as caught.
TIMEOUT = 120
RAW_PREFIX = re.compile(r"(?:^|[^\w])(?:u8|u|U|L)?R$")


@dataclass
class Mutant:
    start: int
    end: int
    before: str
    after: str
    line: int


def masked(text: str) -> str:
    """The code with every comment and every string or character literal blanked, its
    newlines kept, so no change lands in one."""
    out = list(text)
    index, length = 0, len(text)
    while index < length:
        two = text[index:index + 2]
        if two == "//":
            end = text.find("\n", index)
        elif two == "/*":
            end = text.find("*/", index + 2)
            end = -1 if end == -1 else end + 2
        elif text[index] == '"' and RAW_PREFIX.search(text[max(0, index - 3):index]):
            opened = text.find("(", index)
            closing = ")" + text[index + 1:opened] + '"'
            end = text.find(closing, opened)
            end = -1 if end == -1 else end + len(closing)
        elif text[index] in "\"'":
            end = index + 1
            while end < length and text[end] != text[index]:
                end += 2 if text[end] == "\\" else 1
            end += 1
        else:
            index += 1
            continue
        end = length if end == -1 else min(end, length)
        for at in range(index, end):
            if out[at] != "\n":
                out[at] = " "
        index = end
    return "".join(out)


def sites(text: str) -> list[Mutant]:
    code = masked(text)
    found = [Mutant(match.start(), match.end(), match.group(0), after,
                    text.count("\n", 0, match.start()) + 1)
             for pattern, after in CHANGES for match in re.finditer(pattern, code)]
    return sorted(found, key=lambda mutant: mutant.start)


def read(path) -> str:
    with open(path, encoding="utf-8", newline="") as file:
        return file.read()


def write(path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="") as file:
        file.write(text)


def digest(path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def mutants(arguments, runner) -> int:
    # The benches that run the file: its own first, then others whose cases reach it
    # another way, as Views' sessions reach Edits::command. A mutant one of them notices
    # is caught. In the order named, as review.benches sorts them by name, which would put
    # Schedule before Shader and mutate Schedule.cpp.
    named = arguments.bench.split(",")
    benches = sorted(runner.benches(named), key=lambda each: named.index(each.name))
    bench = benches[0]
    # Its own .cpp, else its own header; or another file named: an engine file every bench
    # links, as Binder.cpp, which Schedule's bench runs through every project's bind, a
    # header the benches named include, or one of a part's several .cpp.
    own = ([source for source in bench.sources if source.endswith(".cpp")] or
           list(bench.sources))
    if not arguments.source and len(own) != 1:
        sys.exit(f"{bench.name}: name one of {', '.join(own)} with --source")
    source = runner.SOURCE / (arguments.source or f"{bench.folder}/{own[0]}")
    if not source.is_file():
        sys.exit(f"{bench.name}: no {source.name}")
    status = subprocess.run(["git", "status", "--porcelain", "--", str(source)],
                            cwd=runner.ROOT, capture_output=True, text=True).stdout
    if status.strip():
        sys.exit(f"{source} differs from its commit; mutants start from it as committed")
    original = read(source)
    found = sites(original)
    chosen = found
    if arguments.lines:
        wanted = {int(line) for line in arguments.lines.split(",")}
        chosen = [mutant for mutant in found if mutant.line in wanted]
    elif arguments.limit and arguments.limit < len(found):
        chosen = sorted(random.Random(arguments.seed).sample(found, arguments.limit),
                        key=lambda mutant: mutant.start)
    runner.build("debug", benches)
    built = {each.name: digest(each.binary("debug")) for each in benches}
    baseline = {name: ran["baseline"] for name, ran in
                runner.run_all(benches, "debug", (runner.BASELINE,)).items()}
    for each in benches:
        approved = runner.sections_of(each.record.read_text(encoding="utf-8"))
        if any(sorted(baseline[each.name].get(ident, [])) != sorted(rows)
               for ident, rows in approved.items()):
            sys.exit(f"{each.name}'s rows differ from its records: run compare first")
    runner.TIMEOUT = TIMEOUT
    folder = runner.ROOT / "out" / "build" / "debug"
    target = ["cmake", "--build", str(folder), "--target",
              *(f"bench-{each.name}" for each in benches), "--parallel", runner.JOBS]
    results = []
    try:
        for index, mutant in enumerate(chosen, 1):
            write(source, original[:mutant.start] + mutant.after + original[mutant.end:])
            if subprocess.run(target, cwd=runner.ROOT, capture_output=True).returncode != 0:
                results.append((mutant, "builds not", []))
            elif all(digest(each.binary("debug")) == built[each.name] for each in benches):
                results.append((mutant, "not compiled here", []))
            else:
                try:
                    ran = runner.run_all(benches, "debug", (runner.BASELINE,))
                except SystemExit as stopped:  # a bench the mutant broke before any row
                    results.append((mutant, "caught", [str(stopped)]))
                else:
                    moved = sorted(ident for each in benches
                                   for ident in baseline[each.name]
                                   if sorted(ran[each.name]["baseline"].get(ident, []))
                                   != sorted(baseline[each.name][ident]))
                    results.append((mutant, "caught" if moved else "survived", moved))
            said, what, moved = results[-1][1], f"{mutant.before} -> {mutant.after or '(none)'}", results[-1][2]
            print(f"{index}/{len(chosen)} {source.name}:{mutant.line} {what}: {said}"
                  + (f" by {', '.join(moved[:2])}" if moved else ""), flush=True)
    finally:
        write(source, original)
        subprocess.run(target, cwd=runner.ROOT, capture_output=True)
        runner.run_all(benches, "debug", (runner.BASELINE,))
    lines = original.splitlines()
    counts = {kind: sum(1 for _, said, _ in results if said == kind)
              for kind in ("caught", "survived", "builds not", "not compiled here")}
    commit = subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], cwd=runner.ROOT,
                            capture_output=True, text=True).stdout.strip()
    picked = (f"those on lines {arguments.lines}" if arguments.lines
              else f"seed {arguments.seed}")
    report = [f"# Mutants of src/{source.relative_to(runner.SOURCE).as_posix()} at "
              f"{commit}, run by the benches of {', '.join(each.name for each in benches)}",
              "",
              f"{len(results)} of the {len(found)} sites, {picked}: "
              f"{counts['caught']} caught, {counts['survived']} survived, "
              f"{counts['builds not']} not built, {counts['not compiled here']} not "
              "compiled here.", "", "## Survived", "",
              "Each a change no row of the bench notices: its inputs cannot tell the "
              "two apart there.", ""]
    report += [f"- line {mutant.line}, `{mutant.before}` to `{mutant.after or '(none)'}`: "
               f"`{lines[mutant.line - 1].strip()}`"
               for mutant, said, _ in results if said == "survived"]
    # A header's keeps its suffix, apart from its .cpp's: Bench.h.md beside Bench.md.
    out = runner.OUT / "mutants" / f"{source.name.removesuffix('.cpp')}.md"
    runner.save(out, "\n".join(report) + "\n")
    print(f"{counts['caught']} caught, {counts['survived']} survived, "
          f"{counts['builds not']} not built, {counts['not compiled here']} not compiled "
          f"here; {out.relative_to(runner.ROOT).as_posix()}")
    return 0
