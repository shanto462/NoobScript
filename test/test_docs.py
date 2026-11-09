#!/usr/bin/env python3
"""Keeps the docs honest. Usage: test_docs.py <noob-binary> <repo-root>.

Runs every ```noob block in README.md. Any output marked with `btw → text`
must match exactly, in order. Also runs the example programs.
"""

import pathlib
import re
import subprocess
import sys

BLOCK = re.compile(r"^```noob\n(.*?)^```$", re.M | re.S)
EXPECTED = re.compile(r"btw → (.*)$", re.M)
# Needs a person at the keyboard.
INTERACTIVE = {"guess.noob"}

noob = str(pathlib.Path(sys.argv[1]).resolve())
root = pathlib.Path(sys.argv[2]).resolve()
failures = 0


def run(args, cwd):
    return subprocess.run(
        [noob, *args], cwd=cwd, stdin=subprocess.DEVNULL, capture_output=True, timeout=30
    )


def report(name, problem):
    global failures
    if problem:
        failures += 1
        print(f"FAIL {name}: {problem}")
    else:
        print(f"ok   {name}")


readme = (root / "README.md").read_text(encoding="utf-8")
for match in BLOCK.finditer(readme):
    code = match.group(1)
    line = readme.count("\n", 0, match.start()) + 1
    # With -c, imports resolve against the working directory.
    result = run(["-c", code], root / "examples")
    want = "".join(text.rstrip() + "\n" for text in EXPECTED.findall(code))
    got = result.stdout.decode()
    problem = None
    if result.returncode != 0:
        problem = f"exit {result.returncode}: {result.stderr.decode()}"
    elif want and got != want:
        problem = f"stdout was:\n{got}\nwanted:\n{want}"
    report(f"README.md:{line}", problem)

for path in sorted((root / "examples").glob("*.noob")):
    flags = ["--check"] if path.name in INTERACTIVE else []
    result = run([*flags, str(path)], root)
    report(path.name, None if result.returncode == 0 else f"exit {result.returncode}: {result.stderr.decode()}")

sys.exit(1 if failures else 0)
