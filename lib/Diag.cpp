#include "noob/Diag.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace llvm;

namespace noob {

static constexpr size_t MaxReportedErrors = 20;

void Diag::syntax(SMLoc loc, const Twine &headline, const Twine &detail,
                  bool atEnd) {
  entries_.push_back({loc, headline.str(), detail.str(), atEnd});
}

static bool earlier(SMLoc a, SMLoc b) {
  return a.getPointer() < b.getPointer();
}

bool Diag::incomplete() const {
  if (entries_.empty())
    return false;
  const Entry *first = &entries_.front();
  for (const Entry &entry : entries_)
    if (earlier(entry.loc, first->loc))
      first = &entry;
  return first->atEnd;
}

void Diag::flush() {
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const Entry &a, const Entry &b) {
                     return earlier(a.loc, b.loc);
                   });

  size_t printed = 0, total = 0;
  SMLoc last;
  for (const Entry &entry : entries_) {
    // One mistake often trips two checks at the same spot. Keep the first.
    if (entry.loc == last)
      continue;
    last = entry.loc;
    ++total;
    if (printed < MaxReportedErrors) {
      report(entry.loc, entry.headline, entry.detail);
      ++printed;
    }
  }
  if (total > printed)
    errs() << "...and " << (total - printed) << " more errors\n";
  entries_.clear();
}

void Diag::report(SMLoc loc, const Twine &headline, const Twine &detail) {
  outs().flush();
  if (sober_) {
    sm_.PrintMessage(errs(), loc, SourceMgr::DK_Error, detail);
    return;
  }
  sm_.PrintMessage(errs(), loc, SourceMgr::DK_Error, headline);
  errs() << detail << "\n";
}

std::pair<unsigned, StringRef> Diag::lineAt(SMLoc loc) const {
  unsigned id = sm_.FindBufferContainingLoc(loc);
  if (id == 0)
    return {0, StringRef()};
  const MemoryBuffer *buffer = sm_.getMemoryBuffer(id);
  const char *begin = loc.getPointer();
  const char *end = begin;
  while (begin > buffer->getBufferStart() && begin[-1] != '\n')
    --begin;
  while (end < buffer->getBufferEnd() && *end != '\n' && *end != '\r')
    ++end;
  return {sm_.getLineAndColumn(loc, id).first, StringRef(begin, end - begin)};
}

std::string Diag::describe(SMLoc loc) const {
  unsigned id = sm_.FindBufferContainingLoc(loc);
  if (id == 0)
    return "<unknown>";
  auto [line, column] = sm_.getLineAndColumn(loc, id);
  return (sm_.getMemoryBuffer(id)->getBufferIdentifier() + ":" + Twine(line) +
          ":" + Twine(column))
      .str();
}

} // namespace noob
