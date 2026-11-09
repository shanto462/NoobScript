#pragma once

#include "noob/AST.h"
#include "noob/Value.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallVector.h"

#include <memory>

namespace noob {

class Env;
using EnvRef = llvm::IntrusiveRefCntPtr<Env>;

struct Binding {
  Symbol name;
  Value value;
  bool isConst;
};

/// One lexical scope. Reference-counted because closures keep scopes alive.
///
/// Binding pointers returned here are invalidated by the next define() on the
/// same scope, so use them immediately.
class Env : public llvm::RefCountedBase<Env> {
public:
  explicit Env(Env *parent) : parent_(parent) {}

  Binding *findLocal(Symbol name);
  Binding *lookup(Symbol name);

  /// Adds a binding, replacing any of the same name in this scope.
  void define(Symbol name, Value value, bool isConst);

private:
  EnvRef parent_;
  llvm::SmallVector<Binding, 4> slots_;
  std::unique_ptr<llvm::DenseMap<Symbol, unsigned>> index_;
};

class FunctionObj : public Object {
public:
  FunctionObj(const FunctionDecl *decl, EnvRef closure)
      : Object(Kind::Function), decl(decl), closure(std::move(closure)) {}

  const FunctionDecl *decl;
  EnvRef closure;

  static bool classof(const Object *o) { return o->kind() == Kind::Function; }
};

} // namespace noob
