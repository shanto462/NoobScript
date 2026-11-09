#include "noob/Parser.h"

#include "noob/Lexer.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Path.h"

#include <cmath>
#include <string>

using namespace llvm;

namespace noob {

static constexpr int MaxNesting = 200;
static constexpr const char *GenericHeadline = "bestie what is this 💀";
static constexpr const char *UnclosedHeadline = "you left me on read (unclosed)";

struct Parser::NestingGuard {
  Parser &parser;
  bool ok;
  explicit NestingGuard(Parser &parser)
      : parser(parser), ok(parser.enterNesting()) {}
  ~NestingGuard() { --parser.nesting_; }
};

Parser::Parser(AstContext &ctx, Diag &diag, StringRef source, bool allowShebang)
    : ctx_(ctx), diag_(diag) {
  Lexer(source, diag, allowShebang).lexAll(tokens_);
}

// ---- Token helpers ----------------------------------------------------------

const Token &Parser::peek(size_t ahead) const {
  size_t index = pos_ + ahead;
  return index < tokens_.size() ? tokens_[index] : tokens_.back();
}

bool Parser::atTerminator() const {
  return at(Tok::Newline) || at(Tok::EndOfFile) || at(Tok::RBrace);
}

const Token &Parser::advance() {
  const Token &token = tokens_[pos_];
  if (pos_ + 1 < tokens_.size())
    ++pos_;
  return token;
}

bool Parser::accept(Tok kind) {
  if (!at(kind))
    return false;
  advance();
  return true;
}

bool Parser::expect(Tok kind, StringRef what) {
  if (accept(kind))
    return true;
  errorExpected(what);
  return false;
}

bool Parser::expectTerminator() {
  if (at(Tok::RBrace) || at(Tok::EndOfFile))
    return true;
  if (accept(Tok::Newline))
    return true;
  errorExpected("the end of the line");
  return false;
}

void Parser::skipNewlines() {
  while (at(Tok::Newline))
    advance();
}

void Parser::synchronize() {
  int depth = 0;
  while (!at(Tok::EndOfFile)) {
    if (at(Tok::Newline) && depth == 0)
      return;
    if (at(Tok::LBrace)) {
      // A broken block header: skip the body rather than misreading it.
      ++depth;
    } else if (at(Tok::RBrace)) {
      if (depth == 0)
        return;
      --depth;
    }
    advance();
  }
}

void Parser::errorAt(SMLoc loc, const Twine &detail) {
  diag_.syntax(loc, GenericHeadline, detail);
}

void Parser::errorExpected(const Twine &what) {
  bool ranOut = at(Tok::EndOfFile) && interpDepth_ == 0;

  // Report on the line with the mistake, and let recovery resume right after
  // it instead of swallowing the following line. Stopping at the statement's
  // start guarantees that recovery always moves forward.
  while (pos_ > stmtStart_ && tokens_[pos_ - 1].kind == Tok::Newline)
    --pos_;

  const Token &token = cur();
  if (token.kind == Tok::EndOfFile || token.kind == Tok::Newline) {
    if (pos_ == 0) {
      diag_.syntax(token.loc, GenericHeadline, "expected " + what, ranOut);
      return;
    }
    const Token &previous = tokens_[pos_ - 1];
    diag_.syntax(SMLoc::getFromPointer(previous.end), GenericHeadline,
                 "expected " + what + " after '" + previous.text + "'", ranOut);
    return;
  }
  diag_.syntax(token.loc, GenericHeadline,
               "expected " + what + ", found '" + token.text + "'", ranOut);
}

bool Parser::enterNesting() {
  if (++nesting_ <= MaxNesting)
    return true;
  errorAt(cur().loc, "this code is nested way too deep");
  return false;
}

// ---- Statements -------------------------------------------------------------

ArrayRef<const Stmt *> Parser::parseProgram() {
  SmallVector<const Stmt *, 32> stmts;
  for (;;) {
    skipNewlines();
    if (at(Tok::EndOfFile))
      break;
    if (at(Tok::RBrace)) {
      errorAt(cur().loc, "this '}' doesn't close anything");
      advance();
      continue;
    }
    if (const Stmt *stmt = parseStmt())
      stmts.push_back(stmt);
    else
      synchronize();
  }
  return ctx_.copy(stmts);
}

static bool declaresName(const Stmt *stmt) {
  switch (stmt->kind) {
  case Stmt::Kind::Decl:
  case Stmt::Kind::Func:
  case Stmt::Kind::Squad:
  case Stmt::Kind::Rizzup:
    return true;
  default:
    return false;
  }
}

void Parser::reportUnclosed(SMLoc open) {
  // Nested blocks all fail at end of input, so name only the innermost one.
  if (!reportedUnclosed_)
    diag_.syntax(open, UnclosedHeadline, "this '{' never gets closed",
                 /*atEnd=*/interpDepth_ == 0);
  reportedUnclosed_ = true;
}

bool Parser::parseBlock(Block &out) {
  if (at(Tok::Eq)) {
    errorAt(cur().loc, "'=' assigns a value, use '==' to compare");
    return false;
  }
  if (!at(Tok::LBrace)) {
    errorExpected("a '{' to start a block");
    return false;
  }
  NestingGuard guard(*this);
  if (!guard.ok)
    return false;

  SMLoc open = cur().loc;
  advance();

  SmallVector<const Stmt *, 16> stmts;
  bool needsScope = false;
  for (;;) {
    skipNewlines();
    if (accept(Tok::RBrace))
      break;
    if (at(Tok::EndOfFile)) {
      reportUnclosed(open);
      return false;
    }
    if (const Stmt *stmt = parseStmt()) {
      needsScope |= declaresName(stmt);
      stmts.push_back(stmt);
    } else {
      synchronize();
    }
  }
  out.stmts = ctx_.copy(stmts);
  out.needsScope = needsScope;
  return true;
}

const Stmt *Parser::parseStmt() {
  stmtStart_ = pos_;
  switch (cur().kind) {
  case Tok::KwLowkey:
  case Tok::KwHighkey:
    return parseDecl();
  case Tok::KwYap:
  case Tok::KwMumble:
  case Tok::KwBruh:
    return parsePrint();
  case Tok::KwVibecheck:
    return parseIf();
  case Tok::KwGrind:
    return parseWhile();
  case Tok::KwSpeedrun:
    return parseFor();
  case Tok::KwCook:
    // `cook (` starts a lambda used as an expression.
    if (peek(1).kind == Tok::Ident)
      return parseFuncStmt();
    return parseExprOrAssign();
  case Tok::KwItsgiving:
    return parseReturn();
  case Tok::KwYeet:
    return parseThrow();
  case Tok::KwTryna:
    return parseTry();
  case Tok::KwOngod:
    return parseAssert();
  case Tok::KwTouchgrass:
    return parseExit();
  case Tok::KwGhost:
  case Tok::KwSkibidi:
    return parseLoopControl();
  case Tok::KwServe:
    return parseServe();
  case Tok::KwItdepends:
    return parseMatch();
  case Tok::KwSquad:
    return parseSquad();
  case Tok::KwRizzup:
    return parseRizzup();
  case Tok::KwRegardless:
    errorAt(cur().loc, "'regardless' needs a tryna block right before it");
    return nullptr;
  case Tok::KwNvm:
  case Tok::KwNah:
    errorAt(cur().loc, "'" + cur().text + "' needs a vibecheck right before it");
    return nullptr;
  case Tok::KwCaught4k:
    errorAt(cur().loc, "'caught4k' needs a tryna block right before it");
    return nullptr;
  default:
    return parseExprOrAssign();
  }
}

const Stmt *Parser::parseDecl() {
  SMLoc loc = cur().loc;
  bool isConst = advance().kind == Tok::KwHighkey;
  if (!at(Tok::Ident) && !at(Tok::LBracket) && !at(Tok::LBrace)) {
    errorExpected("a variable name");
    return nullptr;
  }
  const Pattern *pattern = parsePatternPrimary();
  if (!pattern)
    return nullptr;
  if (!expect(Tok::Eq, "'=' and a starting value"))
    return nullptr;
  skipNewlines();
  const Expr *init = parseExpr();
  if (!init || !expectTerminator())
    return nullptr;
  return ctx_.make<DeclStmt>(loc, pattern, isConst, init);
}

const Stmt *Parser::parsePrint() {
  SMLoc loc = cur().loc;
  Tok keyword = advance().kind;
  PrintKind kind = keyword == Tok::KwYap      ? PrintKind::Yap
                   : keyword == Tok::KwMumble ? PrintKind::Mumble
                                              : PrintKind::Bruh;
  SmallVector<const Expr *, 4> args;
  if (!atTerminator()) {
    do {
      skipNewlines();
      const Expr *arg = parseExpr();
      if (!arg)
        return nullptr;
      args.push_back(arg);
    } while (accept(Tok::Comma));
  }
  if (!expectTerminator())
    return nullptr;
  return ctx_.make<PrintStmt>(loc, kind, ctx_.copy(args));
}

const Stmt *Parser::parseIf() {
  SMLoc loc = cur().loc;
  advance();

  SmallVector<IfArm, 4> arms;
  IfArm first;
  first.condition = parseExpr();
  if (!first.condition || !parseBlock(first.body))
    return nullptr;
  arms.push_back(first);

  const Block *elseBody = nullptr;
  for (;;) {
    // `nvm` and `nah` may start the next line, so look past newlines without
    // consuming them unless one of those keywords is really there.
    size_t saved = pos_;
    skipNewlines();
    if (accept(Tok::KwNvm)) {
      IfArm arm;
      arm.condition = parseExpr();
      if (!arm.condition || !parseBlock(arm.body))
        return nullptr;
      arms.push_back(arm);
      continue;
    }
    if (accept(Tok::KwNah)) {
      Block body;
      if (!parseBlock(body))
        return nullptr;
      elseBody = ctx_.make<Block>(body);
      break;
    }
    pos_ = saved;
    break;
  }
  return ctx_.make<IfStmt>(loc, ctx_.copy(arms), elseBody);
}

const Stmt *Parser::parseWhile() {
  SMLoc loc = cur().loc;
  advance();
  const Expr *condition = parseExpr();
  if (!condition)
    return nullptr;
  Block body;
  ++loopDepth_;
  bool ok = parseBlock(body);
  --loopDepth_;
  if (!ok)
    return nullptr;
  return ctx_.make<WhileStmt>(loc, condition, body);
}

bool Parser::parseLoopHeader(LoopHeader &out) {
  if (!at(Tok::Ident) && !at(Tok::LBracket) && !at(Tok::LBrace)) {
    errorExpected("a loop variable name");
    return false;
  }
  out.pattern = parsePatternPrimary();
  if (!out.pattern || !expect(Tok::KwIn, "'in'"))
    return false;
  out.start = parseExpr();
  if (!out.start)
    return false;
  if (accept(Tok::DotDot)) {
    out.end = parseExpr();
    if (!out.end)
      return false;
    if (accept(Tok::KwBy)) {
      out.step = parseExpr();
      if (!out.step)
        return false;
    }
  }
  return true;
}

const Stmt *Parser::parseFor() {
  SMLoc loc = cur().loc;
  advance();
  LoopHeader header;
  if (!parseLoopHeader(header))
    return nullptr;
  Block body;
  ++loopDepth_;
  bool ok = parseBlock(body);
  --loopDepth_;
  if (!ok)
    return nullptr;
  return ctx_.make<ForStmt>(loc, header, body);
}

bool Parser::parseParams(SmallVectorImpl<Param> &out) {
  if (!expect(Tok::LParen, "'(' and a parameter list"))
    return false;

  DenseSet<Symbol> seen;
  bool sawDefault = false;
  skipNewlines();
  while (!at(Tok::RParen)) {
    bool isRest = accept(Tok::DotDotDot);
    if (!at(Tok::Ident)) {
      errorExpected("a parameter name");
      return false;
    }
    const Token &param = advance();
    Symbol symbol = ctx_.intern(param.text);
    if (!seen.insert(symbol).second)
      diag_.syntax(param.loc, "two params with the same name? pick one bestie",
                   "parameter '" + param.text + "' is listed twice");

    const Expr *defaultValue = nullptr;
    if (!isRest && accept(Tok::Eq)) {
      defaultValue = parseExpr();
      if (!defaultValue)
        return false;
      sawDefault = true;
    } else if (!isRest && sawDefault) {
      errorAt(param.loc, "required parameters must come before ones with defaults");
    }
    out.push_back({symbol, defaultValue, isRest});
    skipNewlines();
    if (isRest) {
      if (!at(Tok::RParen)) {
        errorAt(cur().loc, "a ...rest parameter must be the last one");
        return false;
      }
      break;
    }
    if (!accept(Tok::Comma))
      break;
    skipNewlines();
  }
  return expect(Tok::RParen, "')' to close the parameter list");
}

const FunctionDecl *Parser::buildFunction(Symbol name, ArrayRef<Param> params,
                                          function_ref<bool(Block &)> parseBody) {
  // `ghost` and `skibidi` can't reach a loop outside the function.
  int savedLoopDepth = loopDepth_;
  bool savedSawServe = sawServe_;
  loopDepth_ = 0;
  sawServe_ = false;
  ++functionDepth_;
  Block body;
  bool ok = parseBody(body);
  --functionDepth_;
  bool isGenerator = sawServe_;
  sawServe_ = savedSawServe;
  loopDepth_ = savedLoopDepth;
  if (!ok)
    return nullptr;

  auto *decl = ctx_.make<FunctionDecl>();
  decl->name = name;
  decl->body = body;
  decl->isGenerator = isGenerator;
  for (const Param &param : params) {
    decl->hasRest |= param.isRest;
    decl->requiredParams += !param.isRest && !param.defaultValue;
  }
  SmallVector<Param, 4> copied(params.begin(), params.end());
  decl->params = ctx_.copy(copied);
  return decl;
}

const FunctionDecl *Parser::parseFunctionRest(Symbol name) {
  SmallVector<Param, 4> params;
  if (!parseParams(params))
    return nullptr;
  return buildFunction(name, params, [&](Block &body) { return parseBlock(body); });
}

const Stmt *Parser::parseFuncStmt() {
  SMLoc loc = cur().loc;
  advance();
  Symbol name = ctx_.intern(advance().text);
  const FunctionDecl *decl = parseFunctionRest(name);
  if (!decl)
    return nullptr;
  return ctx_.make<FuncStmt>(loc, decl);
}

const Stmt *Parser::parseReturn() {
  SMLoc loc = cur().loc;
  advance();
  if (functionDepth_ == 0)
    diag_.syntax(loc, "it's giving... nothing, you're not in a cook",
                 "'itsgiving' can only be used inside a function");
  const Expr *value = nullptr;
  if (!atTerminator()) {
    value = parseExpr();
    if (!value)
      return nullptr;
  }
  if (!expectTerminator())
    return nullptr;
  return ctx_.make<ReturnStmt>(loc, value);
}

const Stmt *Parser::parseThrow() {
  SMLoc loc = cur().loc;
  advance();
  const Expr *value = parseExpr();
  if (!value || !expectTerminator())
    return nullptr;
  return ctx_.make<ThrowStmt>(loc, value);
}

const Stmt *Parser::parseTry() {
  SMLoc loc = cur().loc;
  advance();
  Block body;
  if (!parseBlock(body))
    return nullptr;

  size_t saved = pos_;
  skipNewlines();
  Symbol errorName = nullptr;
  Block handler;
  if (accept(Tok::KwCaught4k)) {
    if (!at(Tok::Ident)) {
      errorExpected("a name for the error");
      return nullptr;
    }
    errorName = ctx_.intern(advance().text);
    if (!parseBlock(handler))
      return nullptr;
    saved = pos_;
    skipNewlines();
  }

  const Block *finallyBody = nullptr;
  if (accept(Tok::KwRegardless)) {
    Block block;
    if (!parseBlock(block))
      return nullptr;
    finallyBody = ctx_.make<Block>(block);
  } else if (errorName) {
    pos_ = saved; // Those newlines belong to whatever comes next.
  } else {
    errorExpected("'caught4k' or 'regardless' after the tryna block");
    return nullptr;
  }
  return ctx_.make<TryStmt>(loc, body, errorName, handler, finallyBody);
}

const Stmt *Parser::parseServe() {
  SMLoc loc = cur().loc;
  advance();
  if (functionDepth_ == 0)
    diag_.syntax(loc, "serving who? you're not in a cook",
                 "'serve' can only be used inside a function");
  sawServe_ = true;
  const Expr *value = nullptr;
  if (!atTerminator()) {
    value = parseExpr();
    if (!value)
      return nullptr;
  }
  if (!expectTerminator())
    return nullptr;
  return ctx_.make<ServeStmt>(loc, value);
}

const Stmt *Parser::parseMatch() {
  SMLoc loc = cur().loc;
  advance();
  const Expr *subject = parseExpr();
  if (!subject)
    return nullptr;
  if (!at(Tok::LBrace)) {
    errorExpected("a '{' to start the arms");
    return nullptr;
  }
  NestingGuard guard(*this);
  if (!guard.ok)
    return nullptr;
  SMLoc open = cur().loc;
  advance();

  SmallVector<MatchArm, 8> arms;
  const Block *elseBody = nullptr;
  for (;;) {
    skipNewlines();
    if (accept(Tok::RBrace))
      break;
    if (at(Tok::EndOfFile)) {
      reportUnclosed(open);
      return nullptr;
    }
    stmtStart_ = pos_;
    if (accept(Tok::KwNah)) {
      Block body;
      if (!parseBlock(body)) {
        synchronize();
        continue;
      }
      elseBody = ctx_.make<Block>(body);
      continue;
    }
    if (!atWord("on")) {
      errorExpected("'on' or 'nah'");
      synchronize();
      continue;
    }
    advance();
    MatchArm arm;
    arm.guard = nullptr;
    arm.pattern = parsePattern();
    bool ok = arm.pattern != nullptr;
    if (ok && atWord("when")) {
      advance();
      arm.guard = parseExpr();
      ok = arm.guard != nullptr;
    }
    if (ok)
      ok = parseBlock(arm.body);
    if (ok)
      arms.push_back(arm);
    else
      synchronize();
  }
  return ctx_.make<MatchStmt>(loc, subject, ctx_.copy(arms), elseBody);
}

const Stmt *Parser::parseSquad() {
  SMLoc loc = cur().loc;
  advance();
  if (!at(Tok::Ident)) {
    errorExpected("a name for the squad");
    return nullptr;
  }
  Symbol name = ctx_.intern(advance().text);

  const Expr *parent = nullptr;
  if (atWord("copies")) {
    advance();
    parent = parsePostfix();
    if (!parent)
      return nullptr;
  }

  if (!at(Tok::LBrace)) {
    errorExpected("a '{' to start the squad");
    return nullptr;
  }
  NestingGuard guard(*this);
  if (!guard.ok)
    return nullptr;
  SMLoc open = cur().loc;
  advance();

  ++squadDepth_;
  SmallVector<const FunctionDecl *, 8> methods;
  bool closed = false;
  for (;;) {
    skipNewlines();
    if (accept(Tok::RBrace)) {
      closed = true;
      break;
    }
    if (at(Tok::EndOfFile)) {
      reportUnclosed(open);
      break;
    }
    stmtStart_ = pos_;
    if (!at(Tok::KwCook) || peek(1).kind != Tok::Ident) {
      errorAt(cur().loc, "a squad body can only contain cook methods");
      synchronize();
      continue;
    }
    advance();
    Symbol methodName = ctx_.intern(advance().text);
    if (const FunctionDecl *decl = parseFunctionRest(methodName))
      methods.push_back(decl);
    else
      synchronize();
  }
  --squadDepth_;
  if (!closed)
    return nullptr;
  return ctx_.make<SquadStmt>(loc, name, parent, ctx_.copy(methods));
}

const Stmt *Parser::parseRizzup() {
  SMLoc loc = cur().loc;
  advance();
  if (!at(Tok::String)) {
    errorExpected("a file path in quotes");
    return nullptr;
  }
  const Token &pathToken = advance();
  StringRef path = pathToken.text;

  StringRef aliasName;
  if (atWord("as")) {
    advance();
    if (!at(Tok::Ident)) {
      errorExpected("a name after 'as'");
      return nullptr;
    }
    aliasName = advance().text;
  } else {
    aliasName = sys::path::stem(path);
    if (!Lexer::isValidName(aliasName)) {
      errorAt(pathToken.loc, "'" + aliasName + "' isn't a valid name, add 'as' and pick one");
      return nullptr;
    }
  }
  if (!expectTerminator())
    return nullptr;
  return ctx_.make<RizzupStmt>(loc, ctx_.save(path), ctx_.intern(aliasName));
}

// ---- Patterns ---------------------------------------------------------------

const Pattern *Parser::parsePattern() {
  SMLoc loc = cur().loc;
  const Pattern *first = parsePatternPrimary();
  if (!first || !at(Tok::Bar))
    return first;

  SmallVector<const Pattern *, 4> alternatives{first};
  while (accept(Tok::Bar)) {
    skipNewlines();
    const Pattern *next = parsePatternPrimary();
    if (!next)
      return nullptr;
    alternatives.push_back(next);
  }
  auto *pattern = ctx_.make<Pattern>(Pattern::Kind::Or, loc);
  pattern->items = ctx_.copy(alternatives);
  return pattern;
}

const Pattern *Parser::parsePatternPrimary() {
  NestingGuard guard(*this);
  if (!guard.ok)
    return nullptr;

  SMLoc loc = cur().loc;
  switch (cur().kind) {
  case Tok::Ident: {
    StringRef text = advance().text;
    if (text == "_")
      return ctx_.make<Pattern>(Pattern::Kind::Wildcard, loc);
    auto *pattern = ctx_.make<Pattern>(Pattern::Kind::Bind, loc);
    pattern->name = ctx_.intern(text);
    return pattern;
  }
  case Tok::LBracket:
    return parseListPattern();
  case Tok::LBrace:
    return parseMapPattern();
  case Tok::Number:
  case Tok::String:
  case Tok::KwNocap:
  case Tok::KwCap:
  case Tok::KwNpc:
  case Tok::Minus: {
    const Expr *low = parseUnary();
    if (!low)
      return nullptr;
    if (!accept(Tok::DotDot)) {
      auto *pattern = ctx_.make<Pattern>(Pattern::Kind::Literal, loc);
      pattern->low = low;
      return pattern;
    }
    const Expr *high = parseUnary();
    if (!high)
      return nullptr;
    auto *pattern = ctx_.make<Pattern>(Pattern::Kind::Range, loc);
    pattern->low = low;
    pattern->high = high;
    return pattern;
  }
  default:
    errorExpected("a pattern");
    return nullptr;
  }
}

const Pattern *Parser::parseListPattern() {
  auto *pattern = ctx_.make<Pattern>(Pattern::Kind::List, cur().loc);
  advance();
  SmallVector<const Pattern *, 4> items;
  skipNewlines();
  while (!at(Tok::RBracket)) {
    if (accept(Tok::DotDotDot)) {
      if (!at(Tok::Ident)) {
        errorExpected("a name after '...'");
        return nullptr;
      }
      pattern->rest = parsePatternPrimary();
      if (!pattern->rest)
        return nullptr;
      skipNewlines();
      break;
    }
    const Pattern *item = parsePattern();
    if (!item)
      return nullptr;
    items.push_back(item);
    skipNewlines();
    if (!accept(Tok::Comma))
      break;
    skipNewlines();
  }
  if (!expect(Tok::RBracket, "']' or ','"))
    return nullptr;
  pattern->items = ctx_.copy(items);
  return pattern;
}

const Pattern *Parser::parseMapPattern() {
  auto *pattern = ctx_.make<Pattern>(Pattern::Kind::Map, cur().loc);
  advance();
  SmallVector<MapPatternEntry, 4> entries;
  skipNewlines();
  while (!at(Tok::RBrace)) {
    const Expr *key = parseUnary();
    if (!key)
      return nullptr;
    skipNewlines();
    if (!expect(Tok::Colon, "':' after the key"))
      return nullptr;
    skipNewlines();
    const Pattern *value = parsePattern();
    if (!value)
      return nullptr;
    entries.push_back({key, value});
    skipNewlines();
    if (!accept(Tok::Comma))
      break;
    skipNewlines();
  }
  if (!expect(Tok::RBrace, "'}' or ','"))
    return nullptr;
  pattern->entries = ctx_.copy(entries);
  return pattern;
}

const Stmt *Parser::parseAssert() {
  SMLoc loc = cur().loc;
  advance();
  const Expr *condition = parseExpr();
  if (!condition)
    return nullptr;
  const Expr *message = nullptr;
  if (accept(Tok::Comma)) {
    skipNewlines();
    message = parseExpr();
    if (!message)
      return nullptr;
  }
  if (!expectTerminator())
    return nullptr;
  return ctx_.make<AssertStmt>(loc, condition, message);
}

const Stmt *Parser::parseExit() {
  SMLoc loc = cur().loc;
  advance();
  const Expr *code = nullptr;
  if (!atTerminator()) {
    code = parseExpr();
    if (!code)
      return nullptr;
  }
  if (!expectTerminator())
    return nullptr;
  return ctx_.make<ExitStmt>(loc, code);
}

const Stmt *Parser::parseLoopControl() {
  SMLoc loc = cur().loc;
  bool isBreak = advance().kind == Tok::KwGhost;
  if (loopDepth_ == 0) {
    if (isBreak)
      diag_.syntax(loc, "ghosting what? there's no loop here",
                   "'ghost' can only be used inside a loop");
    else
      diag_.syntax(loc, "skibidi outside a loop is just noise",
                   "'skibidi' can only be used inside a loop");
  }
  if (!expectTerminator())
    return nullptr;
  if (isBreak)
    return ctx_.make<BreakStmt>(loc);
  return ctx_.make<ContinueStmt>(loc);
}

static std::optional<AssignOp> assignOpFor(Tok kind) {
  switch (kind) {
  case Tok::Eq: return AssignOp::Set;
  case Tok::PlusEq: return AssignOp::Add;
  case Tok::MinusEq: return AssignOp::Sub;
  case Tok::StarEq: return AssignOp::Mul;
  case Tok::SlashEq: return AssignOp::Div;
  default: return std::nullopt;
  }
}

const Stmt *Parser::parseExprOrAssign() {
  SMLoc loc = cur().loc;
  const Expr *expr = parseExpr();
  if (!expr)
    return nullptr;

  std::optional<AssignOp> op = assignOpFor(cur().kind);
  if (!op) {
    if (!expectTerminator())
      return nullptr;
    return ctx_.make<ExprStmt>(loc, expr);
  }

  const auto *ident = dyn_cast<IdentExpr>(expr);
  const auto *index = dyn_cast<IndexExpr>(expr);
  const auto *member = dyn_cast<MemberExpr>(expr);
  bool assignable = (ident && ident->name->text != "me") || (index && !index->optional) ||
                    (member && !member->optional);
  if (!assignable) {
    errorAt(cur().loc, "you can only assign to a variable or an item like a[0]");
    return nullptr;
  }
  advance();
  skipNewlines();
  const Expr *value = parseExpr();
  if (!value || !expectTerminator())
    return nullptr;
  return ctx_.make<AssignStmt>(loc, expr, *op, value);
}

// ---- Expressions ------------------------------------------------------------

const Expr *Parser::parseExpr() {
  NestingGuard guard(*this);
  if (!guard.ok)
    return nullptr;
  const Expr *condition = parseBinary(1);
  if (!condition || !at(Tok::Question))
    return condition;

  SMLoc loc = cur().loc;
  advance();
  skipNewlines();
  const Expr *thenValue = parseExpr();
  if (!thenValue)
    return nullptr;
  skipNewlines();
  if (!expect(Tok::Colon, "':' and the other value of the ternary"))
    return nullptr;
  skipNewlines();
  const Expr *elseValue = parseExpr();
  if (!elseValue)
    return nullptr;
  return ctx_.make<TernaryExpr>(loc, condition, thenValue, elseValue);
}

struct BinaryInfo {
  BinaryOp op;
  int precedence;
  bool isPipe = false;
};

static std::optional<BinaryInfo> binaryInfo(Tok kind) {
  switch (kind) {
  case Tok::PipeArrow: return BinaryInfo{BinaryOp::Add, 1, /*isPipe=*/true};
  case Tok::QuestionQuestion: return BinaryInfo{BinaryOp::Nullish, 2};
  case Tok::OrOr: return BinaryInfo{BinaryOp::Or, 3};
  case Tok::AndAnd: return BinaryInfo{BinaryOp::And, 4};
  case Tok::EqEq: return BinaryInfo{BinaryOp::Eq, 5};
  case Tok::BangEq: return BinaryInfo{BinaryOp::Ne, 5};
  case Tok::Lt: return BinaryInfo{BinaryOp::Lt, 6};
  case Tok::LtEq: return BinaryInfo{BinaryOp::Le, 6};
  case Tok::Gt: return BinaryInfo{BinaryOp::Gt, 6};
  case Tok::GtEq: return BinaryInfo{BinaryOp::Ge, 6};
  case Tok::Plus: return BinaryInfo{BinaryOp::Add, 7};
  case Tok::Minus: return BinaryInfo{BinaryOp::Sub, 7};
  case Tok::Star: return BinaryInfo{BinaryOp::Mul, 8};
  case Tok::Slash: return BinaryInfo{BinaryOp::Div, 8};
  case Tok::Percent:
  case Tok::KwRatio: return BinaryInfo{BinaryOp::Mod, 8};
  default: return std::nullopt;
  }
}

const Expr *Parser::parseBinary(int minPrecedence) {
  const Expr *lhs = parseUnary();
  while (lhs) {
    std::optional<BinaryInfo> info = binaryInfo(cur().kind);
    if (!info || info->precedence < minPrecedence)
      break;
    SMLoc loc = cur().loc;
    advance();
    skipNewlines();
    const Expr *rhs = parseBinary(info->precedence + 1);
    if (!rhs)
      return nullptr;
    if (info->isPipe) {
      // `x |> f(y)` is exactly `f(x, y)`, and `x |> f` is `f(x)`.
      SmallVector<const Expr *, 4> args{lhs};
      const Expr *callee = rhs;
      const auto *call = dyn_cast<CallExpr>(rhs);
      if (call && rhs != lastParenthesized_) {
        args.append(call->args.begin(), call->args.end());
        callee = call->callee;
      }
      lhs = ctx_.make<CallExpr>(loc, callee, ctx_.copy(args));
      continue;
    }
    lhs = ctx_.make<BinaryExpr>(loc, info->op, lhs, rhs);
  }
  return lhs;
}

const Expr *Parser::parseUnary() {
  if (at(Tok::KwHoldup) || at(Tok::KwSendit)) {
    NestingGuard guard(*this);
    if (!guard.ok)
      return nullptr;
    SMLoc loc = cur().loc;
    bool isSpawn = advance().kind == Tok::KwSendit;
    const Expr *operand = parseUnary();
    if (!operand)
      return nullptr;
    if (!isSpawn)
      return ctx_.make<AwaitExpr>(loc, operand);
    const auto *call = dyn_cast<CallExpr>(operand);
    if (!call) {
      errorAt(loc, "'sendit' needs a function call, like sendit f()");
      return nullptr;
    }
    return ctx_.make<SpawnExpr>(loc, call);
  }
  if (at(Tok::Minus) || at(Tok::Bang) || at(Tok::KwSus)) {
    NestingGuard guard(*this);
    if (!guard.ok)
      return nullptr;
    SMLoc loc = cur().loc;
    UnaryOp op = advance().kind == Tok::Minus ? UnaryOp::Neg : UnaryOp::Not;
    const Expr *operand = parseUnary();
    if (!operand)
      return nullptr;
    return ctx_.make<UnaryExpr>(loc, op, operand);
  }
  return parsePower();
}

const Expr *Parser::parsePower() {
  const Expr *base = parsePostfix();
  if (!base || !at(Tok::StarStar))
    return base;
  NestingGuard guard(*this);
  if (!guard.ok)
    return nullptr;
  SMLoc loc = cur().loc;
  advance();
  skipNewlines();
  const Expr *exponent = parseUnary();
  if (!exponent)
    return nullptr;
  return ctx_.make<BinaryExpr>(loc, BinaryOp::Pow, base, exponent);
}

bool Parser::parseArgs(Tok closer, SmallVectorImpl<const Expr *> &out) {
  skipNewlines();
  while (!at(closer)) {
    const Expr *arg = parseSpreadOrExpr();
    if (!arg)
      return false;
    out.push_back(arg);
    skipNewlines();
    if (!accept(Tok::Comma))
      break;
    skipNewlines();
  }
  return expect(closer, closer == Tok::RParen ? "')' or ','" : "']' or ','");
}

const Expr *Parser::parseSpreadOrExpr() {
  if (!at(Tok::DotDotDot))
    return parseExpr();
  SMLoc loc = cur().loc;
  advance();
  const Expr *inner = parseExpr();
  if (!inner)
    return nullptr;
  return ctx_.make<SpreadExpr>(loc, inner);
}

const Expr *Parser::parseIndexOrSlice(const Expr *object, SMLoc loc, bool optional) {
  skipNewlines();
  const Expr *start = nullptr;
  if (!at(Tok::Colon)) {
    start = parseExpr();
    if (!start)
      return nullptr;
    skipNewlines();
  }
  if (!accept(Tok::Colon)) {
    if (!expect(Tok::RBracket, "']'"))
      return nullptr;
    return ctx_.make<IndexExpr>(loc, object, start, optional);
  }
  skipNewlines();
  const Expr *end = nullptr;
  if (!at(Tok::RBracket)) {
    end = parseExpr();
    if (!end)
      return nullptr;
    skipNewlines();
  }
  if (!expect(Tok::RBracket, "']'"))
    return nullptr;
  return ctx_.make<SliceExpr>(loc, object, start, end, optional);
}

const Expr *Parser::parsePostfix() {
  const Expr *expr = parsePrimary();
  // Once a chain has seen `?.`, every later step also tolerates npc.
  bool optional = false;
  while (expr) {
    SMLoc loc = cur().loc;
    if (accept(Tok::LParen)) {
      SmallVector<const Expr *, 4> args;
      if (!parseArgs(Tok::RParen, args))
        return nullptr;
      expr = ctx_.make<CallExpr>(expr->loc, expr, ctx_.copy(args), optional);
    } else if (accept(Tok::LBracket)) {
      expr = parseIndexOrSlice(expr, loc, optional);
    } else if (at(Tok::Dot) || at(Tok::QuestionDot)) {
      optional |= advance().kind == Tok::QuestionDot;
      if (!at(Tok::Ident)) {
        errorExpected("a field name");
        return nullptr;
      }
      Symbol name = ctx_.intern(advance().text);
      expr = ctx_.make<MemberExpr>(loc, expr, name, optional);
    } else {
      break;
    }
  }
  return expr;
}

const Expr *Parser::parsePrimary() {
  const Token &token = cur();
  switch (token.kind) {
  case Tok::Number: {
    advance();
    double value = 0;
    // Only overflow can fail here, since the lexer validated the shape.
    if (token.text.getAsDouble(value, /*AllowInexact=*/true))
      value = HUGE_VAL;
    return ctx_.make<NumberExpr>(token.loc, value);
  }
  case Tok::String:
    advance();
    return parseString(token);
  case Tok::KwNocap:
    advance();
    return ctx_.make<BoolExpr>(token.loc, true);
  case Tok::KwCap:
    advance();
    return ctx_.make<BoolExpr>(token.loc, false);
  case Tok::KwNpc:
    advance();
    return ctx_.make<NpcExpr>(token.loc);
  case Tok::Ident:
    if (peek(1).kind == Tok::Arrow)
      return parseArrow();
    advance();
    return ctx_.make<IdentExpr>(token.loc, ctx_.intern(token.text));
  case Tok::KwMe:
    advance();
    if (squadDepth_ == 0)
      diag_.syntax(token.loc, "who's me? you're not in a squad",
                   "'me' can only be used inside a squad method");
    return ctx_.make<IdentExpr>(token.loc, ctx_.intern("me"));
  case Tok::KwOg: {
    advance();
    if (squadDepth_ == 0)
      diag_.syntax(token.loc, "og who? you're not in a squad",
                   "'og' can only be used inside a squad method");
    if (!expect(Tok::Dot, "'.' and a method name"))
      return nullptr;
    if (!at(Tok::Ident)) {
      errorExpected("a method name");
      return nullptr;
    }
    return ctx_.make<OgMemberExpr>(token.loc, ctx_.intern(advance().text));
  }
  case Tok::LParen: {
    if (arrowAhead())
      return parseArrow();
    advance();
    skipNewlines();
    const Expr *inner = parseExpr();
    if (!inner)
      return nullptr;
    skipNewlines();
    if (!expect(Tok::RParen, "')'"))
      return nullptr;
    lastParenthesized_ = inner;
    return inner;
  }
  case Tok::LBracket:
    return parseList();
  case Tok::LBrace:
    return parseMap();
  case Tok::KwCook:
    return parseLambda();
  default:
    errorExpected("an expression");
    return nullptr;
  }
}

const Expr *Parser::parseList() {
  SMLoc loc = cur().loc;
  advance();
  SmallVector<const Expr *, 8> items;
  skipNewlines();
  if (!at(Tok::RBracket)) {
    const Expr *first = parseSpreadOrExpr();
    if (!first)
      return nullptr;
    skipNewlines();

    if (accept(Tok::KwSpeedrun)) {
      LoopHeader header;
      if (!parseLoopHeader(header))
        return nullptr;
      skipNewlines();
      const Expr *filter = nullptr;
      if (accept(Tok::KwVibecheck)) {
        filter = parseExpr();
        if (!filter)
          return nullptr;
        skipNewlines();
      }
      if (!expect(Tok::RBracket, "']' to end the comprehension"))
        return nullptr;
      return ctx_.make<ListCompExpr>(loc, first, header, filter);
    }

    items.push_back(first);
    if (!accept(Tok::Comma)) {
      if (!expect(Tok::RBracket, "']' or ','"))
        return nullptr;
      return ctx_.make<ListExpr>(loc, ctx_.copy(items));
    }
  }
  if (!parseArgs(Tok::RBracket, items))
    return nullptr;
  return ctx_.make<ListExpr>(loc, ctx_.copy(items));
}

const Expr *Parser::parseMap() {
  SMLoc loc = cur().loc;
  advance();
  SmallVector<MapPair, 8> pairs;
  skipNewlines();
  while (!at(Tok::RBrace)) {
    if (accept(Tok::DotDotDot)) {
      const Expr *spread = parseExpr();
      if (!spread)
        return nullptr;
      pairs.push_back({nullptr, spread});
      skipNewlines();
      if (!accept(Tok::Comma))
        break;
      skipNewlines();
      continue;
    }
    const Expr *key = parseExpr();
    if (!key)
      return nullptr;
    skipNewlines();
    if (!expect(Tok::Colon, "':' after the map key"))
      return nullptr;
    skipNewlines();
    const Expr *value = parseExpr();
    if (!value)
      return nullptr;
    pairs.push_back({key, value});
    skipNewlines();
    if (!accept(Tok::Comma))
      break;
    skipNewlines();
  }
  if (!expect(Tok::RBrace, "'}' or ','"))
    return nullptr;
  return ctx_.make<MapExpr>(loc, ctx_.copy(pairs));
}

const Expr *Parser::parseLambda() {
  SMLoc loc = cur().loc;
  advance();
  const FunctionDecl *decl = parseFunctionRest(nullptr);
  if (!decl)
    return nullptr;
  return ctx_.make<LambdaExpr>(loc, decl);
}

bool Parser::arrowAhead() const {
  int depth = 0;
  for (size_t i = pos_; i < tokens_.size(); ++i) {
    if (tokens_[i].kind == Tok::LParen) {
      ++depth;
    } else if (tokens_[i].kind == Tok::RParen && --depth == 0) {
      return i + 1 < tokens_.size() && tokens_[i + 1].kind == Tok::Arrow;
    } else if (tokens_[i].kind == Tok::EndOfFile) {
      return false;
    }
  }
  return false;
}

const Expr *Parser::parseArrow() {
  SMLoc loc = cur().loc;
  SmallVector<Param, 4> params;
  if (at(Tok::Ident)) {
    params.push_back({ctx_.intern(advance().text), nullptr, false});
  } else if (!parseParams(params)) {
    return nullptr;
  }
  if (!expect(Tok::Arrow, "'=>'"))
    return nullptr;
  skipNewlines();

  const FunctionDecl *decl = buildFunction(nullptr, params, [&](Block &body) {
    if (at(Tok::LBrace))
      return parseBlock(body);
    SMLoc valueLoc = cur().loc;
    const Expr *value = parseExpr();
    if (!value)
      return false;
    SmallVector<const Stmt *, 1> stmts{ctx_.make<ReturnStmt>(valueLoc, value)};
    body.stmts = ctx_.copy(stmts);
    return true;
  });
  if (!decl)
    return nullptr;
  return ctx_.make<LambdaExpr>(loc, decl);
}

const Expr *Parser::parseInterpolation(StringRef source) {
  std::vector<Token> tokens;
  Lexer(source, diag_).lexAll(tokens);

  std::swap(tokens_, tokens);
  size_t savedPos = pos_;
  size_t savedStmtStart = stmtStart_;
  pos_ = 0;
  stmtStart_ = 0;
  ++interpDepth_;

  const Expr *expr = parseExpr();
  if (expr && !at(Tok::EndOfFile)) {
    errorExpected("'}' to end the ${...}");
    expr = nullptr;
  }

  --interpDepth_;
  pos_ = savedPos;
  stmtStart_ = savedStmtStart;
  std::swap(tokens_, tokens);
  return expr;
}

const Expr *Parser::parseString(const Token &token) {
  const char *p = token.text.begin();
  const char *end = token.text.end();
  std::string literal;
  SmallVector<const Expr *, 4> parts;
  bool failed = false;

  auto flushLiteral = [&] {
    if (!literal.empty())
      parts.push_back(ctx_.make<StringExpr>(token.loc, ctx_.save(literal)));
    literal.clear();
  };

  while (p < end) {
    char c = *p;
    if (c == '\\' && p + 1 < end) {
      char escape = p[1];
      switch (escape) {
      case 'n': literal += '\n'; break;
      case 't': literal += '\t'; break;
      case 'r': literal += '\r'; break;
      case '0': literal += '\0'; break;
      case '"': literal += '"'; break;
      case '\\': literal += '\\'; break;
      case '$': literal += '$'; break;
      default:
        errorAt(SMLoc::getFromPointer(p),
                "'\\" + StringRef(&escape, 1) + "' isn't a valid escape");
        failed = true;
        break;
      }
      p += 2;
      continue;
    }
    if (c == '$' && p + 1 < end && p[1] == '{') {
      const char *close = Lexer::findInterpEnd(p + 2, end);
      flushLiteral();
      const Expr *part = parseInterpolation(StringRef(p + 2, close - (p + 2)));
      if (part)
        parts.push_back(part);
      else
        failed = true;
      p = close + 1;
      continue;
    }
    literal += c;
    ++p;
  }

  if (failed)
    return nullptr;

  bool hasExpr = false;
  for (const Expr *part : parts)
    hasExpr |= !isa<StringExpr>(part);
  if (!hasExpr) {
    // At most one part can exist here, because literals only split around ${}.
    std::string text = parts.empty() ? literal
                                     : cast<StringExpr>(parts[0])->value.str() + literal;
    return ctx_.make<StringExpr>(token.loc, ctx_.save(text));
  }
  flushLiteral();
  return ctx_.make<InterpExpr>(token.loc, ctx_.copy(parts));
}

} // namespace noob
