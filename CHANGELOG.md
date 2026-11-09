# Changelog

Notable changes to NoobScript. This project uses [semantic versioning](https://semver.org). Until 1.0, minor versions may change the language.

## 0.2.0 - 2026-11-09

### Added

- Squads (classes) with inheritance, `me`, and `og`.
- Modules with `rizzup`.
- Async with `sendit` and `holdup`, `firstone`, and group chats (channels), on a fiber scheduler.
- Generators with `serve`.
- Pattern matching with `itdepends`, and destructuring in `lowkey`, `highkey`, and `speedrun`.
- Arrow functions, default and rest parameters, spread, list comprehensions, slices, negative indexes, ternary, pipe, `??`, and `?.`.
- `regardless` blocks on `tryna`.
- Built-ins for transforming collections, and for reading and writing files.
- Python-style command line: `-c`, `-`, piped scripts, `-i`, `-V`, and script arguments in `cliargs`.
- `--stack-size` and `--worker-threads`, with `NOOB_MIN_STACK` and `NOOB_WORKER_THREADS`.

### Changed

- Main script stack is now a fixed 8 MiB fiber, or `--stack-size` if larger.
- `spill` and `nap` no longer block other tasks.
- Negative indexes now count from the end instead of being an error.

## 0.1.0 - 2026-11-09

### Added

- Tree-walking interpreter with a whole-file syntax check before anything runs.
- Variables, control flow, closures, lists, maps, string interpolation, `yeet`/`tryna`/`caught4k`, built-ins, and a REPL.
- `--check`, `--trace`, and `--sober`.
