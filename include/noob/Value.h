#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace noob {

/// Base class of everything that lives on the heap.
class Object : public llvm::RefCountedBase<Object> {
public:
  enum class Kind { String, List, Map, Function, Builtin };

  virtual ~Object() = default;
  Kind kind() const { return kind_; }

protected:
  explicit Object(Kind kind) : kind_(kind) {}

private:
  Kind kind_;
};

class Value {
public:
  Value() : tag_(Tag::Npc), number_(0) {}
  explicit Value(Object *object) : tag_(Tag::Object), number_(0), object_(object) {}

  static Value number(double n) {
    Value v;
    v.tag_ = Tag::Number;
    v.number_ = n;
    return v;
  }
  static Value boolean(bool b) {
    Value v;
    v.tag_ = Tag::Bool;
    v.boolean_ = b;
    return v;
  }
  static Value string(std::string s);

  bool isNpc() const { return tag_ == Tag::Npc; }
  bool isBool() const { return tag_ == Tag::Bool; }
  bool isNumber() const { return tag_ == Tag::Number; }
  bool isObject() const { return tag_ == Tag::Object; }

  bool asBool() const { return boolean_; }
  double asNumber() const { return number_; }
  Object *object() const { return object_.get(); }

  /// Returns the object as a T, or null if this value is something else.
  template <typename T> T *as() const {
    return isObject() ? llvm::dyn_cast<T>(object_.get()) : nullptr;
  }

  /// Only `cap` and `npc` are falsy.
  bool truthy() const { return !(isNpc() || (isBool() && !boolean_)); }

  llvm::StringRef typeName() const;

  /// The text `yap` prints, where a top-level string has no quotes.
  std::string display() const;

  /// The text shown inside containers and at the REPL, with quoted strings.
  std::string repr() const;

  bool equals(const Value &other) const;

private:
  enum class Tag : uint8_t { Npc, Bool, Number, Object };

  Tag tag_;
  union {
    bool boolean_;
    double number_;
  };
  llvm::IntrusiveRefCntPtr<Object> object_;
};

std::string formatNumber(double n);

class StringObj : public Object {
public:
  explicit StringObj(std::string text)
      : Object(Kind::String), text_(std::move(text)) {}

  llvm::StringRef text() const { return text_; }

  /// Length in code points. Cached, so ASCII indexing stays constant time.
  size_t length() const;

  /// Byte offset of the code point at `index`, or the byte size if past it.
  size_t byteOffset(size_t index) const;

  static bool classof(const Object *o) { return o->kind() == Kind::String; }

private:
  std::string text_;
  mutable std::optional<size_t> length_;
};

class ListObj : public Object {
public:
  ListObj() : Object(Kind::List) {}
  explicit ListObj(std::vector<Value> items)
      : Object(Kind::List), items(std::move(items)) {}

  std::vector<Value> items;

  static bool classof(const Object *o) { return o->kind() == Kind::List; }
};

/// A hash map that remembers insertion order.
class MapObj : public Object {
public:
  struct Entry {
    Value key;
    Value value;
    bool live;
  };

  MapObj() : Object(Kind::Map) {}

  static bool isValidKey(const Value &key);

  /// `key` must satisfy isValidKey for all of these.
  const Value *get(const Value &key) const;
  void set(const Value &key, Value value);
  void erase(const Value &key);

  size_t size() const { return liveCount_; }
  std::vector<Value> keys() const;

  /// Entries in insertion order. Skip any that aren't live.
  llvm::ArrayRef<Entry> entries() const { return entries_; }

  static bool classof(const Object *o) { return o->kind() == Kind::Map; }

private:
  void compact();

  std::vector<Entry> entries_;
  llvm::StringMap<unsigned> index_;
  size_t liveCount_ = 0;
};

class Interpreter;

/// One call to a built-in function, with helpers that validate arguments.
/// Each helper raises a NoobScript error and returns false on a bad argument.
struct BuiltinCall {
  Interpreter &interp;
  llvm::StringRef name;
  llvm::ArrayRef<Value> args;
  llvm::SMLoc loc;
  Value &result;

  bool string(size_t i, StringObj *&out);
  bool list(size_t i, ListObj *&out);
  bool map(size_t i, MapObj *&out);
  bool number(size_t i, double &out);
  bool whole(size_t i, int64_t &out);
  bool mapKey(size_t i);
  bool typeError(size_t i, llvm::StringRef wanted);
  bool error(const llvm::Twine &headline, const llvm::Twine &detail);
};

using BuiltinFn = bool (*)(BuiltinCall &);

class BuiltinObj : public Object {
public:
  BuiltinObj(llvm::StringRef name, unsigned minArgs, unsigned maxArgs, BuiltinFn fn)
      : Object(Kind::Builtin), name(name), minArgs(minArgs), maxArgs(maxArgs), fn(fn) {}

  llvm::StringRef name;
  unsigned minArgs;
  unsigned maxArgs;
  BuiltinFn fn;

  static bool classof(const Object *o) { return o->kind() == Kind::Builtin; }
};

} // namespace noob
