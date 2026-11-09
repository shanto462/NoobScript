#include "noob/Interpreter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <thread>

using namespace llvm;

namespace noob {

static constexpr const char *TypeHeadline =
    "those types are not compatible, it's giving divorce";
static constexpr double MaxSafeWhole = 9007199254740992.0;

// ---- Argument checking ------------------------------------------------------

bool BuiltinCall::error(const Twine &headline, const Twine &detail) {
  return interp.fail(loc, headline, detail);
}

bool BuiltinCall::typeError(size_t i, StringRef wanted) {
  return error(TypeHeadline, "`" + name + "` wants " + wanted + " for argument " +
                                 Twine(i + 1) + ", got " + args[i].typeName());
}

bool BuiltinCall::string(size_t i, StringObj *&out) {
  out = args[i].as<StringObj>();
  return out || typeError(i, "a string");
}

bool BuiltinCall::list(size_t i, ListObj *&out) {
  out = args[i].as<ListObj>();
  return out || typeError(i, "a list");
}

bool BuiltinCall::map(size_t i, MapObj *&out) {
  out = args[i].as<MapObj>();
  return out || typeError(i, "a map");
}

bool BuiltinCall::number(size_t i, double &out) {
  if (!args[i].isNumber())
    return typeError(i, "a number");
  out = args[i].asNumber();
  return true;
}

bool BuiltinCall::whole(size_t i, int64_t &out) {
  if (!args[i].isNumber() || args[i].asNumber() != std::floor(args[i].asNumber()) ||
      std::fabs(args[i].asNumber()) > MaxSafeWhole)
    return typeError(i, "a whole number");
  out = static_cast<int64_t>(args[i].asNumber());
  return true;
}

bool BuiltinCall::mapKey(size_t i) {
  return MapObj::isValidKey(args[i]) || typeError(i, "a string or number key");
}

// ---- Input and conversion ---------------------------------------------------

static bool spill(BuiltinCall &c) {
  if (!c.args.empty())
    outs() << c.args[0].display();
  outs().flush();

  std::string line;
  if (!std::getline(std::cin, line)) {
    c.result = Value();
    return true;
  }
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  c.result = Value::string(std::move(line));
  return true;
}

static bool clout(BuiltinCall &c) {
  const Value &value = c.args[0];
  if (auto *string = value.as<StringObj>())
    c.result = Value::number(string->length());
  else if (auto *list = value.as<ListObj>())
    c.result = Value::number(list->items.size());
  else if (auto *map = value.as<MapObj>())
    c.result = Value::number(map->size());
  else
    return c.typeError(0, "a string, list, or map");
  return true;
}

static bool vibeof(BuiltinCall &c) {
  c.result = Value::string(c.args[0].typeName().str());
  return true;
}

static bool towords(BuiltinCall &c) {
  c.result = Value::string(c.args[0].display());
  return true;
}

/// Accepts plain decimal numbers only, so "nan", "inf", and "0x10" stay npc.
static bool looksNumeric(StringRef text) {
  text.consume_front("-") || text.consume_front("+");
  size_t digits = 0;
  auto takeDigits = [&] {
    while (!text.empty() && isDigit(text.front())) {
      text = text.drop_front();
      ++digits;
    }
  };
  takeDigits();
  if (text.consume_front("."))
    takeDigits();
  if (digits == 0)
    return false;
  if (text.consume_front("e") || text.consume_front("E")) {
    text.consume_front("-") || text.consume_front("+");
    digits = 0;
    takeDigits();
    if (digits == 0)
      return false;
  }
  return text.empty();
}

static bool todigits(BuiltinCall &c) {
  const Value &value = c.args[0];
  c.result = Value();
  if (value.isNumber()) {
    c.result = value;
    return true;
  }
  auto *string = value.as<StringObj>();
  if (!string)
    return true;
  StringRef text = string->text().trim();
  double number;
  if (looksNumeric(text) && !text.getAsDouble(number, /*AllowInexact=*/true))
    c.result = Value::number(number);
  return true;
}

// ---- Lists and maps ---------------------------------------------------------

static bool slide(BuiltinCall &c) {
  ListObj *list;
  if (!c.list(0, list))
    return false;
  list->items.push_back(c.args[1]);
  return true;
}

static bool dip(BuiltinCall &c) {
  ListObj *list;
  if (!c.list(0, list))
    return false;
  if (list->items.empty())
    return c.error("that list is empty, nothing to dip with", "can't dip from an empty list");
  c.result = std::move(list->items.back());
  list->items.pop_back();
  return true;
}

static bool yoink(BuiltinCall &c) {
  ListObj *list;
  int64_t index;
  if (!c.list(0, list) || !c.whole(1, index))
    return false;
  if (index < 0 || static_cast<size_t>(index) >= list->items.size())
    return c.error("that index is out of bounds, touch grass",
                   "index " + Twine(index) + " on a list of length " +
                       Twine(list->items.size()));
  c.result = std::move(list->items[index]);
  list->items.erase(list->items.begin() + index);
  return true;
}

static bool gotit(BuiltinCall &c) {
  const Value &container = c.args[0];
  const Value &item = c.args[1];
  bool found = false;
  if (auto *string = container.as<StringObj>()) {
    StringObj *needle;
    if (!c.string(1, needle))
      return false;
    found = string->text().contains(needle->text());
  } else if (auto *list = container.as<ListObj>()) {
    for (const Value &candidate : list->items)
      found |= candidate.equals(item);
  } else if (auto *map = container.as<MapObj>()) {
    found = MapObj::isValidKey(item) && map->get(item);
  } else {
    return c.typeError(0, "a string, list, or map");
  }
  c.result = Value::boolean(found);
  return true;
}

static bool receipts(BuiltinCall &c) {
  MapObj *map;
  if (!c.map(0, map))
    return false;
  c.result = Value(new ListObj(map->keys()));
  return true;
}

static bool unsend(BuiltinCall &c) {
  MapObj *map;
  if (!c.map(0, map) || !c.mapKey(1))
    return false;
  map->erase(c.args[1]);
  return true;
}

// ---- Strings ----------------------------------------------------------------

static Value characters(StringObj *string) {
  std::vector<Value> items;
  size_t length = string->length();
  items.reserve(length);
  for (size_t i = 0; i < length; ++i)
    items.push_back(Value::string(
        string->text().slice(string->byteOffset(i), string->byteOffset(i + 1)).str()));
  return Value(new ListObj(std::move(items)));
}

static bool breakup(BuiltinCall &c) {
  StringObj *string, *separator;
  if (!c.string(0, string) || !c.string(1, separator))
    return false;
  if (separator->text().empty()) {
    c.result = characters(string);
    return true;
  }
  std::vector<Value> items;
  StringRef rest = string->text();
  for (;;) {
    auto [head, tail] = rest.split(separator->text());
    items.push_back(Value::string(head.str()));
    // split() returns a null tail only when the separator wasn't found.
    if (tail.data() == nullptr)
      break;
    rest = tail;
  }
  c.result = Value(new ListObj(std::move(items)));
  return true;
}

static bool linkup(BuiltinCall &c) {
  ListObj *list;
  StringObj *separator;
  if (!c.list(0, list) || !c.string(1, separator))
    return false;
  std::string text;
  bool first = true;
  for (const Value &item : list->items) {
    if (!first)
      text += separator->text();
    first = false;
    text += item.display();
  }
  c.result = Value::string(std::move(text));
  return true;
}

static bool snip(BuiltinCall &c) {
  StringObj *string;
  int64_t start, length;
  if (!c.string(0, string) || !c.whole(1, start) || !c.whole(2, length))
    return false;
  if (start < 0 || length < 0)
    return c.error("that index is out of bounds, touch grass",
                   "snip's start and length can't be negative");
  size_t from = string->byteOffset(start);
  size_t to = string->byteOffset(static_cast<size_t>(start) + static_cast<size_t>(length));
  c.result = Value::string(string->text().slice(from, to).str());
  return true;
}

static bool loud(BuiltinCall &c) {
  StringObj *string;
  if (!c.string(0, string))
    return false;
  c.result = Value::string(string->text().upper());
  return true;
}

static bool quiet(BuiltinCall &c) {
  StringObj *string;
  if (!c.string(0, string))
    return false;
  c.result = Value::string(string->text().lower());
  return true;
}

// ---- Math and system --------------------------------------------------------

template <double (*F)(double)> static bool math1(BuiltinCall &c) {
  double value;
  if (!c.number(0, value))
    return false;
  c.result = Value::number(F(value));
  return true;
}

static double noobFloor(double x) { return std::floor(x); }
static double noobCeil(double x) { return std::ceil(x); }
static double noobRound(double x) { return std::round(x); }
static double noobAbs(double x) { return std::fabs(x); }
static double noobSqrt(double x) { return std::sqrt(x); }

static bool rng(BuiltinCall &c) {
  int64_t low, high;
  if (!c.whole(0, low) || !c.whole(1, high))
    return false;
  if (low > high)
    return c.error("rng range is backwards", "rng's min can't be bigger than its max");
  std::uniform_int_distribution<int64_t> distribution(low, high);
  c.result = Value::number(static_cast<double>(distribution(c.interp.random())));
  return true;
}

static bool rn(BuiltinCall &c) {
  using namespace std::chrono;
  auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
  c.result = Value::number(static_cast<double>(now.count()));
  return true;
}

static bool nap(BuiltinCall &c) {
  double ms;
  if (!c.number(0, ms))
    return false;
  if (!(ms >= 0))
    return c.error("can't nap for negative time", "nap needs a non-negative number of ms");
  outs().flush();
  std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
  return true;
}

// ---- Registration -----------------------------------------------------------

namespace {
struct BuiltinDef {
  const char *name;
  unsigned minArgs;
  unsigned maxArgs;
  BuiltinFn fn;
};
} // namespace

static const BuiltinDef Builtins[] = {
    {"spill", 0, 1, spill},
    {"clout", 1, 1, clout},
    {"vibeof", 1, 1, vibeof},
    {"towords", 1, 1, towords},
    {"todigits", 1, 1, todigits},
    {"slide", 2, 2, slide},
    {"dip", 1, 1, dip},
    {"yoink", 2, 2, yoink},
    {"gotit", 2, 2, gotit},
    {"receipts", 1, 1, receipts},
    {"unsend", 2, 2, unsend},
    {"breakup", 2, 2, breakup},
    {"linkup", 2, 2, linkup},
    {"snip", 3, 3, snip},
    {"loud", 1, 1, loud},
    {"quiet", 1, 1, quiet},
    {"floor", 1, 1, math1<noobFloor>},
    {"ceil", 1, 1, math1<noobCeil>},
    {"round", 1, 1, math1<noobRound>},
    {"abs", 1, 1, math1<noobAbs>},
    {"sqrt", 1, 1, math1<noobSqrt>},
    {"rng", 2, 2, rng},
    {"rn", 0, 0, rn},
    {"nap", 1, 1, nap},
};

void installBuiltins(AstContext &ctx, Env &env) {
  for (const BuiltinDef &def : Builtins)
    env.define(ctx.intern(def.name),
               Value(new BuiltinObj(def.name, def.minArgs, def.maxArgs, def.fn)),
               /*isConst=*/true);
}

} // namespace noob
