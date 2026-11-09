# NoobScript

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

## Build

```sh
brew install llvm cmake ninja
cmake -S . -B build -G Ninja -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

Works like `python`:

```sh
noob script.noob a b          # run a file, with a and b in the list cliargs
noob -c 'yap 1 + 1'           # run a string
echo 'yap "hi"' | noob        # run a piped script (or: noob -)
noob                          # open the REPL
noob -i script.noob           # run a file, then a REPL with its variables
noob --check script.noob      # syntax check only
noob --trace script.noob      # show each statement as it runs
noob --sober script.noob      # plain error messages
```

`cmake --install build --prefix ~/.local` puts `noob` on your PATH, or run `build/noob` directly.

Each task, generator, and worker thread gets a 2 MiB stack and there's one worker per logical CPU, matching Rust and Tokio. Change them with `--stack-size=BYTES` and `--worker-threads=N`, or `NOOB_MIN_STACK` and `NOOB_WORKER_THREADS`.

Exit codes: `0` success, `N` from `touchgrass N`, `1` uncaught error, `2` syntax error, `64` bad usage.

The full language reference is in [DESIGN.md](DESIGN.md), and there are more programs in [examples/](examples/).
