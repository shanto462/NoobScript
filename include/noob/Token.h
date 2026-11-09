#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"

namespace noob {

enum class Tok {
  EndOfFile,
  Newline,
  Ident,
  Number,
  String,

  // Keywords.
  KwLowkey,
  KwHighkey,
  KwYap,
  KwMumble,
  KwBruh,
  KwVibecheck,
  KwNvm,
  KwNah,
  KwGrind,
  KwSpeedrun,
  KwIn,
  KwBy,
  KwGhost,
  KwSkibidi,
  KwCook,
  KwItsgiving,
  KwYeet,
  KwTryna,
  KwCaught4k,
  KwOngod,
  KwTouchgrass,
  KwNocap,
  KwCap,
  KwNpc,
  KwSus,
  KwRatio,
  KwSquad,
  KwMe,
  KwOg,
  KwRizzup,
  KwSendit,
  KwHoldup,
  KwServe,
  KwItdepends,
  KwRegardless,

  // Punctuation.
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Colon,
  DotDot,
  DotDotDot,
  Dot,
  Arrow,
  Question,
  QuestionDot,
  QuestionQuestion,
  PipeArrow,
  Bar,

  // Operators.
  Plus,
  Minus,
  Star,
  StarStar,
  Slash,
  Percent,
  Bang,
  Eq,
  EqEq,
  BangEq,
  Lt,
  LtEq,
  Gt,
  GtEq,
  AndAnd,
  OrOr,
  PlusEq,
  MinusEq,
  StarEq,
  SlashEq,
};

struct Token {
  Tok kind;
  /// Source text. For strings this is the part between the quotes.
  llvm::StringRef text;
  llvm::SMLoc loc;
  /// One past the last source character, including any closing quote.
  const char *end;
};

} // namespace noob
