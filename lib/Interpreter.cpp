#include "noob/Interpreter.h"

#include "noob/Parser.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace llvm;

#define TRY(expr)                                                              \
  do {                                                                         \
    if (!(expr))                                                               \
      return false;                                                            \
  } while (false)

#define TRY_FLOW(expr)                                                         \
  do {                                                                         \
    if (!(expr))                                                               \
      return unwind();                                                         \
  } while (false)

namespace noob {

static constexpr size_t MaxCallDepth = 1000;
static constexpr size_t MaxTraceFrames = 10;
/// Stack kept in reserve so that reporting an overflow can't itself overflow.
static constexpr uintptr_t StackReserve = 64u << 10;

static constexpr const char *TypeHeadline =
    "those types are not compatible, it's giving divorce";
static constexpr const char *RecursionHeadline = "you're cooked (too much recursion)";
static constexpr const char *PatternHeadline = "that doesn't fit the pattern";

static uintptr_t currentStackAddress() {
  return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
}

Interpreter::Interpreter(AstContext &ctx, Diag &diag, InterpreterOptions options)
    : scheduler_(options.runtime), ctx_(ctx), diag_(diag), trace_(options.trace),
      repl_(options.repl), builtins_(new Env(nullptr)), globals_(new Env(builtins_.get())),
      meSymbol_(ctx.intern("me")), ogSymbol_(ctx.intern("og")),
      glowupSymbol_(ctx.intern("glowup")), random_(std::random_device{}()) {
  installBuiltins(ctx_, *builtins_);

  std::vector<Value> args;
  for (const std::string &arg : options.scriptArgs)
    args.push_back(Value::string(arg));
  builtins_->define(ctx_.intern("cliargs"), Value(new ListObj(std::move(args))),
                    /*isConst=*/true);
}

bool Interpreter::stackExhausted() const {
  return currentStackAddress() < scheduler_.current()->stackLow() + StackReserve;
}

// ---- Entry points -----------------------------------------------------------

bool Interpreter::drive(std::function<void()> body, bool waitForAllTasks) {
  // Shared, because a deadlocked REPL statement can outlive this call and be
  // woken by a later one.
  auto finished = std::make_shared<bool>(false);
  auto *main = new TaskContext();
  size_t stackSize = std::max<size_t>(MainStackSize, scheduler_.config().stackSize);
  ++liveTasks_;
  bool started = scheduler_.start(
      main,
      [this, finished, body = std::move(body)] {
        body();
        *finished = true;
        --liveTasks_;
      },
      stackSize);
  if (!started) {
    delete main;
    --liveTasks_;
    errs() << "noob: couldn't allocate a stack for the main script\n";
    aborted_ = true;
    return true;
  }
  return scheduler_.runUntil([&] {
    return exitRequested_ || aborted_ || (waitForAllTasks ? liveTasks_ == 0 : *finished);
  });
}

int Interpreter::runProgram(ArrayRef<const Stmt *> program) {
  aborted_ = false;
  bool completed = drive(
      [this, program] {
        for (const Stmt *stmt : program) {
          Flow flow = exec(stmt, *globals_);
          if (flow == Flow::Exit)
            return;
          if (flow == Flow::Throw) {
            reportUncaught(ctx().pending);
            aborted_ = true;
            return;
          }
        }
      },
      /*waitForAllTasks=*/true);

  if (!completed) {
    reportDeadlock();
    return 1;
  }
  if (exitRequested_)
    return exitCode_;
  if (aborted_)
    return 1;
  return reportUnobservedFailures() ? 1 : 0;
}

bool Interpreter::runReplStatement(const Stmt *stmt) {
  bool completed = drive(
      [this, stmt] {
        Flow flow = Flow::Normal;
        if (const auto *exprStmt = dyn_cast<ExprStmt>(stmt)) {
          Value value;
          if (eval(exprStmt->expr, *globals_, value)) {
            if (!value.isNpc())
              outs() << value.repr() << "\n";
          } else {
            flow = unwind();
          }
        } else {
          flow = exec(stmt, *globals_);
        }
        if (flow == Flow::Throw)
          reportUncaught(ctx().pending);
      },
      /*waitForAllTasks=*/false);

  if (!completed)
    reportDeadlock();
  reportUnobservedFailures();
  outs().flush();
  return !exitRequested_;
}

void Interpreter::setMainScript(StringRef path) {
  SmallString<256> realPath;
  if (!sys::fs::real_path(path, realPath))
    modules_[realPath] = Value(new ModuleObj(ctx_.save(sys::path::stem(realPath)), globals_));
}

void Interpreter::reportDeadlock() {
  diag_.report(lastWaitLoc_, "everyone's left on read (deadlock)",
               "every task is waiting and nothing can wake any of them");
}

bool Interpreter::reportUnobservedFailures() {
  bool any = false;
  for (const Value &value : failedTasks_) {
    auto *task = value.as<TaskObj>();
    if (task->observed)
      continue;
    reportUncaught(task->error);
    task->observed = true;
    any = true;
  }
  failedTasks_.clear();
  return any;
}

void Interpreter::reportUncaught(const PendingError &error) {
  diag_.report(error.loc, error.headline, error.detail);
  if (error.depth == 0)
    return;
  errs() << "call trace:\n";
  for (const Frame &frame : error.frames)
    errs() << "  in " << frame.name << ", called at " << diag_.describe(frame.callSite) << "\n";
  if (error.depth > error.frames.size())
    errs() << "  ...and " << (error.depth - error.frames.size()) << " more\n";
}

bool Interpreter::fail(SMLoc loc, const Twine &headline, const Twine &detail) {
  TaskContext &context = ctx();
  PendingError &error = context.pending;
  error.detail = detail.str();
  error.value = Value::string(error.detail);
  error.loc = loc;
  error.headline = headline.str();

  error.depth = context.callStack.size();
  error.frames.clear();
  size_t count = std::min(context.callStack.size(), MaxTraceFrames);
  for (size_t i = 0; i < count; ++i)
    error.frames.push_back(context.callStack[context.callStack.size() - 1 - i]);
  return false;
}

bool Interpreter::failUndefined(SMLoc loc, Symbol name) {
  return fail(loc, "who is `" + name->text + "`?? never met them",
              "`" + name->text + "` hasn't been declared with lowkey or highkey");
}

void Interpreter::trace(const Stmt *stmt) {
  auto [line, text] = diag_.lineAt(stmt->loc);
  outs().flush();
  errs() << "[trace] " << line << " | " << text.ltrim() << "\n";
}

// ---- Statements -------------------------------------------------------------

Flow Interpreter::exec(const Stmt *stmt, Env &env) {
  if (trace_)
    trace(stmt);

  switch (stmt->kind) {
  case Stmt::Kind::Decl: return execDecl(cast<DeclStmt>(stmt), env);
  case Stmt::Kind::Assign: return execAssign(cast<AssignStmt>(stmt), env);
  case Stmt::Kind::Print: return execPrint(cast<PrintStmt>(stmt), env);
  case Stmt::Kind::If: return execIf(cast<IfStmt>(stmt), env);
  case Stmt::Kind::While: return execWhile(cast<WhileStmt>(stmt), env);
  case Stmt::Kind::For: return execFor(cast<ForStmt>(stmt), env);
  case Stmt::Kind::Serve: return execServe(cast<ServeStmt>(stmt), env);
  case Stmt::Kind::Match: return execMatch(cast<MatchStmt>(stmt), env);
  case Stmt::Kind::Squad: return execSquad(cast<SquadStmt>(stmt), env);
  case Stmt::Kind::Rizzup: return execRizzup(cast<RizzupStmt>(stmt), env);
  case Stmt::Kind::Func: return execFunc(cast<FuncStmt>(stmt), env);
  case Stmt::Kind::Return: return execReturn(cast<ReturnStmt>(stmt), env);
  case Stmt::Kind::Throw: return execThrow(cast<ThrowStmt>(stmt), env);
  case Stmt::Kind::Try: return execTry(cast<TryStmt>(stmt), env);
  case Stmt::Kind::Assert: return execAssert(cast<AssertStmt>(stmt), env);
  case Stmt::Kind::Exit: return execExit(cast<ExitStmt>(stmt), env);
  case Stmt::Kind::Break: return Flow::Break;
  case Stmt::Kind::Continue: return Flow::Continue;
  case Stmt::Kind::Expr: {
    Value ignored;
    TRY_FLOW(eval(cast<ExprStmt>(stmt)->expr, env, ignored));
    return Flow::Normal;
  }
  }
  return Flow::Normal;
}

Flow Interpreter::execStmts(ArrayRef<const Stmt *> stmts, Env &env) {
  for (const Stmt *stmt : stmts) {
    Flow flow = exec(stmt, env);
    if (flow != Flow::Normal)
      return flow;
  }
  return Flow::Normal;
}

Flow Interpreter::execBlock(const Block &block, Env &env) {
  if (!block.needsScope)
    return execStmts(block.stmts, env);
  EnvRef scope(new Env(&env));
  return execStmts(block.stmts, *scope);
}

bool Interpreter::checkRedeclare(SMLoc loc, Symbol name, Env &env) {
  if ((repl_ && &env == globals_.get()) || !env.findLocal(name))
    return true;
  return fail(loc, "`" + name->text + "` already exists, pick a new name bestie",
              "`" + name->text + "` is already declared in this scope");
}

Flow Interpreter::execDecl(const DeclStmt *stmt, Env &env) {
  Value value;
  TRY_FLOW(eval(stmt->init, env, value));
  TRY_FLOW(bindPattern(*stmt->pattern, value, env, stmt->isConst, /*strict=*/true));
  return Flow::Normal;
}

static BinaryOp binaryOpFor(AssignOp op) {
  switch (op) {
  case AssignOp::Add: return BinaryOp::Add;
  case AssignOp::Sub: return BinaryOp::Sub;
  case AssignOp::Mul: return BinaryOp::Mul;
  case AssignOp::Div:
  case AssignOp::Set: break;
  }
  return BinaryOp::Div;
}

Flow Interpreter::execAssign(const AssignStmt *stmt, Env &env) {
  if (const auto *ident = dyn_cast<IdentExpr>(stmt->target)) {
    Value value;
    if (stmt->op == AssignOp::Set) {
      TRY_FLOW(eval(stmt->value, env, value));
    } else {
      Binding *binding = env.lookup(ident->name);
      if (!binding) {
        failUndefined(ident->loc, ident->name);
        return Flow::Throw;
      }
      Value current = binding->value;
      Value rhs;
      TRY_FLOW(eval(stmt->value, env, rhs));
      TRY_FLOW(binaryOp(binaryOpFor(stmt->op), current, rhs, stmt->loc, value));
    }

    // Look up last, because evaluating the value may have added bindings.
    Binding *binding = env.lookup(ident->name);
    if (!binding) {
      failUndefined(ident->loc, ident->name);
      return Flow::Throw;
    }
    if (binding->isConst) {
      fail(ident->loc, "`" + ident->name->text + "` is highkey, it's not changing for you",
           "`" + ident->name->text + "` is a constant and can't be reassigned");
      return Flow::Throw;
    }
    binding->value = std::move(value);
    return Flow::Normal;
  }

  if (const auto *member = dyn_cast<MemberExpr>(stmt->target)) {
    Value object, value;
    TRY_FLOW(eval(member->object, env, object));
    if (stmt->op == AssignOp::Set) {
      TRY_FLOW(eval(stmt->value, env, value));
    } else {
      Value current, rhs;
      TRY_FLOW(memberGet(object, member->name, member->loc, current));
      TRY_FLOW(eval(stmt->value, env, rhs));
      TRY_FLOW(binaryOp(binaryOpFor(stmt->op), current, rhs, stmt->loc, value));
    }
    TRY_FLOW(memberSet(object, member->name, std::move(value), member->loc));
    return Flow::Normal;
  }

  const auto *target = cast<IndexExpr>(stmt->target);
  Value object, index, value;
  TRY_FLOW(eval(target->object, env, object));
  TRY_FLOW(eval(target->index, env, index));
  if (stmt->op == AssignOp::Set) {
    TRY_FLOW(eval(stmt->value, env, value));
  } else {
    Value current, rhs;
    TRY_FLOW(indexGet(object, index, target->loc, current));
    TRY_FLOW(eval(stmt->value, env, rhs));
    TRY_FLOW(binaryOp(binaryOpFor(stmt->op), current, rhs, stmt->loc, value));
  }
  TRY_FLOW(indexSet(object, index, std::move(value), target->loc));
  return Flow::Normal;
}

Flow Interpreter::execPrint(const PrintStmt *stmt, Env &env) {
  std::string text;
  bool first = true;
  for (const Expr *arg : stmt->args) {
    Value value;
    TRY_FLOW(eval(arg, env, value));
    if (!first)
      text += ' ';
    first = false;
    text += value.display();
  }

  switch (stmt->printKind) {
  case PrintKind::Yap:
    outs() << text << "\n";
    break;
  case PrintKind::Mumble:
    outs() << text;
    outs().flush();
    break;
  case PrintKind::Bruh:
    outs().flush();
    errs() << text << "\n";
    break;
  }
  return Flow::Normal;
}

Flow Interpreter::execIf(const IfStmt *stmt, Env &env) {
  for (const IfArm &arm : stmt->arms) {
    Value condition;
    TRY_FLOW(eval(arm.condition, env, condition));
    if (condition.truthy())
      return execBlock(arm.body, env);
  }
  if (stmt->elseBody)
    return execBlock(*stmt->elseBody, env);
  return Flow::Normal;
}

Flow Interpreter::execWhile(const WhileStmt *stmt, Env &env) {
  for (;;) {
    Value condition;
    TRY_FLOW(eval(stmt->condition, env, condition));
    if (!condition.truthy())
      return Flow::Normal;
    Flow flow = execBlock(stmt->body, env);
    if (flow == Flow::Break)
      return Flow::Normal;
    if (flow != Flow::Normal && flow != Flow::Continue)
      return flow;
  }
}

bool Interpreter::runLoop(const LoopHeader &header, Env &env, function_ref<bool(Env &)> body) {
  bool failed = false;
  auto pass = [&](Value value) {
    // A fresh scope each pass, so closures capture this pass's value.
    EnvRef scope(new Env(&env));
    if (!bindPattern(*header.pattern, value, *scope, /*isConst=*/false, /*strict=*/false)) {
      failed = true;
      return false;
    }
    return body(*scope);
  };

  if (!header.end) {
    Value iterable;
    TRY(eval(header.start, env, iterable));
    TRY(forEachValue(iterable, header.start->loc, pass));
    return !failed;
  }

  Value start, end, stepValue = Value::number(1);
  TRY(eval(header.start, env, start));
  TRY(eval(header.end, env, end));
  if (header.step)
    TRY(eval(header.step, env, stepValue));
  if (!start.isNumber() || !end.isNumber() || !stepValue.isNumber())
    return fail(header.start->loc, TypeHeadline, "a speedrun range needs numbers");

  double first = start.asNumber();
  double last = end.asNumber();
  double step = stepValue.asNumber();
  if (step == 0)
    return fail(header.step->loc, "a step of 0? that's not a speedrun, that's a standstill",
                "the 'by' step of a speedrun can't be 0");

  // Multiplying instead of accumulating avoids floating-point drift. The
  // positive form of each test also makes NaN end the loop.
  for (uint64_t i = 0;; ++i) {
    double current = first + static_cast<double>(i) * step;
    if (!(step > 0 ? current <= last : current >= last))
      return true;
    if (!pass(Value::number(current)))
      return !failed;
  }
}

Flow Interpreter::execFor(const ForStmt *stmt, Env &env) {
  Flow result = Flow::Normal;
  bool ok = runLoop(stmt->header, env, [&](Env &scope) {
    Flow flow = execBlock(stmt->body, scope);
    if (flow == Flow::Normal || flow == Flow::Continue)
      return true;
    result = flow == Flow::Break ? Flow::Normal : flow;
    return false;
  });
  return ok ? result : unwind();
}

bool Interpreter::forEachValue(const Value &iterable, SMLoc loc,
                               function_ref<bool(Value)> visit) {
  // Keeps the container alive even if the body drops the last reference.
  Value holder = iterable;
  if (auto *list = iterable.as<ListObj>()) {
    // Re-checks the size each pass, since the body may change the list.
    for (size_t i = 0; i < list->items.size(); ++i)
      if (!visit(list->items[i]))
        return true;
    return true;
  }
  if (auto *string = iterable.as<StringObj>()) {
    size_t length = string->length();
    for (size_t i = 0; i < length; ++i) {
      size_t from = string->byteOffset(i);
      size_t to = string->byteOffset(i + 1);
      if (!visit(Value::string(string->text().slice(from, to).str())))
        return true;
    }
    return true;
  }
  if (auto *map = iterable.as<MapObj>()) {
    for (Value &key : map->keys())
      if (!visit(std::move(key)))
        return true;
    return true;
  }
  if (auto *generator = iterable.as<GeneratorObj>()) {
    for (;;) {
      Value value;
      bool done = false;
      TRY(advanceGenerator(generator, loc, value, done));
      if (done || !visit(std::move(value)))
        return true;
    }
  }
  if (auto *channel = iterable.as<ChannelObj>()) {
    for (;;) {
      Value value;
      bool closed = false;
      channelReceive(channel, loc, value, closed);
      if (closed || !visit(std::move(value)))
        return true;
    }
  }
  return fail(loc, TypeHeadline,
              "can't speedrun through a " + iterable.typeName() +
                  ", it needs a list, string, map, range, generator, or groupchat");
}

Flow Interpreter::execFunc(const FuncStmt *stmt, Env &env) {
  TRY_FLOW(checkRedeclare(stmt->loc, stmt->decl->name, env));
  env.define(stmt->decl->name, Value(new FunctionObj(stmt->decl, EnvRef(&env))),
             /*isConst=*/false);
  return Flow::Normal;
}

Flow Interpreter::execReturn(const ReturnStmt *stmt, Env &env) {
  Value value;
  if (stmt->value)
    TRY_FLOW(eval(stmt->value, env, value));
  ctx().returnValue = std::move(value);
  return Flow::Return;
}

Flow Interpreter::execThrow(const ThrowStmt *stmt, Env &env) {
  Value value;
  TRY_FLOW(eval(stmt->value, env, value));
  fail(stmt->loc, "you yeeted and nobody caught it", "uncaught yeet: " + value.display());
  ctx().pending.value = std::move(value);
  return Flow::Throw;
}

Flow Interpreter::execTry(const TryStmt *stmt, Env &env) {
  Flow flow = execBlock(stmt->body, env);
  if (flow == Flow::Throw && stmt->errorName) {
    EnvRef scope(new Env(&env));
    scope->define(stmt->errorName, std::move(ctx().pending.value), /*isConst=*/false);
    flow = execBlock(stmt->handler, *scope);
  }
  if (!stmt->finallyBody || flow == Flow::Exit)
    return flow;

  // The cleanup may raise and catch errors of its own, so set aside whatever
  // was in flight. If the cleanup completes normally, that outcome stands.
  PendingError savedError = ctx().pending;
  Value savedReturn = ctx().returnValue;
  Flow cleanup = execBlock(*stmt->finallyBody, env);
  if (cleanup != Flow::Normal)
    return cleanup;
  ctx().pending = std::move(savedError);
  ctx().returnValue = std::move(savedReturn);
  return flow;
}

Flow Interpreter::execServe(const ServeStmt *stmt, Env &env) {
  Value value;
  if (stmt->value)
    TRY_FLOW(eval(stmt->value, env, value));
  ctx().generator->yielded = std::move(value);
  scheduler_.yieldToCaller();
  return Flow::Normal;
}

Flow Interpreter::execMatch(const MatchStmt *stmt, Env &env) {
  Value subject;
  TRY_FLOW(eval(stmt->subject, env, subject));
  for (const MatchArm &arm : stmt->arms) {
    EnvRef scope(new Env(&env));
    bool matched = false;
    TRY_FLOW(matchPattern(*arm.pattern, subject, *scope, /*isConst=*/false, /*strict=*/false,
                          matched));
    if (!matched)
      continue;
    if (arm.guard) {
      Value guard;
      TRY_FLOW(eval(arm.guard, *scope, guard));
      if (!guard.truthy())
        continue;
    }
    return execBlock(arm.body, *scope);
  }
  if (stmt->elseBody)
    return execBlock(*stmt->elseBody, env);
  return Flow::Normal;
}

Flow Interpreter::execSquad(const SquadStmt *stmt, Env &env) {
  auto *squad = new ClassObj(stmt->name->text);
  Value holder(squad);
  if (stmt->parent) {
    Value parent;
    TRY_FLOW(eval(stmt->parent, env, parent));
    if (!parent.as<ClassObj>()) {
      fail(stmt->parent->loc, TypeHeadline,
           "a squad can only copy another squad, got " + parent.typeName());
      return Flow::Throw;
    }
    squad->parent = std::move(parent);
  }
  for (const FunctionDecl *decl : stmt->methods)
    squad->methods[decl->name] = Value(new FunctionObj(decl, EnvRef(&env), squad));
  TRY_FLOW(checkRedeclare(stmt->loc, stmt->name, env));
  env.define(stmt->name, std::move(holder), /*isConst=*/false);
  return Flow::Normal;
}

Flow Interpreter::execRizzup(const RizzupStmt *stmt, Env &env) {
  static constexpr const char *ImportHeadline = "couldn't rizz up that file";
  SourceMgr &sources = diag_.sources();

  SmallString<256> path(stmt->path);
  if (!sys::path::is_absolute(path)) {
    unsigned buffer = sources.FindBufferContainingLoc(stmt->loc);
    StringRef importer = sources.getMemoryBuffer(buffer)->getBufferIdentifier();
    SmallString<256> joined(sys::path::parent_path(importer));
    sys::path::append(joined, stmt->path);
    path = joined;
  }
  SmallString<256> realPath;
  if (std::error_code error = sys::fs::real_path(path, realPath)) {
    fail(stmt->loc, ImportHeadline, "can't open '" + path + "': " + error.message());
    return Flow::Throw;
  }

  Value module;
  auto found = modules_.find(realPath);
  if (found != modules_.end()) {
    module = found->second;
    if (module.as<ModuleObj>()->loading) {
      fail(stmt->loc, "these files keep rizzing each other up (circular import)",
           "'" + stmt->path + "' is already in the middle of loading");
      return Flow::Throw;
    }
  } else {
    auto file = MemoryBuffer::getFile(realPath, /*IsText=*/true);
    if (!file) {
      fail(stmt->loc, ImportHeadline, "can't open '" + path + "': " + file.getError().message());
      return Flow::Throw;
    }
    StringRef source = (*file)->getBuffer();
    const UTF8 *begin = reinterpret_cast<const UTF8 *>(source.begin());
    if (!isLegalUTF8String(&begin, reinterpret_cast<const UTF8 *>(source.end()))) {
      fail(stmt->loc, ImportHeadline, "'" + path + "' isn't valid UTF-8");
      return Flow::Throw;
    }
    sources.AddNewSourceBuffer(std::move(*file), SMLoc());

    Parser parser(ctx_, diag_, source, /*allowShebang=*/true);
    ArrayRef<const Stmt *> program = parser.parseProgram();
    if (diag_.hasErrors()) {
      fail(stmt->loc, ImportHeadline, "syntax error in " + diag_.takeFirstError());
      return Flow::Throw;
    }

    EnvRef moduleEnv(new Env(builtins_.get()));
    auto *object = new ModuleObj(ctx_.save(sys::path::stem(realPath)), moduleEnv);
    module = Value(object);
    modules_[realPath] = module;

    Flow flow = execStmts(program, *moduleEnv);
    if (flow != Flow::Normal) {
      modules_.erase(realPath.str());
      return flow;
    }
    object->loading = false;
  }

  TRY_FLOW(checkRedeclare(stmt->loc, stmt->alias, env));
  env.define(stmt->alias, std::move(module), /*isConst=*/false);
  return Flow::Normal;
}

Flow Interpreter::execAssert(const AssertStmt *stmt, Env &env) {
  Value condition;
  TRY_FLOW(eval(stmt->condition, env, condition));
  if (condition.truthy())
    return Flow::Normal;

  std::string message = "the 'ongod' condition was false";
  if (stmt->message) {
    Value value;
    TRY_FLOW(eval(stmt->message, env, value));
    message = value.display();
  }
  fail(stmt->loc, "you swore on god and lied", message);
  return Flow::Throw;
}

Flow Interpreter::execExit(const ExitStmt *stmt, Env &env) {
  int code = 0;
  if (stmt->code) {
    Value value;
    TRY_FLOW(eval(stmt->code, env, value));
    if (!value.isNumber() || value.asNumber() != std::floor(value.asNumber()) ||
        std::fabs(value.asNumber()) > 1e9) {
      fail(stmt->code->loc, TypeHeadline,
           "touchgrass needs a whole number exit code, got " + value.repr());
      return Flow::Throw;
    }
    code = static_cast<int>(value.asNumber());
  }
  exitCode_ = code;
  exitRequested_ = true;
  return Flow::Exit;
}

// ---- Patterns ---------------------------------------------------------------

bool Interpreter::bindPattern(const Pattern &pattern, const Value &value, Env &env, bool isConst,
                              bool strict) {
  bool matched = false;
  TRY(matchPattern(pattern, value, env, isConst, strict, matched));
  if (matched)
    return true;
  return fail(pattern.loc, PatternHeadline, "this pattern can't take apart " + value.repr());
}

bool Interpreter::matchPattern(const Pattern &pattern, const Value &value, Env &env, bool isConst,
                               bool strict, bool &matched) {
  matched = false;
  switch (pattern.kind) {
  case Pattern::Kind::Wildcard:
    matched = true;
    return true;
  case Pattern::Kind::Bind:
    if (strict)
      TRY(checkRedeclare(pattern.loc, pattern.name, env));
    env.define(pattern.name, value, isConst);
    matched = true;
    return true;
  case Pattern::Kind::Literal: {
    Value literal;
    TRY(eval(pattern.low, env, literal));
    matched = literal.equals(value);
    return true;
  }
  case Pattern::Kind::Range: {
    Value low, high;
    TRY(eval(pattern.low, env, low));
    TRY(eval(pattern.high, env, high));
    if (!low.isNumber() || !high.isNumber())
      return fail(pattern.loc, TypeHeadline, "a range pattern needs numbers");
    matched = value.isNumber() && value.asNumber() >= low.asNumber() &&
              value.asNumber() <= high.asNumber();
    return true;
  }
  case Pattern::Kind::List: {
    auto *list = value.as<ListObj>();
    if (!list)
      return true;
    size_t count = pattern.items.size();
    if (pattern.rest ? list->items.size() < count : list->items.size() != count)
      return true;
    for (size_t i = 0; i < count; ++i) {
      TRY(matchPattern(*pattern.items[i], list->items[i], env, isConst, strict, matched));
      if (!matched)
        return true;
    }
    if (pattern.rest) {
      std::vector<Value> rest(list->items.begin() + count, list->items.end());
      TRY(matchPattern(*pattern.rest, Value(new ListObj(std::move(rest))), env, isConst, strict,
                       matched));
      return true;
    }
    matched = true;
    return true;
  }
  case Pattern::Kind::Map: {
    auto *map = value.as<MapObj>();
    auto *instance = value.as<InstanceObj>();
    if (!map && !instance)
      return true;
    for (const MapPatternEntry &entry : pattern.entries) {
      Value key;
      TRY(eval(entry.key, env, key));
      const Value *field = nullptr;
      if (map) {
        if (MapObj::isValidKey(key))
          field = map->get(key);
      } else if (auto *name = key.as<StringObj>()) {
        auto it = instance->fields.find(ctx_.intern(name->text()));
        if (it != instance->fields.end())
          field = &it->second;
      }
      matched = false;
      if (!field)
        return true;
      Value fieldValue = *field;
      TRY(matchPattern(*entry.pattern, fieldValue, env, isConst, strict, matched));
      if (!matched)
        return true;
    }
    matched = true;
    return true;
  }
  case Pattern::Kind::Or:
    for (const Pattern *alternative : pattern.items) {
      TRY(matchPattern(*alternative, value, env, isConst, strict, matched));
      if (matched)
        return true;
    }
    return true;
  }
  return true;
}

// ---- Expressions ------------------------------------------------------------

bool Interpreter::eval(const Expr *expr, Env &env, Value &out) {
  if (stackExhausted())
    return fail(expr->loc, RecursionHeadline, "ran out of stack evaluating this expression");

  switch (expr->kind) {
  case Expr::Kind::Number:
    out = Value::number(cast<NumberExpr>(expr)->value);
    return true;
  case Expr::Kind::String:
    out = Value::string(cast<StringExpr>(expr)->value.str());
    return true;
  case Expr::Kind::Interp:
    return evalInterp(cast<InterpExpr>(expr), env, out);
  case Expr::Kind::Bool:
    out = Value::boolean(cast<BoolExpr>(expr)->value);
    return true;
  case Expr::Kind::Npc:
    out = Value();
    return true;
  case Expr::Kind::Ident: {
    const auto *ident = cast<IdentExpr>(expr);
    Binding *binding = env.lookup(ident->name);
    if (!binding)
      return failUndefined(ident->loc, ident->name);
    out = binding->value;
    return true;
  }
  case Expr::Kind::List:
    return evalList(cast<ListExpr>(expr), env, out);
  case Expr::Kind::Map:
    return evalMap(cast<MapExpr>(expr), env, out);
  case Expr::Kind::Lambda:
    out = Value(new FunctionObj(cast<LambdaExpr>(expr)->decl, EnvRef(&env)));
    return true;
  case Expr::Kind::Unary:
    return evalUnary(cast<UnaryExpr>(expr), env, out);
  case Expr::Kind::Binary:
    return evalBinary(cast<BinaryExpr>(expr), env, out);
  case Expr::Kind::Call:
    return evalCall(cast<CallExpr>(expr), env, out);
  case Expr::Kind::Index: {
    const auto *index = cast<IndexExpr>(expr);
    Value object, key;
    TRY(eval(index->object, env, object));
    if (index->optional && object.isNpc()) {
      out = Value();
      return true;
    }
    TRY(eval(index->index, env, key));
    return indexGet(object, key, index->loc, out);
  }
  case Expr::Kind::Member:
    return evalMember(cast<MemberExpr>(expr), env, out);
  case Expr::Kind::OgMember:
    return evalOgMember(cast<OgMemberExpr>(expr), env, out);
  case Expr::Kind::Slice:
    return evalSlice(cast<SliceExpr>(expr), env, out);
  case Expr::Kind::Ternary: {
    const auto *ternary = cast<TernaryExpr>(expr);
    Value condition;
    TRY(eval(ternary->condition, env, condition));
    return eval(condition.truthy() ? ternary->thenValue : ternary->elseValue, env, out);
  }
  case Expr::Kind::Spread:
    return fail(expr->loc, TypeHeadline, "'...' only works inside a list, map, or call");
  case Expr::Kind::ListComp:
    return evalListComp(cast<ListCompExpr>(expr), env, out);
  case Expr::Kind::Spawn:
    return evalSpawn(cast<SpawnExpr>(expr), env, out);
  case Expr::Kind::Await:
    return evalAwait(cast<AwaitExpr>(expr), env, out);
  }
  return false;
}

bool Interpreter::evalInterp(const InterpExpr *expr, Env &env, Value &out) {
  std::string text;
  for (const Expr *part : expr->parts) {
    if (const auto *literal = dyn_cast<StringExpr>(part)) {
      text += literal->value;
      continue;
    }
    Value value;
    TRY(eval(part, env, value));
    text += value.display();
  }
  out = Value::string(std::move(text));
  return true;
}

bool Interpreter::evalArgs(ArrayRef<const Expr *> exprs, Env &env, SmallVectorImpl<Value> &out) {
  for (const Expr *expr : exprs) {
    if (const auto *spread = dyn_cast<SpreadExpr>(expr)) {
      Value iterable;
      TRY(eval(spread->inner, env, iterable));
      TRY(forEachValue(iterable, spread->loc, [&](Value value) {
        out.push_back(std::move(value));
        return true;
      }));
      continue;
    }
    Value value;
    TRY(eval(expr, env, value));
    out.push_back(std::move(value));
  }
  return true;
}

bool Interpreter::evalList(const ListExpr *expr, Env &env, Value &out) {
  SmallVector<Value, 8> items;
  TRY(evalArgs(expr->items, env, items));
  out = Value(new ListObj(std::vector<Value>(items.begin(), items.end())));
  return true;
}

bool Interpreter::evalMap(const MapExpr *expr, Env &env, Value &out) {
  auto *map = new MapObj();
  Value holder(map);
  for (const MapPair &pair : expr->pairs) {
    Value key, value;
    if (!pair.key) {
      TRY(eval(pair.value, env, value));
      auto *source = value.as<MapObj>();
      if (!source)
        return fail(pair.value->loc, TypeHeadline, "can only spread a map here, got " + value.typeName());
      for (const MapObj::Entry &entry : source->entries())
        if (entry.live)
          map->set(entry.key, entry.value);
      continue;
    }
    TRY(eval(pair.key, env, key));
    TRY(eval(pair.value, env, value));
    if (!MapObj::isValidKey(key))
      return fail(pair.key->loc, TypeHeadline,
                  "map keys must be strings or numbers, got " + key.typeName());
    map->set(key, std::move(value));
  }
  out = std::move(holder);
  return true;
}

bool Interpreter::evalUnary(const UnaryExpr *expr, Env &env, Value &out) {
  Value operand;
  TRY(eval(expr->operand, env, operand));
  if (expr->op == UnaryOp::Not) {
    out = Value::boolean(!operand.truthy());
    return true;
  }
  if (!operand.isNumber())
    return fail(expr->loc, TypeHeadline, "can't negate a " + operand.typeName());
  out = Value::number(-operand.asNumber());
  return true;
}

bool Interpreter::evalBinary(const BinaryExpr *expr, Env &env, Value &out) {
  Value lhs;
  TRY(eval(expr->lhs, env, lhs));

  if (expr->op == BinaryOp::Nullish) {
    if (!lhs.isNpc()) {
      out = std::move(lhs);
      return true;
    }
    return eval(expr->rhs, env, out);
  }

  if (expr->op == BinaryOp::And || expr->op == BinaryOp::Or) {
    bool shortCircuit = lhs.truthy() == (expr->op == BinaryOp::Or);
    if (shortCircuit) {
      out = Value::boolean(lhs.truthy());
      return true;
    }
    Value rhs;
    TRY(eval(expr->rhs, env, rhs));
    out = Value::boolean(rhs.truthy());
    return true;
  }

  Value rhs;
  TRY(eval(expr->rhs, env, rhs));
  return binaryOp(expr->op, lhs, rhs, expr->loc, out);
}

static StringRef opSpelling(BinaryOp op) {
  switch (op) {
  case BinaryOp::Add: return "+";
  case BinaryOp::Sub: return "-";
  case BinaryOp::Mul: return "*";
  case BinaryOp::Div: return "/";
  case BinaryOp::Mod: return "ratio";
  case BinaryOp::Pow: return "**";
  case BinaryOp::Eq: return "==";
  case BinaryOp::Ne: return "!=";
  case BinaryOp::Lt: return "<";
  case BinaryOp::Le: return "<=";
  case BinaryOp::Gt: return ">";
  case BinaryOp::Ge: return ">=";
  case BinaryOp::And: return "&&";
  case BinaryOp::Or: return "||";
  case BinaryOp::Nullish: return "??";
  }
  return "?";
}

/// Modulo whose result takes the sign of the divisor, so `-1 ratio 3` is 2.
static double flooredMod(double a, double b) {
  double remainder = std::fmod(a, b);
  if (remainder != 0 && (remainder < 0) != (b < 0))
    remainder += b;
  return remainder;
}

bool Interpreter::binaryOp(BinaryOp op, const Value &lhs, const Value &rhs, SMLoc loc,
                           Value &out) {
  auto typeError = [&] {
    return fail(loc, TypeHeadline,
                "can't use '" + opSpelling(op) + "' on " + lhs.typeName() + " and " +
                    rhs.typeName());
  };

  if (op == BinaryOp::Eq || op == BinaryOp::Ne) {
    out = Value::boolean(lhs.equals(rhs) == (op == BinaryOp::Eq));
    return true;
  }

  if (op == BinaryOp::Add && !(lhs.isNumber() && rhs.isNumber())) {
    if (lhs.as<StringObj>() || rhs.as<StringObj>()) {
      out = Value::string(lhs.display() + rhs.display());
      return true;
    }
    auto *a = lhs.as<ListObj>();
    auto *b = rhs.as<ListObj>();
    if (!a || !b)
      return typeError();
    std::vector<Value> joined = a->items;
    joined.insert(joined.end(), b->items.begin(), b->items.end());
    out = Value(new ListObj(std::move(joined)));
    return true;
  }

  if (op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge) {
    auto compare = [&](auto a, auto b) {
      switch (op) {
      case BinaryOp::Lt: return a < b;
      case BinaryOp::Le: return a <= b;
      case BinaryOp::Gt: return a > b;
      default: return a >= b;
      }
    };
    if (lhs.isNumber() && rhs.isNumber()) {
      out = Value::boolean(compare(lhs.asNumber(), rhs.asNumber()));
      return true;
    }
    auto *a = lhs.as<StringObj>();
    auto *b = rhs.as<StringObj>();
    if (!a || !b)
      return typeError();
    out = Value::boolean(compare(a->text().compare(b->text()), 0));
    return true;
  }

  if (!lhs.isNumber() || !rhs.isNumber())
    return typeError();

  double a = lhs.asNumber();
  double b = rhs.asNumber();
  if ((op == BinaryOp::Div || op == BinaryOp::Mod) && b == 0)
    return fail(loc, "dividing by zero? in this economy?", "can't divide by zero");

  double result = 0;
  switch (op) {
  case BinaryOp::Add: result = a + b; break;
  case BinaryOp::Sub: result = a - b; break;
  case BinaryOp::Mul: result = a * b; break;
  case BinaryOp::Div: result = a / b; break;
  case BinaryOp::Mod: result = flooredMod(a, b); break;
  case BinaryOp::Pow: result = std::pow(a, b); break;
  default: break;
  }
  out = Value::number(result);
  return true;
}

bool Interpreter::evalCallParts(const CallExpr *expr, Env &env, Value &callee,
                                SmallVectorImpl<Value> &args, bool &skipped) {
  skipped = false;
  TRY(eval(expr->callee, env, callee));
  if (expr->optional && callee.isNpc()) {
    skipped = true;
    return true;
  }
  return evalArgs(expr->args, env, args);
}

bool Interpreter::evalCall(const CallExpr *expr, Env &env, Value &out) {
  Value callee;
  SmallVector<Value, 4> args;
  bool skipped;
  TRY(evalCallParts(expr, env, callee, args, skipped));
  if (skipped) {
    out = Value();
    return true;
  }
  return call(callee, args, expr->loc, out);
}

bool Interpreter::call(const Value &callee, ArrayRef<Value> args, SMLoc loc, Value &out) {
  if (auto *builtin = callee.as<BuiltinObj>()) {
    if (args.size() < builtin->minArgs || args.size() > builtin->maxArgs) {
      std::string wanted = builtin->minArgs == builtin->maxArgs
                               ? std::to_string(builtin->minArgs)
                               : std::to_string(builtin->minArgs) + " to " +
                                     std::to_string(builtin->maxArgs);
      return fail(loc, "wrong number of args, count again",
                  "`" + builtin->name + "` wants " + wanted + " but got " + Twine(args.size()));
    }
    out = Value();
    BuiltinCall context{*this, builtin->name, args, loc, out};
    return builtin->fn(context);
  }
  if (auto *function = callee.as<FunctionObj>())
    return callFunction(function, nullptr, args, loc, out);
  if (auto *bound = callee.as<BoundMethodObj>())
    return callFunction(bound->function.as<FunctionObj>(), &bound->receiver, args, loc, out);
  if (callee.as<ClassObj>())
    return instantiate(callee, args, loc, out);
  return fail(loc, "that's not a function, that's just a " + callee.typeName(),
              "tried to call a " + callee.typeName());
}

bool Interpreter::instantiate(const Value &squad, ArrayRef<Value> args, SMLoc loc, Value &out) {
  auto *classObj = squad.as<ClassObj>();
  Value instance(new InstanceObj(squad));
  if (const Value *constructor = classObj->findMethod(glowupSymbol_)) {
    Value ignored;
    Value function = *constructor;
    TRY(callFunction(function.as<FunctionObj>(), &instance, args, loc, ignored));
  } else if (!args.empty()) {
    return fail(loc, "wrong number of args, count again",
                "`" + classObj->name + "` has no glowup, so it takes no arguments but got " +
                    Twine(args.size()));
  }
  out = std::move(instance);
  return true;
}

bool Interpreter::callFunction(FunctionObj *function, const Value *receiver, ArrayRef<Value> args,
                               SMLoc loc, Value &out) {
  const FunctionDecl *decl = function->decl;
  StringRef name = decl->name ? decl->name->text : StringRef("<anonymous cook>");
  size_t total = decl->params.size();
  if (args.size() < decl->requiredParams || (!decl->hasRest && args.size() > total)) {
    std::string wanted = std::to_string(decl->requiredParams);
    if (decl->hasRest)
      wanted = "at least " + wanted;
    else if (decl->requiredParams != total)
      wanted += " to " + std::to_string(total);
    return fail(loc, "wrong number of args, count again",
                "`" + name + "` wants " + wanted + " but got " + Twine(args.size()));
  }

  TaskContext &context = ctx();
  if (context.callStack.size() >= MaxCallDepth)
    return fail(loc, RecursionHeadline,
                "function calls went more than " + Twine(MaxCallDepth) + " deep");

  EnvRef scope(new Env(function->closure.get()));
  if (receiver) {
    scope->define(meSymbol_, *receiver, /*isConst=*/true);
    scope->define(ogSymbol_, function->owner->parent, /*isConst=*/true);
  }
  for (size_t i = 0; i < total; ++i) {
    const Param &param = decl->params[i];
    if (param.isRest) {
      std::vector<Value> rest;
      if (args.size() > i)
        rest.assign(args.begin() + i, args.end());
      scope->define(param.name, Value(new ListObj(std::move(rest))), /*isConst=*/false);
    } else if (i < args.size()) {
      scope->define(param.name, args[i], /*isConst=*/false);
    } else {
      // Evaluated in the new scope so it can see earlier parameters.
      Value value;
      TRY(eval(param.defaultValue, *scope, value));
      scope->define(param.name, std::move(value), /*isConst=*/false);
    }
  }

  if (decl->isGenerator) {
    out = Value(new GeneratorObj(scheduler_, decl, std::move(scope)));
    return true;
  }

  context.callStack.push_back({name, loc});
  Flow flow = execStmts(decl->body.stmts, *scope);
  context.callStack.pop_back();

  if (flow == Flow::Throw || flow == Flow::Exit)
    return false;
  if (flow == Flow::Return)
    out = std::move(context.returnValue);
  else
    out = Value();
  context.returnValue = Value();
  return true;
}

bool Interpreter::evalMember(const MemberExpr *expr, Env &env, Value &out) {
  Value object;
  TRY(eval(expr->object, env, object));
  if (expr->optional && object.isNpc()) {
    out = Value();
    return true;
  }
  return memberGet(object, expr->name, expr->loc, out);
}

bool Interpreter::memberGet(const Value &object, Symbol name, SMLoc loc, Value &out) {
  auto missing = [&] {
    return fail(loc, "`" + object.typeName() + "` doesn't have a `" + name->text + "`",
                "there's no `" + name->text + "` on this " + object.typeName());
  };

  if (auto *instance = object.as<InstanceObj>()) {
    auto field = instance->fields.find(name);
    if (field != instance->fields.end()) {
      out = field->second;
      return true;
    }
    const Value *method = instance->classObj()->findMethod(name);
    if (!method)
      return missing();
    out = Value(new BoundMethodObj(object, *method));
    return true;
  }
  if (auto *map = object.as<MapObj>()) {
    const Value *found = map->get(Value::string(name->text.str()));
    out = found ? *found : Value();
    return true;
  }
  if (auto *module = object.as<ModuleObj>()) {
    Binding *binding = module->env->findLocal(name);
    if (!binding)
      return missing();
    out = binding->value;
    return true;
  }
  return fail(loc, TypeHeadline,
              "can't read `" + name->text + "` from a " + object.typeName());
}

bool Interpreter::memberSet(const Value &object, Symbol name, Value value, SMLoc loc) {
  if (auto *instance = object.as<InstanceObj>()) {
    instance->fields[name] = std::move(value);
    return true;
  }
  if (auto *map = object.as<MapObj>()) {
    map->set(Value::string(name->text.str()), std::move(value));
    return true;
  }
  return fail(loc, TypeHeadline, "can't set `" + name->text + "` on a " + object.typeName());
}

bool Interpreter::evalOgMember(const OgMemberExpr *expr, Env &env, Value &out) {
  Binding *me = env.lookup(meSymbol_);
  Binding *og = env.lookup(ogSymbol_);
  auto *parent = og ? og->value.as<ClassObj>() : nullptr;
  if (!me || !parent)
    return fail(expr->loc, "og who? this squad doesn't copy anyone",
                "'og' needs a squad declared with 'copies'");
  const Value *method = parent->findMethod(expr->name);
  if (!method)
    return fail(expr->loc, "`" + parent->name + "` doesn't have a `" + expr->name->text + "`",
                "there's no `" + expr->name->text + "` further up the chain");
  out = Value(new BoundMethodObj(me->value, *method));
  return true;
}

bool Interpreter::sliceBound(const Expr *expr, Env &env, int64_t length, int64_t fallback,
                             int64_t &out) {
  out = fallback;
  if (!expr)
    return true;
  Value value;
  TRY(eval(expr, env, value));
  if (!value.isNumber() || value.asNumber() != std::floor(value.asNumber()))
    return fail(expr->loc, TypeHeadline, "a slice bound must be a whole number, got " + value.repr());
  double bound = value.asNumber();
  if (bound < 0)
    bound += static_cast<double>(length);
  out = static_cast<int64_t>(std::clamp(bound, 0.0, static_cast<double>(length)));
  return true;
}

bool Interpreter::evalSlice(const SliceExpr *expr, Env &env, Value &out) {
  Value object;
  TRY(eval(expr->object, env, object));
  if (expr->optional && object.isNpc()) {
    out = Value();
    return true;
  }
  auto *list = object.as<ListObj>();
  auto *string = object.as<StringObj>();
  if (!list && !string)
    return fail(expr->loc, TypeHeadline, "can't slice a " + object.typeName());

  int64_t length = list ? list->items.size() : string->length();
  int64_t start, end;
  TRY(sliceBound(expr->start, env, length, 0, start));
  TRY(sliceBound(expr->end, env, length, length, end));
  end = std::max(start, end);
  if (list)
    out = Value(new ListObj(std::vector<Value>(list->items.begin() + start, list->items.begin() + end)));
  else
    out = Value::string(string->text().slice(string->byteOffset(start), string->byteOffset(end)).str());
  return true;
}

bool Interpreter::evalListComp(const ListCompExpr *expr, Env &env, Value &out) {
  std::vector<Value> items;
  bool failed = false;
  bool ok = runLoop(expr->header, env, [&](Env &scope) {
    if (expr->filter) {
      Value keep;
      if (!eval(expr->filter, scope, keep)) {
        failed = true;
        return false;
      }
      if (!keep.truthy())
        return true;
    }
    Value value;
    if (!eval(expr->element, scope, value)) {
      failed = true;
      return false;
    }
    items.push_back(std::move(value));
    return true;
  });
  if (!ok || failed)
    return false;
  out = Value(new ListObj(std::move(items)));
  return true;
}

// ---- Tasks ------------------------------------------------------------------

bool Interpreter::evalSpawn(const SpawnExpr *expr, Env &env, Value &out) {
  Value callee;
  SmallVector<Value, 4> args;
  bool skipped;
  TRY(evalCallParts(expr->call, env, callee, args, skipped));
  if (skipped) {
    out = Value();
    return true;
  }

  Value task(new TaskObj());
  auto *fiber = new TaskContext();
  ++liveTasks_;
  bool started = scheduler_.start(
      fiber,
      [this, task, callee, argv = std::vector<Value>(args.begin(), args.end()),
       loc = expr->loc]() mutable { runTask(task, callee, std::move(argv), loc); },
      scheduler_.config().stackSize);
  if (!started) {
    delete fiber;
    --liveTasks_;
    return fail(expr->loc, "too many tasks, the group chat is full",
                "couldn't allocate a stack for a new task");
  }
  out = std::move(task);
  return true;
}

void Interpreter::runTask(Value holder, Value callee, std::vector<Value> args, SMLoc loc) {
  auto *task = holder.as<TaskObj>();
  Value result;
  bool ok = call(callee, args, loc, result);

  task->done = true;
  if (ok) {
    task->result = std::move(result);
  } else if (!exitRequested_) {
    task->failed = true;
    task->error = std::move(ctx().pending);
    failedTasks_.push_back(holder);
  }
  for (Fiber *waiter : task->waiters)
    scheduler_.wake(waiter);
  --liveTasks_;
}

void Interpreter::waitForTask(TaskObj *task, SMLoc loc) {
  Fiber *self = scheduler_.current();
  while (!task->done) {
    task->waiters.push_back(self);
    lastWaitLoc_ = loc;
    scheduler_.park();
    llvm::erase(task->waiters, self);
  }
}

bool Interpreter::finishAwait(TaskObj *task, Value &out) {
  task->observed = true;
  if (task->failed) {
    ctx().pending = task->error;
    return false;
  }
  out = task->result;
  return true;
}

bool Interpreter::evalAwait(const AwaitExpr *expr, Env &env, Value &out) {
  Value value;
  TRY(eval(expr->operand, env, value));

  if (auto *task = value.as<TaskObj>()) {
    waitForTask(task, expr->loc);
    return !exitRequested_ && finishAwait(task, out);
  }
  if (auto *list = value.as<ListObj>()) {
    std::vector<Value> pending = list->items;
    std::vector<Value> results;
    results.reserve(pending.size());
    for (const Value &item : pending) {
      Value result = item;
      if (auto *task = item.as<TaskObj>()) {
        waitForTask(task, expr->loc);
        if (exitRequested_ || !finishAwait(task, result))
          return false;
      }
      results.push_back(std::move(result));
    }
    out = Value(new ListObj(std::move(results)));
    return true;
  }
  out = std::move(value);
  return true;
}

bool Interpreter::awaitFirst(ListObj *tasks, SMLoc loc, Value &out) {
  if (tasks->items.empty())
    return fail(loc, "firstone of nothing? there's nobody to wait for",
                "firstone needs at least one task");
  std::vector<Value> entrants = tasks->items;
  Fiber *self = scheduler_.current();
  for (;;) {
    for (const Value &entrant : entrants) {
      auto *task = entrant.as<TaskObj>();
      if (!task) {
        out = entrant;
        return true;
      }
      if (task->done) {
        // The race was the await, so the losers count as observed too.
        for (const Value &other : entrants)
          if (auto *otherTask = other.as<TaskObj>())
            otherTask->observed = true;
        return finishAwait(task, out);
      }
    }
    for (const Value &entrant : entrants)
      entrant.as<TaskObj>()->waiters.push_back(self);
    lastWaitLoc_ = loc;
    scheduler_.park();
    // Waiter lists hold raw fibers, so none may outlive this wait.
    for (const Value &entrant : entrants)
      llvm::erase(entrant.as<TaskObj>()->waiters, self);
  }
}

bool Interpreter::advanceGenerator(GeneratorObj *generator, SMLoc loc, Value &out, bool &done) {
  done = false;
  out = Value();
  if (generator->finished) {
    done = true;
    return true;
  }
  if (generator->running)
    return fail(loc, "that generator is already running",
                "a generator can't be advanced from inside itself");

  // The body may drop the last outside reference while it runs.
  Value keepAlive(generator);
  if (!generator->fiber) {
    auto *fiber = new TaskContext();
    fiber->generator = generator;
    bool prepared = scheduler_.prepare(fiber, [this, generator] { runGeneratorBody(generator); },
                                       scheduler_.config().stackSize);
    if (!prepared) {
      delete fiber;
      return fail(loc, "too many generators, the group chat is full",
                  "couldn't allocate a stack for a generator");
    }
    generator->fiber = fiber;
  }

  generator->running = true;
  scheduler_.resumeNested(generator->fiber);
  generator->running = false;

  if (exitRequested_)
    return false;
  if (generator->failed) {
    generator->failed = false;
    ctx().pending = std::move(generator->error);
    return false;
  }
  if (generator->finished) {
    done = true;
    return true;
  }
  out = std::move(generator->yielded);
  return true;
}

void Interpreter::runGeneratorBody(GeneratorObj *generator) {
  Flow flow = execStmts(generator->decl->body.stmts, *generator->scope);
  if (flow == Flow::Throw) {
    generator->failed = true;
    generator->error = std::move(ctx().pending);
  }
  generator->finished = true;
  generator->fiber = nullptr;
  generator->scope = nullptr;
}

bool Interpreter::channelSend(ChannelObj *channel, Value value, SMLoc loc) {
  Fiber *self = scheduler_.current();
  while (channel->capacity && channel->buffer.size() >= channel->capacity && !channel->closed) {
    channel->senders.push_back(self);
    lastWaitLoc_ = loc;
    scheduler_.park();
  }
  if (channel->closed)
    return fail(loc, "that group chat is dead, nobody's reading",
                "can't dm a groupchat after leavechat");
  channel->buffer.push_back(std::move(value));
  if (!channel->receivers.empty()) {
    scheduler_.wake(channel->receivers.front());
    channel->receivers.pop_front();
  }
  return true;
}

void Interpreter::channelReceive(ChannelObj *channel, SMLoc loc, Value &out, bool &closed) {
  Fiber *self = scheduler_.current();
  while (channel->buffer.empty() && !channel->closed) {
    channel->receivers.push_back(self);
    lastWaitLoc_ = loc;
    scheduler_.park();
  }
  closed = channel->buffer.empty();
  out = Value();
  if (closed)
    return;
  out = std::move(channel->buffer.front());
  channel->buffer.pop_front();
  if (!channel->senders.empty()) {
    scheduler_.wake(channel->senders.front());
    channel->senders.pop_front();
  }
}

void Interpreter::channelClose(ChannelObj *channel) {
  channel->closed = true;
  for (Fiber *fiber : channel->receivers)
    scheduler_.wake(fiber);
  for (Fiber *fiber : channel->senders)
    scheduler_.wake(fiber);
  channel->receivers.clear();
  channel->senders.clear();
}

bool Interpreter::checkedIndex(const Value &index, size_t size, StringRef what, SMLoc loc,
                               size_t &out) {
  if (!index.isNumber() || index.asNumber() != std::floor(index.asNumber()))
    return fail(loc, TypeHeadline,
                what + " index must be a whole number, got " + index.repr());
  double number = index.asNumber();
  if (number < 0)
    number += static_cast<double>(size);
  if (number < 0 || number >= static_cast<double>(size))
    return fail(loc, "that index is out of bounds, touch grass",
                "index " + formatNumber(index.asNumber()) + " on a " + what + " of length " +
                    Twine(size));
  out = static_cast<size_t>(number);
  return true;
}

bool Interpreter::indexGet(const Value &object, const Value &index, SMLoc loc, Value &out) {
  if (auto *list = object.as<ListObj>()) {
    size_t i;
    TRY(checkedIndex(index, list->items.size(), "list", loc, i));
    out = list->items[i];
    return true;
  }
  if (auto *string = object.as<StringObj>()) {
    size_t i;
    TRY(checkedIndex(index, string->length(), "string", loc, i));
    out = Value::string(string->text().slice(string->byteOffset(i), string->byteOffset(i + 1)).str());
    return true;
  }
  if (auto *map = object.as<MapObj>()) {
    if (!MapObj::isValidKey(index))
      return fail(loc, TypeHeadline,
                  "map keys must be strings or numbers, got " + index.typeName());
    const Value *found = map->get(index);
    out = found ? *found : Value();
    return true;
  }
  return fail(loc, TypeHeadline, "can't index into a " + object.typeName());
}

bool Interpreter::indexSet(const Value &object, const Value &index, Value value, SMLoc loc) {
  if (auto *list = object.as<ListObj>()) {
    size_t i;
    TRY(checkedIndex(index, list->items.size(), "list", loc, i));
    list->items[i] = std::move(value);
    return true;
  }
  if (auto *map = object.as<MapObj>()) {
    if (!MapObj::isValidKey(index))
      return fail(loc, TypeHeadline,
                  "map keys must be strings or numbers, got " + index.typeName());
    map->set(index, std::move(value));
    return true;
  }
  if (object.as<StringObj>())
    return fail(loc, TypeHeadline, "strings can't be changed in place");
  return fail(loc, TypeHeadline, "can't index into a " + object.typeName());
}

} // namespace noob
