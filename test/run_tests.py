#!/usr/bin/env python3
"""Golden test runner for NoobScript.

Usage: run_tests.py <noob-binary> <cases-dir>

Each case is a .noob file with expectations in `btw` comments:

    btw OUT: <text>     one expected line of stdout, in order (exact match)
    btw ERR: <text>     text that must appear somewhere in stderr
    btw NOERR:          stderr must be empty
    btw EXIT: <n>       expected exit code (default 0)
    btw STDIN: <text>   one line fed to stdin
    btw ARGS: <flags>   flags passed before the script name
"""

import pathlib
import re
import subprocess
import sys

DIRECTIVE = re.compile(r"btw (OUT|ERR|NOERR|EXIT|STDIN|ARGS):(?: (.*))?$")


def parse_case(path):
    case = {"out": [], "err": [], "noerr": False, "exit": 0, "stdin": [], "args": []}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = DIRECTIVE.search(line)
        if not match:
            continue
        kind, value = match.group(1), match.group(2) or ""
        if kind == "OUT":
            case["out"].append(value)
        elif kind == "ERR":
            case["err"].append(value)
        elif kind == "NOERR":
            case["noerr"] = True
        elif kind == "EXIT":
            case["exit"] = int(value)
        elif kind == "STDIN":
            case["stdin"].append(value)
        elif kind == "ARGS":
            case["args"] = value.split()
    return case


def run_case(binary, path):
    case = parse_case(path)
    stdin = "".join(line + "\n" for line in case["stdin"])
    try:
        result = subprocess.run(
            [binary, *case["args"], path.name],
            cwd=path.parent,
            input=stdin.encode(),
            capture_output=True,
            timeout=30,
        )
    except subprocess.TimeoutExpired:
        return ["timed out"]

    stdout = result.stdout.decode(errors="replace")
    stderr = result.stderr.decode(errors="replace")
    expected = "".join(line + "\n" for line in case["out"])

    problems = []
    if result.returncode != case["exit"]:
        problems.append(f"exit code {result.returncode}, wanted {case['exit']}")
    if stdout != expected:
        problems.append(f"stdout was:\n{stdout}\nwanted:\n{expected}")
    for needle in case["err"]:
        if needle not in stderr:
            problems.append(f"stderr is missing {needle!r}")
    if case["noerr"] and stderr:
        problems.append("stderr should be empty")
    if problems:
        problems.append(f"full stderr:\n{stderr}")
    return problems


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    cases = sorted(pathlib.Path(sys.argv[2]).glob("*.noob"))
    failed = 0
    for path in cases:
        problems = run_case(binary, path)
        if problems:
            failed += 1
            print(f"FAIL {path.name}")
            for problem in problems:
                print(f"  {problem}")
        else:
            print(f"ok   {path.name}")
    print(f"{len(cases) - failed}/{len(cases)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
