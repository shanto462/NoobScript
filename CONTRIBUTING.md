# Contributing

Bug reports, slang suggestions, and pull requests are all welcome.

## Getting set up

Follow [Build](README.md#build) and [Test](README.md#test) in the README. CI builds with `-Werror` and also runs every test under AddressSanitizer and UBSan, so it's worth running those locally for changes to the interpreter or runtime.

## Where things live

| Path | What's there |
|---|---|
| `DESIGN.md` | The language reference and architecture. It's the source of truth for behaviour. |
| `lib/Lexer.cpp`, `lib/Parser.cpp` | Tokens, grammar, and every syntax error |
| `lib/Interpreter.cpp` | Statement and expression execution |
| `lib/Builtins.cpp` | Built-in functions |
| `lib/Runtime.cpp` | Fibers, scheduler, timers, and worker threads |
| `tools/noob.cpp` | The command line and REPL |
| `test/cases/` | Golden tests |

## Making a change

- A change to the language updates `DESIGN.md` in the same pull request.
- A new feature or bug fix comes with a test. Golden tests are `.noob` files with expectations in `btw` comments, and the format is described at the top of `test/run_tests.py`.
- Work out expected test output from the spec, not by pasting what the interpreter printed.
- Errors keep the two-part format: a joke headline and a plain explanation of the real problem, which is all `--sober` shows.
- The code builds without exceptions or RTTI, following LLVM's conventions. Use `isa`, `cast`, and `dyn_cast` for type dispatch.
- Script values aren't thread-safe. Only blocking C++ work may run on a worker thread.

By contributing, you agree that your work is licensed under the [MIT License](LICENSE).

Everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
