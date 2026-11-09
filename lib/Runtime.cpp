#include "noob/Runtime.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "noob_switch_context is only implemented for AArch64"
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NOOB_ASAN 1
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif
#endif

#define NOOB_STRINGIFY_IMPL(x) #x
#define NOOB_STRINGIFY(x) NOOB_STRINGIFY_IMPL(x)
#define NOOB_ASM_SYMBOL(name) NOOB_STRINGIFY(__USER_LABEL_PREFIX__) #name

// Saves the AAPCS64 callee-saved state on the current stack and adopts another.
// Only callee-saved registers need keeping, because to the compiler this is an
// ordinary call. x18 is left alone since it's reserved by the platform.
__asm__(".text\n"
        ".globl " NOOB_ASM_SYMBOL(noob_switch_context) "\n"
        ".p2align 2\n"
        NOOB_ASM_SYMBOL(noob_switch_context) ":\n"
        "  sub sp, sp, #160\n"
        "  stp x19, x20, [sp, #0]\n"
        "  stp x21, x22, [sp, #16]\n"
        "  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n"
        "  stp x27, x28, [sp, #64]\n"
        "  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n"
        "  stp d10, d11, [sp, #112]\n"
        "  stp d12, d13, [sp, #128]\n"
        "  stp d14, d15, [sp, #144]\n"
        "  mov x9, sp\n"
        "  str x9, [x0]\n"
        "  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0]\n"
        "  ldp x21, x22, [sp, #16]\n"
        "  ldp x23, x24, [sp, #32]\n"
        "  ldp x25, x26, [sp, #48]\n"
        "  ldp x27, x28, [sp, #64]\n"
        "  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n"
        "  ldp d10, d11, [sp, #112]\n"
        "  ldp d12, d13, [sp, #128]\n"
        "  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #160\n"
        "  ret\n");

extern "C" void noob_switch_context(void **saveSp, void *loadSp);

namespace noob {

using Clock = std::chrono::steady_clock;

// Must match the frame noob_switch_context builds. A new fiber's frame only
// needs x30, which is the first thing it "returns" to, and a null x29 so frame
// walkers stop at the fiber's entry.
static constexpr size_t SwitchFrameSize = 160;
static constexpr size_t SwitchFrameX30Slot = 11;

// Keeps a busy spawn loop off mmap without pinning unbounded address space.
static constexpr size_t MaxFreeStacks = 256;

// Far beyond any real sleep, and small enough that converting to the clock's
// nanoseconds can't overflow.
static constexpr double MaxSleepMs = 1e12;

// fiberMain is entered by a raw return, so it can't be passed an argument.
static Scheduler *ActiveScheduler = nullptr;

// ---- Fiber ------------------------------------------------------------------

Fiber::~Fiber() = default;

// ---- Worker pool ------------------------------------------------------------

namespace {

struct Job {
  std::function<void()> work;
  Fiber *waiter;
  bool done = false; // Only touched on the scheduler's thread.
};

/// Runs blocking C++ work off the scheduler's thread.
///
/// A pool is never freed, and its threads are never joined. A worker may sit
/// in a blocking read for the life of the process, so joining could hang at
/// exit, and freeing the mutex under it would be a use-after-free the moment
/// the read returned. Abandoning a pool costs one small allocation, and idle
/// workers still exit, so only a worker that really is stuck stays behind.
struct WorkerPool {
  WorkerPool(size_t stackSize, unsigned maxThreads)
      : stackSize(stackSize), maxThreads(maxThreads) {}

  /// Queues `job`, adding a thread if none is free to take it. Returns false,
  /// with the job not queued, if the pool has no thread and can't create one.
  bool submit(Job *job) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push_back(job);
    if (queue.size() > idle && threads < maxThreads)
      spawnWorker();
    if (threads == 0) {
      queue.pop_back();
      return false;
    }
    workReady.notify_one();
    return true;
  }

  std::mutex mutex;
  std::condition_variable workReady; // Workers wait here.
  std::condition_variable workDone;  // The scheduler waits here.
  std::deque<Job *> queue;
  std::vector<Job *> completed;
  size_t stackSize;
  unsigned maxThreads;
  unsigned threads = 0;
  unsigned idle = 0;
  bool orphaned = false;

private:
  void spawnWorker() {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
      return;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, stackSize);
    pthread_t thread;
    if (pthread_create(&thread, &attr, workerMain, this) == 0)
      ++threads;
    pthread_attr_destroy(&attr);
  }

  static void *workerMain(void *arg) {
    auto *pool = static_cast<WorkerPool *>(arg);
    std::unique_lock<std::mutex> lock(pool->mutex);
    for (;;) {
      ++pool->idle;
      pool->workReady.wait(
          lock, [&] { return !pool->queue.empty() || pool->orphaned; });
      --pool->idle;
      if (pool->orphaned)
        return nullptr;

      Job *job = pool->queue.front();
      pool->queue.pop_front();
      lock.unlock();
      job->work();
      lock.lock();

      if (pool->orphaned) {
        // The fiber waiting on this job will never run again.
        lock.unlock();
        delete job;
        return nullptr;
      }
      pool->completed.push_back(job);
      pool->workDone.notify_one();
    }
  }
};

struct Timer {
  Clock::time_point deadline;
  uint64_t sequence;
  Fiber *fiber;
  bool *fired; // Lives on the sleeping fiber's stack.
};

struct FiresLater {
  bool operator()(const Timer &a, const Timer &b) const {
    return std::tie(a.deadline, a.sequence) > std::tie(b.deadline, b.sequence);
  }
};

} // namespace

// ---- Scheduler state --------------------------------------------------------

struct Scheduler::Impl {
  explicit Impl(Scheduler &owner)
      : owner(owner), pageSize(static_cast<size_t>(sysconf(_SC_PAGESIZE))) {
    workerStackSize = roundedStackSize(owner.config_.stackSize);
    taskMappingSize = workerStackSize + pageSize;
  }

  size_t roundedStackSize(size_t requested) const {
    size_t size = std::max(requested, MinStackSize);
    return (size + pageSize - 1) & ~(pageSize - 1);
  }

  /// Finds or maps a stack with a guard page at its low end.
  bool acquireStack(size_t stackSize, void *&mapping, size_t &mappingSize) {
    // Guards the rounding and guard-page arithmetic below. No real mapping
    // gets near this, so mmap can report the failure for everything smaller.
    if (stackSize > SIZE_MAX / 2)
      return false;
    mappingSize = roundedStackSize(stackSize) + pageSize;
    if (mappingSize == taskMappingSize && !freeStacks.empty()) {
      mapping = freeStacks.back();
      freeStacks.pop_back();
      return true;
    }
    int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_STACK
    flags |= MAP_STACK;
#endif
    mapping = mmap(nullptr, mappingSize, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mapping == MAP_FAILED)
      return false;
    if (mprotect(mapping, pageSize, PROT_NONE) != 0) {
      munmap(mapping, mappingSize);
      return false;
    }
    return true;
  }

  void releaseStack(void *mapping, size_t mappingSize) {
#if NOOB_ASAN
    // Frames that never returned leave redzones behind. Shadow memory outlives
    // the mapping, so this matters when unmapping too.
    __asan_unpoison_memory_region(static_cast<char *>(mapping) + pageSize,
                                  mappingSize - pageSize);
#endif
    if (mappingSize == taskMappingSize && freeStacks.size() < MaxFreeStacks) {
      freeStacks.push_back(mapping);
      return;
    }
    munmap(mapping, mappingSize);
  }

  void releaseFiber(Fiber *fiber) {
    live.erase(fiber);
    releaseStack(fiber->mapping_, fiber->mappingSize_);
    delete fiber;
  }

  WorkerPool &workerPool() {
    if (!pool) {
      unsigned threads = owner.config_.workerThreads;
      if (threads == 0)
        threads = std::max(std::thread::hardware_concurrency(), 1u);
      pool = new WorkerPool(workerStackSize, threads);
    }
    return *pool;
  }

  void completeFinishedJobs() {
    // Fibers that never use the pool shouldn't pay for its mutex on each turn.
    if (inFlight.empty())
      return;
    std::vector<Job *> finished;
    {
      std::lock_guard<std::mutex> lock(pool->mutex);
      finished.swap(pool->completed);
    }
    for (Job *job : finished) {
      inFlight.erase(job);
      job->done = true;
      owner.wake(job->waiter);
    }
  }

  void fireDueTimers() {
    if (timers.empty())
      return;
    Clock::time_point now = Clock::now();
    while (!timers.empty() && timers.top().deadline <= now) {
      Timer timer = timers.top();
      timers.pop();
      *timer.fired = true;
      owner.wake(timer.fiber);
    }
  }

  /// Blocks until the next timer is due or a blocking job completes.
  void waitForEvent() {
    bool hasDeadline = !timers.empty();
    Clock::time_point deadline;
    if (hasDeadline)
      deadline = timers.top().deadline;
    if (!pool) {
      std::this_thread::sleep_until(deadline);
      return;
    }
    std::unique_lock<std::mutex> lock(pool->mutex);
    auto completed = [&] { return !pool->completed.empty(); };
    if (hasDeadline)
      pool->workDone.wait_until(lock, deadline, completed);
    else
      pool->workDone.wait(lock, completed);
  }

  /// Cuts the pool loose from this scheduler, which is going away. Jobs left
  /// in `inFlight` afterwards are still running on a worker.
  void abandonPool() {
    if (!pool)
      return;
    std::lock_guard<std::mutex> lock(pool->mutex);
    for (Job *job : pool->completed) {
      inFlight.erase(job);
      delete job;
    }
    pool->completed.clear();
    for (Job *job : pool->queue) {
      inFlight.erase(job);
      delete job;
    }
    pool->queue.clear();
    pool->orphaned = true;
    pool->workReady.notify_all();
  }

#if NOOB_ASAN
  /// Completes the switch that landed on the current stack. `fakeStack` is
  /// what this stack saved when it was switched away from, null on first entry.
  void finishSwitchAnnotation(void *fakeStack) {
    const void *oldBottom = nullptr;
    size_t oldSize = 0;
    __sanitizer_finish_switch_fiber(fakeStack, &oldBottom, &oldSize);
    if (arrivedFromScheduler) {
      schedulerBottom = oldBottom;
      schedulerSize = oldSize;
    }
  }

  void *schedulerFakeStack = nullptr;
  const void *schedulerBottom = nullptr;
  size_t schedulerSize = 0;
  bool arrivedFromScheduler = false;
#else
  void finishSwitchAnnotation(void *) {}
#endif

  Scheduler &owner;
  size_t pageSize;
  size_t workerStackSize;
  size_t taskMappingSize;
  void *schedulerSp = nullptr;
  std::deque<Fiber *> ready;
  std::priority_queue<Timer, std::vector<Timer>, FiresLater> timers;
  uint64_t nextTimerSequence = 0;
  std::vector<void *> freeStacks;
  std::unordered_set<Fiber *> live;
  std::unordered_set<Job *> inFlight;
  Fiber *zombie = nullptr;
  WorkerPool *pool = nullptr; // Never freed. See WorkerPool.
};

// ---- Scheduler --------------------------------------------------------------

Scheduler::Scheduler(RuntimeConfig config)
    : config_(config), impl_(std::make_unique<Impl>(*this)) {
  assert(!ActiveScheduler && "only one Scheduler may exist at a time");
  ActiveScheduler = this;
}

Scheduler::~Scheduler() {
  Impl &impl = *impl_;
  impl.abandonPool();
  ActiveScheduler = nullptr;

  // exit() on a fiber lands here on that fiber's stack, which can't be unmapped
  // from under us. The process is ending, so let the OS have everything.
  if (current_)
    return;

  reapZombies();

  // A running job may be writing through pointers into its fiber's stack, so
  // that fiber has to stay mapped. It can never run again, so it's a leak of
  // address space, not a hazard.
  for (Job *job : impl.inFlight)
    impl.live.erase(job->waiter);

  std::unordered_set<Fiber *> live = std::move(impl.live);
  impl.live.clear();
  for (Fiber *fiber : live)
    impl.releaseFiber(fiber);

  for (void *mapping : impl.freeStacks)
    munmap(mapping, impl.taskMappingSize);
}

bool Scheduler::setUp(Fiber *fiber, std::function<void()> entry,
                      size_t stackSize) {
  Impl &impl = *impl_;
  void *mapping = nullptr;
  size_t mappingSize = 0;
  if (!impl.acquireStack(stackSize, mapping, mappingSize))
    return false;

  uintptr_t low = reinterpret_cast<uintptr_t>(mapping) + impl.pageSize;
  size_t usable = mappingSize - impl.pageSize;
  // Page alignment of the top gives the 16-byte alignment AArch64 requires.
  auto *frame = reinterpret_cast<uintptr_t *>(low + usable - SwitchFrameSize);
  std::memset(frame, 0, SwitchFrameSize);
  frame[SwitchFrameX30Slot] = reinterpret_cast<uintptr_t>(&Scheduler::fiberMain);

  fiber->sp_ = frame;
  fiber->mapping_ = mapping;
  fiber->mappingSize_ = mappingSize;
  fiber->stackLow_ = low;
  fiber->entry_ = std::move(entry);
#if NOOB_ASAN
  fiber->asanBottom_ = reinterpret_cast<const void *>(low);
  fiber->asanSize_ = usable;
#else
  // Fiber's layout can't depend on the build, so these stay declared.
  (void)fiber->fakeStack_;
  (void)fiber->asanBottom_;
  (void)fiber->asanSize_;
#endif
  impl.live.insert(fiber);
  return true;
}

bool Scheduler::start(Fiber *fiber, std::function<void()> entry,
                      size_t stackSize) {
  if (!setUp(fiber, std::move(entry), stackSize))
    return false;
  fiber->state_ = Fiber::State::Ready;
  impl_->ready.push_back(fiber);
  return true;
}

bool Scheduler::prepare(Fiber *fiber, std::function<void()> entry,
                        size_t stackSize) {
  if (!setUp(fiber, std::move(entry), stackSize))
    return false;
  fiber->state_ = Fiber::State::New;
  return true;
}

void Scheduler::switchTo(Fiber *from, Fiber *to, bool dying) {
  Impl &impl = *impl_;
  void **saveSp = from ? &from->sp_ : &impl.schedulerSp;
  void *loadSp = to ? to->sp_ : impl.schedulerSp;
  current_ = to;
  if (to)
    to->state_ = Fiber::State::Running;

#if NOOB_ASAN
  void **fakeStack = from ? &from->fakeStack_ : &impl.schedulerFakeStack;
  impl.arrivedFromScheduler = !from;
  // A null save tells ASan this stack is dying, so it frees the fake stack.
  __sanitizer_start_switch_fiber(dying ? nullptr : fakeStack,
                                 to ? to->asanBottom_ : impl.schedulerBottom,
                                 to ? to->asanSize_ : impl.schedulerSize);
#else
  (void)dying;
#endif

  noob_switch_context(saveSp, loadSp);

#if NOOB_ASAN
  impl.finishSwitchAnnotation(*fakeStack);
#endif
}

void Scheduler::fiberMain() {
  Scheduler *self = ActiveScheduler;
  self->impl_->finishSwitchAnnotation(nullptr);

  Fiber *fiber = self->current_;
  fiber->entry_();
  // Captures may own interpreter state. Release them while this is still the
  // current fiber, because the Fiber object is deleted later from another stack.
  fiber->entry_ = nullptr;
  self->finishCurrent();
}

void Scheduler::finishCurrent() {
  Fiber *self = current_;
  self->state_ = Fiber::State::Done;
  // Can't free this stack while standing on it. Whoever we switch to does it.
  assert(!impl_->zombie);
  impl_->zombie = self;
  switchTo(self, self->caller_, /*dying=*/true);
  __builtin_trap(); // A finished fiber is never resumed.
}

void Scheduler::reapZombies() {
  Impl &impl = *impl_;
  if (!impl.zombie)
    return;
  Fiber *zombie = impl.zombie;
  impl.zombie = nullptr;
  impl.releaseFiber(zombie);
}

void Scheduler::park() {
  assert(current_ && "park() outside a fiber");
  Fiber *self = current_;
  self->state_ = Fiber::State::Parked;
  switchTo(self, nullptr, /*dying=*/false);
}

void Scheduler::wake(Fiber *fiber) {
  if (fiber->state_ != Fiber::State::Parked)
    return;
  fiber->state_ = Fiber::State::Ready;
  impl_->ready.push_back(fiber);
}

void Scheduler::sleepFor(double ms) {
  assert(current_ && "sleepFor() outside a fiber");
  Impl &impl = *impl_;
  // Written so NaN also lands on zero.
  ms = (ms > 0) ? std::min(ms, MaxSleepMs) : 0;
  auto delay = std::chrono::ceil<Clock::duration>(
      std::chrono::duration<double, std::milli>(ms));

  // Waiting on the timer's own firing, rather than re-reading the clock, means
  // a fiber can't leave here with its heap entry still pointing at it. Even a
  // zero sleep goes through the heap so other ready fibers get a turn first.
  bool fired = false;
  impl.timers.push(
      {Clock::now() + delay, impl.nextTimerSequence++, current_, &fired});
  while (!fired)
    park();
}

void Scheduler::runBlocking(std::function<void()> work) {
  assert(current_ && "runBlocking() outside a fiber");
  Impl &impl = *impl_;
  Job *job = new Job{std::move(work), current_};
  if (!impl.workerPool().submit(job)) {
    // Out of threads with none running. Stalling the other fibers for a while
    // beats never finishing this one.
    job->work();
    delete job;
    return;
  }
  impl.inFlight.insert(job);
  while (!job->done)
    park();
  delete job;
}

void Scheduler::resumeNested(Fiber *fiber) {
  assert(current_ && "resumeNested() outside a fiber");
  assert(isSuspended(fiber) && "can only resume a prepared or yielded fiber");
  Fiber *self = current_;
  fiber->caller_ = self;
  self->state_ = Fiber::State::Nested;
  switchTo(self, fiber, /*dying=*/false);
  reapZombies();
}

void Scheduler::yieldToCaller() {
  assert(current_ && current_->caller_ && "yieldToCaller() with no caller");
  Fiber *self = current_;
  self->state_ = Fiber::State::Yielded;
  switchTo(self, self->caller_, /*dying=*/false);
}

bool Scheduler::isSuspended(const Fiber *fiber) const {
  return fiber->state_ == Fiber::State::New ||
         fiber->state_ == Fiber::State::Yielded;
}

void Scheduler::destroySuspended(Fiber *fiber) {
  assert(isSuspended(fiber) && "fiber is still running or waiting");
  impl_->releaseFiber(fiber);
}

bool Scheduler::runUntil(llvm::function_ref<bool()> done) {
  assert(!current_ && "runUntil() inside a fiber");
  Impl &impl = *impl_;
  for (;;) {
    if (done())
      return true;
    impl.completeFinishedJobs();
    impl.fireDueTimers();

    if (!impl.ready.empty()) {
      Fiber *next = impl.ready.front();
      impl.ready.pop_front();
      switchTo(nullptr, next, /*dying=*/false);
      reapZombies();
      continue;
    }

    if (impl.timers.empty() && impl.inFlight.empty())
      return false;
    impl.waitForEvent();
  }
}

} // namespace noob
