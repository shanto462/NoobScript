// Standalone tests for the Scheduler in lib/Runtime.cpp. Returns non-zero on
// failure. Timing checks are lower bounds only, so a loaded machine can't make
// them flake.

#include "noob/Runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace noob;
using Clock = std::chrono::steady_clock;

static int Failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      ++Failures;                                                              \
    }                                                                          \
  } while (false)

static constexpr size_t TaskStack = 256u << 10;

/// Counts its own destruction, to observe when the Scheduler frees fibers.
class CountedFiber : public Fiber {
public:
  explicit CountedFiber(int *destroyed = nullptr) : destroyed_(destroyed) {}
  ~CountedFiber() override {
    if (destroyed_)
      ++*destroyed_;
  }

private:
  int *destroyed_;
};

static RuntimeConfig config(unsigned workers = 0, size_t stackSize = TaskStack) {
  RuntimeConfig result;
  result.stackSize = stackSize;
  result.workerThreads = workers;
  return result;
}

static bool never() { return false; }

static double millisSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Spins until `flag` is set, giving up after a few seconds so a broken pool
/// fails a CHECK instead of hanging the run.
static bool waitForFlag(const std::atomic<bool> &flag) {
  auto start = Clock::now();
  while (!flag.load()) {
    if (millisSince(start) > 10000)
      return false;
    usleep(100);
  }
  return true;
}

static void testFifo() {
  Scheduler sched(config());
  std::string order;
  for (char id : {'a', 'b', 'c'}) {
    bool ok = sched.start(new Fiber, [&sched, &order, id] {
      order += id;
      sched.sleepFor(0);
      order += id;
      sched.sleepFor(0);
      order += id;
    }, TaskStack);
    CHECK(ok);
  }
  CHECK(sched.current() == nullptr);
  CHECK(!sched.runUntil(never));
  CHECK(order == "abcabcabc");
}

static void testStartFromOutsideAndInside() {
  Scheduler sched(config());
  int destroyed = 0;
  std::string order;
  CHECK(sched.start(new CountedFiber(&destroyed), [&] {
    CHECK(sched.current() != nullptr);
    order += 'a';
    CHECK(sched.start(new CountedFiber(&destroyed), [&] { order += 'c'; },
                      TaskStack));
    order += 'b';
  }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(order == "abc");
  CHECK(destroyed == 2);
  CHECK(sched.current() == nullptr);
}

static void testRunUntilStopsEarly() {
  Scheduler sched(config());
  int count = 0;
  for (int i = 0; i < 4; ++i)
    CHECK(sched.start(new Fiber, [&] { ++count; }, TaskStack));
  CHECK(sched.runUntil([&] { return count == 2; }));
  CHECK(count == 2);
  CHECK(sched.runUntil([&] { return count == 4; }));
}

static void testParkWake() {
  Scheduler sched(config());
  Fiber *sleeper = new Fiber;
  std::string order;
  bool ready = false;
  CHECK(sched.start(sleeper, [&] {
    order += "1";
    while (!ready)
      sched.park();
    order += "4";
  }, TaskStack));
  CHECK(sched.start(new Fiber, [&] {
    order += "2";
    ready = true;
    sched.wake(sleeper);
    // A second wake must not queue it twice.
    sched.wake(sleeper);
    order += "3";
  }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(order == "1234");
}

static void testSpuriousWake() {
  Scheduler sched(config());
  int runs = 0;
  bool release = false;
  Fiber *generator = new Fiber;
  Fiber *waiter = new Fiber;
  Fiber *queued = new Fiber;
  int generatorSteps = 0;

  CHECK(sched.prepare(generator, [&] {
    ++generatorSteps;
    sched.yieldToCaller();
    ++generatorSteps;
  }, TaskStack));

  CHECK(sched.start(waiter, [&] {
    int wakeups = 0;
    while (!release) {
      ++wakeups;
      sched.park();
    }
    // Woken once spuriously, once for real.
    CHECK(wakeups == 2);
    ++runs;
  }, TaskStack));

  CHECK(sched.start(new Fiber, [&] {
    Fiber *self = sched.current();
    sched.wake(self); // Running.
    sched.wake(queued); // Ready, and must not be queued twice.
    sched.resumeNested(generator);
    sched.wake(generator); // Yielded.
    CHECK(generatorSteps == 1);
    sched.resumeNested(generator);
    CHECK(generatorSteps == 2);

    sched.wake(waiter); // Parked with its condition still false.
    sched.sleepFor(0);
    release = true;
    sched.wake(waiter);
  }, TaskStack));

  CHECK(sched.start(queued, [&] { ++runs; }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(runs == 2);
}

static void testNestedCallerIgnoresWake() {
  Scheduler sched(config());
  Fiber *inner = new Fiber;
  Fiber *outer = new Fiber;
  std::string order;
  CHECK(sched.prepare(inner, [&] {
    sched.wake(outer); // outer is Nested, so this does nothing.
    order += 'i';
    sched.sleepFor(0);
    order += 'j';
  }, TaskStack));
  CHECK(sched.start(outer, [&] {
    order += 'o';
    sched.resumeNested(inner);
    order += 'p';
  }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(order == "oijp");
}

static void testTimers() {
  Scheduler sched(config());
  std::string order;
  auto sleeper = [&](double ms, char id) {
    return [&sched, &order, ms, id] {
      sched.sleepFor(ms);
      order += id;
    };
  };
  CHECK(sched.start(new Fiber, sleeper(60, 'c'), TaskStack));
  CHECK(sched.start(new Fiber, sleeper(20, 'a'), TaskStack));
  CHECK(sched.start(new Fiber, sleeper(40, 'b'), TaskStack));
  auto start = Clock::now();
  CHECK(!sched.runUntil(never));
  CHECK(order == "abc");
  CHECK(millisSince(start) >= 60);
}

static void testSleepDoesNotBlockOthers() {
  Scheduler sched(config());
  bool sleeperDone = false;
  int turns = 0;
  CHECK(sched.start(new Fiber, [&] {
    auto start = Clock::now();
    sched.sleepFor(30);
    CHECK(millisSince(start) >= 30);
    sleeperDone = true;
  }, TaskStack));
  CHECK(sched.start(new Fiber, [&] {
    while (!sleeperDone) {
      ++turns;
      sched.sleepFor(0);
    }
  }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(turns > 10);
}

static void testSleepIgnoresSpuriousWake() {
  Scheduler sched(config());
  Fiber *sleeper = new Fiber;
  double slept = 0;
  CHECK(sched.start(sleeper, [&] {
    auto start = Clock::now();
    sched.sleepFor(30);
    slept = millisSince(start);
  }, TaskStack));
  CHECK(sched.start(new Fiber, [&] {
    for (int i = 0; i < 5; ++i) {
      sched.wake(sleeper);
      sched.sleepFor(1);
    }
  }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(slept >= 30);
}

static void testSleepEdgeValues() {
  Scheduler sched(config());
  int finished = 0;
  CHECK(sched.start(new Fiber, [&] {
    sched.sleepFor(-5);
    sched.sleepFor(0.0 / 0.0);
    ++finished;
  }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(finished == 1);
}

static void testRunBlockingProgress(unsigned workers) {
  Scheduler sched(config(workers));
  std::atomic<bool> fiberProgressed{false};
  bool blockingDone = false;
  pthread_t workerThread{};
  bool sawTimeout = false;

  CHECK(sched.start(new Fiber, [&] {
    sched.runBlocking([&] {
      workerThread = pthread_self();
      // Only finishes if the other fiber runs while this job does.
      if (!waitForFlag(fiberProgressed))
        sawTimeout = true;
    });
    blockingDone = true;
  }, TaskStack));
  CHECK(sched.start(new Fiber, [&] {
    sched.sleepFor(0);
    fiberProgressed = true;
  }, TaskStack));

  CHECK(!sched.runUntil(never));
  CHECK(blockingDone);
  CHECK(!sawTimeout);
  CHECK(!pthread_equal(workerThread, pthread_self()));
}

static void testRunBlockingSerialWithOneWorker() {
  Scheduler sched(config(1));
  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
  int finished = 0;
  for (int i = 0; i < 5; ++i) {
    CHECK(sched.start(new Fiber, [&] {
      sched.runBlocking([&] {
        int now = ++active;
        int seen = maxActive.load();
        while (now > seen && !maxActive.compare_exchange_weak(seen, now)) {
        }
        usleep(2000);
        --active;
      });
      ++finished;
    }, TaskStack));
  }
  CHECK(!sched.runUntil(never));
  CHECK(finished == 5);
  CHECK(maxActive.load() == 1);
}

static void testRunBlockingParallel() {
  Scheduler sched(config(4));
  std::atomic<int> started{0};
  std::atomic<bool> allStarted{false};
  bool sawTimeout = false;
  int finished = 0;
  for (int i = 0; i < 4; ++i) {
    CHECK(sched.start(new Fiber, [&] {
      sched.runBlocking([&] {
        // Each job waits for all of them, so the pool must run them at once.
        if (++started == 4)
          allStarted = true;
        if (!waitForFlag(allStarted))
          sawTimeout = true;
      });
      ++finished;
    }, TaskStack));
  }
  CHECK(!sched.runUntil(never));
  CHECK(finished == 4);
  CHECK(!sawTimeout);
}

static void testRunBlockingDefaultWorkers() {
  Scheduler sched(config(0));
  int total = 0;
  for (int i = 0; i < 16; ++i) {
    CHECK(sched.start(new Fiber, [&sched, &total, i] {
      int value = 0;
      sched.runBlocking([&value, i] { value = i + 1; });
      total += value;
    }, TaskStack));
  }
  CHECK(!sched.runUntil(never));
  CHECK(total == 16 * 17 / 2);
}

static void testWorkerStackSize() {
#ifdef __APPLE__
  size_t requested = 1u << 20;
  Scheduler sched(config(1, requested));
  size_t actual = 0;
  CHECK(sched.start(new Fiber, [&] {
    sched.runBlocking([&] { actual = pthread_get_stacksize_np(pthread_self()); });
  }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(actual >= requested);
  CHECK(actual < requested + (64u << 10));
#endif
}

static void testGenerator() {
  Scheduler sched(config());
  int destroyed = 0;
  auto *gen = new CountedFiber(&destroyed);
  int yielded = 0;
  std::vector<int> seen;
  bool completed = false;

  CHECK(sched.prepare(gen, [&] {
    for (int i = 1; i <= 5; ++i) {
      yielded = i;
      sched.yieldToCaller();
    }
    yielded = -1;
  }, TaskStack));
  CHECK(sched.isSuspended(gen));

  CHECK(sched.start(new Fiber, [&] {
    for (int i = 0; i < 5; ++i) {
      sched.resumeNested(gen);
      seen.push_back(yielded);
      CHECK(sched.isSuspended(gen));
      CHECK(destroyed == 0);
    }
    sched.resumeNested(gen);
    // The generator ran off the end. It's reaped by the time we get back.
    CHECK(yielded == -1);
    CHECK(destroyed == 1);
    completed = true;
  }, TaskStack));

  CHECK(!sched.runUntil(never));
  CHECK(completed);
  CHECK((seen == std::vector<int>{1, 2, 3, 4, 5}));
}

static void testResumedByDifferentFibers() {
  Scheduler sched(config());
  Fiber *gen = new Fiber;
  int value = 0;
  CHECK(sched.prepare(gen, [&] {
    value = 1;
    sched.yieldToCaller();
    value = 2;
  }, TaskStack));
  CHECK(sched.start(new Fiber, [&] {
    sched.resumeNested(gen);
    CHECK(value == 1);
  }, TaskStack));
  CHECK(sched.start(new Fiber, [&] {
    sched.resumeNested(gen);
    CHECK(value == 2);
  }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(value == 2);
}

static void testNestedParks() {
  Scheduler sched(config());
  Fiber *gen = new Fiber;
  std::string order;
  bool ready = false;

  CHECK(sched.prepare(gen, [&] {
    order += 'b';
    while (!ready)
      sched.park();
    order += 'd';
    sched.yieldToCaller();
    order += 'f';
    sched.sleepFor(1);
    order += 'g';
  }, TaskStack));

  CHECK(sched.start(new Fiber, [&] {
    order += 'a';
    sched.resumeNested(gen);
    order += 'e';
    sched.resumeNested(gen);
    order += 'h';
  }, TaskStack));

  CHECK(sched.start(new Fiber, [&] {
    // Runs because gen parked, even though its caller is stuck in Nested.
    order += 'c';
    ready = true;
    sched.wake(gen);
  }, TaskStack));

  CHECK(!sched.runUntil(never));
  CHECK(order == "abcdefgh");
}

static void testNestedChain() {
  Scheduler sched(config());
  constexpr int Depth = 32;
  int destroyed = 0;
  std::string order;
  std::vector<Fiber *> chain;
  for (int i = 0; i < Depth; ++i)
    chain.push_back(new CountedFiber(&destroyed));

  std::function<void(int)> descend = [&](int i) {
    if (i == Depth) {
      // Parks at the bottom of the chain, with every ancestor left Nested.
      sched.sleepFor(1);
      order += '!';
      return;
    }
    CHECK(sched.prepare(chain[i], [&descend, &sched, i] {
      descend(i + 1);
      sched.yieldToCaller();
    }, TaskStack));
    sched.resumeNested(chain[i]);
    order += '.';
  };

  CHECK(sched.start(new Fiber, [&] {
    descend(0);
    // Each level yielded to its caller and is still suspended.
    CHECK(destroyed == 0);
    for (int i = 0; i < Depth; ++i) {
      CHECK(sched.isSuspended(chain[i]));
      sched.resumeNested(chain[i]);
    }
    CHECK(destroyed == Depth);
  }, TaskStack));

  CHECK(!sched.runUntil(never));
  CHECK(order == "!" + std::string(Depth, '.'));
}

static void testDestroySuspended() {
  Scheduler sched(config());
  int destroyed = 0;
  uintptr_t firstLow = 0;
  uintptr_t secondLow = 0;
  auto capture = std::make_shared<int>(7);

  CHECK(sched.start(new Fiber, [&] {
    // Never started: the entry's captures go with it.
    auto *fresh = new CountedFiber(&destroyed);
    CHECK(sched.prepare(fresh, [capture] { CHECK(false); }, TaskStack));
    CHECK(capture.use_count() == 2);
    CHECK(sched.isSuspended(fresh));
    firstLow = fresh->stackLow();
    sched.destroySuspended(fresh);
    CHECK(destroyed == 1);
    CHECK(capture.use_count() == 1);

    // Yielded mid-way, and its stack goes back on the free list.
    auto *gen = new CountedFiber(&destroyed);
    CHECK(sched.prepare(gen, [&] {
      sched.yieldToCaller();
      CHECK(false);
    }, TaskStack));
    secondLow = gen->stackLow();
    CHECK(secondLow == firstLow);
    sched.resumeNested(gen);
    CHECK(sched.isSuspended(gen));
    sched.destroySuspended(gen);
    CHECK(destroyed == 2);

    auto *again = new CountedFiber(&destroyed);
    CHECK(sched.prepare(again, [] {}, TaskStack));
    CHECK(again->stackLow() == secondLow);
    sched.resumeNested(again);
    CHECK(destroyed == 3);
  }, TaskStack));

  CHECK(!sched.runUntil(never));
}

static void testStackReuseAfterFinish() {
  Scheduler sched(config());
  uintptr_t first = 0;
  uintptr_t second = 0;
  Fiber *a = new Fiber;
  CHECK(sched.start(a, [&] { first = sched.current()->stackLow(); }, TaskStack));
  CHECK(!sched.runUntil(never));
  Fiber *b = new Fiber;
  CHECK(sched.start(b, [&] { second = sched.current()->stackLow(); }, TaskStack));
  CHECK(!sched.runUntil(never));
  CHECK(first != 0);
  CHECK(first == second);
}

static void testDeadlock() {
  int destroyed = 0;
  {
    Scheduler sched(config());
    CHECK(sched.start(new CountedFiber(&destroyed), [&] { sched.park(); },
                      TaskStack));
    CHECK(!sched.runUntil(never));
    CHECK(destroyed == 0);
    // Still usable after reporting a deadlock.
    int ran = 0;
    CHECK(sched.start(new Fiber, [&] { ++ran; }, TaskStack));
    CHECK(!sched.runUntil(never));
    CHECK(ran == 1);
  }
  // Tearing the scheduler down frees what never finished.
  CHECK(destroyed == 1);
}

static void testEntryCapturesDroppedOnFiber() {
  Scheduler sched(config());
  Fiber *fiber = new Fiber;
  bool ranOnFiber = false;
  auto deleter = [&](int *p) {
    ranOnFiber = sched.current() == fiber;
    delete p;
  };
  {
    std::shared_ptr<int> payload(new int(1), deleter);
    CHECK(sched.start(fiber, [payload] {}, TaskStack));
  }
  CHECK(!sched.runUntil(never));
  CHECK(ranOnFiber);
}

static void testManyFibers() {
  Scheduler sched(config());
  constexpr int Count = 4000;
  constexpr int Rounds = 125;
  int destroyed = 0;
  long long turns = 0;
  std::vector<Fiber *> fibers;
  std::vector<bool> gate(Count, false);
  fibers.reserve(Count);

  // A token passes around the ring, so each fiber runs once per round.
  for (int i = 0; i < Count; ++i)
    fibers.push_back(new CountedFiber(&destroyed));
  for (int i = 0; i < Count; ++i) {
    CHECK(sched.start(fibers[i], [&, i] {
      for (int round = 0; round < Rounds; ++round) {
        while (!gate[i])
          sched.park();
        gate[i] = false;
        ++turns;
        // The very last turn has nobody left to pass to, and the fiber it
        // would wake has already finished and been freed.
        if (turns == (long long)Count * Rounds)
          break;
        int next = (i + 1) % Count;
        gate[next] = true;
        sched.wake(fibers[next]);
      }
    }, TaskStack));
  }
  gate[0] = true;
  auto start = Clock::now();
  CHECK(!sched.runUntil(never));
  double elapsed = millisSince(start);
  CHECK(turns == (long long)Count * Rounds);
  CHECK(destroyed == Count);
  std::printf("  %d fibers, %lld turns, ~%lld switches in %.1f ms\n", Count,
              turns, 2 * turns, elapsed);
}

/// Burns stack until less than `margin` bytes remain, switches away from down
/// there, and returns the lowest frame address reached.
__attribute__((noinline)) static uintptr_t burnStack(Scheduler &sched,
                                                     uintptr_t low,
                                                     size_t margin) {
  volatile char buffer[512];
  // Escaping the array stops the optimizer shrinking it to the two bytes used.
  __asm__ volatile("" : : "r"(buffer) : "memory");
  buffer[0] = 1;
  buffer[sizeof(buffer) - 1] = 2;
  auto lowest = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  if (lowest > low + margin)
    lowest = burnStack(sched, low, margin);
  else
    sched.sleepFor(0);
  // Reading it afterwards keeps this from being a tail call.
  return lowest + (buffer[0] - 1);
}

static void testDeepRecursion() {
  constexpr size_t StackSize = 512u << 10;
  constexpr size_t Margin = 32u << 10;
  Scheduler sched(config(0, StackSize));
  uintptr_t low = 0;
  uintptr_t lowest = 0;
  CHECK(sched.start(new Fiber, [&] {
    low = sched.current()->stackLow();
    lowest = burnStack(sched, low, Margin);
  }, StackSize));
  CHECK(!sched.runUntil(never));
  CHECK(lowest > low);
  CHECK(lowest <= low + Margin);
  std::printf("  reached within %zu KiB of the end of a %zu KiB stack\n",
              (lowest - low) >> 10, StackSize >> 10);
}

// Runs first, before any test has started a worker thread that fork() would
// leave behind in a half-copied state.
static void testGuardPage() {
  std::fflush(stdout);
  std::fflush(stderr);
  pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    // ASan reports the crash at length, and that's the expected outcome here.
    int devNull = open("/dev/null", O_WRONLY);
    dup2(devNull, 1);
    dup2(devNull, 2);
    Scheduler sched(config());
    sched.start(new Fiber, [&] {
      auto *low = reinterpret_cast<volatile char *>(sched.current()->stackLow());
      low[-1] = 1;
      _exit(0); // Only reached if the guard page isn't there.
    }, TaskStack);
    sched.runUntil(never);
    _exit(0);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(!(WIFEXITED(status) && WEXITSTATUS(status) == 0));
}

static size_t expectedUsable(size_t requested) {
  size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  size_t size = std::max(requested, MinStackSize);
  return (size + page - 1) & ~(page - 1);
}

static void checkStackExtent(size_t requested) {
  Scheduler sched(config());
  size_t usable = expectedUsable(requested);
  uintptr_t low = 0;
  uintptr_t here = 0;
  CHECK(sched.start(new Fiber, [&] {
    low = sched.current()->stackLow();
    here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    // The first usable address really is usable.
    *reinterpret_cast<volatile char *>(low) = 1;
  }, requested));
  CHECK(!sched.runUntil(never));
  // fiberMain's frame sits at the very top of the stack, with a few small
  // frames under it, so this pins the stack's size to well under a page.
  CHECK(here > low);
  CHECK(here <= low + usable);
  CHECK(low + usable - here < (8u << 10));
}

static void testCustomStackSize() {
  checkStackExtent(1u << 20);
  checkStackExtent((1u << 20) + 1);
  checkStackExtent(MainStackSize);
  // Clamped up to the minimum.
  checkStackExtent(1024);
}

static void testMapFailure() {
  Scheduler sched(config());
  size_t tooBig = size_t(1) << 62;
  Fiber fiber;
  CHECK(!sched.start(&fiber, [] {}, tooBig));
  CHECK(!sched.prepare(&fiber, [] {}, tooBig));
  CHECK(!sched.start(&fiber, [] {}, SIZE_MAX));
  // The stack-allocated fiber stays ours: nothing was queued.
  CHECK(!sched.runUntil(never));
}

static void measureSwitches() {
  constexpr int Direct = 1000000;
  constexpr int ViaScheduler = 500000;
  double directNs = 0;
  double schedulerNs = 0;
  {
    Scheduler sched(config());
    Fiber *gen = new Fiber;
    CHECK(sched.prepare(gen, [&] {
      for (;;)
        sched.yieldToCaller();
    }, TaskStack));
    CHECK(sched.start(new Fiber, [&] {
      auto start = Clock::now();
      for (int i = 0; i < Direct; ++i)
        sched.resumeNested(gen);
      directNs = millisSince(start) * 1e6 / (2.0 * Direct);
      sched.destroySuspended(gen);
    }, TaskStack));
    CHECK(!sched.runUntil(never));
  }
  {
    Scheduler sched(config());
    Fiber *a = new Fiber;
    Fiber *b = new Fiber;
    int count = 0;
    Clock::time_point start;
    CHECK(sched.start(a, [&] {
      start = Clock::now();
      while (count < ViaScheduler) {
        ++count;
        sched.wake(b);
        sched.park();
      }
      // b leaves first, having woken us, so it's already gone.
    }, TaskStack));
    CHECK(sched.start(b, [&] {
      while (count < ViaScheduler) {
        sched.wake(a);
        sched.park();
      }
      sched.wake(a);
    }, TaskStack));
    CHECK(!sched.runUntil(never));
    // Each hop is two switches: fiber to scheduler to fiber.
    schedulerNs = millisSince(start) * 1e6 / (4.0 * ViaScheduler);
  }
  std::printf("  direct switch: %.1f ns, via scheduler: %.1f ns per switch\n",
              directNs, schedulerNs);
}

// Runs last: it leaves a worker stuck in read() until the process exits.
static void testOrphanedWorkers() {
  int unblockPipe[2];
  int stuckPipe[2];
  CHECK(pipe(unblockPipe) == 0);
  CHECK(pipe(stuckPipe) == 0);
  std::atomic<int> started{0};
  std::atomic<bool> unblocked{false};
  {
    Scheduler sched(config(2));
    CHECK(sched.start(new Fiber, [&] {
      sched.runBlocking([&] {
        char c;
        ++started;
        CHECK(read(unblockPipe[0], &c, 1) == 1);
        unblocked = true;
      });
    }, TaskStack));
    CHECK(sched.start(new Fiber, [&] {
      sched.runBlocking([&] {
        char c;
        ++started;
        // Never written to, so this blocks until the process exits.
        (void)read(stuckPipe[0], &c, 1);
      });
    }, TaskStack));
    // runUntil only re-checks its condition after a scheduling event, so a
    // change made by a worker has to be noticed by a fiber.
    bool bothStarted = false;
    CHECK(sched.start(new Fiber, [&] {
      while (started.load() != 2)
        sched.sleepFor(1);
      bothStarted = true;
    }, TaskStack));
    CHECK(sched.runUntil([&] { return bothStarted; }));
    // Destroying the scheduler must not wait for either worker.
  }

  char c = 1;
  CHECK(write(unblockPipe[1], &c, 1) == 1);
  CHECK(waitForFlag(unblocked));
  // Give the worker a moment to run the abandoned-job path under ASan.
  usleep(20000);

  // The pool is per-Scheduler, and one is free to come after the last.
  Scheduler next(config(1));
  int value = 0;
  CHECK(next.start(new Fiber, [&] {
    next.runBlocking([&] { value = 42; });
  }, TaskStack));
  CHECK(!next.runUntil(never));
  CHECK(value == 42);
}

int main() {
  struct Test {
    const char *name;
    void (*run)();
  };
  const Test tests[] = {
      {"guard page", testGuardPage},
      {"fifo", testFifo},
      {"start from outside and inside", testStartFromOutsideAndInside},
      {"runUntil stops early", testRunUntilStopsEarly},
      {"park/wake", testParkWake},
      {"spurious wake", testSpuriousWake},
      {"nested caller ignores wake", testNestedCallerIgnoresWake},
      {"timers", testTimers},
      {"sleep does not block others", testSleepDoesNotBlockOthers},
      {"sleep ignores spurious wake", testSleepIgnoresSpuriousWake},
      {"sleep edge values", testSleepEdgeValues},
      {"runBlocking progress, 1 worker", [] { testRunBlockingProgress(1); }},
      {"runBlocking progress, 4 workers", [] { testRunBlockingProgress(4); }},
      {"runBlocking serial with one worker", testRunBlockingSerialWithOneWorker},
      {"runBlocking parallel", testRunBlockingParallel},
      {"runBlocking default workers", testRunBlockingDefaultWorkers},
      {"worker stack size", testWorkerStackSize},
      {"generator", testGenerator},
      {"resumed by different fibers", testResumedByDifferentFibers},
      {"nested fiber parks", testNestedParks},
      {"nested chain", testNestedChain},
      {"destroySuspended", testDestroySuspended},
      {"stack reuse after finish", testStackReuseAfterFinish},
      {"deadlock", testDeadlock},
      {"entry captures dropped on fiber", testEntryCapturesDroppedOnFiber},
      {"many fibers", testManyFibers},
      {"deep recursion", testDeepRecursion},
      {"custom stack size", testCustomStackSize},
      {"map failure", testMapFailure},
      {"measure switches", measureSwitches},
      {"orphaned workers", testOrphanedWorkers},
  };
  for (const Test &test : tests) {
    std::printf("%s\n", test.name);
    std::fflush(stdout);
    test.run();
  }
  if (Failures) {
    std::fprintf(stderr, "%d check(s) failed\n", Failures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
