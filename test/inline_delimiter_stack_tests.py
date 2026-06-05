#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import shlex
import subprocess
import sys


LONG_BODY = 5000


def run_cmark(program, extension, markdown, timeout=2.0):
    command = shlex.split(program) + ["-e", extension]
    try:
        return subprocess.run(
            command,
            input=markdown,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        sys.stderr.write(
            "{} timed out on {} bytes of input\n".format(extension, len(markdown))
        )
        sys.exit(1)


def require(condition, message):
    if not condition:
        sys.stderr.write(message + "\n")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", required=True)
    args = parser.parse_args()

    long_math = "$" + ("x" * LONG_BODY) + "$"
    long_math_result = run_cmark(args.program, "math", long_math)
    require(long_math_result.returncode == 0, long_math_result.stderr)
    require(
        '<span class="math math-inline">' in long_math_result.stdout,
        "expected long dollar math to parse",
    )

    long_backslash_math = "\\\\(" + ("x" * LONG_BODY) + "\\\\)"
    long_backslash_math_result = run_cmark(
        args.program, "math", long_backslash_math
    )
    require(
        long_backslash_math_result.returncode == 0,
        long_backslash_math_result.stderr,
    )
    require(
        '<span class="math math-inline">' in long_backslash_math_result.stdout,
        "expected long backslash math to parse",
    )

    long_citation = "【" + ("x" * LONG_BODY) + "】"
    long_citation_result = run_cmark(
        args.program, "ms_copilot_citation", long_citation
    )
    require(long_citation_result.returncode == 0, long_citation_result.stderr)
    require(
        '<span class="ms-copilot-citation"' in long_citation_result.stdout,
        "expected long citation to parse",
    )

    math_pathological = " ".join(["$`x"] * 20000)
    math_pathological_result = run_cmark(args.program, "math", math_pathological)
    require(math_pathological_result.returncode == 0, math_pathological_result.stderr)

    backslash_pathological = " ".join(["\\\\(x"] * 20000)
    backslash_pathological_result = run_cmark(
        args.program, "math", backslash_pathological
    )
    require(
        backslash_pathological_result.returncode == 0,
        backslash_pathological_result.stderr,
    )

    citation_pathological = " ".join(["【x"] * 20000)
    citation_pathological_result = run_cmark(
        args.program, "ms_copilot_citation", citation_pathological
    )
    require(
        citation_pathological_result.returncode == 0,
        citation_pathological_result.stderr,
    )


if __name__ == "__main__":
    main()
