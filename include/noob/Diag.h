#pragma once

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

#include <string>
#include <utility>

namespace noob {

/// Reports errors as a joke headline plus a plain explanation.
///
/// Syntax errors are collected and printed in source order by flush(), which
/// lets the REPL throw them away when the input is merely unfinished. Runtime
/// errors go straight out through report().
class Diag {
public:
  Diag(llvm::SourceMgr &sm, bool sober) : sm_(sm), sober_(sober) {}

  /// Records a syntax error. `atEnd` marks an error caused by running out of
  /// input, which the REPL treats as "keep reading".
  void syntax(llvm::SMLoc loc, const llvm::Twine &headline,
              const llvm::Twine &detail, bool atEnd = false);

  bool hasErrors() const { return !entries_.empty(); }

  /// True if the earliest error is an out-of-input error.
  bool incomplete() const;

  void flush();
  void clear() { entries_.clear(); }

  /// Prints one error immediately.
  void report(llvm::SMLoc loc, const llvm::Twine &headline,
              const llvm::Twine &detail);

  /// Returns the 1-based line number and text of the line containing `loc`.
  std::pair<unsigned, llvm::StringRef> lineAt(llvm::SMLoc loc) const;

  /// Returns "file:line:col" for `loc`.
  std::string describe(llvm::SMLoc loc) const;

  bool sober() const { return sober_; }
  llvm::SourceMgr &sources() { return sm_; }

  /// Returns "file:line:col: detail" for the earliest syntax error, and
  /// discards all of them. Used when an error becomes a catchable value.
  std::string takeFirstError();

private:
  struct Entry {
    llvm::SMLoc loc;
    std::string headline;
    std::string detail;
    bool atEnd;
  };

  llvm::SourceMgr &sm_;
  bool sober_;
  llvm::SmallVector<Entry, 8> entries_;
};

} // namespace noob
