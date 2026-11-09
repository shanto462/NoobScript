#pragma once

#include "noob/AST.h"
#include "noob/Diag.h"
#include "noob/Environment.h"
#include "noob/Runtime.h"
#include "noob/Value.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"

#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace noob {

/// How a statement finished. Payloads for Return and Throw are stored in the
/// interpreter. C++ exceptions are off, so this carries all non-local exits.
enum class Flow { Normal, Break, Continue, Return, Throw, Exit };

struct InterpreterOptions {
  bool trace = false;
  bool repl = false;
  RuntimeConfig runtime;
  std::vector<std::string> scriptArgs;
};

/// Per-fiber interpreter state. Each task and generator has its own.
struct TaskContext : Fiber {
  std::vector<Frame> callStack;
  PendingError pending;
  Value returnValue;
  GeneratorObj *generator = nullptr;
};

class Interpreter {
public:
  Interpreter(AstContext &ctx, Diag &diag, InterpreterOptions options);

  /// Runs top-level statements one at a time and returns the exit code.
  int runProgram(llvm::ArrayRef<const Stmt *> program);

  /// Runs one REPL statement, reporting errors without stopping. Returns false
  /// once the session should exit, with the code available from exitCode().
  bool runReplStatement(const Stmt *stmt);
  int exitCode() const { return exitCode_; }

  /// True once `touchgrass` has run.
  bool exited() const { return exitRequested_; }

  /// Registers the main script, so importing it counts as a circular import.
  void setMainScript(llvm::StringRef path);

  /// Switches on REPL rules, such as allowing top-level redeclaration.
  void enterRepl() { repl_ = true; }

  /// Raises a catchable runtime error. Always returns false, so callers can
  /// write `return fail(...)`.
  bool fail(llvm::SMLoc loc, const llvm::Twine &headline, const llvm::Twine &detail);

  /// Calls any callable value.
  bool call(const Value &callee, llvm::ArrayRef<Value> args, llvm::SMLoc loc, Value &out);

  /// Walks a list, string, map, generator, or group chat. `visit` returns
  /// false to stop early. Returns false only if the iteration itself failed.
  bool forEachValue(const Value &iterable, llvm::SMLoc loc,
                    llvm::function_ref<bool(Value)> visit);

  bool advanceGenerator(GeneratorObj *generator, llvm::SMLoc loc, Value &out, bool &done);
  bool awaitFirst(ListObj *tasks, llvm::SMLoc loc, Value &out);
  bool channelSend(ChannelObj *channel, Value value, llvm::SMLoc loc);
  void channelReceive(ChannelObj *channel, llvm::SMLoc loc, Value &out, bool &closed);
  void channelClose(ChannelObj *channel);

  Scheduler &scheduler() { return scheduler_; }
  std::mt19937_64 &random() { return random_; }

private:
  // Statements.
  Flow exec(const Stmt *stmt, Env &env);
  Flow execStmts(llvm::ArrayRef<const Stmt *> stmts, Env &env);
  Flow execBlock(const Block &block, Env &env);
  Flow execDecl(const DeclStmt *stmt, Env &env);
  Flow execAssign(const AssignStmt *stmt, Env &env);
  Flow execPrint(const PrintStmt *stmt, Env &env);
  Flow execIf(const IfStmt *stmt, Env &env);
  Flow execWhile(const WhileStmt *stmt, Env &env);
  Flow execFor(const ForStmt *stmt, Env &env);
  Flow execFunc(const FuncStmt *stmt, Env &env);
  Flow execReturn(const ReturnStmt *stmt, Env &env);
  Flow execThrow(const ThrowStmt *stmt, Env &env);
  Flow execTry(const TryStmt *stmt, Env &env);
  Flow execAssert(const AssertStmt *stmt, Env &env);
  Flow execExit(const ExitStmt *stmt, Env &env);
  Flow execServe(const ServeStmt *stmt, Env &env);
  Flow execMatch(const MatchStmt *stmt, Env &env);
  Flow execSquad(const SquadStmt *stmt, Env &env);
  Flow execRizzup(const RizzupStmt *stmt, Env &env);

  /// Runs `body` once per value in a fresh scope with the pattern bound.
  /// `body` returns false to stop. Returns false if the loop itself failed.
  bool runLoop(const LoopHeader &header, Env &env, llvm::function_ref<bool(Env &)> body);

  bool checkRedeclare(llvm::SMLoc loc, Symbol name, Env &env);
  Flow unwind() const { return exitRequested_ ? Flow::Exit : Flow::Throw; }

  // Patterns. Return false after raising an error. `matched` is the result.
  bool matchPattern(const Pattern &pattern, const Value &value, Env &env, bool isConst,
                    bool strict, bool &matched);
  bool bindPattern(const Pattern &pattern, const Value &value, Env &env, bool isConst,
                   bool strict);

  // Expressions. All return false after raising an error or requesting exit.
  bool eval(const Expr *expr, Env &env, Value &out);
  bool evalInterp(const InterpExpr *expr, Env &env, Value &out);
  bool evalList(const ListExpr *expr, Env &env, Value &out);
  bool evalMap(const MapExpr *expr, Env &env, Value &out);
  bool evalUnary(const UnaryExpr *expr, Env &env, Value &out);
  bool evalBinary(const BinaryExpr *expr, Env &env, Value &out);
  bool evalCall(const CallExpr *expr, Env &env, Value &out);
  bool evalMember(const MemberExpr *expr, Env &env, Value &out);
  bool evalOgMember(const OgMemberExpr *expr, Env &env, Value &out);
  bool evalSlice(const SliceExpr *expr, Env &env, Value &out);
  bool evalListComp(const ListCompExpr *expr, Env &env, Value &out);
  bool evalSpawn(const SpawnExpr *expr, Env &env, Value &out);
  bool evalAwait(const AwaitExpr *expr, Env &env, Value &out);
  bool evalArgs(llvm::ArrayRef<const Expr *> exprs, Env &env, llvm::SmallVectorImpl<Value> &out);
  bool evalCallParts(const CallExpr *expr, Env &env, Value &callee,
                     llvm::SmallVectorImpl<Value> &args, bool &skipped);
  bool sliceBound(const Expr *expr, Env &env, int64_t length, int64_t fallback, int64_t &out);
  bool binaryOp(BinaryOp op, const Value &lhs, const Value &rhs, llvm::SMLoc loc, Value &out);
  bool callFunction(FunctionObj *function, const Value *receiver, llvm::ArrayRef<Value> args,
                    llvm::SMLoc loc, Value &out);
  bool instantiate(const Value &squad, llvm::ArrayRef<Value> args, llvm::SMLoc loc, Value &out);
  bool indexGet(const Value &object, const Value &index, llvm::SMLoc loc, Value &out);
  bool indexSet(const Value &object, const Value &index, Value value, llvm::SMLoc loc);
  bool memberGet(const Value &object, Symbol name, llvm::SMLoc loc, Value &out);
  bool memberSet(const Value &object, Symbol name, Value value, llvm::SMLoc loc);
  bool checkedIndex(const Value &index, size_t size, llvm::StringRef what, llvm::SMLoc loc,
                    size_t &out);

  // Tasks.
  bool drive(std::function<void()> body, bool waitForAllTasks);
  void runTask(Value task, Value callee, std::vector<Value> args, llvm::SMLoc loc);
  void runGeneratorBody(GeneratorObj *generator);
  void waitForTask(TaskObj *task, llvm::SMLoc loc);
  bool finishAwait(TaskObj *task, Value &out);
  bool reportUnobservedFailures();
  void reportDeadlock();

  TaskContext &ctx() const { return *static_cast<TaskContext *>(scheduler_.current()); }
  bool failUndefined(llvm::SMLoc loc, Symbol name);
  bool stackExhausted() const;
  void trace(const Stmt *stmt);
  void reportUncaught(const PendingError &error);

  // The scheduler must outlive every object that can hold a fiber.
  Scheduler scheduler_;

  AstContext &ctx_;
  Diag &diag_;
  bool trace_;
  bool repl_;

  EnvRef builtins_;
  EnvRef globals_;
  Symbol meSymbol_;
  Symbol ogSymbol_;
  Symbol glowupSymbol_;

  bool exitRequested_ = false;
  bool aborted_ = false;
  int exitCode_ = 0;

  size_t liveTasks_ = 0;
  std::vector<Value> failedTasks_;
  llvm::SMLoc lastWaitLoc_;
  llvm::StringMap<Value> modules_;
  std::mt19937_64 random_;
};

/// Adds every built-in function to `env`, as constants.
void installBuiltins(AstContext &ctx, Env &env);

} // namespace noob
