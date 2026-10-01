#!/usr/bin/env python3
"""Check that each W4 injection goes red *saying the right thing*.

    python3 tools/inject-int21.py
    python3 tools/inject-int21.py version      # one, by substring

------------------------------------------------------------------------
Why this is not tools/verify-mutations.py

That tool answers "does any case notice this defect at all", which is the
right question for a campaign over a whole layer: it is mechanical, it
runs every suite, and it says which cases went red.

This answers a narrower question, and one that tool deliberately does not
ask: *which* assertion caught it, and whether the sentence it printed
names the fault that was injected. That matters because the two failures
are not alike. An injection that goes red pointing at a different file
costs more time than one that stays green -- it sends the reader
somewhere the bug is not, and it does so carrying the authority of a
passing check.

So the table of defects is not duplicated here; it comes from
verify-mutations.py's M5_MUTATIONS, which is where the campaign reads it.
What is here and not there is the expected diagnosis, one per mutation,
and every mutation in that table must have one or this exits without
running anything. A new mutation added there without a sentence here is
an injection nobody has said the meaning of.
"""

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import atexit
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
DEST = tempfile.mkdtemp(prefix="funyos-inject-", dir="/var/tmp")
atexit.register(shutil.rmtree, DEST, ignore_errors=True)

# What each injection must be caught by, keyed by the mutation's name in
# verify-mutations.py. The value is a fragment of the failure line: the
# thing the checker said, not the thing it should have said.
EXPECTED = {
    "the version halves swapped":
        "AL, the major version",
    "09h prints the terminator too":
        "the terminator was not printed",
    "the string walk never stops on unmapped memory":
        "and nothing was printed",
    "a successful call leaves the carry alone":
        "CF after AH=09h",
    "25h writes the address the wrong way round":
        "the offset in the table",
    "35h hands back the offset in ES":
        "ES, the stub's segment",
    "1Ah keeps a linear address":
        "ES, the segment given",
    "the default DTA ignores the PSP segment":
        "BX",
    "0Eh ignores its argument":
        "AX, the error code",
    "19h always answers with drive A:":
        "drive       = 05",
    "an unknown function returns an error code in AX":
        "AX",
    "an error sets the carry but no code":
        "select A:   = 000F CF=1",
    "4Ch exits with zero instead of AL":
        "the exit code",
    "INT 20h exits with a code of its own":
        "INT 20h exits with zero",
    "the trap does not report the exit":
        "the stop",
    "the trap never writes a service's flags back":
        "CF, after the stub's IRET",
    "the loader points every segment but CS at zero":
        "DS: expected 0x1000, got 0x0000",
}


def load_mutations():
    """The campaign's own table, so there is one place defects are written."""
    path = os.path.join(HERE, "verify-mutations.py")
    spec = importlib.util.spec_from_file_location("verify_mutations", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.M5_MUTATIONS


def prepare():
    if os.path.exists(DEST):
        shutil.rmtree(DEST)
    os.makedirs(DEST)

    for part in ("dos", "libk", "tools"):
        shutil.copytree(os.path.join(SRC, part), os.path.join(DEST, part))


def run(name, path, old, new, expected):
    prepare()

    target = os.path.join(DEST, "dos", path)
    text = open(target, encoding="utf-8").read()

    if text.count(old) != 1:
        return "PATCH", "the anchor appears %d time(s)" % text.count(old)

    open(target, "w", encoding="utf-8").write(text.replace(old, new))

    # Every suite, not just this layer's.
    #
    # A mutation in dos/psp.c is caught by test_dos and not by test_int21,
    # and one in the trap is caught by both -- so running only the suite the
    # mutation was written for would report a real catch as GREEN. The
    # question this script asks is "which assertion noticed", and the answer
    # is allowed to come from another file.
    #
    # The dos/Makefile prints "====> 8086 tests FAILED" only when the
    # suites ran and at least one failed. Its absence with a nonzero exit
    # is a compile error, and that is counted as its own outcome rather than
    # as a catch: -Werror turns some of these into build failures, and
    # counting the compiler as the checker is how a campaign reports
    # covering something that nothing tests.
    #
    # The decode is lenient on purpose. A mutation that sends a guest's
    # pointer somewhere it should not go makes it print whatever is there,
    # and a failure message quoting that memory is not valid UTF-8 -- so a
    # strict decode turns a caught injection into a traceback, which reads
    # like the harness breaking rather than like the assertion working.
    try:
        result = subprocess.run(["make", "-B", "test"],
                                cwd=os.path.join(DEST, "dos"),
                                capture_output=True, text=True,
                                errors="replace", timeout=1800)
    except subprocess.TimeoutExpired:
        return "HANG", "the suite never finished"

    output = result.stdout + result.stderr

    if result.returncode == 0:
        return "GREEN", "no assertion noticed it"

    if "====> 8086 tests FAILED" not in output:
        return "BUILD", "it did not compile, so nothing was tested"

    if expected not in output:
        said = [l.strip() for l in output.splitlines() if "FAIL" in l]
        return "WRONG", "it said %r, not %r" % (
            said[0] if said else "(nothing)", expected)

    return "caught", expected


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None

    mutations = load_mutations()

    missing = [m[0] for m in mutations if m[0] not in EXPECTED]
    if missing:
        print("these mutations have no expected diagnosis here:")
        for name in missing:
            print("   ", name)
        return 2

    print("%-52s %-8s %s" % ("injection", "result", "the line it failed on"))
    print("-" * 110)

    bad = 0
    ran = 0

    for name, path, old, new in mutations:
        if only and only not in name:
            continue

        ran += 1
        result, detail = run(name, path, old, new, EXPECTED[name])

        if result != "caught":
            bad += 1

        print("%-52s %-8s %s" % (name, result, detail))

    print("-" * 110)
    print("%d of %d caught, each with the diagnosis it aimed at"
          % (ran - bad, ran))

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
