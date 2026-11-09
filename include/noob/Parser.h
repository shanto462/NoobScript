#pragma once

#include "noob/AST.h"
#include "noob/Diag.h"
#include "noob/Token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <vector>

namespace noob {

/// Recursive-descent parser and syntax checker.
///
/// Errors are recorded in the Diag. After an error the parser resynchronizes
/// at the next line, so one run can report several mistakes. A program is only
/// safe to run if the Diag has no errors afterwards.
class Parser {
public:
  Parser(AstContext &ctx, Diag &diag, llvm::StringRef source,
         bool allowShebang = false);

  llvm::ArrayRef<const Stmt *> parseProgram();

private:
  // Statements.
  const Stmt *parseStmt();
  const Stmt *parseDecl();
  const Stmt *parsePrint();
  const Stmt *parseIf();
  const Stmt *parseWhile();
  const Stmt *parseFor();
  const Stmt *parseFuncStmt();
  const Stmt *parseReturn();
  const Stmt *parseThrow();
  const Stmt *parseTry();
  const Stmt *parseAssert();
  const Stmt *parseExit();
  const Stmt *parseLoopControl();
  const Stmt *parseExprOrAssign();
  const Stmt *parseServe();
  const Stmt *parseMatch();
  const Stmt *parseSquad();
  const Stmt *parseRizzup();
  bool parseLoopHeader(LoopHeader &out);
  const Pattern *parsePattern();
  const Pattern *parsePatternPrimary();
  const Pattern *parseListPattern();
  const Pattern *parseMapPattern();
  bool parseBlock(Block &out);
  const FunctionDecl *parseFunctionRest(Symbol name);
  bool parseParams(llvm::SmallVectorImpl<Param> &out);
  const FunctionDecl *buildFunction(Symbol name, llvm::ArrayRef<Param> params,
                                    llvm::function_ref<bool(Block &)> parseBody);

  // Expressions.
  const Expr *parseExpr();
  const Expr *parseBinary(int minPrecedence);
  const Expr *parseUnary();
  const Expr *parsePower();
  const Expr *parsePostfix();
  const Expr *parsePrimary();
  const Expr *parseString(const Token &token);
  const Expr *parseInterpolation(llvm::StringRef source);
  const Expr *parseList();
  const Expr *parseMap();
  const Expr *parseLambda();
  const Expr *parseArrow();
  bool arrowAhead() const;
  const Expr *parseIndexOrSlice(const Expr *object, llvm::SMLoc loc, bool optional);
  const Expr *parseSpreadOrExpr();
  bool parseArgs(Tok closer, llvm::SmallVectorImpl<const Expr *> &out);

  // Token helpers.
  const Token &cur() const { return tokens_[pos_]; }
  const Token &peek(size_t ahead) const;
  bool at(Tok kind) const { return cur().kind == kind; }
  bool atWord(llvm::StringRef word) const {
    return at(Tok::Ident) && cur().text == word;
  }
  bool atTerminator() const;
  const Token &advance();
  bool accept(Tok kind);
  bool expect(Tok kind, llvm::StringRef what);
  bool expectTerminator();
  void skipNewlines();
  void synchronize();

  void errorExpected(const llvm::Twine &what);
  void errorAt(llvm::SMLoc loc, const llvm::Twine &detail);
  void reportUnclosed(llvm::SMLoc open);

  /// Bounds recursion so absurdly nested input can't overflow the stack.
  bool enterNesting();
  struct NestingGuard;

  AstContext &ctx_;
  Diag &diag_;
  std::vector<Token> tokens_;
  size_t pos_ = 0;
  size_t stmtStart_ = 0;
  int loopDepth_ = 0;
  int functionDepth_ = 0;
  int squadDepth_ = 0;
  bool sawServe_ = false;
  /// Lets `x |> (f())` call the result of f() rather than become f(x).
  const Expr *lastParenthesized_ = nullptr;
  int nesting_ = 0;
  int interpDepth_ = 0;
  bool reportedUnclosed_ = false;
};

} // namespace noob
