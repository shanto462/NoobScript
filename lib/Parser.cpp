#include "noob/Parser.h"

#include "noob/Lexer.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Casting.h"

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
  return stmt->kind == Stmt::Kind::Decl || stmt->kind == Stmt::Kind::Func;
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
      // Nested blocks all fail here, so name only the innermost one.
      if (!reportedUnclosed_)
        diag_.syntax(open, UnclosedHeadline, "this '{' never gets closed",
                     /*atEnd=*/interpDepth_ == 0);
      reportedUnclosed_ = true;
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
  if (!at(Tok::Ident)) {
    errorExpected("a variable name");
    return nullptr;
  }
  Symbol name = ctx_.intern(advance().text);
  if (!expect(Tok::Eq, "'=' and a starting value"))
    return nullptr;
  skipNewlines();
  const Expr *init = parseExpr();
  if (!init || !expectTerminator())
    return nullptr;
  return ctx_.make<DeclStmt>(loc, name, isConst, init);
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

const Stmt *Parser::parseFor() {
  SMLoc loc = cur().loc;
  advance();
  if (!at(Tok::Ident)) {
    errorExpected("a loop variable name");
    return nullptr;
  }
  Symbol var = ctx_.intern(advance().text);
  if (!expect(Tok::KwIn, "'in'"))
    return nullptr;

  const Expr *first = parseExpr();
  if (!first)
    return nullptr;

  const Expr *end = nullptr;
  const Expr *step = nullptr;
  if (accept(Tok::DotDot)) {
    end = parseExpr();
    if (!end)
      return nullptr;
    if (accept(Tok::KwBy)) {
      step = parseExpr();
      if (!step)
        return nullptr;
    }
  }

  Block body;
  ++loopDepth_;
  bool ok = parseBlock(body);
  --loopDepth_;
  if (!ok)
    return nullptr;

  if (end)
    return ctx_.make<ForRangeStmt>(loc, var, first, end, step, body);
  return ctx_.make<ForEachStmt>(loc, var, first, body);
}

const FunctionDecl *Parser::parseFunctionRest(Symbol name) {
  if (!expect(Tok::LParen, "'(' and a parameter list"))
    return nullptr;

  SmallVector<Symbol, 4> params;
  DenseSet<Symbol> seen;
  skipNewlines();
  if (!at(Tok::RParen)) {
    do {
      skipNewlines();
      if (!at(Tok::Ident)) {
        errorExpected("a parameter name");
        return nullptr;
      }
      const Token &param = advance();
      Symbol symbol = ctx_.intern(param.text);
      if (!seen.insert(symbol).second)
        diag_.syntax(param.loc, "two params with the same name? pick one bestie",
                     "parameter '" + param.text + "' is listed twice");
      params.push_back(symbol);
      skipNewlines();
    } while (accept(Tok::Comma));
  }
  if (!expect(Tok::RParen, "')' to close the parameter list"))
    return nullptr;

  // `ghost` and `skibidi` can't reach a loop outside the function.
  int savedLoopDepth = loopDepth_;
  loopDepth_ = 0;
  ++functionDepth_;
  Block body;
  bool ok = parseBlock(body);
  --functionDepth_;
  loopDepth_ = savedLoopDepth;
  if (!ok)
    return nullptr;

  auto *decl = ctx_.make<FunctionDecl>();
  decl->name = name;
  decl->params = ctx_.copy(params);
  decl->body = body;
  return decl;
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
  skipNewlines();
  if (!expect(Tok::KwCaught4k, "'caught4k' after the tryna block"))
    return nullptr;
  if (!at(Tok::Ident)) {
    errorExpected("a name for the error");
    return nullptr;
  }
  Symbol errorName = ctx_.intern(advance().text);
  Block handler;
  if (!parseBlock(handler))
    return nullptr;
  return ctx_.make<TryStmt>(loc, body, errorName, handler);
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

  if (!isa<IdentExpr>(expr) && !isa<IndexExpr>(expr)) {
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
  return parseBinary(1);
}

struct BinaryInfo {
  BinaryOp op;
  int precedence;
};

static std::optional<BinaryInfo> binaryInfo(Tok kind) {
  switch (kind) {
  case Tok::OrOr: return BinaryInfo{BinaryOp::Or, 1};
  case Tok::AndAnd: return BinaryInfo{BinaryOp::And, 2};
  case Tok::EqEq: return BinaryInfo{BinaryOp::Eq, 3};
  case Tok::BangEq: return BinaryInfo{BinaryOp::Ne, 3};
  case Tok::Lt: return BinaryInfo{BinaryOp::Lt, 4};
  case Tok::LtEq: return BinaryInfo{BinaryOp::Le, 4};
  case Tok::Gt: return BinaryInfo{BinaryOp::Gt, 4};
  case Tok::GtEq: return BinaryInfo{BinaryOp::Ge, 4};
  case Tok::Plus: return BinaryInfo{BinaryOp::Add, 5};
  case Tok::Minus: return BinaryInfo{BinaryOp::Sub, 5};
  case Tok::Star: return BinaryInfo{BinaryOp::Mul, 6};
  case Tok::Slash: return BinaryInfo{BinaryOp::Div, 6};
  case Tok::Percent:
  case Tok::KwRatio: return BinaryInfo{BinaryOp::Mod, 6};
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
    lhs = ctx_.make<BinaryExpr>(loc, info->op, lhs, rhs);
  }
  return lhs;
}

const Expr *Parser::parseUnary() {
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
    const Expr *arg = parseExpr();
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

const Expr *Parser::parsePostfix() {
  const Expr *expr = parsePrimary();
  while (expr) {
    SMLoc loc = cur().loc;
    if (accept(Tok::LParen)) {
      SmallVector<const Expr *, 4> args;
      if (!parseArgs(Tok::RParen, args))
        return nullptr;
      expr = ctx_.make<CallExpr>(expr->loc, expr, ctx_.copy(args));
    } else if (accept(Tok::LBracket)) {
      skipNewlines();
      const Expr *index = parseExpr();
      if (!index)
        return nullptr;
      skipNewlines();
      if (!expect(Tok::RBracket, "']'"))
        return nullptr;
      expr = ctx_.make<IndexExpr>(loc, expr, index);
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
    advance();
    return ctx_.make<IdentExpr>(token.loc, ctx_.intern(token.text));
  case Tok::LParen: {
    advance();
    skipNewlines();
    const Expr *inner = parseExpr();
    if (!inner)
      return nullptr;
    skipNewlines();
    if (!expect(Tok::RParen, "')'"))
      return nullptr;
    return inner;
  }
  case Tok::LBracket:
    return parseList();
  case Tok::LBrace:
    return parseMap();
  case Tok::KwCook:
    return parseLambda();
  case Tok::KwReserved:
    errorAt(token.loc, "'" + token.text + "' is reserved for a future version");
    return nullptr;
  default:
    errorExpected("an expression");
    return nullptr;
  }
}

const Expr *Parser::parseList() {
  SMLoc loc = cur().loc;
  advance();
  SmallVector<const Expr *, 8> items;
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
