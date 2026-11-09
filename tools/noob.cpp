#include "noob/AST.h"
#include "noob/Diag.h"
#include "noob/Interpreter.h"
#include "noob/Parser.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/LineEditor/LineEditor.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>

using namespace llvm;
using namespace noob;

static constexpr int ExitOk = 0;
static constexpr int ExitSyntaxError = 2;
static constexpr int ExitBadUsage = 64;

static cl::OptionCategory NoobCategory("NoobScript options");

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("[script.noob]"),
                                      cl::init(""), cl::cat(NoobCategory));
static cl::list<std::string> ScriptArgs(cl::ConsumeAfter, cl::desc("[args]"),
                                        cl::cat(NoobCategory));
static cl::opt<bool> CheckOnly("check", cl::desc("Syntax check only, run nothing"),
                               cl::cat(NoobCategory));
static cl::opt<bool> Trace("trace", cl::desc("Print each statement's line as it executes"),
                           cl::cat(NoobCategory));
static cl::opt<bool> Sober("sober", cl::desc("Plain error messages, no jokes"),
                           cl::cat(NoobCategory));

static bool isValidUtf8(StringRef text) {
  const UTF8 *begin = reinterpret_cast<const UTF8 *>(text.begin());
  const UTF8 *end = reinterpret_cast<const UTF8 *>(text.end());
  return isLegalUTF8String(&begin, end);
}

static int runFile() {
  auto file = MemoryBuffer::getFile(InputFile, /*IsText=*/true);
  if (!file) {
    errs() << "noob: can't open '" << InputFile << "': " << file.getError().message() << "\n";
    return ExitBadUsage;
  }
  StringRef source = (*file)->getBuffer();
  if (!isValidUtf8(source)) {
    errs() << "noob: '" << InputFile << "' isn't valid UTF-8\n";
    return ExitBadUsage;
  }

  SourceMgr sources;
  sources.AddNewSourceBuffer(std::move(*file), SMLoc());

  AstContext ast;
  Diag diag(sources, Sober);

  // Syntax check first, so a typo anywhere means nothing runs.
  Parser parser(ast, diag, source, /*allowShebang=*/true);
  ArrayRef<const Stmt *> program = parser.parseProgram();
  if (diag.hasErrors()) {
    diag.flush();
    return ExitSyntaxError;
  }

  if (CheckOnly) {
    if (Sober)
      outs() << InputFile << ": no syntax errors\n";
    else
      outs() << InputFile << ": passed the vibe check, no syntax errors\n";
    return ExitOk;
  }

  Interpreter interpreter(ast, diag, Trace, /*repl=*/false);
  int code = interpreter.runProgram(program);
  outs().flush();
  return code;
}

static int runRepl() {
  SourceMgr sources;
  AstContext ast;
  Diag diag(sources, Sober);
  Interpreter interpreter(ast, diag, Trace, /*repl=*/true);

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
    sources.AddNewSourceBuffer(std::move(buffer), SMLoc());

    Parser parser(ast, diag, source);
    ArrayRef<const Stmt *> program = parser.parseProgram();
    if (diag.incomplete()) {
      diag.clear();
      continue;
    }
    pending.clear();
    if (diag.hasErrors()) {
      diag.flush();
      continue;
    }
    for (const Stmt *stmt : program)
      if (!interpreter.runReplStatement(stmt))
        return interpreter.exitCode();
  }
  outs() << "\n";
  return ExitOk;
}

int main(int argc, char **argv) {
  cl::SetVersionPrinter([](raw_ostream &os) { os << "NoobScript " << NOOB_VERSION << "\n"; });
  cl::HideUnrelatedOptions(NoobCategory);
  if (!cl::ParseCommandLineOptions(argc, argv, "NoobScript interpreter\n", &errs()))
    return ExitBadUsage;

  if (InputFile.empty()) {
    if (CheckOnly) {
      errs() << "noob: --check needs a script to check\n";
      return ExitBadUsage;
    }
    return runRepl();
  }
  return runFile();
}
