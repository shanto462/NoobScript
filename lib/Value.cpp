#include "noob/Value.h"

#include "noob/Environment.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <charconv>
#include <cmath>
#include <cstring>

using namespace llvm;

namespace noob {

// ---- Environment ------------------------------------------------------------

static constexpr size_t IndexThreshold = 8;

Binding *Env::findLocal(Symbol name) {
  if (index_) {
    auto it = index_->find(name);
    return it == index_->end() ? nullptr : &slots_[it->second];
  }
  for (Binding &slot : slots_)
    if (slot.name == name)
      return &slot;
  return nullptr;
}

Binding *Env::lookup(Symbol name) {
  for (Env *env = this; env; env = env->parent_.get())
    if (Binding *binding = env->findLocal(name))
      return binding;
  return nullptr;
}

void Env::define(Symbol name, Value value, bool isConst) {
  if (Binding *existing = findLocal(name)) {
    existing->value = std::move(value);
    existing->isConst = isConst;
    return;
  }
  slots_.push_back({name, std::move(value), isConst});
  if (index_) {
    (*index_)[name] = slots_.size() - 1;
  } else if (slots_.size() > IndexThreshold) {
    index_ = std::make_unique<DenseMap<Symbol, unsigned>>();
    for (size_t i = 0; i < slots_.size(); ++i)
      (*index_)[slots_[i].name] = i;
  }
}

const Value *ClassObj::findMethod(Symbol method) const {
  for (const ClassObj *squad = this; squad; squad = squad->parent.as<ClassObj>()) {
    auto it = squad->methods.find(method);
    if (it != squad->methods.end())
      return &it->second;
  }
  return nullptr;
}

bool ClassObj::isOrCopies(const ClassObj *other) const {
  for (const ClassObj *squad = this; squad; squad = squad->parent.as<ClassObj>())
    if (squad == other)
      return true;
  return false;
}

GeneratorObj::~GeneratorObj() {
  if (fiber && scheduler.isSuspended(fiber))
    scheduler.destroySuspended(fiber);
}

// ---- Values -----------------------------------------------------------------

Value Value::string(std::string s) { return Value(new StringObj(std::move(s))); }

StringRef Value::typeName() const {
  switch (tag_) {
  case Tag::Npc: return "npc";
  case Tag::Bool: return "bool";
  case Tag::Number: return "number";
  case Tag::Object: break;
  }
  switch (object_->kind()) {
  case Object::Kind::String: return "string";
  case Object::Kind::List: return "list";
  case Object::Kind::Map: return "map";
  case Object::Kind::Function:
  case Object::Kind::Builtin:
  case Object::Kind::BoundMethod: return "function";
  case Object::Kind::Class: return "squad";
  case Object::Kind::Instance: return as<InstanceObj>()->classObj()->name;
  case Object::Kind::Module: return "module";
  case Object::Kind::Task: return "task";
  case Object::Kind::Generator: return "generator";
  case Object::Kind::Channel: return "groupchat";
  }
  return "npc";
}

std::string formatNumber(double n) {
  // Whole numbers that a double represents exactly print without a decimal.
  if (n == std::floor(n) && std::fabs(n) <= 9007199254740992.0)
    return std::to_string(static_cast<long long>(n));
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), n);
  return std::string(buffer, result.ptr);
}

using ObjectStack = SmallVectorImpl<const Object *>;

static void appendRepr(std::string &out, const Value &value, ObjectStack &stack);

static void appendQuoted(std::string &out, StringRef text) {
  out += '"';
  for (char c : text) {
    switch (c) {
    case '\n': out += "\\n"; break;
    case '\t': out += "\\t"; break;
    case '\r': out += "\\r"; break;
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    default: out += c; break;
    }
  }
  out += '"';
}

static void appendObjectRepr(std::string &out, const Value &value, ObjectStack &stack) {
  if (auto *string = value.as<StringObj>()) {
    appendQuoted(out, string->text());
    return;
  }
  if (auto *function = value.as<FunctionObj>()) {
    out += "<cook";
    if (function->decl->name) {
      out += ' ';
      out += function->decl->name->text.str();
    }
    out += '>';
    return;
  }
  if (auto *builtin = value.as<BuiltinObj>()) {
    out += "<cook " + builtin->name.str() + ">";
    return;
  }

  if (auto *bound = value.as<BoundMethodObj>()) {
    appendObjectRepr(out, bound->function, stack);
    return;
  }
  if (auto *squad = value.as<ClassObj>()) {
    out += "<squad " + squad->name.str() + ">";
    return;
  }
  if (auto *instance = value.as<InstanceObj>()) {
    out += "<" + instance->classObj()->name.str() + ">";
    return;
  }
  if (auto *module = value.as<ModuleObj>()) {
    out += "<module " + module->name.str() + ">";
    return;
  }
  if (value.as<TaskObj>() || value.as<GeneratorObj>() || value.as<ChannelObj>()) {
    out += "<" + value.typeName().str() + ">";
    return;
  }

  // A list can contain itself, so stop at anything already being printed.
  bool isList = value.as<ListObj>() != nullptr;
  if (is_contained(stack, value.object())) {
    out += isList ? "[...]" : "{...}";
    return;
  }
  stack.push_back(value.object());
  if (isList) {
    out += '[';
    bool first = true;
    for (const Value &item : value.as<ListObj>()->items) {
      if (!first)
        out += ", ";
      first = false;
      appendRepr(out, item, stack);
    }
    out += ']';
  } else {
    out += '{';
    bool first = true;
    for (const MapObj::Entry &entry : value.as<MapObj>()->entries()) {
      if (!entry.live)
        continue;
      if (!first)
        out += ", ";
      first = false;
      appendRepr(out, entry.key, stack);
      out += ": ";
      appendRepr(out, entry.value, stack);
    }
    out += '}';
  }
  stack.pop_back();
}

static void appendRepr(std::string &out, const Value &value, ObjectStack &stack) {
  if (value.isNpc())
    out += "npc";
  else if (value.isBool())
    out += value.asBool() ? "nocap" : "cap";
  else if (value.isNumber())
    out += formatNumber(value.asNumber());
  else
    appendObjectRepr(out, value, stack);
}

std::string Value::repr() const {
  std::string out;
  SmallVector<const Object *, 8> stack;
  appendRepr(out, *this, stack);
  return out;
}

std::string Value::display() const {
  if (auto *string = as<StringObj>())
    return string->text().str();
  return repr();
}

bool Value::equals(const Value &other) const {
  if (tag_ != other.tag_)
    return false;
  switch (tag_) {
  case Tag::Npc: return true;
  case Tag::Bool: return boolean_ == other.boolean_;
  case Tag::Number: return number_ == other.number_;
  case Tag::Object: break;
  }
  auto *a = as<StringObj>();
  auto *b = other.as<StringObj>();
  if (a && b)
    return a->text() == b->text();
  return object_.get() == other.object_.get();
}

// ---- Strings ----------------------------------------------------------------

static bool isContinuationByte(char c) {
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

size_t StringObj::length() const {
  if (!length_) {
    size_t count = 0;
    for (char c : text_)
      count += !isContinuationByte(c);
    length_ = count;
  }
  return *length_;
}

size_t StringObj::byteOffset(size_t index) const {
  if (index >= length())
    return text_.size();
  if (length() == text_.size())
    return index;

  size_t offset = 0;
  for (; index > 0; --index) {
    ++offset;
    while (offset < text_.size() && isContinuationByte(text_[offset]))
      ++offset;
  }
  return offset;
}

// ---- Maps -------------------------------------------------------------------

bool MapObj::isValidKey(const Value &key) {
  if (key.isNumber())
    return !std::isnan(key.asNumber());
  return key.as<StringObj>() != nullptr;
}

/// Encodes a key so strings and numbers can share one hash table.
static void encodeKey(const Value &key, SmallString<32> &out) {
  if (key.isNumber()) {
    double n = key.asNumber();
    if (n == 0)
      n = 0; // Folds -0 into 0.
    out.push_back('n');
    char bytes[sizeof(double)];
    std::memcpy(bytes, &n, sizeof(double));
    out.append(bytes, bytes + sizeof(double));
    return;
  }
  out.push_back('s');
  out.append(key.as<StringObj>()->text());
}

const Value *MapObj::get(const Value &key) const {
  SmallString<32> encoded;
  encodeKey(key, encoded);
  auto it = index_.find(encoded);
  return it == index_.end() ? nullptr : &entries_[it->second].value;
}

void MapObj::set(const Value &key, Value value) {
  SmallString<32> encoded;
  encodeKey(key, encoded);
  auto [it, inserted] = index_.try_emplace(encoded, entries_.size());
  if (inserted) {
    entries_.push_back({key, std::move(value), true});
    ++liveCount_;
  } else {
    entries_[it->second].value = std::move(value);
  }
}

void MapObj::erase(const Value &key) {
  SmallString<32> encoded;
  encodeKey(key, encoded);
  auto it = index_.find(encoded);
  if (it == index_.end())
    return;
  Entry &entry = entries_[it->second];
  entry.live = false;
  entry.key = Value();
  entry.value = Value();
  index_.erase(it);
  --liveCount_;
  if (entries_.size() > 2 * liveCount_ + 8)
    compact();
}

void MapObj::compact() {
  std::vector<Entry> live;
  live.reserve(liveCount_);
  index_.clear();
  for (Entry &entry : entries_) {
    if (!entry.live)
      continue;
    SmallString<32> encoded;
    encodeKey(entry.key, encoded);
    index_[encoded] = live.size();
    live.push_back(std::move(entry));
  }
  entries_ = std::move(live);
}

std::vector<Value> MapObj::keys() const {
  std::vector<Value> out;
  out.reserve(liveCount_);
  for (const Entry &entry : entries_)
    if (entry.live)
      out.push_back(entry.key);
  return out;
}

} // namespace noob
