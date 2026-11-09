#include "noob/Lexer.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

using namespace llvm;

namespace noob {

static constexpr int MaxStringNesting = 32;

static SMLoc locOf(const char *p) { return SMLoc::getFromPointer(p); }

static Tok keywordOrIdent(StringRef word) {
  return StringSwitch<Tok>(word)
      .Case("lowkey", Tok::KwLowkey)
      .Case("highkey", Tok::KwHighkey)
      .Case("yap", Tok::KwYap)
      .Case("mumble", Tok::KwMumble)
      .Case("bruh", Tok::KwBruh)
      .Case("vibecheck", Tok::KwVibecheck)
      .Case("nvm", Tok::KwNvm)
      .Case("nah", Tok::KwNah)
      .Case("grind", Tok::KwGrind)
      .Case("speedrun", Tok::KwSpeedrun)
      .Case("in", Tok::KwIn)
      .Case("by", Tok::KwBy)
      .Case("ghost", Tok::KwGhost)
      .Case("skibidi", Tok::KwSkibidi)
      .Case("cook", Tok::KwCook)
      .Case("itsgiving", Tok::KwItsgiving)
      .Case("yeet", Tok::KwYeet)
      .Case("tryna", Tok::KwTryna)
      .Case("caught4k", Tok::KwCaught4k)
      .Case("ongod", Tok::KwOngod)
      .Case("touchgrass", Tok::KwTouchgrass)
      .Case("nocap", Tok::KwNocap)
      .Case("cap", Tok::KwCap)
      .Case("npc", Tok::KwNpc)
      .Case("sus", Tok::KwSus)
      .Case("ratio", Tok::KwRatio)
      .Case("rizzup", Tok::KwReserved)
      .Case("squad", Tok::KwReserved)
      .Default(Tok::Ident);
}

static const char *skipInterp(const char *p, const char *end, int nesting);

/// Given a pointer just past an opening quote, returns a pointer just past the
/// closing quote, or null if the string never closes on its line.
static const char *skipString(const char *p, const char *end, int nesting) {
  if (nesting > MaxStringNesting)
    return nullptr;
  while (p < end) {
    char c = *p;
    if (c == '\n')
      return nullptr;
    if (c == '"')
      return p + 1;
    if (c == '\\') {
      p += 2;
      continue;
    }
    if (c == '$' && p + 1 < end && p[1] == '{') {
      p = skipInterp(p + 2, end, nesting + 1);
      if (!p)
        return nullptr;
      ++p; // The closing brace.
      continue;
    }
    ++p;
  }
  return nullptr;
}

static const char *skipInterp(const char *p, const char *end, int nesting) {
  int depth = 0;
  while (p < end) {
    char c = *p;
    if (c == '\n')
      return nullptr;
    if (c == '"') {
      p = skipString(p + 1, end, nesting);
      if (!p)
        return nullptr;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (depth == 0)
        return p;
      --depth;
    }
    ++p;
  }
  return nullptr;
}

const char *Lexer::findInterpEnd(const char *p, const char *end) {
  return skipInterp(p, end, 0);
}

Lexer::Lexer(StringRef source, Diag &diag, bool allowShebang)
    : start_(source.begin()), cur_(source.begin()), end_(source.end()),
      diag_(diag) {
  if (allowShebang && source.starts_with("#!"))
    while (cur_ < end_ && *cur_ != '\n')
      ++cur_;
}

void Lexer::lexAll(std::vector<Token> &out) {
  for (;;) {
    while (cur_ < end_ && (*cur_ == ' ' || *cur_ == '\t' || *cur_ == '\r'))
      ++cur_;

    if (cur_ >= end_) {
      out.push_back({Tok::EndOfFile, StringRef(end_, 0), locOf(end_), end_});
      return;
    }

    start_ = cur_;
    char c = *cur_;
    if (c == '\n') {
      ++cur_;
      out.push_back({Tok::Newline, StringRef(start_, 1), locOf(start_), cur_});
    } else if (isAlpha(c) || c == '_') {
      lexIdentifier(out);
    } else if (isDigit(c)) {
      lexNumber(out);
    } else if (c == '"') {
      lexString(out);
    } else {
      lexPunctuation(out);
    }
  }
}

void Lexer::lexIdentifier(std::vector<Token> &out) {
  while (cur_ < end_ && (isAlnum(*cur_) || *cur_ == '_'))
    ++cur_;
  StringRef word(start_, cur_ - start_);
  if (word == "btw") {
    while (cur_ < end_ && *cur_ != '\n')
      ++cur_;
    return;
  }
  out.push_back({keywordOrIdent(word), word, locOf(start_), cur_});
}

void Lexer::lexNumber(std::vector<Token> &out) {
  auto digitAt = [&](const char *p) { return p < end_ && isDigit(*p); };

  while (digitAt(cur_))
    ++cur_;
  // Requiring a digit after the dot keeps `1..5` a range.
  if (cur_ < end_ && *cur_ == '.' && digitAt(cur_ + 1)) {
    ++cur_;
    while (digitAt(cur_))
      ++cur_;
  }
  if (cur_ < end_ && (*cur_ == 'e' || *cur_ == 'E')) {
    const char *p = cur_ + 1;
    if (p < end_ && (*p == '+' || *p == '-'))
      ++p;
    if (digitAt(p)) {
      cur_ = p;
      while (digitAt(cur_))
        ++cur_;
    }
  }
  out.push_back(
      {Tok::Number, StringRef(start_, cur_ - start_), locOf(start_), cur_});
}

void Lexer::lexString(std::vector<Token> &out) {
  const char *close = skipString(cur_ + 1, end_, 0);
  if (!close) {
    diag_.syntax(locOf(start_), "you left me on read (unclosed)",
                 "this string is missing its closing quote");
    while (cur_ < end_ && *cur_ != '\n')
      ++cur_;
    out.push_back({Tok::String, StringRef(start_, 0), locOf(start_), cur_});
    return;
  }
  cur_ = close;
  StringRef body(start_ + 1, close - start_ - 2);
  out.push_back({Tok::String, body, locOf(start_), cur_});
}

void Lexer::lexPunctuation(std::vector<Token> &out) {
  char c = *cur_++;
  auto next = [&](char expected) {
    if (cur_ < end_ && *cur_ == expected) {
      ++cur_;
      return true;
    }
    return false;
  };
  auto emit = [&](Tok kind) {
    out.push_back({kind, StringRef(start_, cur_ - start_), locOf(start_), cur_});
  };

  switch (c) {
  case '(': return emit(Tok::LParen);
  case ')': return emit(Tok::RParen);
  case '{': return emit(Tok::LBrace);
  case '}': return emit(Tok::RBrace);
  case '[': return emit(Tok::LBracket);
  case ']': return emit(Tok::RBracket);
  case ',': return emit(Tok::Comma);
  case ':': return emit(Tok::Colon);
  case '%': return emit(Tok::Percent);
  case '+': return emit(next('=') ? Tok::PlusEq : Tok::Plus);
  case '-': return emit(next('=') ? Tok::MinusEq : Tok::Minus);
  case '/': return emit(next('=') ? Tok::SlashEq : Tok::Slash);
  case '<': return emit(next('=') ? Tok::LtEq : Tok::Lt);
  case '>': return emit(next('=') ? Tok::GtEq : Tok::Gt);
  case '=': return emit(next('=') ? Tok::EqEq : Tok::Eq);
  case '!': return emit(next('=') ? Tok::BangEq : Tok::Bang);
  case '*':
    if (next('*'))
      return emit(Tok::StarStar);
    return emit(next('=') ? Tok::StarEq : Tok::Star);
  case '.':
    if (next('.'))
      return emit(Tok::DotDot);
    break;
  case '&':
    if (next('&'))
      return emit(Tok::AndAnd);
    break;
  case '|':
    if (next('|'))
      return emit(Tok::OrOr);
    break;
  case ';':
    diag_.syntax(locOf(start_), "this ain't C, drop the semicolon",
                 "statements end at the end of the line, no ';' needed");
    // Treat it as the line break the writer meant.
    return emit(Tok::Newline);
  default:
    break;
  }

  // Step over a whole UTF-8 sequence so an emoji is reported once.
  while (cur_ < end_ && (static_cast<unsigned char>(*cur_) & 0xC0) == 0x80)
    ++cur_;
  diag_.syntax(locOf(start_), "bestie what is this 💀",
               "unexpected character '" + StringRef(start_, cur_ - start_) +
                   "'");
}

} // namespace noob
