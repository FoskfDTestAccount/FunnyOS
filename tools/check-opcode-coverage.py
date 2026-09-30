#!/usr/bin/env python3
"""Report which 8086 opcodes no task document mentions.

    check-opcode-coverage.py [docs/tasks]

Scans the M3 task assignments for hexadecimal byte references -- single
bytes and ranges -- and reports which of the 256 opcode slots appear
nowhere, minus the ones the assignments acknowledge on purpose.

WHAT THIS IS AND IS NOT

It is a smoke detector, not a proof. The documents contain plenty of hex
that is not an opcode: interrupt vector numbers, byte counts, register
values in examples. So a byte being *mentioned* proves nothing, and this
script never claims coverage.

A byte being mentioned *nowhere*, though, is a real signal. The 8086
opcode map is irregular -- the segments that do different things are not
contiguous, and it is entirely possible to partition it between five
people and leave a hole nobody notices. That is exactly what happened
when these assignments were written: LES and LDS live at C4 and C5, in a
gap between two ranges that were both assigned, and LEA lives at 8D,
between two more. Neither was missed until this was run.

THE ACKNOWLEDGED LIST

The task overview has a table of bytes that are deliberately unassigned,
with the reason for each -- prefixes the run loop consumes, encodings
that are not instructions on this processor, and the 8087 block that
belongs to a later milestone. This script reads that table and subtracts
it, so the exit status means something:

    0   every unmentioned byte is one the documents account for
    1   something is unmentioned and unexplained -- go and look at it

The table is parsed rather than duplicated into a file here, because the
document has to explain *why* each byte is unassigned and a second copy
of the list would drift from it. If the section cannot be found, nothing
is treated as acknowledged and everything is reported: a check that
cannot read its own exceptions should be loud, not quiet.
"""

import re
import sys
from pathlib import Path

# Two hex digits, standing alone as a word. The documents write opcodes in
# backticks, in tables, and inside ranges; this catches all three and
# accepts that it will also catch other numbers.
SINGLE = re.compile(r"\b([0-9A-F]{2})\b")

# A range, written 80-83 or 70-7F or `E4`-`E7`: two hex bytes joined by a
# hyphen. The documents write them all three ways, so the backticks are
# optional on each side.
RANGE = re.compile(r"`?\b([0-9A-F]{2})\b`?-`?\b([0-9A-F]{2})\b`?")

# The section of the overview that lists bytes nobody is meant to claim.
ACKNOWLEDGED_HEADING = "### 没有任何任务认领的操作码"
ACKNOWLEDGED_END = "**一张覆盖检查表**"


def opcodes_in(text):
    """Every opcode byte the text could plausibly be naming."""
    found = set()

    for match in RANGE.finditer(text):
        low = int(match.group(1), 16)
        high = int(match.group(2), 16)
        # Only expand things that look like a range of opcodes. A pair
        # like "00-0F" is one; a pair like "1A-2B" spanning a hundred
        # values is more likely two unrelated numbers.
        if 0 <= low <= high <= 0xFF and high - low <= 0x40:
            found.update(range(low, high + 1))

    for match in SINGLE.finditer(text):
        found.add(int(match.group(1), 16))

    return found


def acknowledged_in(overview):
    """The bytes the overview says are deliberately unassigned."""
    if not overview.exists():
        return set(), False

    text = overview.read_text(encoding="utf-8")

    start = text.find(ACKNOWLEDGED_HEADING)
    if start < 0:
        return set(), False

    end = text.find(ACKNOWLEDGED_END, start)
    section = text[start:end if end > 0 else len(text)]

    return opcodes_in(section), True


def main():
    directory = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/tasks")

    if not directory.is_dir():
        print(f"not a directory: {directory}", file=sys.stderr)
        return 2

    documents = sorted(directory.glob("M3-*.md"))

    if not documents:
        print(f"no task documents in {directory}", file=sys.stderr)
        return 2

    # Which document names each opcode, so a gap can be chased.
    mentions = {op: [] for op in range(0x100)}

    for document in documents:
        text = document.read_text(encoding="utf-8")
        for op in opcodes_in(text):
            mentions[op].append(document.name)

    acknowledged, list_found = acknowledged_in(directory / "README.md")

    missing = [op for op in range(0x100)
               if not mentions[op] and op not in acknowledged]

    print(f"Scanned {len(documents)} document(s) in {directory}")

    if not list_found:
        print("WARNING: the acknowledged-unassigned table was not found in")
        print("         README.md, so nothing was excluded. Everything")
        print("         unmentioned is reported below.")
    else:
        print(f"{len(acknowledged)} byte(s) acknowledged as unassigned")

    print()

    if not missing:
        print("Every unmentioned opcode is one the documents account for.")
        return 0

    print(f"{len(missing)} opcode slot(s) unmentioned and unexplained:")
    print()

    for start in range(0, len(missing), 8):
        row = missing[start:start + 8]
        print("    " + "  ".join(f"{op:02X}" for op in row))

    print()
    print("Each one needs an answer: claimed by somebody, not an instruction")
    print("on this processor, or missed. If it is deliberately unassigned,")
    print("add it to the table in docs/tasks/README.md with the reason.")

    return 1


if __name__ == "__main__":
    sys.exit(main())
