#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import shlex
import subprocess
import sys


MAX_DEPTH = 64


def nested_details(depth):
    parts = []
    for i in range(depth):
        parts.append("<details>\n")
        parts.append("<summary>Title {}</summary>\n".format(i + 1))
    parts.append("Body\n")
    for _ in range(depth):
        parts.append("</details>\n")
    return "".join(parts)


def render_xml(program, markdown):
    command = shlex.split(program) + [
        "-t",
        "xml",
        "-e",
        "ms_copilot_accordion",
    ]
    return subprocess.run(
        command,
        input=markdown,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding="utf-8",
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

    allowed = render_xml(args.program, nested_details(MAX_DEPTH))
    require(allowed.returncode == 0, allowed.stderr)
    require(
        allowed.stdout.count("<ms-copilot-accordion>") == MAX_DEPTH,
        "expected {} converted accordions at the depth limit, got {}".format(
            MAX_DEPTH, allowed.stdout.count("<ms-copilot-accordion>")
        ),
    )
    require(
        "<text xml:space=\"preserve\">Body</text>" in allowed.stdout,
        "expected body text in converted depth-limit input",
    )

    excessive = render_xml(args.program, nested_details(MAX_DEPTH + 1))
    require(excessive.returncode == 0, excessive.stderr)
    require(
        excessive.stdout.count("<ms-copilot-accordion>") == 0,
        "expected over-depth input to stay raw, got {} converted accordions".format(
            excessive.stdout.count("<ms-copilot-accordion>")
        ),
    )
    require(
        "&lt;details&gt;" in excessive.stdout,
        "expected raw details HTML in over-depth output",
    )


if __name__ == "__main__":
    main()
