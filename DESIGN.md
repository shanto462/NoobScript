# NoobScript Design (v1)

A small, dynamically typed scripting language with Gen Z slang for keywords.
Written in C++ on top of LLVM's support libraries. Source files use `.noob`.

```noob
btw the classic
cook vibe(n) {
    vibecheck n ratio 15 == 0 { itsgiving "fizzbuzz" }
    nvm n ratio 3 == 0 { itsgiving "fizz" }
    nvm n ratio 5 == 0 { itsgiving "buzz" }
    nah { itsgiving n }
}

speedrun i in 1..15 {
    yap vibe(i)
}
```

## 1. Goals and execution model

- **No compilation.** No bytecode, no LLVM IR, no JIT, no output binary. The AST is walked directly.
- **Two steps only:** syntax-check the source, then interpret it one statement at a time.
- **Funny on the surface, normal underneath.** Keywords and error headlines are jokes. Semantics are boring and predictable, and every error also states the real problem in plain English.

```
source ──> Lexer ──> Parser ──> AST ──> Interpreter
           └─── syntax check ───┘       (one top-level statement at a time)
```

| Phase | When | Catches |
|---|---|---|
| Syntax check | Whole file, before anything runs | Bad tokens, bad grammar, `ghost` outside a loop, `itsgiving` outside a function, invalid assignment targets, duplicate parameters |
| Interpretation | As each statement is reached | Undefined names, type errors, reassigning a `highkey`, wrong argument counts, everything else |

If the syntax check fails, **no statement runs**, so a typo on line 90 can't leave lines 1 to 89 half-applied.
Everything else is deliberately a runtime error, reported only if that line is actually reached.

A "statement" is the unit of execution. A multi-line `grind { ... }` is one top-level statement, and its inner statements run one by one as the interpreter reaches them.

### Where LLVM fits

NoobScript links only `LLVMSupport` (plus `LLVMLineEditor` for the REPL).

| Need | LLVM facility |
|---|---|
| Caret-style errors with line and column | `SourceMgr`, `SMLoc`, `SMDiagnostic` |
| Reading files and stdin | `MemoryBuffer` |
| Command-line flags | `cl::opt` |
| Colored output | `raw_ostream` |
| AST allocation that lives until exit | `BumpPtrAllocator`, `StringSaver` |
| AST type dispatch | `isa`, `cast`, `dyn_cast` with `classof` |
| Containers | `StringRef`, `SmallVector`, `DenseMap`, `StringMap` |
| Runtime object lifetime | `RefCountedBase`, `IntrusiveRefCntPtr` |
| UTF-8 handling | `ConvertUTF` |
| REPL history and editing | `LineEditor` |

Two alternatives were rejected because they conflict with "no compilation":

- **ORC JIT per statement.** It generates and compiles machine code for every statement, which is compilation.
- **LLVM's IR `Interpreter`.** It still needs a compile step to IR, and it adds a typed-IR lowering of a dynamic language for no benefit.

The AST stays independent of the interpreter, so a JIT backend could be added later without touching the front end.

## 2. Language reference

### Keywords

| NoobScript | Means | Example |
|---|---|---|
| `lowkey` | declare a variable | `lowkey age = 19` |
| `highkey` | declare a constant | `highkey PI = 3.14` |
| `yap` | print with newline | `yap "hello", name` |
| `mumble` | print, no newline | `mumble "loading..."` |
| `bruh` | print to stderr | `bruh "that's not good"` |
| `vibecheck` | if | `vibecheck x > 1 { }` |
| `nvm` | else if | `nvm x > 0 { }` |
| `nah` | else | `nah { }` |
| `grind` | while | `grind hp > 0 { }` |
| `speedrun` ... `in` ... `by` | for | `speedrun i in 1..10 by 2 { }` |
| `ghost` | break | `ghost` |
| `skibidi` | continue | `skibidi` |
| `cook` | define a function | `cook add(a, b) { }` |
| `itsgiving` | return | `itsgiving a + b` |
| `yeet` | throw | `yeet "it's so over"` |
| `tryna` | try | `tryna { }` |
| `caught4k` | catch | `caught4k err { }` |
| `ongod` | assert | `ongod x > 0, "x gotta be positive"` |
| `touchgrass` | exit the program | `touchgrass 1` |
| `nocap` | true | `lowkey ready = nocap` |
| `cap` | false | `lowkey broke = cap` |
| `npc` | null | `lowkey plans = npc` |
| `sus` | logical not, same as `!` | `vibecheck sus ready { }` |
| `ratio` | modulo, same as `%` | `10 ratio 3` |
| `btw` | comment to end of line | `btw trust me` |

Section 7 adds classes, modules, async, generators, pattern matching, and more.

### Types

| Type | Literals | Notes |
|---|---|---|
| number | `42`, `3.14`, `1e3` | 64-bit float. Whole values print without a decimal point. |
| string | `"hi ${name}\n"` | Immutable UTF-8. Length and indexing count code points, so `clout("💀")` is 1. |
| bool | `nocap`, `cap` | Prints as written. |
| npc | `npc` | The absence of a value. |
| list | `[1, "two", nocap]` | Mutable, shared by reference. |
| map | `{"a": 1}` | Mutable, shared by reference, keeps insertion order. Keys are strings or numbers. |
| function | `cook(x) { itsgiving x }` | First-class closures. |

Only `cap` and `npc` are falsy. `0`, `""`, and `[]` are truthy.

### Layout rules

- A newline ends a statement. Semicolons are a syntax error, with a joke about it.
- Newlines are ignored after a binary operator or comma, and inside `()`, `[]`, and map literals.
- `nvm`, `nah`, and `caught4k` may sit on the same line as the closing `}` or on the next line.
- Blocks are always `{ }`. An empty block is valid.
- A `#!` first line is ignored, so scripts can be executable.

### Operators, loosest to tightest

| Level | Operators | Associativity |
|---|---|---|
| 1 | `\|\|` | left, short-circuit, yields a bool |
| 2 | `&&` | left, short-circuit, yields a bool |
| 3 | `==` `!=` | left |
| 4 | `<` `<=` `>` `>=` | left |
| 5 | `+` `-` | left |
| 6 | `*` `/` `%` `ratio` | left |
| 7 | unary `-` `!` `sus` | right |
| 8 | `**` | right, so `-2 ** 2` is `-4` |
| 9 | call `f(x)`, index `a[i]` | left |

- `+` adds numbers, joins two lists, and concatenates if either side is a string.
- `==` compares numbers, strings, bools, and npc by value, and lists, maps, and functions by identity. Different types are simply not equal.
- Ordering works on two numbers or two strings. Anything else is an error.
- `/` is float division. Dividing or taking `ratio` by zero is an error.
- `ratio` takes the sign of the divisor, so `-1 ratio 3` is `2`.
- Assignment (`=`, `+=`, `-=`, `*=`, `/=`) is a statement, not an expression, so `vibecheck x = 1` is a syntax error that suggests `==`.

### Variables and scope

- Scope is lexical, and each `{ }` opens a new scope.
- Shadowing an outer name is allowed. Declaring a name twice in the same scope is an error, except at the REPL top level.
- Assigning to an undeclared name is an error, because `lowkey` is required.
- A `highkey` binding can't be reassigned, but a list or map it holds can still be changed.
- Nothing is hoisted. A `cook` statement defines its function when it runs, so a top-level call must come after the definition. Function bodies look names up when called, so mutual recursion works.

### Control flow

```noob
vibecheck score >= 90 {
    yap "W"
} nvm score >= 50 {
    yap "mid"
} nah {
    yap "L + ratio"
}

grind hp > 0 {
    hp -= 10
    vibecheck hp == 50 { skibidi }
    vibecheck hp < 20 { ghost }
}

speedrun i in 1..5 { yap i }            btw 1 2 3 4 5, end included
speedrun i in 10..1 by -3 { yap i }     btw 10 7 4 1
speedrun x in [1, 2] { yap x }          btw list items
speedrun c in "hey" { yap c }           btw characters
speedrun k in {"a": 1} { yap k }        btw map keys
```

- Ranges exist only in a `speedrun` header.
- Without `by`, the step is 1, and a start above the end runs zero times.
- `by 0` is an error.
- The loop variable is fresh on each pass, so closures capture that pass's value.

### Functions

```noob
cook greet(name) {
    itsgiving "hey ${name}"
}

cook counter() {
    lowkey n = 0
    itsgiving cook() {
        n += 1
        itsgiving n
    }
}

lowkey tick = counter()
tick()
yap tick()   btw 2
```

- A function with no `itsgiving` returns `npc`.
- Calling with the wrong number of arguments is an error.
- Call depth is capped at 1000, past which a catchable error is raised instead of crashing the process.

### Collections

```noob
lowkey squad_goals = ["rizz", "aura"]
squad_goals[0] = "drip"
slide(squad_goals, "W")

lowkey stats = {"aura": 9000}
stats["rizz"] = 1
yap stats["nothing"]   btw npc
```

Reading a missing map key gives `npc`. A list or string index must be a whole number in range, otherwise it's an error. Negative indexes count from the end.

### Errors

```noob
tryna {
    yeet "it's so over"
} caught4k err {
    yap "we're so back: ${err}"
}

ongod clout(items) > 0, "bro brought nothing"
```

- `yeet` can throw any value.
- Built-in runtime errors arrive at `caught4k` as strings.
- An uncaught error prints a message and call trace, then exits with code 1.
- `touchgrass` isn't an error, so `tryna` can't catch it.

### Built-in functions

These are ordinary global functions, not keywords.

| Function | Does |
|---|---|
| `spill(prompt?)` | Read a line from stdin. Returns `npc` at end of input. |
| `clout(x)` | Length of a string, list, or map |
| `vibeof(x)` | Type name: `"number"`, `"string"`, `"bool"`, `"npc"`, `"list"`, `"map"`, `"function"` |
| `towords(x)` | Convert to a string |
| `todigits(x)` | Parse to a number, `npc` if it can't |
| `slide(list, x)` | Append |
| `dip(list)` | Remove and return the last item |
| `yoink(list, i)` | Remove and return the item at `i` |
| `gotit(x, item)` | Substring, list item, or map key present? |
| `receipts(map)` | List of keys |
| `unsend(map, key)` | Remove a key |
| `breakup(str, sep)` | Split into a list |
| `linkup(list, sep)` | Join into a string |
| `snip(str, start, len)` | Substring |
| `loud(str)`, `quiet(str)` | Upper and lower case |
| `floor`, `ceil`, `round`, `abs`, `sqrt` | Math |
| `rng(min, max)` | Random whole number, both ends included |
| `rn()` | Current time in milliseconds |
| `nap(ms)` | Sleep |

### Grammar

Terminals in `'quotes'`. `term` is a newline, end of file, or a lookahead at `}`.

```ebnf
program    = { NL | stmt } EOF ;
block      = '{' { NL | stmt } '}' ;

stmt       = decl | assign | print | if | while | for | func
           | return | throw | try | assert | exit
           | 'ghost' term | 'skibidi' term | expr term ;

decl       = ('lowkey' | 'highkey') IDENT '=' expr term ;
assign     = target ('=' | '+=' | '-=' | '*=' | '/=') expr term ;
target     = IDENT { index } ;                 (* checked after parsing an expr *)
print      = ('yap' | 'mumble' | 'bruh') [ args ] term ;
if         = 'vibecheck' expr block { 'nvm' expr block } [ 'nah' block ] ;
while      = 'grind' expr block ;
for        = 'speedrun' IDENT 'in' expr [ '..' expr [ 'by' expr ] ] block ;
func       = 'cook' IDENT params block ;
return     = 'itsgiving' [ expr ] term ;
throw      = 'yeet' expr term ;
try        = 'tryna' block 'caught4k' IDENT block ;
assert     = 'ongod' expr [ ',' expr ] term ;
exit       = 'touchgrass' [ expr ] term ;

params     = '(' [ IDENT { ',' IDENT } ] ')' ;
args       = expr { ',' expr } ;

expr       = or ;
or         = and { '||' and } ;
and        = equality { '&&' equality } ;
equality   = compare { ('==' | '!=') compare } ;
compare    = sum { ('<' | '<=' | '>' | '>=') sum } ;
sum        = product { ('+' | '-') product } ;
product    = unary { ('*' | '/' | '%' | 'ratio') unary } ;
unary      = ('-' | '!' | 'sus') unary | power ;
power      = postfix [ '**' unary ] ;
postfix    = primary { '(' [ args ] ')' | index } ;
index      = '[' expr ']' ;
primary    = NUMBER | STRING | 'nocap' | 'cap' | 'npc' | IDENT
           | '(' expr ')' | list | map | lambda ;
list       = '[' [ args ] ']' ;
map        = '{' [ pair { ',' pair } ] '}' ;
pair       = expr ':' expr ;
lambda     = 'cook' params block ;
```

A `{` is never ambiguous: blocks follow a statement header, and map literals appear only where an expression is expected.

## 3. Errors and tooling

Each error has a joke headline, then the real explanation, via `SMDiagnostic`:

```
vibes.noob:3:12: error: bestie what is this 💀
    yap 1 +
           ^
expected an expression after '+'
```

| Situation | Headline |
|---|---|
| Generic syntax error | bestie what is this 💀 |
| Semicolon | this ain't C, drop the semicolon |
| Unclosed block or string | you left me on read (unclosed) |
| Undefined name | who is `x`?? never met them |
| Reassigning `highkey` | `x` is highkey, it's not changing for you |
| Type error | those types are not compatible, it's giving divorce |
| Divide by zero | dividing by zero? in this economy? |
| Index out of range | that index is out of bounds, touch grass |
| Failed `ongod` | you swore on god and lied |
| Call depth exceeded | you're cooked (too much recursion) |

The syntax check recovers at the next newline, so one run reports up to 20 errors. `--sober` drops the headlines and prints only the plain explanation.

```
noob script.noob [args]   run a file
noob -c 'code' [args]     run a string
noob - [args]             run the script on stdin
noob                      REPL if stdin is a terminal, otherwise run the script on stdin
noob -i [script.noob]     run the script if given, then a REPL with its variables, even when piped
noob -V, --version        print the version
noob --check script.noob  syntax check only, runs nothing
noob --trace script.noob  print each statement's line as it executes
noob --sober ...          plain error messages
noob --stack-size=BYTES   stack for each task, generator, and worker thread (default 2 MiB)
noob --worker-threads=N   worker threads for blocking work (default: one per logical CPU)
```

The defaults follow Rust and Tokio, and the environment variables `NOOB_MIN_STACK` and `NOOB_WORKER_THREADS` set the same things, like `RUST_MIN_STACK` and `TOKIO_WORKER_THREADS`. A flag beats its environment variable. The main script's stack is 8 MiB, or `--stack-size` if that's larger. The smallest allowed stack is 128 KiB.

Anything after the script (or after the `-c` code) goes to the script in the global list `cliargs` and isn't treated as a flag. `cmake --install` puts `noob` on your PATH.

Exit codes: `0` success or `touchgrass`, `N` from `touchgrass N`, `1` uncaught runtime error, `2` syntax error, `64` bad usage.

**REPL.** Prompts are `noob> ` and `  ..> `. The parser reports "incomplete" when it runs out of input inside an open construct, and the REPL then reads another line. Expression statements echo non-`npc` results. State persists across lines.

## 4. Implementation

```
CMakeLists.txt
include/noob/   Token.h Lexer.h AST.h Parser.h Value.h Environment.h Interpreter.h Builtins.h Diag.h
lib/            Lexer.cpp Parser.cpp Value.cpp Interpreter.cpp Builtins.cpp Diag.cpp
tools/noob.cpp  CLI and REPL
test/           golden tests: *.noob with expected stdout, stderr, and exit code in `btw` comments
examples/
```

Built as C++20 with `-fno-exceptions -fno-rtti`, following LLVM's conventions.

**Lexer.** Hand-written, operating on a `MemoryBuffer`. It always emits `NEWLINE` tokens. Dropping newlines by bracket depth in the lexer would break lambdas inside call arguments, whose bodies need them, so the parser decides where newlines are insignificant. A `.` is part of a number only when a digit follows it, so `1..5` lexes as a range. Identifiers are ASCII. Strings and comments may hold any UTF-8.

**String interpolation.** The parser runs a sub-lexer over each `${...}` range of the same buffer, so diagnostics inside interpolations point to the right place.

**Parser.** Recursive descent with precedence climbing. It tracks loop and function nesting for the `ghost`, `skibidi`, and `itsgiving` checks, and returns one of `Ok`, `Errors`, or `Incomplete`. `Incomplete` is what drives the REPL.

**AST.** A `Stmt` and `Expr` hierarchy using a `Kind` enum with `classof`. Nodes come from a `BumpPtrAllocator` and each carries an `SMLoc`. Nothing is freed before exit, so closures can always point into the AST.

**Values.**

```cpp
class Value {
  enum class Tag : uint8_t { Npc, Bool, Number, Object } tag;
  union { bool b; double n; };
  llvm::IntrusiveRefCntPtr<Object> obj;   // String, List, Map, Function, Builtin
};
```

**Environment.** A chain of scopes with small name-to-binding tables keyed by interned identifier pointers. Each binding stores a `Value` and an `isConst` flag. Scopes are reference-counted so closures can keep them alive.

**Interpreter.** With C++ exceptions off, `throw` isn't available for control flow. Statements return a completion signal, and expression evaluation reports failure through its return value:

```cpp
enum class Flow { Normal, Break, Continue, Return, Throw, Exit };

Flow exec(const Stmt *s, Env &env);               // Return and Throw payloads live in the interpreter
bool eval(const Expr *e, Env &env, Value &out);   // false means an error or exit is unwinding

int runProgram(ArrayRef<const Stmt *> program) {
  for (const Stmt *stmt : program) {
    Flow flow = exec(stmt, *globals_);
    if (flow == Flow::Throw) { reportUncaught(); return 1; }
    if (flow == Flow::Exit) return exitCode_;
  }
  return 0;
}
```

**Known limitation.** Reference counting doesn't collect cycles, so a closure stored in the scope it captures, including every named recursive function, stays allocated until exit. That's acceptable for short-lived scripts. A cycle collector can come later if needed.

## 5. Out of scope

Big integers, static types, parallel execution of NoobScript code, cancelling tasks, a JSON or regex library, and any kind of static checking beyond syntax.

## 6. Build order

1. **Scaffold:** CMake, LLVM linkage, `noob --version`.
2. **Lexer and diagnostics:** tokens, joke headlines, `--sober`.
3. **Parser and `--check`:** full grammar, recovery, the context checks.
4. **Core interpreter:** values, scopes, expressions, `yap`, variables, `--trace`.
5. **Control flow and functions:** conditionals, loops, closures, recursion limit.
6. **Collections, strings, interpolation, built-ins.**
7. **Errors:** `yeet`, `tryna`, `ongod`, `touchgrass`, call traces.
8. **REPL, examples, README.**

Each step adds golden tests. A small CTest runner compares output and exit codes, so the tests don't depend on LLVM's `FileCheck` and `lit`, which aren't installed here.

**Prerequisite:** the LLVM libraries aren't installed on this machine yet (Apple's clang doesn't include them). `brew install llvm` provides them, and CMake finds them with `-DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm`.

## 7. Modern features (v2)

Everything in this section is additive. New keywords: `squad`, `me`, `og`, `rizzup`, `sendit`, `holdup`, `serve`, `itdepends`, `regardless`. The words `copies`, `as`, `on`, and `when` are special only where shown, and stay usable as names.

### Expressions

| Syntax | Meaning |
|---|---|
| `cond ? a : b` | Ternary. Loosest of all operators. |
| `x \|> f`, `x \|> f(y)` | Pipe, same as `f(x)` and `f(x, y)`. Looser than `??`. |
| `a ?? b` | `b` only if `a` is `npc`. Short-circuits. Looser than `\|\|`. |
| `a.b` | Field, method, map key, or module member. |
| `a?.b` | `npc` if `a` is `npc`. Every `.x`, `[i]`, and `(...)` later in the same chain is also skipped when its left side is `npc`. |
| `xs[-1]` | Negative indexes count from the end. |
| `xs[1:3]`, `xs[:2]`, `xs[1:]`, `xs[:]` | Slice of a list or string. Always a copy, out-of-range bounds clamp. Not assignable. |
| `x => x + 1`, `(a, b) => a + b`, `() => { ... }` | Arrow functions. A block body needs `itsgiving`. |
| `[...xs, 1]`, `f(...xs)`, `{...m, "k": 1}` | Spread. Lists and calls take anything `speedrun` can walk. Map literals take maps. |
| `[x * 2 speedrun x in xs vibecheck x > 1]` | List comprehension. One loop, optional filter, ranges allowed. |

Reading a field that doesn't exist on a squad instance or module is an error. `m.k` on a map is the same as `m["k"]`, so a missing key gives `npc`.

Full precedence, loosest first: ternary, `\|>`, `??`, `\|\|`, `&&`, equality, comparison, `+ -`, `* / % ratio`, unary (`- ! sus holdup sendit`), `**`, postfix.

### Parameters

`cook f(a, b = 2, ...rest) { }`. Defaults are evaluated at each call and can use earlier parameters. Required parameters must come first, and a `...rest` parameter must be last and collects extras into a list.

### Destructuring and patterns

`lowkey`, `highkey`, and `speedrun` take a pattern in place of a name. A pattern that doesn't fit is an error.

```noob
lowkey [a, b, ...rest] = [1, 2, 3, 4]
lowkey {"x": px, "y": py} = {"x": 1, "y": 2}
speedrun [k, v] in entries(stats) { yap k, v }
```

| Pattern | Matches |
|---|---|
| `name` | Anything, and binds it |
| `_` | Anything, binds nothing |
| `1`, `-1`, `"a"`, `nocap`, `npc` | An equal value |
| `1..5` | A number within the range, both ends included |
| `[p, q, ...rest]` | A list of exactly that length, or at least that length with `...rest` |
| `{"k": p}` | A map or squad instance that has every listed key |
| `p \| q` | Either pattern |

`itdepends` runs the first arm that fits. If none does, nothing happens.

```noob
itdepends value {
    on 0 { yap "zero" }
    on 1 | 2 { yap "small" }
    on [a, b] when a > b { yap "descending" }
    on {"name": n} { yap "hi ${n}" }
    nah { yap "no idea" }
}
```

### Errors

`tryna { } caught4k e { } regardless { }`. Either `caught4k` or `regardless` may be left out, but not both. `regardless` runs however the block ended, except `touchgrass`. If the `regardless` block itself exits via `yeet`, `itsgiving`, `ghost`, or `skibidi`, that replaces the original outcome.

### Squads (classes)

```noob
squad Dog {
    cook glowup(name) { me.name = name }
    cook speak() { itsgiving "${me.name} says woof" }
}

squad Puppy copies Dog {
    cook glowup(name) {
        og.glowup(name)
        me.age = 0
    }
    cook speak() { itsgiving og.speak() + " (squeaky)" }
}

lowkey p = Puppy("rex")
yap p.speak()   btw rex says woof (squeaky)
```

- Calling a squad creates an instance and runs `glowup` if one exists anywhere up the chain. With no `glowup`, passing arguments is an error.
- A squad body contains only `cook` methods.
- `me` is the instance. `og.name` finds `name` starting at the parent of the squad where the running method was defined. Using `me` or `og` outside a squad is a syntax error.
- Arrow functions and lambdas inside a method keep `me`.
- `obj.method` without a call gives a function bound to `obj`.
- Fields are created by assigning to them. Fields hide methods of the same name.
- `vibeof(p)` is `"Puppy"`, and `vibeof(Dog)` is `"squad"`. `memberof(p, Dog)` checks the whole chain.
- Instances print as `<Puppy>`, and squads as `<squad Puppy>`.

### Modules

`rizzup "lib/mathz.noob"` binds `mathz`, and `rizzup "x.noob" as y` binds `y`. Without `as`, the file's base name must be a valid identifier, or it's a syntax error. Paths are relative to the importing file. A file's top level runs once, however many times it's imported, and syntax errors in it surface as a catchable error at the `rizzup`. Every top-level name is visible as `mathz.name`. A circular import is an error.

### Generators

A function containing `serve value` is a generator. Calling it runs nothing and returns a generator. `speedrun` walks it, `nextup(g)` returns the next value or `npc` when finished, and `cooked(g)` says whether it has finished. A `yeet` inside propagates to whoever is advancing it. `serve` outside a function is a syntax error.

```noob
cook naturals() {
    lowkey n = 1
    grind nocap {
        serve n
        n += 1
    }
}
```

### Async

Tasks are cooperative fibers on one thread, run in first-in first-out order. A task only gives way at `holdup`, `nap`, `spill`, `readfile`, `writefile`, `dm` on a full chat, or `checkdms`, so scripts have no data races. There's no function coloring, so any function can wait.

```noob
cook fetch(id, ms) {
    nap(ms)
    itsgiving "got ${id}"
}

lowkey a = sendit fetch(1, 20)
lowkey b = sendit fetch(2, 10)
yap holdup a, holdup b          btw got 1 got 2
yap holdup [sendit fetch(3, 1), sendit fetch(4, 1)]
```

- `sendit f(x)` evaluates `f` and its arguments now, queues the call as a task, and returns the task. It starts when the current task next gives way. The operand must be a call, otherwise it's a syntax error.
- `holdup task` waits and gives the result, or rethrows the task's error, preserving its original message and location.
- `holdup [t1, t2]` waits for each in order and gives a list. Non-task values in the list, and `holdup` on a non-task value, pass through unchanged without waiting.
- `firstone([t1, t2])` waits for whichever finishes first and gives its result, or rethrows its error.
- The program ends once the main script and every task have finished. `touchgrass` from any task ends everything immediately. An uncaught error in the main script also ends everything immediately, with exit code 1.
- A failed task that nothing ever awaited is reported when the program ends, and the exit code is 1.
- If every task is waiting and nothing can wake any of them, the program reports `everyone's left on read (deadlock)` and exits with code 1. This can't be caught.

Channels are called group chats:

| Function | Does |
|---|---|
| `groupchat(size?)` | Make a channel. Unbounded unless `size` (at least 1) is given. |
| `dm(chat, value)` | Send. Waits while a bounded chat is full. Sending on a closed chat is an error. |
| `checkdms(chat)` | Receive, waiting if empty. Returns `npc` once closed and drained. |
| `leavechat(chat)` | Close. |

`speedrun msg in chat { }` receives until the chat is closed and drained. `vibeof` gives `"task"`, `"generator"`, `"module"`, and `"groupchat"`, and they print as `<task>`, `<generator>`, `<module name>`, and `<groupchat>`.

### More built-ins

| Function | Does |
|---|---|
| `remix(xs, f)` | Map to a new list |
| `keep(xs, f)` | Filter to a new list |
| `boil(xs, f, start)` | Reduce, calling `f(acc, x)` |
| `rank(xs, key?)` | New list sorted ascending and stably. Keys must be all numbers or all strings. |
| `flip(x)` | New reversed list or string |
| `entries(m)`, `vals(m)` | `[[k, v], ...]`, and the values |
| `numbered(xs)` | `[[0, x], [1, y], ...]` |
| `pairup(a, b)` | `[[a0, b0], ...]`, stopping at the shorter list |
| `memberof(x, Squad)` | Whether `x` is an instance of `Squad` or a child of it |
| `readfile(path)` | File contents as a string. Runs off-thread, and other tasks continue. |
| `writefile(path, text)` | Replace a file. Runs off-thread. |

`nap` and `spill` wait without blocking other tasks.

### Async runtime internals

LLVM provides a thread pool but no event loop, timers, or awaitable tasks, and its pool can't set the stack size of its threads. `Scheduler` in `lib/Runtime.cpp` therefore supplies all of it: a single-threaded run queue of stackful fibers with a small hand-written context switch (AArch64 only for now), a timer heap, and a pthread worker pool that only ever runs blocking C++ work and never touches NoobScript values. All NoobScript code, including the main script, runs on a fiber, so `Interpreter` keeps its call stack and pending error per fiber. Generators are fibers too, resumed directly by whoever advances them.
