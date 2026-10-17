#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation check for the upload pipeline tests (make mutate).

Each mutant breaks one rule of upsplit.c.inc. The unmodified sources must
pass tests/up-pipe.c and tests/up-split.c; every mutant must fail at least
one of them (assertion, crash or timeout).
"""
import os
import shutil
import subprocess
import sys
import tempfile

SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = ["up-pipe", "up-split"]

MUTANTS = [
    ("M1 no back-pressure: core 0 ignores the consumer tail",
     "\troom = UP_ENTRIES - (head - tail);\n",
     "\troom = UP_ENTRIES + 0 * (head - tail);\n"),
    ("M2 head published before the records (no fence)",
     "\t\t\t/* The records are visible before the head covering them. */\n"
     "\t\t\tbarrier();\n",
     ""),
    ("M3 slot handed back before core 1 used the record",
     "\trec = up_slot(u->tail);\n",
     "\trec = up_slot(u->tail);\n\tSPLIT_ST(&s[US_TAIL], u->tail + 1);\n"),
    ("M4 q[5]=0 before core 0 stopped",
     "\t\tif (up_quiet()) {\n",
     "\t\tif (up_quiet() || 1) {\n"),
    ("M5 queued records survive an RRO stop/restart",
     "\t\ts[US_DROPPED] += head - tail;\n\t\tSPLIT_ST(&s[US_TAIL], head);\n",
     "\t\ts[US_DROPPED] += 0 * (head - tail);\n", 2),
    ("M6 no key generation recheck on core 1",
     "\t\t\t\tif ((s[US_EPOCH] ^ (mac_high >> 16)) & 65535) {\n",
     "\t\t\t\tif (0 && ((s[US_EPOCH] ^ (mac_high >> 16)) & 65535)) {\n"),
    ("M7 core 1 trusts core 0's identity verdict",
     "\t\tif (!eligible || (u->r.st1 & 255) != ((class >> 24) & 3) ||\n"
     "\t\t    u->r.st2 != SPLIT_LD(&rec[UR_MAC_LOW]) ||\n"
     "\t\t    u->r.st3 != (mac_high & 65535)) {\n",
     "\t\tif (!eligible) {\n"),
    ("M8 retried record counted again",
     "\t\t\tif (!retry)\n\t\t\t\tq[27]++;\n",
     "\t\t\t(void)retry;\n\t\t\tq[27]++;\n"),
    ("M9 error record treated as a frame",
     "\tif (class & UR_ERROR) {\n",
     "\tif (0 && (class & UR_ERROR)) {\n"),
    ("M10 buffer state not checked by core 1",
     "\tif ((state & 255) != 1) {\n",
     "\tif (0 && (state & 255) != 1) {\n"),
]


def run_tests(directory):
    """Return {test: failure reason or None} for every test."""
    results = {}
    for test in TESTS:
        build = subprocess.run(["make", "-s", "build/test-" + test],
                               cwd=directory, capture_output=True, text=True)
        if build.returncode:
            results[test] = "build: " + build.stderr.strip().splitlines()[-1]
            continue
        try:
            result = subprocess.run(["./build/test-" + test], cwd=directory,
                                    capture_output=True, text=True,
                                    timeout=120)
        except subprocess.TimeoutExpired:
            results[test] = "timeout"
            continue
        reason = None
        if result.returncode:
            reason = (result.stderr.strip().splitlines() or ["?"])[-1]
            reason = reason.split("Assertion ")[-1][:90]
        results[test] = reason
    return results


def main():
    work = tempfile.mkdtemp(prefix="upsplit-mutate-")
    try:
        base = os.path.join(work, "base")
        shutil.copytree(SRC, base, ignore=shutil.ignore_patterns("build"))
        failures = {t: r for t, r in run_tests(base).items() if r}
        if failures:
            print("FAIL: unmodified sources fail:", failures)
            return 1
        survivors = 0
        for mutant in MUTANTS:
            name, old, new = mutant[:3]
            count = mutant[3] if len(mutant) > 3 else 1
            directory = os.path.join(work, name.split()[0])
            shutil.copytree(base, directory,
                            ignore=shutil.ignore_patterns("build"))
            path = os.path.join(directory, "upsplit.c.inc")
            text = open(path).read()
            if text.count(old) != count:
                print("FAIL: %s: pattern found %d times" % (name, text.count(old)))
                return 1
            open(path, "w").write(text.replace(old, new))
            failures = {t: r for t, r in run_tests(directory).items() if r}
            if failures:
                print("killed   %s" % name)
                for test, reason in failures.items():
                    print("           %-9s %s" % (test, reason))
            else:
                print("SURVIVED %s" % name)
                survivors += 1
        if survivors:
            print("FAIL: %d mutant(s) survived" % survivors)
            return 1
        print("PASS: all %d upload pipeline mutants killed" % len(MUTANTS))
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
