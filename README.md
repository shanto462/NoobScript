# NoobScript

[![CI](https://github.com/shanto462/NoobScript/actions/workflows/ci.yml/badge.svg)](https://github.com/shanto462/NoobScript/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A scripting language with Gen Z slang for keywords. It syntax-checks your file, then interprets it one statement at a time, with no compilation step. Written in C++ on LLVM's support libraries.

```noob
cook flex(name) {
    itsgiving "${name} is him fr"
}

speedrun i in 1..3 {
    vibecheck i == 2 { skibidi }
    yap flex("noob"), i
}

tryna {
    yeet "it's so over"
} caught4k err {
    yap "we're so back: ${err}"
}
```

Also has squads (classes), modules, `sendit`/`holdup` async with group chats (channels), generators, pattern matching, destructuring, arrow functions, and more.

> **Status:** 0.x and still young. The language may change between minor versions, and only Apple silicon macOS is built and tested. See the [changelog](CHANGELOG.md).

## Requirements

- An Apple silicon Mac or other AArch64 system. The fiber context switch is AArch64-only, so x86-64 builds stop with a compile error. Only macOS has been tested.
- A C++20 compiler. Apple clang 21 is what's been tested.
- LLVM development libraries. LLVM 23 is what's been tested. Apple's own clang doesn't include them.
- CMake 3.20 or newer, and Ninja (optional, but the commands below use it).
- Python 3, only for running the tests.

```sh
brew install llvm cmake ninja
```

## Build

```sh
cmake -S . -B build -G Ninja -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"
cmake --build build
```

This produces `build/noob`. The default is a Release build, and `-DCMAKE_BUILD_TYPE=Debug` gives a debug one.

## Test

```sh
ctest --test-dir build --output-on-failure      # language and CLI tests
cmake --build build --target runtime_test && build/runtime_test   # fiber runtime
```

To run the language tests under AddressSanitizer and UBSan, build into a separate directory. Sanitized stack frames are much larger, so the tests need bigger stacks:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
cmake --build build-asan
NOOB_MIN_STACK=67108864 python3 test/run_tests.py build-asan/noob test/cases
```

## Install

```sh
cmake --install build --prefix ~/.local
```

That installs one file, `~/.local/bin/noob`. Add `~/.local/bin` to your `PATH` if it isn't there already, and use another `--prefix` to install elsewhere. To uninstall, delete that file.

The binary links against Homebrew's LLVM library, so keep `llvm` installed. After a `brew upgrade llvm` that changes the LLVM version, rebuild and reinstall.

## Command line

`noob` works like `python`. Options go before the script, because everything after it belongs to the script.

```
noob [options] [script.noob | -] [args]
```

| Command | What it does |
|---|---|
| `noob script.noob a b` | Run a file |
| `noob -c 'yap 1 + 1' a b` | Run a string |
| `noob - a b` | Run the script on stdin |
| `echo 'yap "hi"' \| noob` | With no script and piped stdin, run stdin |
| `noob` | With no script on a terminal, open the REPL |
| `noob -i` | Open the REPL even when stdin is piped |
| `noob -i script.noob` | Run a file, then open a REPL with its variables |

| Option | What it does |
|---|---|
| `-c <code>` | Run this code instead of a file |
| `-i` | Open a REPL after the script, even if it failed |
| `--check` | Syntax check only, run nothing |
| `--trace` | Print each statement's line to stderr as it runs |
| `--sober` | Plain error messages, no jokes |
| `--stack-size=<bytes>` | Stack for each task, generator, and worker thread |
| `--worker-threads=<n>` | Worker threads for blocking work |
| `-V`, `--version` | Print the version |
| `--help` | List the options |

**Script arguments.** Whatever follows the script, or follows the code with `-c`, arrives in the global list `cliargs`, without the script's name. Flags in that position go to the script too.

```sh
noob -c 'yap cliargs' one --two    # ["one", "--two"]
```

**Running a script directly.** The first line can be a shebang:

```sh
printf '#!/usr/bin/env noob\nyap "hi"\n' > hi.noob
chmod +x hi.noob
./hi.noob
```

**The whole file is checked first.** A syntax error anywhere means no statement runs, and up to 20 errors are reported at once. Everything else is reported only when that line runs.

**REPL.** Statements run as you enter them, and the prompt changes to `..>` while a block or expression is unfinished. Expressions echo their value unless it's `npc`. You can redeclare top-level names. Leave with `touchgrass` or Ctrl-D. History is kept in `~/.noob-history`. Background tasks from `sendit` only advance while a statement is running.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `N` | `touchgrass N` |
| `1` | Uncaught error, deadlock, or a failed task nothing awaited |
| `2` | Syntax error |
| `64` | Bad usage: unknown option, unreadable script, invalid UTF-8, or a bad setting |

### Runtime settings

The defaults follow Rust and Tokio. A flag beats its environment variable, and the environment variable beats the default.

| Setting | Flag | Environment variable | Default |
|---|---|---|---|
| Stack per task, generator, and worker thread | `--stack-size` | `NOOB_MIN_STACK` | 2097152 bytes (2 MiB) |
| Worker threads for blocking work | `--worker-threads` | `NOOB_WORKER_THREADS` | One per logical CPU |

- Sizes are plain byte counts, with no `K` or `M` suffixes. The minimum stack is 131072 bytes (128 KiB).
- The main script's stack is 8 MiB, or `--stack-size` if that's larger.
- Stack memory is committed as it's used, so a large size mostly costs address space.
- Workers only handle `spill`, `readfile`, and `writefile`. Script code always runs on one thread, so more workers won't speed it up.
- Function calls are capped at 1000 deep. Running out of stack before that is a catchable error, not a crash.

The full language reference is in [DESIGN.md](DESIGN.md), and there are more programs in [examples/](examples/).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Please report vulnerabilities privately, as described in [SECURITY.md](SECURITY.md).

## License

[MIT](LICENSE)
