#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import shlex
import subprocess
import sys


def run_cmark(program, markdown, output_format):
    command = shlex.split(program) + [
        "-e",
        "ms_copilot_annotation",
        "-t",
        output_format,
    ]
    return subprocess.run(
        command,
        input=markdown,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding="utf-8",
        timeout=2.0,
        check=False,
    )


def require(condition, message):
    if not condition:
        sys.stderr.write(message + "\n")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", required=True)
    args = parser.parse_args()

    markdown = "Meet <Person>**Aman** Kumar</Person> today.\n"

    commonmark = run_cmark(args.program, markdown, "commonmark")
    require(commonmark.returncode == 0, commonmark.stderr)
    require(
        "<Person>**Aman** Kumar</Person>" in commonmark.stdout,
        "expected commonmark renderer to preserve annotation content",
    )

    plaintext = run_cmark(args.program, markdown, "plaintext")
    require(plaintext.returncode == 0, plaintext.stderr)
    require(
        "Meet Aman Kumar today." in plaintext.stdout,
        "expected plaintext renderer to preserve annotation text",
    )

    latex = run_cmark(args.program, markdown, "latex")
    require(latex.returncode == 0, latex.stderr)
    require(
        "Meet \\textbf{Aman} Kumar today." in latex.stdout,
        "expected latex renderer to preserve annotation children",
    )


if __name__ == "__main__":
    main()
