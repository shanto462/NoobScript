# Roadmap

Where NoobScript could go next. Nothing here is built yet, the order can change, and slang names and code sketches are placeholders. Ideas and pushback are welcome in [issues](https://github.com/shanto462/NoobScript/issues).

## What comes first, and why

1. **Run everywhere.** Today NoobScript only builds on AArch64 and is only tested on macOS, which shuts out most potential users and most CI.
2. **Bytes, streams, and a real I/O loop.** Files, sockets, processes, and HTTP all depend on them. Worker threads work for occasional file reads, but not for thousands of connections.
3. **A standard library.** With I/O in place, most of it can be written in NoobScript itself.

## 0.3: Portability and foundations

| Item | Why |
|---|---|
| x86-64 fiber context switch | Intel Macs, most Linux machines, and most servers |
| Linux support, with CI for Linux and macOS on both architectures | The runtime only needs small changes, but they're untested |
| Windows support | Needs Windows fibers and a different I/O backend, so it may slip |
| Cycle collector | Reference counting leaks cycles, including every named recursive function, which rules out long-running programs |
| Error objects with a message, type, and stack | Errors are plain strings today, so code can't tell kinds apart |
| Task cancellation and timeouts | A started task can't currently be stopped |
| Benchmarks in CI, and faster variable lookup | Resolving variables to slots at parse time would be the biggest cheap speedup |

## 0.4: Bytes, streams, and I/O

```noob
btw sketch only
lowkey file = openup("big.log")
speedrun line in file.lines() {
    vibecheck gotit(line, "ERROR") { yap line }
}
```

| Item | Why |
|---|---|
| `bytes` type: mutable buffers, slicing, encoding, and decoding | Strings must be valid text, so binary data has no home |
| Reader and writer streams with buffering and backpressure | One interface for files, sockets, pipes, and memory |
| File handles: open, seek, append, and read by line or chunk | `readfile` loads everything into memory |
| Filesystem: list, stat, make, move, remove, and walk directories | Scripting basics |
| Event loop on kqueue and epoll | Scales past the worker pool and keeps the single-threaded model |
| Standard input, output, and error as streams | So they compose with everything else |
| Subprocesses with piped stdin and stdout | A scripting language has to be able to run other programs |
| TCP and UDP sockets, including async servers | The base for networking |
| Timers and intervals | Beyond a one-off `nap` |

## 0.5: Standard library

Loaded with `rizzup "std/json"`, shipped inside the binary, and written in NoobScript where speed allows.

| Module | Contents |
|---|---|
| `std/strings` | Trim, replace, starts-with, ends-with, pad, repeat, Unicode case |
| `std/regex` | Search, match, replace, and split |
| `std/json` | Parse and serialize |
| `std/math` | Trigonometry, logs, min, max, clamp, constants |
| `std/random` | Seeded generators, shuffle, choice |
| `std/collections` | Sets, deques, heaps, ordered maps |
| `std/iter` | Lazy map, filter, take, zip, chunk over any iterable |
| `std/time` | Dates, durations, formatting, time zones |
| `std/path`, `std/os` | Paths, environment variables, platform info |
| `std/fmt` | Number and string formatting, tables |
| `std/args` | Command-line flag parsing on top of `cliargs` |
| `std/log` | Levelled logging |
| `std/test` | Unit tests with assertions and reports |
| `std/hash`, `std/base64` | Checksums and encodings |
| `std/csv` | Parse and write |
| `std/http` | Client and server over sockets. TLS comes after that. |

## 0.6: Language features

| Item | Why |
|---|---|
| Custom printing, equality, ordering, and operators on squads | Instances can only print as `<Name>` today |
| Iterator protocol for squads | So `speedrun` works on user types |
| Enums | Named cases that work with `itdepends` |
| `itdepends` and `vibecheck` as expressions | Fewer temporary variables |
| Named arguments and destructuring parameters | Clearer calls, and `[a, b] => a` |
| Static members, getters, and setters | Rounding out squads |
| Scoped resources, closing files and sockets automatically | Safer than `regardless` by hand |
| Async generators | Stream values from async work |
| Multi-line strings and block comments | Quality of life |
| Integer type and big integers | Floats lose precision past 2^53 |
| Optional type hints, checked by `--check` | Catching bugs before running, while staying optional |

## 0.7: Concurrency

| Item | Why |
|---|---|
| Task groups (structured concurrency) | Tasks that can't outlive their parent, and one failure cancels its siblings |
| Waiting on several group chats at once | The missing piece for channel-based programs |
| Mutexes, semaphores, and wait groups | Coordinating across suspension points |
| Isolated workers, each with its own interpreter, passing messages | Real parallelism for CPU-heavy work without shared-memory data races |

## Tooling

| Item | Why |
|---|---|
| Syntax highlighting for VS Code and other editors | The cheapest big improvement to daily use |
| `noob fmt` | One formatting style |
| Language server | Inline errors using the existing syntax checker, then completion |
| `noob test` | Runs `std/test` suites |
| REPL completion and better multi-line editing | The REPL is minimal today |
| Debugger and profiler | Built on the existing `--trace` hook |
| Fuzzing in CI | Fuzzing found a parser hang during development, but it's only run by hand |
| Package manager and a registry | Once the language has settled |
| C++ embedding API and native extensions | Using NoobScript inside other programs |
| Packaged releases | Homebrew formula and prebuilt binaries, when there's a release process |

## Not planned

- **Ahead-of-time compilation or a JIT.** "Syntax check, then interpret" is the point of the project.
- **Shared-memory threads for script code.** Parallelism goes through isolated workers instead.
- **Required static types.** Hints stay optional.
