#!/usr/bin/env python3
"""Command-line behaviour tests. Usage: test_cli.py <noob-binary>."""

import os
import pathlib
import subprocess
import sys
import tempfile

noob = str(pathlib.Path(sys.argv[1]).resolve())
failures = 0


def check(name, args, want_out, want_code=0, stdin="", want_err=None, env=None):
    global failures
    result = subprocess.run(
        [noob, *args], input=stdin.encode(), capture_output=True, timeout=20,
        env={**os.environ, **(env or {})},
    )
    out, err = result.stdout.decode(), result.stderr.decode()
    if out != want_out or result.returncode != want_code or (want_err and want_err not in err):
        failures += 1
        print(f"FAIL {name}: exit {result.returncode}, stdout {out!r}, stderr {err!r}")
    else:
        print(f"ok   {name}")


with tempfile.TemporaryDirectory() as tmp:
    script = pathlib.Path(tmp) / "s.noob"
    script.write_text('lowkey who = "file"\nyap who, cliargs\n')
    broken = pathlib.Path(tmp) / "bad.noob"
    broken.write_text("yap (\n")

    check("file", [str(script)], 'file []\n')
    check("file args", [str(script), "a", "--b"], 'file ["a", "--b"]\n')
    check("-c", ["-c", 'yap 1 + 1'], "2\n")
    check("-c args", ["-c", "yap cliargs", "x", "-y"], '["x", "-y"]\n')
    check("-c syntax error", ["-c", "yap ("], "", 2, want_err="<string>:1:")
    check("piped script", [], "piped\n", stdin='yap "piped"\n')
    check("dash", ["-", "z"], '["z"]\n', stdin="yap cliargs\n")
    check("-V", ["-V"], "NoobScript 0.2.0\n")
    check("--version", ["--version"], "NoobScript 0.2.0\n")
    check("--check", ["--check", str(script)], f"{script}: passed the vibe check, no syntax errors\n")
    check("--check piped", ["--check"], "<stdin>: passed the vibe check, no syntax errors\n", stdin="yap 1\n")
    check("--check bad", ["--check", str(broken)], "", 2)
    check("missing file", ["nope.noob"], "", 64)
    check("bad flag", ["--bogus"], "", 64)
    check("touchgrass code", ["-c", "touchgrass 9"], "", 9)

    banner = "NoobScript 0.2.0. touchgrass to leave.\n"
    def repl_out(body):
        return banner + body

    result = subprocess.run([noob, "-i"], input=b"1 + 1\nlowkey x = 3\nx\ntouchgrass 4\n", capture_output=True, timeout=20)
    text = result.stdout.decode()
    if result.returncode != 4 or "2\n" not in text or "3\n" not in text or banner not in text:
        failures += 1
        print(f"FAIL -i repl: {result.returncode} {text!r}")
    else:
        print("ok   -i repl")

    result = subprocess.run([noob, "-i", str(script)], input=b"who + '!'\nwho\n".replace(b"'", b'"'), capture_output=True, timeout=20)
    text = result.stdout.decode()
    if "file []\n" not in text or '"file!"' not in text or '"file"' not in text:
        failures += 1
        print(f"FAIL -i script: {text!r}")
    else:
        print("ok   -i keeps script variables")

    # Runtime configuration.
    deep = "cook d(n) { itsgiving d(n + 1) }\ntryna { sendit d(0)\nyap holdup sendit d(0) } caught4k e { yap e }\ntouchgrass\n"
    check("--stack-size small still safe", ["--stack-size=131072", "-c", deep], "ran out of stack evaluating this expression\n")
    check("--stack-size too small", ["--stack-size=4096", "-c", "yap 1"], "", 64, want_err="at least")
    check("env stack bad", ["-c", "yap 1"], "", 64, env={"NOOB_MIN_STACK": "lots"}, want_err="NOOB_MIN_STACK")
    check("env stack too small", ["-c", "yap 1"], "", 64, env={"NOOB_MIN_STACK": "10"})
    check("flag beats env", ["--stack-size=2097152", "-c", "yap 1"], "1\n", env={"NOOB_MIN_STACK": "10"})
    check("--worker-threads", ["--worker-threads=1", "-c", 'yap 2'], "2\n")
    check("--worker-threads 0", ["--worker-threads=0", "-c", "yap 2"], "", 64)

sys.exit(1 if failures else 0)
