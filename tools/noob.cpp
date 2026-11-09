#include "noob/AST.h"
#include "noob/Diag.h"
#include "noob/Interpreter.h"
#include "noob/Parser.h"
#include "noob/Runtime.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/LineEditor/LineEditor.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;
using namespace noob;

static constexpr int ExitOk = 0;
static constexpr int ExitSyntaxError = 2;
static constexpr int ExitBadUsage = 64;

static cl::OptionCategory NoobCategory("NoobScript options");

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("[script.noob | -]"),
                                      cl::init(""), cl::cat(NoobCategory));
static cl::list<std::string> ScriptArgs(cl::ConsumeAfter, cl::desc("[args]"),
                                        cl::cat(NoobCategory));
static cl::opt<std::string> Code("c", cl::desc("Run this code instead of a file"),
                                 cl::value_desc("code"), cl::cat(NoobCategory));
static cl::opt<bool> Interactive("i", cl::desc("Open a REPL after the script, or even when piped"),
                                 cl::cat(NoobCategory));
static cl::opt<bool> ShowVersion("V", cl::desc("Print the version"), cl::cat(NoobCategory));
static cl::opt<bool> CheckOnly("check", cl::desc("Syntax check only, run nothing"),
                               cl::cat(NoobCategory));
static cl::opt<bool> Trace("trace", cl::desc("Print each statement's line as it executes"),
                           cl::cat(NoobCategory));
static cl::opt<bool> Sober("sober", cl::desc("Plain error messages, no jokes"),
                           cl::cat(NoobCategory));
static cl::opt<unsigned long long>
    StackSize("stack-size",
              cl::desc("Stack bytes per task, generator, and worker (default 2 MiB)"),
              cl::value_desc("bytes"), cl::cat(NoobCategory));
static cl::opt<unsigned>
    WorkerThreads("worker-threads",
                  cl::desc("Worker threads for blocking work (default: logical CPUs)"),
                  cl::value_desc("n"), cl::cat(NoobCategory));

static bool isValidUtf8(StringRef text) {
  const UTF8 *begin = reinterpret_cast<const UTF8 *>(text.begin());
  const UTF8 *end = reinterpret_cast<const UTF8 *>(text.end());
  return isLegalUTF8String(&begin, end);
}

/// Reads an unsigned integer from the environment, like RUST_MIN_STACK.
/// Returns false if it's set but isn't a number.
static bool readEnvNumber(const char *name, std::optional<unsigned long long> &out) {
  std::optional<std::string> text = sys::Process::GetEnv(name);
  if (!text)
    return true;
  unsigned long long value;
  if (StringRef(*text).getAsInteger(10, value)) {
    errs() << "noob: " << name << " must be a whole number, got '" << *text << "'\n";
    return false;
  }
  out = value;
  return true;
}

/// Flags beat environment variables, which beat the defaults.
static bool buildRuntimeConfig(RuntimeConfig &config) {
  std::optional<unsigned long long> stack, workers;
  if (!readEnvNumber("NOOB_MIN_STACK", stack) || !readEnvNumber("NOOB_WORKER_THREADS", workers))
    return false;
  if (StackSize.getNumOccurrences())
    stack = StackSize;
  if (WorkerThreads.getNumOccurrences())
    workers = WorkerThreads;

  if (stack) {
    if (*stack < MinStackSize) {
      errs() << "noob: the stack size must be at least " << MinStackSize << " bytes\n";
      return false;
    }
    config.stackSize = *stack;
  }
  if (workers) {
    if (*workers < 1) {
      errs() << "noob: there must be at least 1 worker thread\n";
      return false;
    }
    config.workerThreads = static_cast<unsigned>(*workers);
  }
  return true;
}

namespace {

/// Everything that lives for the whole run.
struct Session {
  SourceMgr sources;
  AstContext ast;
  Diag diag;
  Interpreter interpreter;

  explicit Session(InterpreterOptions options)
      : diag(sources, Sober), interpreter(ast, diag, std::move(options)) {}
};

} // namespace

static int runBuffer(Session &session, std::unique_ptr<MemoryBuffer> buffer, StringRef name,
                     bool fromFile) {
  StringRef source = buffer->getBuffer();
  if (!isValidUtf8(source)) {
    errs() << "noob: '" << name << "' isn't valid UTF-8\n";
    return ExitBadUsage;
  }
  session.sources.AddNewSourceBuffer(std::move(buffer), SMLoc());

  // Syntax check first, so a typo anywhere means nothing runs.
  Parser parser(session.ast, session.diag, source, /*allowShebang=*/true);
  ArrayRef<const Stmt *> program = parser.parseProgram();
  if (session.diag.hasErrors()) {
    session.diag.flush();
    return ExitSyntaxError;
  }

  if (fromFile)
    session.interpreter.setMainScript(name);

  if (CheckOnly) {
    outs() << name << (Sober ? ": no syntax errors\n" : ": passed the vibe check, no syntax errors\n");
    return ExitOk;
  }
  return session.interpreter.runProgram(program);
}

static int runRepl(Session &session) {
  session.interpreter.enterRepl();
  LineEditor editor("noob");
  outs() << "NoobScript " << NOOB_VERSION << ". touchgrass to leave.\n";
  outs().flush();

  std::string pending;
  for (;;) {
    editor.setPrompt(pending.empty() ? "noob> " : "  ..> ");
    std::optional<std::string> line = editor.readLine();
    if (!line)
      break;
    pending += *line;
    pending += '\n';
    if (StringRef(pending).trim().empty()) {
      pending.clear();
      continue;
    }
    if (!isValidUtf8(pending)) {
      errs() << "noob: that input isn't valid UTF-8\n";
      pending.clear();
      continue;
    }

    // The buffer stays owned by the SourceMgr, so the AST can point into it.
    auto buffer = MemoryBuffer::getMemBufferCopy(pending, "<repl>");
    StringRef source = buffer->getBuffer();
    session.sources.AddNewSourceBuffer(std::move(buffer), SMLoc());

    Parser parser(session.ast, session.diag, source);
    ArrayRef<const Stmt *> program = parser.parseProgram();
    if (session.diag.incomplete()) {
      session.diag.clear();
      continue;
    }
    pending.clear();
    if (session.diag.hasErrors()) {
      session.diag.flush();
      continue;
    }
    for (const Stmt *stmt : program)
      if (!session.interpreter.runReplStatement(stmt))
        return session.interpreter.exitCode();
  }
  outs() << "\n";
  return ExitOk;
}

/// Returns the process exit code.
static int run() {
  bool hasCode = Code.getNumOccurrences() > 0;
  bool stdinIsTerminal = sys::Process::StandardInIsUserInput();

  // With -c there's no script file, so every positional is a script argument.
  std::vector<std::string> args;
  if (hasCode && !InputFile.empty())
    args.push_back(InputFile);
  args.insert(args.end(), ScriptArgs.begin(), ScriptArgs.end());

  bool fromStdin = !hasCode && (InputFile == "-" || (InputFile.empty() && !stdinIsTerminal && !Interactive));
  bool fromFile = !hasCode && !fromStdin && !InputFile.empty();

  if (CheckOnly && !hasCode && !fromStdin && !fromFile) {
    errs() << "noob: --check needs a script to check\n";
    return ExitBadUsage;
  }

  InterpreterOptions options;
  options.trace = Trace;
  options.scriptArgs = std::move(args);
  if (!buildRuntimeConfig(options.runtime))
    return ExitBadUsage;

  std::unique_ptr<MemoryBuffer> buffer;
  std::string name;
  if (hasCode) {
    name = "<string>";
    buffer = MemoryBuffer::getMemBufferCopy(Code, name);
  } else if (fromStdin) {
    name = "<stdin>";
    auto input = MemoryBuffer::getSTDIN();
    if (!input) {
      errs() << "noob: can't read stdin: " << input.getError().message() << "\n";
      return ExitBadUsage;
    }
    // getSTDIN names its buffer "<stdin>" already.
    buffer = std::move(*input);
  } else if (fromFile) {
    name = InputFile;
    auto file = MemoryBuffer::getFile(InputFile, /*IsText=*/true);
    if (!file) {
      errs() << "noob: can't open '" << InputFile << "': " << file.getError().message() << "\n";
      return ExitBadUsage;
    }
    buffer = std::move(*file);
  }

  // Deliberately never destroyed: parked fibers may still reference it, and
  // the process is about to exit anyway.
  auto *session = new Session(std::move(options));

  int code = ExitOk;
  if (buffer) {
    code = runBuffer(*session, std::move(buffer), name, fromFile);
    if (!Interactive || CheckOnly || session->interpreter.exited() || code == ExitBadUsage)
      return code;
  }
  return runRepl(*session);
}

int main(int argc, char **argv) {
  cl::SetVersionPrinter([](raw_ostream &os) { os << "NoobScript " << NOOB_VERSION << "\n"; });
  cl::HideUnrelatedOptions(NoobCategory);
  if (!cl::ParseCommandLineOptions(argc, argv, "NoobScript interpreter\n", &errs()))
    return ExitBadUsage;

  if (ShowVersion) {
    outs() << "NoobScript " << NOOB_VERSION << "\n";
    return ExitOk;
  }

  int code = run();
  outs().flush();
  // Skips destructors: tasks and blocking workers may still be running.
  std::_Exit(code);
}
