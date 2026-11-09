#pragma once

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace noob {

/// Tunables, with defaults that follow Rust and Tokio.
struct RuntimeConfig {
  /// Stack for each task, generator, and worker thread. Pages commit lazily.
  size_t stackSize = 2u << 20;
  /// Worker threads for blocking work. Zero means one per logical CPU.
  unsigned workerThreads = 0;
};

/// The smallest stack the runtime accepts.
inline constexpr size_t MinStackSize = 128u << 10;
/// The main script's stack, unless RuntimeConfig::stackSize is larger.
inline constexpr size_t MainStackSize = 8u << 20;

/// A stackful coroutine. Subclass it to attach per-task state.
///
/// Once start() or prepare() succeeds, the Scheduler owns the Fiber and deletes
/// it after it finishes.
class Fiber {
public:
  virtual ~Fiber();

  /// Lowest usable address of this fiber's stack, for overflow checks.
  uintptr_t stackLow() const { return stackLow_; }

private:
  friend class Scheduler;

  enum class State { New, Ready, Running, Parked, Nested, Yielded, Done };

  void *sp_ = nullptr;
  void *mapping_ = nullptr;
  size_t mappingSize_ = 0;
  uintptr_t stackLow_ = 0;
  State state_ = State::New;
  Fiber *caller_ = nullptr;
  std::function<void()> entry_;

  // AddressSanitizer bookkeeping. Unused in normal builds.
  void *fakeStack_ = nullptr;
  const void *asanBottom_ = nullptr;
  size_t asanSize_ = 0;
};

/// A single-threaded cooperative scheduler.
///
/// Every method except the constructor and runUntil must be called from code
/// running on a fiber, unless noted. Only one Scheduler may exist at a time.
/// Waiting primitives may wake spuriously, so callers re-check their condition
/// in a loop around park().
class Scheduler {
public:
  explicit Scheduler(RuntimeConfig config = {});
  ~Scheduler();
  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;

  const RuntimeConfig &config() const { return config_; }

  /// Allocates a stack and queues the fiber to run `entry`. Returns false if
  /// no stack could be mapped, and the caller keeps ownership of the fiber.
  /// May also be called from outside any fiber.
  bool start(Fiber *fiber, std::function<void()> entry, size_t stackSize);

  /// Like start(), but doesn't queue it. Run it with resumeNested().
  bool prepare(Fiber *fiber, std::function<void()> entry, size_t stackSize);

  /// The running fiber, or null outside any fiber.
  Fiber *current() const { return current_; }

  /// Suspends the current fiber until someone calls wake() on it.
  void park();

  /// Queues a parked fiber. Does nothing if the fiber isn't parked.
  void wake(Fiber *fiber);

  /// Suspends only the current fiber for at least `ms` milliseconds.
  void sleepFor(double ms);

  /// Runs `work` on the thread pool and suspends only the current fiber until
  /// it's done. `work` must not touch interpreter state.
  void runBlocking(std::function<void()> work);

  /// Switches straight to a prepared or yielded fiber. Returns when it calls
  /// yieldToCaller() or finishes, even if it parks and resumes in between.
  void resumeNested(Fiber *fiber);

  /// Switches back to the fiber that last called resumeNested() on this one.
  void yieldToCaller();

  /// True if `fiber` is stopped at yieldToCaller() or hasn't started.
  bool isSuspended(const Fiber *fiber) const;

  /// Frees a fiber stopped at yieldToCaller() or never started. Destructors
  /// of objects on its stack do not run.
  void destroySuspended(Fiber *fiber);

  /// Drives fibers from outside any fiber until `done` returns true, which is
  /// checked between switches. Returns false on deadlock: nothing is ready, no
  /// timer is pending, and no blocking work is running.
  bool runUntil(llvm::function_ref<bool()> done);

private:
  struct Impl;

  bool setUp(Fiber *fiber, std::function<void()> entry, size_t stackSize);
  void switchTo(Fiber *from, Fiber *to, bool dying);
  void finishCurrent();
  void reapZombies();
  static void fiberMain();

  RuntimeConfig config_;
  std::unique_ptr<Impl> impl_;
  Fiber *current_ = nullptr;
};

} // namespace noob
