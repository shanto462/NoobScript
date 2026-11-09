#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/SMLoc.h"

#include <cstring>
#include <memory>
#include <utility>

namespace noob {

/// An interned identifier. Two names are equal exactly when their pointers are.
struct Name {
  llvm::StringRef text;
};
using Symbol = const Name *;

/// Owns every AST node and identifier. Nodes are trivially destructible and
/// live until exit, so closures can always point into the AST.
class AstContext {
public:
  template <typename T, typename... Args> T *make(Args &&...args) {
    return new (alloc_.Allocate<T>()) T(std::forward<Args>(args)...);
  }

  template <typename T>
  llvm::ArrayRef<T> copy(const llvm::SmallVectorImpl<T> &items) {
    if (items.empty())
      return {};
    T *data = alloc_.Allocate<T>(items.size());
    std::uninitialized_copy(items.begin(), items.end(), data);
    return llvm::ArrayRef<T>(data, items.size());
  }

  llvm::StringRef save(llvm::StringRef text) {
    if (text.empty())
      return llvm::StringRef("", 0);
    char *data = alloc_.Allocate<char>(text.size());
    std::memcpy(data, text.data(), text.size());
    return llvm::StringRef(data, text.size());
  }

  Symbol intern(llvm::StringRef text) {
    Symbol &slot = names_[text];
    if (!slot)
      slot = make<Name>(Name{save(text)});
    return slot;
  }

private:
  llvm::BumpPtrAllocator alloc_;
  llvm::StringMap<Symbol> names_;
};

enum class BinaryOp { Add, Sub, Mul, Div, Mod, Pow, Eq, Ne, Lt, Le, Gt, Ge, And, Or };
enum class UnaryOp { Neg, Not };
enum class AssignOp { Set, Add, Sub, Mul, Div };
enum class PrintKind { Yap, Mumble, Bruh };

// ---- Expressions ------------------------------------------------------------

struct Expr {
  enum class Kind {
    Number, String, Interp, Bool, Npc, Ident, List, Map, Lambda, Unary, Binary, Call, Index,
  };
  Kind kind;
  llvm::SMLoc loc;

protected:
  Expr(Kind kind, llvm::SMLoc loc) : kind(kind), loc(loc) {}
};

template <Expr::Kind K> struct ExprOf : Expr {
  explicit ExprOf(llvm::SMLoc loc) : Expr(K, loc) {}
  static bool classof(const Expr *e) { return e->kind == K; }
};

struct NumberExpr : ExprOf<Expr::Kind::Number> {
  double value;
  NumberExpr(llvm::SMLoc loc, double value) : ExprOf(loc), value(value) {}
};

struct StringExpr : ExprOf<Expr::Kind::String> {
  llvm::StringRef value;
  StringExpr(llvm::SMLoc loc, llvm::StringRef value) : ExprOf(loc), value(value) {}
};

struct InterpExpr : ExprOf<Expr::Kind::Interp> {
  llvm::ArrayRef<const Expr *> parts;
  InterpExpr(llvm::SMLoc loc, llvm::ArrayRef<const Expr *> parts)
      : ExprOf(loc), parts(parts) {}
};

struct BoolExpr : ExprOf<Expr::Kind::Bool> {
  bool value;
  BoolExpr(llvm::SMLoc loc, bool value) : ExprOf(loc), value(value) {}
};

struct NpcExpr : ExprOf<Expr::Kind::Npc> {
  using ExprOf::ExprOf;
};

struct IdentExpr : ExprOf<Expr::Kind::Ident> {
  Symbol name;
  IdentExpr(llvm::SMLoc loc, Symbol name) : ExprOf(loc), name(name) {}
};

struct ListExpr : ExprOf<Expr::Kind::List> {
  llvm::ArrayRef<const Expr *> items;
  ListExpr(llvm::SMLoc loc, llvm::ArrayRef<const Expr *> items)
      : ExprOf(loc), items(items) {}
};

struct MapPair {
  const Expr *key;
  const Expr *value;
};

struct MapExpr : ExprOf<Expr::Kind::Map> {
  llvm::ArrayRef<MapPair> pairs;
  MapExpr(llvm::SMLoc loc, llvm::ArrayRef<MapPair> pairs)
      : ExprOf(loc), pairs(pairs) {}
};

struct UnaryExpr : ExprOf<Expr::Kind::Unary> {
  UnaryOp op;
  const Expr *operand;
  UnaryExpr(llvm::SMLoc loc, UnaryOp op, const Expr *operand)
      : ExprOf(loc), op(op), operand(operand) {}
};

struct BinaryExpr : ExprOf<Expr::Kind::Binary> {
  BinaryOp op;
  const Expr *lhs;
  const Expr *rhs;
  BinaryExpr(llvm::SMLoc loc, BinaryOp op, const Expr *lhs, const Expr *rhs)
      : ExprOf(loc), op(op), lhs(lhs), rhs(rhs) {}
};

struct CallExpr : ExprOf<Expr::Kind::Call> {
  const Expr *callee;
  llvm::ArrayRef<const Expr *> args;
  CallExpr(llvm::SMLoc loc, const Expr *callee, llvm::ArrayRef<const Expr *> args)
      : ExprOf(loc), callee(callee), args(args) {}
};

struct IndexExpr : ExprOf<Expr::Kind::Index> {
  const Expr *object;
  const Expr *index;
  IndexExpr(llvm::SMLoc loc, const Expr *object, const Expr *index)
      : ExprOf(loc), object(object), index(index) {}
};

// ---- Statements -------------------------------------------------------------

struct Stmt {
  enum class Kind {
    Decl, Assign, Print, If, While, ForRange, ForEach, Func, Return, Throw, Try,
    Assert, Exit, Break, Continue, Expr,
  };
  Kind kind;
  llvm::SMLoc loc;

protected:
  Stmt(Kind kind, llvm::SMLoc loc) : kind(kind), loc(loc) {}
};

template <Stmt::Kind K> struct StmtOf : Stmt {
  explicit StmtOf(llvm::SMLoc loc) : Stmt(K, loc) {}
  static bool classof(const Stmt *s) { return s->kind == K; }
};

struct Block {
  llvm::ArrayRef<const Stmt *> stmts;
  /// False when the block declares nothing directly, so it can skip allocating
  /// a scope.
  bool needsScope = false;
};

struct FunctionDecl {
  Symbol name; // Null for a lambda.
  llvm::ArrayRef<Symbol> params;
  Block body;
};

struct LambdaExpr : ExprOf<Expr::Kind::Lambda> {
  const FunctionDecl *decl;
  LambdaExpr(llvm::SMLoc loc, const FunctionDecl *decl) : ExprOf(loc), decl(decl) {}
};

struct DeclStmt : StmtOf<Stmt::Kind::Decl> {
  Symbol name;
  bool isConst;
  const Expr *init;
  DeclStmt(llvm::SMLoc loc, Symbol name, bool isConst, const Expr *init)
      : StmtOf(loc), name(name), isConst(isConst), init(init) {}
};

struct AssignStmt : StmtOf<Stmt::Kind::Assign> {
  const Expr *target; // IdentExpr or IndexExpr.
  AssignOp op;
  const Expr *value;
  AssignStmt(llvm::SMLoc loc, const Expr *target, AssignOp op, const Expr *value)
      : StmtOf(loc), target(target), op(op), value(value) {}
};

struct PrintStmt : StmtOf<Stmt::Kind::Print> {
  PrintKind printKind;
  llvm::ArrayRef<const Expr *> args;
  PrintStmt(llvm::SMLoc loc, PrintKind printKind, llvm::ArrayRef<const Expr *> args)
      : StmtOf(loc), printKind(printKind), args(args) {}
};

struct IfArm {
  const Expr *condition;
  Block body;
};

struct IfStmt : StmtOf<Stmt::Kind::If> {
  llvm::ArrayRef<IfArm> arms;
  const Block *elseBody; // Null when there's no `nah`.
  IfStmt(llvm::SMLoc loc, llvm::ArrayRef<IfArm> arms, const Block *elseBody)
      : StmtOf(loc), arms(arms), elseBody(elseBody) {}
};

struct WhileStmt : StmtOf<Stmt::Kind::While> {
  const Expr *condition;
  Block body;
  WhileStmt(llvm::SMLoc loc, const Expr *condition, Block body)
      : StmtOf(loc), condition(condition), body(body) {}
};

struct ForRangeStmt : StmtOf<Stmt::Kind::ForRange> {
  Symbol var;
  const Expr *start;
  const Expr *end;
  const Expr *step; // Null means 1.
  Block body;
  ForRangeStmt(llvm::SMLoc loc, Symbol var, const Expr *start, const Expr *end,
               const Expr *step, Block body)
      : StmtOf(loc), var(var), start(start), end(end), step(step), body(body) {}
};

struct ForEachStmt : StmtOf<Stmt::Kind::ForEach> {
  Symbol var;
  const Expr *iterable;
  Block body;
  ForEachStmt(llvm::SMLoc loc, Symbol var, const Expr *iterable, Block body)
      : StmtOf(loc), var(var), iterable(iterable), body(body) {}
};

struct FuncStmt : StmtOf<Stmt::Kind::Func> {
  const FunctionDecl *decl;
  FuncStmt(llvm::SMLoc loc, const FunctionDecl *decl) : StmtOf(loc), decl(decl) {}
};

struct ReturnStmt : StmtOf<Stmt::Kind::Return> {
  const Expr *value; // Null means npc.
  ReturnStmt(llvm::SMLoc loc, const Expr *value) : StmtOf(loc), value(value) {}
};

struct ThrowStmt : StmtOf<Stmt::Kind::Throw> {
  const Expr *value;
  ThrowStmt(llvm::SMLoc loc, const Expr *value) : StmtOf(loc), value(value) {}
};

struct TryStmt : StmtOf<Stmt::Kind::Try> {
  Block body;
  Symbol errorName;
  Block handler;
  TryStmt(llvm::SMLoc loc, Block body, Symbol errorName, Block handler)
      : StmtOf(loc), body(body), errorName(errorName), handler(handler) {}
};

struct AssertStmt : StmtOf<Stmt::Kind::Assert> {
  const Expr *condition;
  const Expr *message; // May be null.
  AssertStmt(llvm::SMLoc loc, const Expr *condition, const Expr *message)
      : StmtOf(loc), condition(condition), message(message) {}
};

struct ExitStmt : StmtOf<Stmt::Kind::Exit> {
  const Expr *code; // Null means 0.
  ExitStmt(llvm::SMLoc loc, const Expr *code) : StmtOf(loc), code(code) {}
};

struct BreakStmt : StmtOf<Stmt::Kind::Break> {
  using StmtOf::StmtOf;
};

struct ContinueStmt : StmtOf<Stmt::Kind::Continue> {
  using StmtOf::StmtOf;
};

struct ExprStmt : StmtOf<Stmt::Kind::Expr> {
  const Expr *expr;
  ExprStmt(llvm::SMLoc loc, const Expr *expr) : StmtOf(loc), expr(expr) {}
};

} // namespace noob
