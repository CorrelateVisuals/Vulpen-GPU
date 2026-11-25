#!/usr/bin/env python3
"""Fail when a type node code compiles against changes its layout but by addition.

A node's module is built against runtime/Operator.h, and a dev build swaps modules into
a vulpen built before them, as a running vulpen loads a module built after an engine
change. So each class there keeps its virtual functions in the order it declares them,
a new one last, as their comments say ("Last, so a module built before…"), and each
struct its fields, offsets and size, which a module indexes an array of by. A change
that breaks either is refused naming it; one meant, with every module built again, is
taken in with --update, whose diff to port-layout.txt a person reads in the commit
(A00).

It reads the code with libclang (python3-clang); where that is not installed, it says
so and checks nothing, as CI's Linux job installs it.

Usage: python3 src/tools/gates/port-layout.py [BUILD_DIR] [--update]
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src"
HEADER = "runtime/Operator.h"
LAYOUT = Path(__file__).resolve().parent / "port-layout.txt"
HEAD = ("# The layout of each type runtime/Operator.h declares, as src/tools/gates/"
        "port-layout.py\n# reads it; written by its --update, compared at every build.\n")


def system_includes() -> list[str]:
    """The compiler's include folders, which libclang does not find by itself, but GCC's
    own, whose intrinsics clang's own headers stand in for."""
    said = subprocess.run(["g++", "-std=c++20", "-E", "-x", "c++", "-", "-v"], input="",
                          capture_output=True, text=True).stderr
    block = said.split("#include <...> search starts here:")[1].split("End of search list.")[0]
    return [line.strip() for line in block.splitlines()
            if line.strip() and "/lib/gcc/" not in line]


def layouts() -> dict[str, list[str]]:
    """Each class and struct Operator.h declares in namespace VP, by its name: its size,
    then its virtual functions in their order, or its fields with their offsets."""
    import clang.cindex as ci
    arguments = ["-x", "c++", "-std=c++20", f"-I{SOURCE}",
                 f"-isystem{SOURCE / 'external-libraries'}", "-Wno-everything",
                 *[f"-isystem{folder}" for folder in system_includes()]]
    unit = ci.Index.create().parse("ports.cpp", args=arguments, unsaved_files=[
        ("ports.cpp", f'#include "{HEADER}"\n')])
    errors = [str(found) for found in unit.diagnostics if found.severity >= 3]
    if errors:
        sys.exit("port-layout: " + "\n  ".join(errors))
    kinds = (ci.CursorKind.CLASS_DECL, ci.CursorKind.STRUCT_DECL)
    found: dict[str, list[str]] = {}

    def kind(cursor):
        try:
            return cursor.kind
        except ValueError:  # one these bindings are older than
            return None

    def visit(cursor, scope: str) -> None:
        for child in cursor.get_children():
            if kind(child) == ci.CursorKind.NAMESPACE and child.spelling == "VP" and not scope:
                visit(child, "VP")
            elif (scope and kind(child) in kinds and child.is_definition()
                  and child.location.file and child.location.file.name.endswith(HEADER)):
                name = f"{scope}::{child.spelling}"
                entries = [f"size {child.type.get_size()} align {child.type.get_align()}"]
                for base in child.get_children():
                    if kind(base) == ci.CursorKind.CXX_BASE_SPECIFIER:
                        entries.append(f"base {base.type.spelling}")
                for member in child.get_children():
                    if kind(member) == ci.CursorKind.CXX_METHOD and member.is_virtual_method():
                        const = " const" if member.is_const_method() else ""
                        entries.append(f"virtual {member.result_type.spelling} "
                                       f"{member.displayname}{const}")
                    elif kind(member) == ci.CursorKind.FIELD_DECL:
                        entries.append(f"field {member.type.spelling} {member.spelling} "
                                       f"at {child.type.get_offset(member.spelling) // 8}")
                found[name] = entries
                visit(child, name)
    visit(unit.cursor, "")
    return found


def text_of(found: dict[str, list[str]]) -> str:
    return HEAD + "".join(f"\n{name}\n" + "".join(f"  {entry}\n" for entry in entries)
                          for name, entries in sorted(found.items()))


def kept(text: str) -> dict[str, list[str]]:
    found, name = {}, None
    for line in text.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        if not line.startswith(" "):
            name = line
            found[name] = []
        elif name:
            found[name].append(line.strip())
    return found


def problems(before: dict[str, list[str]], now: dict[str, list[str]]) -> list[str]:
    """A class with virtual functions may gain more after its own; anything else that
    moves is refused."""
    found = []
    for name, entries in before.items():
        if name not in now:
            found.append(f"{name} is gone")
            continue
        virtual = any(entry.startswith("virtual") for entry in entries)
        current = now[name]
        if virtual:
            same = [entry for entry in current if not entry.startswith("size")]
            old = [entry for entry in entries if not entry.startswith("size")]
            for index, entry in enumerate(old):
                if index >= len(same) or same[index] != entry:
                    found.append(f"{name}: {entry} is now "
                                 f"{same[index] if index < len(same) else 'gone'}; a module "
                                 "built before it would call another function: add a "
                                 "virtual function last")
                    break
        elif current != entries:
            changed = ([f"{entry} -> {other}" for entry, other in zip(entries, current)
                        if entry != other]
                       + [f"{entry} added" for entry in current[len(entries):]]
                       + [f"{entry} gone" for entry in entries[len(current):]])
            found.append(f"{name}: {'; '.join(changed)}; a module built before it reads "
                         "another layout")
    return found


def main() -> None:
    if "--view" in sys.argv:  # a view's build changes no engine header
        return
    try:
        import clang.cindex  # noqa: F401 (whether it is installed)
    except ImportError:
        print("port-layout: libclang for Python (python3-clang) is not installed, so the "
              "ports' layout goes unchecked here")
        return
    now = layouts()
    if "--update" in sys.argv:
        temporary = LAYOUT.with_name(LAYOUT.name + ".tmp")
        temporary.write_text(text_of(now), encoding="utf-8", newline="\n")
        temporary.replace(LAYOUT)
        print(f"wrote {LAYOUT.relative_to(ROOT).as_posix()}")
        return
    if not LAYOUT.is_file():
        sys.exit(f"port-layout: no {LAYOUT.name} yet; write it with --update")
    found = problems(kept(LAYOUT.read_text(encoding="utf-8")), now)
    if found:
        sys.exit("port-layout (src/tools/gates/port-layout.py --update takes in a change "
                 "made on purpose, with every module built again):\n  " + "\n  ".join(found))


if __name__ == "__main__":
    main()
