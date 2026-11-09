#include "noob/Interpreter.h"

#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <sys/resource.h>

#include <algorithm>
#include <cmath>

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
static constexpr uintptr_t FallbackStackLimit = 6u << 20;

static constexpr const char *TypeHeadline =
    "those types are not compatible, it's giving divorce";
static constexpr const char *RecursionHeadline = "you're cooked (too much recursion)";

static uintptr_t currentStackAddress() {
  return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
}

/// Uses three quarters of the real stack, leaving room to report the error.
static uintptr_t computeStackLimit() {
  rlimit limit;
  if (getrlimit(RLIMIT_STACK, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY)
    return FallbackStackLimit;
  return limit.rlim_cur / 4 * 3;
}

Interpreter::Interpreter(AstContext &ctx, Diag &diag, bool trace, bool repl)
    : ctx_(ctx), diag_(diag), trace_(trace), repl_(repl),
      builtins_(new Env(nullptr)), globals_(new Env(builtins_.get())),
      stackBase_(currentStackAddress()), stackLimit_(computeStackLimit()),
      random_(std::random_device{}()) {
  installBuiltins(ctx_, *builtins_);
}

bool Interpreter::stackExhausted() const {
  return stackBase_ - currentStackAddress() > stackLimit_;
}

// ---- Entry points -----------------------------------------------------------

int Interpreter::runProgram(ArrayRef<const Stmt *> program) {
  for (const Stmt *stmt : program) {
    Flow flow = exec(stmt, *globals_);
    if (flow == Flow::Throw) {
      reportUncaught();
      return 1;
    }
    if (flow == Flow::Exit)
      return exitCode_;
  }
  return 0;
}

bool Interpreter::runReplStatement(const Stmt *stmt) {
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
    reportUncaught();
  outs().flush();
  return flow != Flow::Exit;
}

void Interpreter::reportUncaught() {
  diag_.report(pendingLoc_, pendingHeadline_, pendingDetail_);
  if (pendingDepth_ == 0)
    return;
  errs() << "call trace:\n";
  for (const Frame &frame : pendingFrames_)
    errs() << "  in " << frame.name << ", called at " << diag_.describe(frame.callSite)
           << "\n";
  if (pendingDepth_ > pendingFrames_.size())
    errs() << "  ...and " << (pendingDepth_ - pendingFrames_.size()) << " more\n";
}

bool Interpreter::fail(SMLoc loc, const Twine &headline, const Twine &detail) {
  pendingDetail_ = detail.str();
  pending_ = Value::string(pendingDetail_);
  pendingLoc_ = loc;
  pendingHeadline_ = headline.str();

  pendingDepth_ = callStack_.size();
  pendingFrames_.clear();
  size_t count = std::min(callStack_.size(), MaxTraceFrames);
  for (size_t i = 0; i < count; ++i)
    pendingFrames_.push_back(callStack_[callStack_.size() - 1 - i]);
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
  case Stmt::Kind::ForRange: return execForRange(cast<ForRangeStmt>(stmt), env);
  case Stmt::Kind::ForEach: return execForEach(cast<ForEachStmt>(stmt), env);
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
  TRY_FLOW(checkRedeclare(stmt->loc, stmt->name, env));
  env.define(stmt->name, std::move(value), stmt->isConst);
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

bool Interpreter::runPass(const Block &body, Env &env, Symbol var, Value value, Flow &result) {
  // A fresh scope each pass, so closures capture this pass's value.
  EnvRef pass(new Env(&env));
  pass->define(var, std::move(value), /*isConst=*/false);
  Flow flow = execBlock(body, *pass);
  if (flow == Flow::Normal || flow == Flow::Continue)
    return true;
  result = flow == Flow::Break ? Flow::Normal : flow;
  return false;
}

Flow Interpreter::execForRange(const ForRangeStmt *stmt, Env &env) {
  Value start, end, stepValue = Value::number(1);
  TRY_FLOW(eval(stmt->start, env, start));
  TRY_FLOW(eval(stmt->end, env, end));
  if (stmt->step)
    TRY_FLOW(eval(stmt->step, env, stepValue));

  if (!start.isNumber() || !end.isNumber() || !stepValue.isNumber()) {
    fail(stmt->start->loc, TypeHeadline, "a speedrun range needs numbers");
    return Flow::Throw;
  }
  double first = start.asNumber();
  double last = end.asNumber();
  double step = stepValue.asNumber();
  if (step == 0) {
    fail(stmt->step->loc, "a step of 0? that's not a speedrun, that's a standstill",
         "the 'by' step of a speedrun can't be 0");
    return Flow::Throw;
  }

  // Multiplying instead of accumulating avoids floating-point drift. The
  // positive form of each test also makes NaN end the loop.
  Flow result = Flow::Normal;
  for (uint64_t i = 0;; ++i) {
    double current = first + static_cast<double>(i) * step;
    if (!(step > 0 ? current <= last : current >= last))
      return Flow::Normal;
    if (!runPass(stmt->body, env, stmt->var, Value::number(current), result))
      return result;
  }
}

Flow Interpreter::execForEach(const ForEachStmt *stmt, Env &env) {
  Value iterable;
  TRY_FLOW(eval(stmt->iterable, env, iterable));

  Flow result = Flow::Normal;
  if (auto *list = iterable.as<ListObj>()) {
    // Re-checks the size each pass, since the body may change the list.
    for (size_t i = 0; i < list->items.size(); ++i)
      if (!runPass(stmt->body, env, stmt->var, list->items[i], result))
        return result;
    return Flow::Normal;
  }
  if (auto *string = iterable.as<StringObj>()) {
    size_t length = string->length();
    for (size_t i = 0; i < length; ++i) {
      size_t from = string->byteOffset(i);
      size_t to = string->byteOffset(i + 1);
      Value character = Value::string(string->text().slice(from, to).str());
      if (!runPass(stmt->body, env, stmt->var, std::move(character), result))
        return result;
    }
    return Flow::Normal;
  }
  if (auto *map = iterable.as<MapObj>()) {
    for (Value &key : map->keys())
      if (!runPass(stmt->body, env, stmt->var, std::move(key), result))
        return result;
    return Flow::Normal;
  }

  fail(stmt->iterable->loc, TypeHeadline,
       "can't speedrun through a " + iterable.typeName() +
           ", it needs a list, string, map, or range");
  return Flow::Throw;
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
  returnValue_ = std::move(value);
  return Flow::Return;
}

Flow Interpreter::execThrow(const ThrowStmt *stmt, Env &env) {
  Value value;
  TRY_FLOW(eval(stmt->value, env, value));
  fail(stmt->loc, "you yeeted and nobody caught it", "uncaught yeet: " + value.display());
  pending_ = std::move(value);
  return Flow::Throw;
}

Flow Interpreter::execTry(const TryStmt *stmt, Env &env) {
  Flow flow = execBlock(stmt->body, env);
  if (flow != Flow::Throw)
    return flow;

  EnvRef scope(new Env(&env));
  scope->define(stmt->errorName, std::move(pending_), /*isConst=*/false);
  pending_ = Value();
  return execBlock(stmt->handler, *scope);
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
    TRY(eval(index->index, env, key));
    return indexGet(object, key, index->loc, out);
  }
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

bool Interpreter::evalList(const ListExpr *expr, Env &env, Value &out) {
  std::vector<Value> items;
  items.reserve(expr->items.size());
  for (const Expr *item : expr->items) {
    Value value;
    TRY(eval(item, env, value));
    items.push_back(std::move(value));
  }
  out = Value(new ListObj(std::move(items)));
  return true;
}

bool Interpreter::evalMap(const MapExpr *expr, Env &env, Value &out) {
  auto *map = new MapObj();
  Value holder(map);
  for (const MapPair &pair : expr->pairs) {
    Value key, value;
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

bool Interpreter::evalCall(const CallExpr *expr, Env &env, Value &out) {
  Value callee;
  TRY(eval(expr->callee, env, callee));
  SmallVector<Value, 4> args;
  args.reserve(expr->args.size());
  for (const Expr *arg : expr->args) {
    Value value;
    TRY(eval(arg, env, value));
    args.push_back(std::move(value));
  }
  return call(callee, args, expr->loc, out);
}

bool Interpreter::call(const Value &callee, ArrayRef<Value> args, SMLoc loc, Value &out) {
  auto arityError = [&](StringRef name, const Twine &wanted) {
    return fail(loc, "wrong number of args, count again",
                "`" + name + "` wants " + wanted + " but got " + Twine(args.size()));
  };

  if (auto *builtin = callee.as<BuiltinObj>()) {
    if (args.size() < builtin->minArgs || args.size() > builtin->maxArgs) {
      std::string wanted = builtin->minArgs == builtin->maxArgs
                               ? std::to_string(builtin->minArgs)
                               : std::to_string(builtin->minArgs) + " to " +
                                     std::to_string(builtin->maxArgs);
      return arityError(builtin->name, wanted);
    }
    out = Value();
    BuiltinCall context{*this, builtin->name, args, loc, out};
    return builtin->fn(context);
  }

  auto *function = callee.as<FunctionObj>();
  if (!function)
    return fail(loc, "that's not a function, that's just a " + callee.typeName(),
                "tried to call a " + callee.typeName());

  const FunctionDecl *decl = function->decl;
  StringRef name = decl->name ? decl->name->text : StringRef("<anonymous cook>");
  if (args.size() != decl->params.size())
    return arityError(name, Twine(decl->params.size()));
  if (callStack_.size() >= MaxCallDepth)
    return fail(loc, RecursionHeadline,
                "function calls went more than " + Twine(MaxCallDepth) + " deep");

  EnvRef scope(new Env(function->closure.get()));
  for (size_t i = 0; i < args.size(); ++i)
    scope->define(decl->params[i], args[i], /*isConst=*/false);

  callStack_.push_back({name, loc});
  Flow flow = execStmts(decl->body.stmts, *scope);
  callStack_.pop_back();

  if (flow == Flow::Throw || flow == Flow::Exit)
    return false;
  if (flow == Flow::Return)
    out = std::move(returnValue_);
  else
    out = Value();
  returnValue_ = Value();
  return true;
}

bool Interpreter::checkedIndex(const Value &index, size_t size, StringRef what, SMLoc loc,
                               size_t &out) {
  if (!index.isNumber() || index.asNumber() != std::floor(index.asNumber()))
    return fail(loc, TypeHeadline,
                what + " index must be a whole number, got " + index.repr());
  double number = index.asNumber();
  if (number < 0 || number >= static_cast<double>(size))
    return fail(loc, "that index is out of bounds, touch grass",
                "index " + formatNumber(number) + " on a " + what + " of length " +
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
