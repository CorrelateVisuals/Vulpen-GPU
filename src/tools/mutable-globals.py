#!/usr/bin/env python3
"""Fail when code built from src/ holds a mutable global (C13): every writable object in
an object file, from its symbol table. Vendored code is ours to build, not to change
(RA02), so its few globals are listed by name.

State has an owner and reaches code through parameters. A global also outlives a live
swap in one module and resets in another, and it is shared by every thread.

Every build runs it after the link, from the compile commands CMake exports.

Usage: python3 src/tools/mutable-globals.py BUILD_DIR
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src"
VENDORED = SOURCE / "external-libraries"
THREAD_LOCAL = (".tdata", ".tbss")
WRITABLE = (".data", ".bss", *THREAD_LOCAL)
# Relocated once at load, then read-only.
NOT_MUTABLE = (".data.rel.ro",)
# The compiler's own: pointers for unwinding, and AddressSanitizer's markers.
GENERATED = ("DW.ref.", "__odr_asan")
VISIBILITY = (".hidden ", ".protected ", ".internal ")
ALLOWED = {
    "VmaBufferImageUsage::UNKNOWN",  # vk_mem_alloc.h, compiled into Resources.cpp
}


def writable(object_file: Path) -> list[str]:
    table = subprocess.run(["objdump", "-t", "-C", object_file],
                           capture_output=True, text=True, check=True).stdout
    names = []
    for line in table.splitlines():
        # value, flags and section, a tab, then size, visibility and name. The flags hold
        # an O for an object, but not for a thread-local one, and a d for a section.
        head, _, tail = line.partition("\t")
        fields = head.split()
        if len(fields) < 3 or len(tail.split()) < 2:
            continue
        flags, section = fields[1:-1], fields[-1]
        if "d" in flags or ("O" not in flags and not section.startswith(THREAD_LOCAL)):
            continue
        name = tail.split(maxsplit=1)[1]
        if name.startswith(VISIBILITY):
            name = name.split(maxsplit=1)[1]
        if (section.startswith(WRITABLE) and not section.startswith(NOT_MUTABLE)
                and not name.startswith(GENERATED) and name not in ALLOWED):
            names.append(f"{name} in {section}")
    return names


def main() -> None:
    build = Path(sys.argv[1])
    entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    found = []
    for entry in entries:
        source = Path(entry["file"]).resolve()
        if SOURCE in source.parents and VENDORED not in source.parents:
            name = source.relative_to(SOURCE).as_posix()
            found += [f"{name}: {symbol}"
                      for symbol in writable(Path(entry["directory"]) / entry["output"])]
    if found:
        sys.exit("mutable globals (C13):\n  " + "\n  ".join(found))


if __name__ == "__main__":
    main()
