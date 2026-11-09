#pragma once

#include "noob/AST.h"
#include "noob/Runtime.h"
#include "noob/Value.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallVector.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

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

class ClassObj;

class FunctionObj : public Object {
public:
  FunctionObj(const FunctionDecl *decl, EnvRef closure, ClassObj *owner = nullptr)
      : Object(Kind::Function), decl(decl), closure(std::move(closure)), owner(owner) {}

  const FunctionDecl *decl;
  EnvRef closure;
  /// The squad that defined this method. A method value only exists while an
  /// instance or the squad itself keeps that squad alive.
  ClassObj *owner;

  static bool classof(const Object *o) { return o->kind() == Kind::Function; }
};

class ClassObj : public Object {
public:
  explicit ClassObj(llvm::StringRef name) : Object(Kind::Class), name(name) {}

  llvm::StringRef name;
  Value parent; // A ClassObj or npc.
  llvm::DenseMap<Symbol, Value> methods;

  /// Finds a method here or up the chain, or returns null.
  const Value *findMethod(Symbol method) const;
  bool isOrCopies(const ClassObj *other) const;

  static bool classof(const Object *o) { return o->kind() == Kind::Class; }
};

class InstanceObj : public Object {
public:
  explicit InstanceObj(Value squad) : Object(Kind::Instance), squad(std::move(squad)) {}

  Value squad; // Always a ClassObj.
  llvm::DenseMap<Symbol, Value> fields;

  ClassObj *classObj() const { return squad.as<ClassObj>(); }

  static bool classof(const Object *o) { return o->kind() == Kind::Instance; }
};

class BoundMethodObj : public Object {
public:
  BoundMethodObj(Value receiver, Value function)
      : Object(Kind::BoundMethod), receiver(std::move(receiver)), function(std::move(function)) {}

  Value receiver;
  Value function; // Always a FunctionObj.

  static bool classof(const Object *o) { return o->kind() == Kind::BoundMethod; }
};

class ModuleObj : public Object {
public:
  ModuleObj(llvm::StringRef name, EnvRef env)
      : Object(Kind::Module), name(name), env(std::move(env)) {}

  llvm::StringRef name;
  EnvRef env;
  bool loading = true;

  static bool classof(const Object *o) { return o->kind() == Kind::Module; }
};

struct Frame {
  llvm::StringRef name;
  llvm::SMLoc callSite;
};

/// A runtime error while it's unwinding, or stored on a finished task.
struct PendingError {
  Value value;
  llvm::SMLoc loc;
  std::string headline;
  std::string detail;
  std::vector<Frame> frames;
  size_t depth = 0;
};

class TaskObj : public Object {
public:
  TaskObj() : Object(Kind::Task) {}

  bool done = false;
  bool failed = false;
  bool observed = false;
  /// Position in the order tasks finished, which `firstone` uses.
  uint64_t finishOrder = 0;
  Value result;
  PendingError error;
  llvm::SmallVector<Fiber *, 2> waiters;

  static bool classof(const Object *o) { return o->kind() == Kind::Task; }
};

class GeneratorObj : public Object {
public:
  GeneratorObj(Scheduler &scheduler, const FunctionDecl *decl, EnvRef scope)
      : Object(Kind::Generator), scheduler(scheduler), decl(decl), scope(std::move(scope)) {}
  ~GeneratorObj() override;

  Scheduler &scheduler;
  const FunctionDecl *decl;
  EnvRef scope;
  Fiber *fiber = nullptr;
  bool running = false;
  bool finished = false;
  bool failed = false;
  Value yielded;
  PendingError error;

  static bool classof(const Object *o) { return o->kind() == Kind::Generator; }
};

class ChannelObj : public Object {
public:
  explicit ChannelObj(size_t capacity) : Object(Kind::Channel), capacity(capacity) {}

  size_t capacity; // Zero means unbounded.
  bool closed = false;
  std::deque<Value> buffer;
  std::deque<Fiber *> receivers;
  std::deque<Fiber *> senders;

  static bool classof(const Object *o) { return o->kind() == Kind::Channel; }
};

} // namespace noob
