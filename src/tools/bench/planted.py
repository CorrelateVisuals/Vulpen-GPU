"""Faults planted for bench.py's guards, for bench.py planted: each guard is run on a
fault made for it, which it must fail on, so a guard that stopped failing shows. This is
the review run on itself, as the canaries of cases/Bench.cpp are for the contexts: a
guard that passes its fault is a plant missed, and the run fails.

  compare, as committed      Text against its records: passes, so the rest mean something
  compare, a row moved       one row of Text's record changed
  compare, a record missing  one section of Text's record gone
  compare, a function gone   a section no case runs
  compare, no device         lavapipe named, but at a path that is not there
  compare, no display        Xvfb installed, but no display started
  run, as committed          every context of the bench's own case: passes
  run, a context broken      each context in turn made the baseline's: its own canary misses
  approve, then compare      a record approved anew, then compared: passes; and one id
  cost, in a sandbox         Log's sweeps, which write files, measured, leaving none

Each runs against a copy of records/ and an out/ of its own, out/bench/planted/, with the
benches the debug build made; the tree's records and the last run's rows stay as they are.
"""
import argparse
import contextlib
import dataclasses
import io
import shutil

# The benches the plants run: Text, quick and pure; Bench, whose case holds the canaries;
# Mechanics, a GPU file; Swapchain, whose rows open a window; Log, whose sweeps write files.
BENCHES = ("Text", "Bench", "Mechanics", "Swapchain", "Log")


def ran(command, arguments: dict) -> tuple[int, str]:
    """A bench.py command's exit and what it printed, as check.py would see them; a
    command that stops with an error fails with it, as it would there."""
    said = io.StringIO()
    try:
        with contextlib.redirect_stdout(said):
            code = command(argparse.Namespace(**arguments))
    except SystemExit as stopped:
        code = stopped.code if isinstance(stopped.code, int) else 1
        said.write(str(stopped.code))
    except Exception as failed:  # noqa: BLE001 (a crash is the command failing)
        code = 1
        said.write(repr(failed))
    return code, said.getvalue()


def without_first_section(rows: str) -> str:
    first = rows.index("\n## ")
    return rows[:first] + rows[rows.index("\n## ", first + 1):]


def first_section(rows: str) -> str:
    first = rows.index("\n## ") + len("\n## ")
    return rows[first:rows.index("\n", first)]


def planted(arguments, runner) -> int:
    out, records = runner.OUT, runner.RECORDS
    plants = runner.OUT / "planted"
    copy = plants / "records"
    runner.build("debug", runner.benches(list(BENCHES)))
    results = []

    def judged(name: str, caught: bool, code: int, printed: str) -> None:
        results.append(caught)
        print(f"{'caught' if caught else 'MISSED'}: {name}"
              + ("" if caught else f" (exit {code}; {printed.strip()[-300:]!r})"), flush=True)

    def plant(name: str, fails: bool, said: str, command, given: dict) -> None:
        code, printed = ran(command, given)
        judged(name, (code != 0) == fails and said in printed, code, printed)

    def fresh() -> None:
        shutil.rmtree(plants, ignore_errors=True)
        shutil.copytree(records, copy)

    def edited(path, change) -> None:
        path.write_text(change(path.read_text(encoding="utf-8")), encoding="utf-8")

    @contextlib.contextmanager
    def replaced(name: str, stand_in):
        kept = getattr(runner, name)
        setattr(runner, name, stand_in)
        try:
            yield
        finally:
            setattr(runner, name, kept)

    @contextlib.contextmanager
    def no_display():
        yield None

    text = copy / "runtime" / "Text.rec"
    compared = {"benches": ["Text"], "preset": "debug"}
    try:
        runner.OUT, runner.RECORDS = plants, copy
        fresh()
        plant("compare, as committed", False, "all the same", runner.command_compare, compared)
        edited(text, lambda rows: rows.replace("\n(", "\n(moved ", 1))
        plant("compare, a row moved", True, "rows moved", runner.command_compare, compared)
        fresh()
        edited(text, without_first_section)
        plant("compare, a record missing", True, "no record yet", runner.command_compare,
              compared)
        fresh()
        edited(text, lambda rows: rows + "\n## Text.h/a function no case runs\nx\n")
        plant("compare, a function gone", True, "no case runs it", runner.command_compare,
              compared)
        fresh()
        with replaced("driver", lambda preset: "/a/driver/that/is/not/there.json"):
            plant("compare, no device", True, "did not run", runner.command_compare,
                  {"benches": ["Mechanics"], "preset": "debug"})
        with replaced("display", no_display):
            plant("compare, no display", True, "did not run", runner.command_compare,
                  {"benches": ["Swapchain"], "preset": "debug"})
        every = {"benches": ["Bench"], "contexts": [], "presets": []}
        plant("run, as committed", False, " 0 moved", runner.command_run, every)
        contexts = runner.CONTEXTS
        for context in contexts[1:]:
            broken = dataclasses.replace(context, bench="baseline", env=(), locale=None,
                                         aslr=False, elsewhere=False, home=False)
            with replaced("CONTEXTS", tuple(broken if each is context else each
                                            for each in contexts)):
                plant(f"run, {context.name} broken", True,
                      f"canary/{context.name}: the same in {context.name}",
                      runner.command_run,
                      {"benches": ["Bench"], "contexts": [context.name], "presets": []})
        fresh()
        text.unlink()
        plant("compare, no record at all", True, "no record yet", runner.command_compare,
              compared)
        ran(runner.command_approve, {"names": ["Text"], "why": "planted"})
        plant("approve, then compare", False, "all the same", runner.command_compare, compared)
        fresh()
        edited(text, without_first_section)
        ran(runner.command_compare, compared)
        ran(runner.command_approve, {"names": [first_section(
            (records / "runtime" / "Text.rec").read_text(encoding="utf-8"))], "why": "planted"})
        plant("approve an id, then compare", False, "all the same", runner.command_compare,
              compared)
        code, printed = ran(runner.command_cost, {"benches": ["Log"], "preset": "debug",
                                                  "base": None})
        left = [name for name in ("run.log", "folder.log") if (plants / name).exists()]
        judged("cost, in a sandbox", code == 0 and not left
               and "Log.cpp/Log::Log a folder for a file: {" in printed
               and (shutil.which("perf") is None or "'instructions': None" not in printed),
               code, printed + (f"; left {left}" if left else ""))
    finally:
        runner.OUT, runner.RECORDS = out, records
        shutil.rmtree(plants, ignore_errors=True)
    print(f"{sum(results)} of {len(results)} plants caught")
    return 0 if all(results) else 1
