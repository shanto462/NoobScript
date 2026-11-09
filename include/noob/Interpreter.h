#pragma once

#include "noob/AST.h"
#include "noob/Diag.h"
#include "noob/Environment.h"
#include "noob/Value.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace noob {

/// How a statement finished. Payloads for Return and Throw are stored in the
/// interpreter. C++ exceptions are off, so this carries all non-local exits.
enum class Flow { Normal, Break, Continue, Return, Throw, Exit };

class Interpreter {
public:
  Interpreter(AstContext &ctx, Diag &diag, bool trace, bool repl);

  /// Runs top-level statements one at a time and returns the exit code.
  int runProgram(llvm::ArrayRef<const Stmt *> program);

  /// Runs one REPL statement, reporting errors without stopping. Returns false
  /// once the session should exit, with the code available from exitCode().
  bool runReplStatement(const Stmt *stmt);
  int exitCode() const { return exitCode_; }

  /// Raises a catchable runtime error. Always returns false, so callers can
  /// write `return fail(...)`.
  bool fail(llvm::SMLoc loc, const llvm::Twine &headline, const llvm::Twine &detail);

  std::mt19937_64 &random() { return random_; }

private:
  struct Frame {
    llvm::StringRef name;
    llvm::SMLoc callSite;
  };

  // Statements.
  Flow exec(const Stmt *stmt, Env &env);
  Flow execStmts(llvm::ArrayRef<const Stmt *> stmts, Env &env);
  Flow execBlock(const Block &block, Env &env);
  Flow execDecl(const DeclStmt *stmt, Env &env);
  Flow execAssign(const AssignStmt *stmt, Env &env);
  Flow execPrint(const PrintStmt *stmt, Env &env);
  Flow execIf(const IfStmt *stmt, Env &env);
  Flow execWhile(const WhileStmt *stmt, Env &env);
  Flow execForRange(const ForRangeStmt *stmt, Env &env);
  Flow execForEach(const ForEachStmt *stmt, Env &env);
  Flow execFunc(const FuncStmt *stmt, Env &env);
  Flow execReturn(const ReturnStmt *stmt, Env &env);
  Flow execThrow(const ThrowStmt *stmt, Env &env);
  Flow execTry(const TryStmt *stmt, Env &env);
  Flow execAssert(const AssertStmt *stmt, Env &env);
  Flow execExit(const ExitStmt *stmt, Env &env);

  /// Runs one loop iteration. Returns false when the loop should stop, in
  /// which case `result` is what the loop statement completes with.
  bool runPass(const Block &body, Env &env, Symbol var, Value value, Flow &result);

  bool checkRedeclare(llvm::SMLoc loc, Symbol name, Env &env);
  Flow unwind() const { return exitRequested_ ? Flow::Exit : Flow::Throw; }

  // Expressions. All return false after raising an error or requesting exit.
  bool eval(const Expr *expr, Env &env, Value &out);
  bool evalInterp(const InterpExpr *expr, Env &env, Value &out);
  bool evalList(const ListExpr *expr, Env &env, Value &out);
  bool evalMap(const MapExpr *expr, Env &env, Value &out);
  bool evalUnary(const UnaryExpr *expr, Env &env, Value &out);
  bool evalBinary(const BinaryExpr *expr, Env &env, Value &out);
  bool evalCall(const CallExpr *expr, Env &env, Value &out);
  bool binaryOp(BinaryOp op, const Value &lhs, const Value &rhs, llvm::SMLoc loc, Value &out);
  bool call(const Value &callee, llvm::ArrayRef<Value> args, llvm::SMLoc loc, Value &out);
  bool indexGet(const Value &object, const Value &index, llvm::SMLoc loc, Value &out);
  bool indexSet(const Value &object, const Value &index, Value value, llvm::SMLoc loc);
  bool checkedIndex(const Value &index, size_t size, llvm::StringRef what, llvm::SMLoc loc,
                    size_t &out);

  bool failUndefined(llvm::SMLoc loc, Symbol name);
  bool stackExhausted() const;
  void trace(const Stmt *stmt);
  void reportUncaught();

  AstContext &ctx_;
  Diag &diag_;
  bool trace_;
  bool repl_;

  EnvRef builtins_;
  EnvRef globals_;

  Value returnValue_;
  bool exitRequested_ = false;
  int exitCode_ = 0;

  // The error currently unwinding.
  Value pending_;
  llvm::SMLoc pendingLoc_;
  std::string pendingHeadline_;
  std::string pendingDetail_;
  std::vector<Frame> pendingFrames_;
  size_t pendingDepth_ = 0;

  std::vector<Frame> callStack_;
  uintptr_t stackBase_;
  uintptr_t stackLimit_;
  std::mt19937_64 random_;
};

/// Adds every built-in function to `env`, as constants.
void installBuiltins(AstContext &ctx, Env &env);

} // namespace noob
