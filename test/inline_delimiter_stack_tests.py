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

    long_formula = "$" + ("x" * LONG_BODY) + "$"
    long_formula_result = run_cmark(args.program, "formula", long_formula)
    require(long_formula_result.returncode == 0, long_formula_result.stderr)
    require(
        '<span class="formula formula-inline">' in long_formula_result.stdout,
        "expected long dollar formula to parse",
    )

    long_backslash_formula = "\\\\(" + ("x" * LONG_BODY) + "\\\\)"
    long_backslash_formula_result = run_cmark(
        args.program, "formula", long_backslash_formula
    )
    require(
        long_backslash_formula_result.returncode == 0,
        long_backslash_formula_result.stderr,
    )
    require(
        '<span class="formula formula-inline">' in long_backslash_formula_result.stdout,
        "expected long backslash formula to parse",
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

    formula_pathological = " ".join(["$`x"] * 20000)
    formula_pathological_result = run_cmark(args.program, "formula", formula_pathological)
    require(formula_pathological_result.returncode == 0, formula_pathological_result.stderr)

    backslash_pathological = " ".join(["\\\\(x"] * 20000)
    backslash_pathological_result = run_cmark(
        args.program, "formula", backslash_pathological
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

    annotation_pathological = " ".join(["<Person>"] * 20000)
    annotation_pathological_result = run_cmark(
        args.program, "ms_copilot_annotation", annotation_pathological
    )
    require(
        annotation_pathological_result.returncode == 0,
        annotation_pathological_result.stderr,
    )


if __name__ == "__main__":
    main()
