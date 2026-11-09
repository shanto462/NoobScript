#pragma once

#include "noob/Diag.h"
#include "noob/Token.h"

#include "llvm/ADT/StringRef.h"

#include <vector>

namespace noob {

class Lexer {
public:
  Lexer(llvm::StringRef source, Diag &diag, bool allowShebang = false);

  /// Appends every token through EndOfFile. Newlines are always emitted, since
  /// only the parser knows where they are insignificant.
  void lexAll(std::vector<Token> &out);

  /// Given a pointer just past "${", returns the matching '}', or null if
  /// there is none before `end`.
  static const char *findInterpEnd(const char *p, const char *end);

  /// True if `word` could be declared as a variable name.
  static bool isValidName(llvm::StringRef word);

private:
  void lexIdentifier(std::vector<Token> &out);
  void lexNumber(std::vector<Token> &out);
  void lexString(std::vector<Token> &out);
  void lexPunctuation(std::vector<Token> &out);

  const char *start_;
  const char *cur_;
  const char *end_;
  Diag &diag_;
};

} // namespace noob
