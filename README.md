# NoobScript

[![CI](https://github.com/shanto462/NoobScript/actions/workflows/ci.yml/badge.svg)](https://github.com/shanto462/NoobScript/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A scripting language with Gen Z slang for keywords and real features underneath: closures, classes, generators, pattern matching, and async tasks. It syntax-checks your file, then interprets it one statement at a time, with no compilation step. Written in C++20 on LLVM's support libraries.

```noob
cook flex(name) {
    itsgiving "${name} is him fr"
}

speedrun i in 1..3 {
    vibecheck i == 2 { skibidi }
    yap flex("noob"), i
}
btw → noob is him fr 1
btw → noob is him fr 3

tryna {
    yeet "it's so over"
} caught4k err {
    yap "we're so back: ${err}"     btw → we're so back: it's so over
}
```

> **Status:** 0.x and still young. The language may change between minor versions, and only Apple silicon macOS is built and tested. See the [changelog](CHANGELOG.md).

**Contents:** [Quick start](#quick-start) · [The slang](#the-slang) · [Language tour](#language-tour) · [Built-ins](#built-in-functions) · [Examples](#example-programs) · [Build](#build) · [Install](#install) · [Command line](#command-line)

## Quick start

```sh
brew install llvm cmake ninja
cmake -S . -B build -G Ninja -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"
cmake --build build
build/noob examples/tour.noob
```

## The slang

| NoobScript | Means | | NoobScript | Means |
|---|---|---|---|---|
| `lowkey` | variable | | `squad` | class |
| `highkey` | constant | | `copies` | extends |
| `yap` | print | | `glowup` | constructor |
| `mumble` | print, no newline | | `me` | this |
| `bruh` | print to stderr | | `og` | super |
| `vibecheck` | if | | `rizzup` | import |
| `nvm` | else if | | `sendit` | start a task |
| `nah` | else | | `holdup` | await |
| `grind` | while | | `serve` | yield |
| `speedrun` | for | | `itdepends` / `on` | match / case |
| `ghost` | break | | `yeet` | throw |
| `skibidi` | continue | | `tryna` | try |
| `cook` | function | | `caught4k` | catch |
| `itsgiving` | return | | `regardless` | finally |
| `nocap` / `cap` | true / false | | `ongod` | assert |
| `npc` | null | | `touchgrass` | exit |
| `sus` | not | | `ratio` | modulo |
| `btw` | comment | | `groupchat` | channel |

## Language tour

In these examples, `btw →` marks what the program prints. Every block here is run by the test suite, so the output shown is real.

### Variables and types

```noob
lowkey aura = 9000          btw a variable
highkey MAX_AURA = 9999     btw a constant
aura += 1
yap aura                    btw → 9001

lowkey plans = npc
yap nocap, cap, plans       btw → nocap cap npc
yap vibeof(aura), vibeof("hi"), vibeof([]), vibeof({})    btw → number string list map
```

Numbers are 64-bit floats. Only `cap` and `npc` are falsy, so `0`, `""`, and `[]` all count as true. Lists and maps are shared by reference.

### Operators

```noob
yap 2 + 3 * 4, 2 ** 10, 7 / 2                btw → 14 1024 3.5
yap 10 ratio 3, -1 % 3                       btw → 1 2
yap nocap && sus cap, cap || nocap           btw → nocap nocap
yap 1 == 1, "a" != "a", [1] == [1]           btw → nocap cap cap
yap 5 > 3 ? "W" : "L", npc ?? "default"      btw → W default
yap 16 |> sqrt, "hey" |> loud                btw → 4 HEY

lowkey nobody = npc
lowkey user = {"pet": {"name": "rex"}}
yap nobody?.pet.name, user?.pet.name         btw → npc rex
```

### Strings

```noob
lowkey name = "bestie"
yap "hey ${name}, 2 + 2 is ${2 + 2}"         btw → hey bestie, 2 + 2 is 4
yap clout("h💀y"), "h💀y"[1]                 btw → 3 💀
yap "abcdef"[1:3], "abcdef"[-2:]             btw → bc ef
yap loud("fr"), breakup("a,b", ","), linkup([1, 2], "-")    btw → FR ["a", "b"] 1-2
```

Strings are immutable UTF-8, and lengths and indexes count characters, so an emoji is one. Slices exclude the end.

### Lists and maps

```noob
lowkey xs = [1, 2, 3, 4]
slide(xs, 5)
yap xs[0], xs[-1], xs[1:3], clout(xs)        btw → 1 5 [2, 3] 5
yap [...xs[:2], 99]                          btw → [1, 2, 99]
yap [x * x speedrun x in xs vibecheck x ratio 2 == 0]    btw → [4, 16]

yap remix(xs, x => x * 2)                    btw → [2, 4, 6, 8, 10]
yap keep(xs, x => x > 3)                     btw → [4, 5]
yap boil(xs, (sum, x) => sum + x, 0)         btw → 15

lowkey stats = {"rizz": 1, "aura": 9000}
stats["drip"] = 7
yap stats.aura, stats["nope"]                btw → 9000 npc
yap receipts(stats)                          btw → ["rizz", "aura", "drip"]
yap {...stats, "rizz": 2}["rizz"]            btw → 2
```

### Control flow

```noob
lowkey score = 72
vibecheck score >= 90 {
    yap "W"
} nvm score >= 50 {
    yap "mid"                                btw → mid
} nah {
    yap "L + ratio"
}

lowkey hp = 30
grind hp > 0 {
    hp -= 10
}
yap hp                                       btw → 0

speedrun i in 1..5 {
    vibecheck i == 2 { skibidi }
    vibecheck i == 4 { ghost }
    yap i
}
btw → 1
btw → 3

yap [i speedrun i in 10..1 by -3]            btw → [10, 7, 4, 1]
yap [c speedrun c in "hey"]                  btw → ["h", "e", "y"]
yap [k speedrun k in {"a": 1, "b": 2}]       btw → ["a", "b"]
```

Ranges include both ends. `speedrun` walks ranges, lists, strings, map keys, generators, and group chats.

### Functions

```noob
cook greet(name, vibe = "immaculate", ...rest) {
    itsgiving "${name}: ${vibe} (+${clout(rest)})"
}
yap greet("rex")                             btw → rex: immaculate (+0)
yap greet("rex", "mid", 1, 2)                btw → rex: mid (+2)

lowkey double = x => x * 2
lowkey add = (a, b) => a + b
lowkey shout = cook(text) {
    itsgiving loud(text) + "!"
}
yap double(4), add(...[3, 4]), shout("yo")   btw → 8 7 YO!

cook counter() {
    lowkey n = 0
    itsgiving () => {
        n += 1
        itsgiving n
    }
}
lowkey tick = counter()
tick()
yap tick()                                   btw → 2
```

### Destructuring and pattern matching

```noob
lowkey [first, second, ...others] = [1, 2, 3, 4]
lowkey {"x": px, "y": py} = {"x": 5, "y": 6}
yap first, others, px + py                   btw → 1 [3, 4] 11

speedrun [key, value] in entries({"rizz": 1}) {
    yap key, value                           btw → rizz 1
}

cook describe(value) {
    itdepends value {
        on 0 { itsgiving "zero" }
        on 1 | 2 { itsgiving "small" }
        on 3..9 { itsgiving "medium" }
        on [a, b] when a > b { itsgiving "going down" }
        on {"name": n} { itsgiving "hi ${n}" }
        nah { itsgiving "no idea" }
    }
}
yap describe(2), describe(5)                 btw → small medium
yap describe([2, 1])                         btw → going down
yap describe({"name": "rex"})                btw → hi rex
yap describe("?")                            btw → no idea
```

### Errors

```noob
tryna {
    yeet "it's so over"
} caught4k err {
    yap "we're so back: ${err}"              btw → we're so back: it's so over
} regardless {
    yap "cleanup always runs"                btw → cleanup always runs
}

tryna { yap 1 / 0 } caught4k err { yap err }                  btw → can't divide by zero
tryna { ongod 1 > 2, "bro lied" } caught4k err { yap err }    btw → bro lied
```

`yeet` can throw any value. Runtime errors are ordinary catchable strings. `touchgrass 3` exits immediately with code 3, and can't be caught.

An uncaught error shows a joke, then the real problem. Add `--sober` to drop the joke:

```text
$ noob -c 'yap 1 / 0'
<string>:1:7: error: dividing by zero? in this economy?
yap 1 / 0
      ^
can't divide by zero
```

### Squads (classes)

```noob
squad Dog {
    cook glowup(name) {
        me.name = name
    }

    cook speak() {
        itsgiving "${me.name} says woof"
    }
}

squad Puppy copies Dog {
    cook speak() {
        itsgiving og.speak() + " (squeaky)"
    }
}

lowkey pup = Puppy("rex")
yap pup.speak()                              btw → rex says woof (squeaky)
yap vibeof(pup), memberof(pup, Dog), pup     btw → Puppy nocap <Puppy>

lowkey speak = pup.speak
pup.name = "max"
yap speak()                                  btw → max says woof (squeaky)
```

`glowup` is the constructor, and `obj.method` without a call stays bound to `obj`.

### Modules

```noob
rizzup "mathz.noob"
rizzup "mathz.noob" as m

yap mathz.square(4), m.PI                    btw → 16 3.14
```

Paths are relative to the importing file, and each file's top level runs once. Every top-level name is exported.

### Generators

```noob
cook naturals() {
    lowkey n = 1
    grind nocap {
        serve n
        n += 1
    }
}

lowkey g = naturals()
yap nextup(g), nextup(g), cooked(g)          btw → 1 2 cap

cook take(source, count) {
    lowkey out = []
    speedrun x in source {
        slide(out, x)
        vibecheck clout(out) == count { ghost }
    }
    itsgiving out
}
yap take(naturals(), 3)                      btw → [1, 2, 3]
```

### Async

```noob
cook fetch(url, ms) {
    nap(ms)
    itsgiving "got ${url}"
}

lowkey a = sendit fetch("/vibes", 30)
lowkey b = sendit fetch("/rizz", 10)
yap holdup b                                 btw → got /rizz
yap holdup a                                 btw → got /vibes

yap holdup [sendit fetch("/x", 1), sendit fetch("/y", 1)]              btw → ["got /x", "got /y"]
yap firstone([sendit fetch("/slow", 40), sendit fetch("/fast", 5)])    btw → got /fast

cook risky() {
    yeet "server said nah"
}
tryna {
    holdup sendit risky()
} caught4k err {
    yap err                                  btw → server said nah
}
```

Tasks are cooperative fibers on one thread, so scripts have no data races, and any function can wait without being marked async. The program ends when every task has finished, and if every task is stuck waiting, NoobScript reports a deadlock instead of hanging.

### Group chats (channels)

```noob
lowkey chat = groupchat(2)      btw holds 2 messages, omit the size for unlimited

cook producer() {
    speedrun i in 1..3 {
        dm(chat, i * 10)        btw waits while the chat is full
    }
    leavechat(chat)
}

sendit producer()
speedrun message in chat {
    yap message
}
btw → 10
btw → 20
btw → 30
```

### Input, files, and arguments

```noob
lowkey name = spill("your name? ") ?? "anon"     btw reads a line, npc at end of input
bruh "this goes to stderr"
mumble "no newline"
yap ""

writefile("/tmp/noob_readme_demo.txt", "bars")
yap clout(readfile("/tmp/noob_readme_demo.txt"))
yap cliargs
```

`spill`, `readfile`, and `writefile` run off-thread, so other tasks keep going.

## Built-in functions

| Function | Does |
|---|---|
| **Types** | |
| `vibeof(x)` | Type name: `number`, `string`, `bool`, `npc`, `list`, `map`, `function`, `squad`, `task`, `generator`, `module`, `groupchat`, or the squad's name |
| `towords(x)` | Convert to a string |
| `todigits(x)` | Parse to a number, or `npc` if it can't |
| `memberof(x, Squad)` | Is `x` an instance of `Squad` or a child of it? |
| **Collections** | |
| `clout(x)` | Length of a string, list, or map |
| `gotit(x, item)` | Substring, list item, or map key present? |
| `slide(list, x)` | Append |
| `dip(list)` | Remove and return the last item |
| `yoink(list, i)` | Remove and return the item at `i` |
| `remix(xs, f)` | Map to a new list |
| `keep(xs, f)` | Filter to a new list |
| `boil(xs, f, start)` | Reduce, calling `f(acc, x)` |
| `rank(xs, key?)` | Sorted copy, stable. Keys must be all numbers or all strings. |
| `flip(x)` | Reversed copy of a list or string |
| `numbered(xs)` | `[[0, x], [1, y], ...]` |
| `pairup(a, b)` | `[[a0, b0], ...]`, up to the shorter list |
| **Maps** | |
| `receipts(m)` | Keys |
| `vals(m)` | Values |
| `entries(m)` | `[[key, value], ...]` |
| `unsend(m, key)` | Remove a key |
| **Strings** | |
| `breakup(s, sep)` | Split into a list. An empty `sep` splits into characters. |
| `linkup(list, sep)` | Join into a string |
| `snip(s, start, len)` | Substring |
| `loud(s)`, `quiet(s)` | Upper and lower case |
| **Math** | |
| `floor`, `ceil`, `round`, `abs`, `sqrt` | The usual |
| `rng(min, max)` | Random whole number, both ends included |
| **Time and I/O** | |
| `rn()` | Current time in milliseconds |
| `nap(ms)` | Sleep without blocking other tasks |
| `spill(prompt?)` | Read a line from stdin, or `npc` at end of input |
| `readfile(path)` | File contents as a string |
| `writefile(path, text)` | Replace a file |
| **Async and generators** | |
| `firstone(tasks)` | Wait for whichever finishes first |
| `groupchat(size?)` | Make a channel |
| `dm(chat, value)` | Send |
| `checkdms(chat)` | Receive, or `npc` once closed and drained |
| `leavechat(chat)` | Close |
| `nextup(g)` | Next generator value, or `npc` when finished |
| `cooked(g)` | Has the generator finished? |

`cliargs` is a global list of the script's arguments.

## Example programs

| File | Shows |
|---|---|
| [`tour.noob`](examples/tour.noob) | A quick pass over the basics |
| [`fizzbuzz.noob`](examples/fizzbuzz.noob) | The classic |
| [`guess.noob`](examples/guess.noob) | An interactive game with `spill` and `rng` |
| [`squads.noob`](examples/squads.noob) | Classes and inheritance, in a fight simulation that imports [`mathz.noob`](examples/mathz.noob) |
| [`patterns.noob`](examples/patterns.noob) | A calculator built on `itdepends` and list patterns |
| [`generators.noob`](examples/generators.noob) | Infinite sequences and generator pipelines |
| [`async_fetch.noob`](examples/async_fetch.noob) | Concurrent tasks, error handling, and `firstone` |
| [`groupchat.noob`](examples/groupchat.noob) | A worker pool over a bounded channel |
| [`wordcount.noob`](examples/wordcount.noob) | Files, arguments, and sorting: `noob examples/wordcount.noob README.md 5` |

The full language reference and design are in [DESIGN.md](DESIGN.md).

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

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Please report vulnerabilities privately, as described in [SECURITY.md](SECURITY.md).

## License

[MIT](LICENSE)
