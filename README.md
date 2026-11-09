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

## Build

```sh
brew install llvm cmake ninja
cmake -S . -B build -G Ninja -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```sh
build/noob examples/tour.noob          # run a script
build/noob --check script.noob         # syntax check only
build/noob --trace script.noob         # show each statement as it runs
build/noob --sober script.noob         # plain error messages
build/noob                             # REPL
```

Exit codes: `0` success, `N` from `touchgrass N`, `1` uncaught error, `2` syntax error, `64` bad usage.

The full language reference is in [DESIGN.md](DESIGN.md), and there are more programs in [examples/](examples/).
