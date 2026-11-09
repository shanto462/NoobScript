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

enum class BinaryOp { Add, Sub, Mul, Div, Mod, Pow, Eq, Ne, Lt, Le, Gt, Ge, And, Or, Nullish };
enum class UnaryOp { Neg, Not };
enum class AssignOp { Set, Add, Sub, Mul, Div };
enum class PrintKind { Yap, Mumble, Bruh };

// ---- Expressions ------------------------------------------------------------

struct Expr {
  enum class Kind {
    Number, String, Interp, Bool, Npc, Ident, List, Map, Lambda, Unary, Binary, Call, Index,
    Member, OgMember, Slice, Ternary, Spread, ListComp, Spawn, Await,
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
  const Expr *key; // Null means `value` is spread into the map.
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
  bool optional; // Part of a `?.` chain: an npc callee gives npc.
  CallExpr(llvm::SMLoc loc, const Expr *callee, llvm::ArrayRef<const Expr *> args,
           bool optional = false)
      : ExprOf(loc), callee(callee), args(args), optional(optional) {}
};

struct IndexExpr : ExprOf<Expr::Kind::Index> {
  const Expr *object;
  const Expr *index;
  bool optional; // Part of a `?.` chain.
  IndexExpr(llvm::SMLoc loc, const Expr *object, const Expr *index, bool optional)
      : ExprOf(loc), object(object), index(index), optional(optional) {}
};

struct MemberExpr : ExprOf<Expr::Kind::Member> {
  const Expr *object;
  Symbol name;
  bool optional; // Part of a `?.` chain.
  MemberExpr(llvm::SMLoc loc, const Expr *object, Symbol name, bool optional)
      : ExprOf(loc), object(object), name(name), optional(optional) {}
};

struct OgMemberExpr : ExprOf<Expr::Kind::OgMember> {
  Symbol name;
  OgMemberExpr(llvm::SMLoc loc, Symbol name) : ExprOf(loc), name(name) {}
};

struct SliceExpr : ExprOf<Expr::Kind::Slice> {
  const Expr *object;
  const Expr *start; // May be null.
  const Expr *end;   // May be null.
  bool optional;     // Part of a `?.` chain.
  SliceExpr(llvm::SMLoc loc, const Expr *object, const Expr *start, const Expr *end,
            bool optional)
      : ExprOf(loc), object(object), start(start), end(end), optional(optional) {}
};

struct TernaryExpr : ExprOf<Expr::Kind::Ternary> {
  const Expr *condition;
  const Expr *thenValue;
  const Expr *elseValue;
  TernaryExpr(llvm::SMLoc loc, const Expr *condition, const Expr *thenValue,
              const Expr *elseValue)
      : ExprOf(loc), condition(condition), thenValue(thenValue), elseValue(elseValue) {}
};

/// `...x`, valid only directly inside a list literal or argument list.
struct SpreadExpr : ExprOf<Expr::Kind::Spread> {
  const Expr *inner;
  SpreadExpr(llvm::SMLoc loc, const Expr *inner) : ExprOf(loc), inner(inner) {}
};

struct SpawnExpr : ExprOf<Expr::Kind::Spawn> {
  const CallExpr *call;
  SpawnExpr(llvm::SMLoc loc, const CallExpr *call) : ExprOf(loc), call(call) {}
};

struct AwaitExpr : ExprOf<Expr::Kind::Await> {
  const Expr *operand;
  AwaitExpr(llvm::SMLoc loc, const Expr *operand) : ExprOf(loc), operand(operand) {}
};

struct Pattern;

struct MapPatternEntry {
  const Expr *key;
  const Pattern *pattern;
};

struct Pattern {
  enum class Kind { Wildcard, Bind, Literal, Range, List, Map, Or };
  Kind kind;
  llvm::SMLoc loc;
  Symbol name = nullptr;                 // Bind.
  const Expr *low = nullptr;             // Literal value, or Range start.
  const Expr *high = nullptr;            // Range end.
  llvm::ArrayRef<const Pattern *> items; // List items, or Or alternatives.
  const Pattern *rest = nullptr;         // List `...rest`, if present.
  llvm::ArrayRef<MapPatternEntry> entries;

  Pattern(Kind kind, llvm::SMLoc loc) : kind(kind), loc(loc) {}
};

/// The shared `pattern in a..b by c` or `pattern in iterable` of a loop.
struct LoopHeader {
  const Pattern *pattern = nullptr;
  const Expr *start = nullptr; // The iterable when `end` is null.
  const Expr *end = nullptr;
  const Expr *step = nullptr; // Null means 1.
};

struct ListCompExpr : ExprOf<Expr::Kind::ListComp> {
  const Expr *element;
  LoopHeader header;
  const Expr *filter; // May be null.
  ListCompExpr(llvm::SMLoc loc, const Expr *element, LoopHeader header, const Expr *filter)
      : ExprOf(loc), element(element), header(header), filter(filter) {}
};

// ---- Statements -------------------------------------------------------------

struct Stmt {
  enum class Kind {
    Decl, Assign, Print, If, While, For, Func, Return, Throw, Try,
    Assert, Exit, Break, Continue, Expr, Serve, Match, Squad, Rizzup,
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

struct Param {
  Symbol name;
  const Expr *defaultValue; // May be null.
  bool isRest;
};

struct FunctionDecl {
  Symbol name; // Null for a lambda.
  llvm::ArrayRef<Param> params;
  Block body;
  size_t requiredParams = 0;
  bool hasRest = false;
  bool isGenerator = false;
};

struct LambdaExpr : ExprOf<Expr::Kind::Lambda> {
  const FunctionDecl *decl;
  LambdaExpr(llvm::SMLoc loc, const FunctionDecl *decl) : ExprOf(loc), decl(decl) {}
};

struct DeclStmt : StmtOf<Stmt::Kind::Decl> {
  const Pattern *pattern;
  bool isConst;
  const Expr *init;
  DeclStmt(llvm::SMLoc loc, const Pattern *pattern, bool isConst, const Expr *init)
      : StmtOf(loc), pattern(pattern), isConst(isConst), init(init) {}
};

struct AssignStmt : StmtOf<Stmt::Kind::Assign> {
  const Expr *target; // IdentExpr, IndexExpr, or MemberExpr.
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

struct ForStmt : StmtOf<Stmt::Kind::For> {
  LoopHeader header;
  Block body;
  ForStmt(llvm::SMLoc loc, LoopHeader header, Block body)
      : StmtOf(loc), header(header), body(body) {}
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
  Symbol errorName;          // Null when there's no `caught4k`.
  Block handler;
  const Block *finallyBody;  // Null when there's no `regardless`.
  TryStmt(llvm::SMLoc loc, Block body, Symbol errorName, Block handler,
          const Block *finallyBody)
      : StmtOf(loc), body(body), errorName(errorName), handler(handler),
        finallyBody(finallyBody) {}
};

struct ServeStmt : StmtOf<Stmt::Kind::Serve> {
  const Expr *value;
  ServeStmt(llvm::SMLoc loc, const Expr *value) : StmtOf(loc), value(value) {}
};

struct MatchArm {
  const Pattern *pattern;
  const Expr *guard; // May be null.
  Block body;
};

struct MatchStmt : StmtOf<Stmt::Kind::Match> {
  const Expr *subject;
  llvm::ArrayRef<MatchArm> arms;
  const Block *elseBody; // Null when there's no `nah`.
  MatchStmt(llvm::SMLoc loc, const Expr *subject, llvm::ArrayRef<MatchArm> arms,
            const Block *elseBody)
      : StmtOf(loc), subject(subject), arms(arms), elseBody(elseBody) {}
};

struct SquadStmt : StmtOf<Stmt::Kind::Squad> {
  Symbol name;
  const Expr *parent; // May be null.
  llvm::ArrayRef<const FunctionDecl *> methods;
  SquadStmt(llvm::SMLoc loc, Symbol name, const Expr *parent,
            llvm::ArrayRef<const FunctionDecl *> methods)
      : StmtOf(loc), name(name), parent(parent), methods(methods) {}
};

struct RizzupStmt : StmtOf<Stmt::Kind::Rizzup> {
  llvm::StringRef path;
  Symbol alias;
  RizzupStmt(llvm::SMLoc loc, llvm::StringRef path, Symbol alias)
      : StmtOf(loc), path(path), alias(alias) {}
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
