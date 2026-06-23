#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import multiprocessing
import sys
from cmark import CMark

COUNT = 20000
TIMEOUT = 5

pathological = {
    "many unclosed directive labels": ":x[" * COUNT,
    "many unclosed directive attributes": ":x{" * COUNT,
    "many colon pairs": "::" * (COUNT * 2),
}

valid = {
    "long valid directive label": (
        ":long[" + ("a" * 1500) + "]",
        '<p><span data-directive="long">' + ("a" * 1500) + "</span></p>\n",
    ),
    "long valid directive attributes": (
        ":long{data-x=" + ("a" * 5000) + "}",
        '<p><span data-directive="long" data-x="' + ("a" * 5000) + '"></span></p>\n',
    ),
}


def run_test(program, inp):
    cmark = CMark(prog=program)
    rc, actual, err = cmark.to_html(inp)
    if rc != 0:
        print("[ERRORED (return code %d)]" % rc)
        print(err)
        sys.exit(1)

    expected = "<p>" + inp + "</p>\n"
    if actual != expected:
        print("[FAILED (mismatch)]")
        print(repr(actual[:200]))
        sys.exit(1)

    print("[PASSED]")


def run_expected_test(program, inp, expected):
    cmark = CMark(prog=program)
    rc, actual, err = cmark.to_html(inp)
    if rc != 0:
        print("[ERRORED (return code %d)]" % rc)
        print(err)
        sys.exit(1)

    if actual != expected:
        print("[FAILED (mismatch)]")
        print(repr(actual[:200]))
        sys.exit(1)

    print("[PASSED]")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Run directive pathological performance tests."
    )
    parser.add_argument("--program", dest="program", nargs="?", default=None)
    args = parser.parse_args(sys.argv[1:])

    passed = 0
    errored = 0

    print("Testing directive pathological cases:")
    for description, inp in pathological.items():
        print(description, "... ", end="")
        sys.stdout.flush()

        p = multiprocessing.Process(target=run_test, args=(args.program, inp))
        p.start()
        p.join(TIMEOUT)

        if p.is_alive():
            p.terminate()
            p.join()
            print("[TIMED OUT]")
            errored += 1
        elif p.exitcode != 0:
            errored += 1
        else:
            passed += 1

    print("Testing directive long valid cases:")
    for description, (inp, expected) in valid.items():
        print(description, "... ", end="")
        sys.stdout.flush()

        p = multiprocessing.Process(
            target=run_expected_test, args=(args.program, inp, expected)
        )
        p.start()
        p.join(TIMEOUT)

        if p.is_alive():
            p.terminate()
            p.join()
            print("[TIMED OUT]")
            errored += 1
        elif p.exitcode != 0:
            errored += 1
        else:
            passed += 1

    print("%d passed, %d errored" % (passed, errored))
    sys.exit(errored)
