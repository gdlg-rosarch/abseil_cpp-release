//
// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

//  Most users requiring mutual exclusion should use Mutex.
//  SpinLock is provided for use in three situations:
//   - for use in code that Mutex itself depends on
//   - to get a faster fast-path release under low contention (without an
//     atomic read-modify-write) In return, SpinLock has worse behaviour under
//     contention, which is why Mutex is preferred in most situations.
//   - for async signal safety (see below)

// SpinLock is async signal safe.  If a spinlock is used within a signal
// handler, all code that acquires the lock must ensure that the signal cannot
// arrive while they are holding the lock.  Typically, this is done by blocking
// the signal.

#ifndef ABSL_BASE_INTERNAL_SPINLOCK_H_
#define ABSL_BASE_INTERNAL_SPINLOCK_H_

#include <stdint.h>
#include <sys/types.h>
#include <atomic>

#include "absl/base/attributes.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/low_level_scheduling.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/base/internal/scheduling_mode.h"
#include "absl/base/internal/tsan_mutex_interface.h"
#include "absl/base/macros.h"
#include "absl/base/port.h"
#include "absl/base/thread_annotations.h"

namespace absl {
namespace base_internal {

class LOCKABLE SpinLock {
 public:
  SpinLock() : lockword_(kSpinLockCooperative) {
    ABSL_TSAN_MUTEX_CREATE(this, __tsan_mutex_not_static);
  }

  // Special constructor for use with static SpinLock objects.  E.g.,
  //
  //    static SpinLock lock(base_internal::kLinkerInitialized);
  //
  // When intialized using this constructor, we depend on the fact
  // that the linker has already initialized the memory appropriately.
  // A SpinLock constructed like this can be freely used from global
  // initializers without worrying about the order in which global
  // initializers run.
  explicit SpinLock(base_internal::LinkerInitialized) {
    // Does nothing; lockword_ is already initialized
    ABSL_TSAN_MUTEX_CREATE(this, 0);
  }

  // Constructors that allow non-cooperative spinlocks to be created for use
  // inside thread schedulers.  Normal clients should not use these.
  explicit SpinLock(base_internal::SchedulingMode mode);
  SpinLock(base_internal::LinkerInitialized,
           base_internal::SchedulingMode mode);

  ~SpinLock() { ABSL_TSAN_MUTEX_DESTROY(this, __tsan_mutex_not_static); }

  // Acquire this SpinLock.
  inline void Lock() EXCLUSIVE_LOCK_FUNCTION() {
    ABSL_TSAN_MUTEX_PRE_LOCK(this, 0);
    if (!TryLockImpl()) {
      SlowLock();
    }
    ABSL_TSAN_MUTEX_POST_LOCK(this, 0, 0);
  }

  // Try to acquire this SpinLock without blocking and return true if the
  // acquisition was successful.  If the lock was not acquired, false is
  // returned.  If this SpinLock is free at the time of the call, TryLock
  // will return true with high probability.
  inline bool TryLock() EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    ABSL_TSAN_MUTEX_PRE_LOCK(this, __tsan_mutex_try_lock);
    bool res = TryLockImpl();
    ABSL_TSAN_MUTEX_POST_LOCK(
        this, __tsan_mutex_try_lock | (res ? 0 : __tsan_mutex_try_lock_failed),
        0);
    return res;
  }

  // Release this SpinLock, which must be held by the calling thread.
  inline void Unlock() UNLOCK_FUNCTION() {
    ABSL_TSAN_MUTEX_PRE_UNLOCK(this, 0);
    uint32_t lock_value = lockword_.load(std::memory_order_relaxed);
    lockword_.store(lock_value & kSpinLockCooperative,
                    std::memory_order_release);

    if ((lock_value & kSpinLockDisabledScheduling) != 0) {
      base_internal::SchedulingGuard::EnableRescheduling(true);
    }
    if ((lock_value & kWaitTimeMask) != 0) {
      // Collect contentionz profile info, and speed the wakeup of any waiter.
      // The wait_cycles value indicates how long this thread spent waiting
      // for the lock.
      SlowUnlock(lock_value);
    }
    ABSL_TSAN_MUTEX_POST_UNLOCK(this, 0);
  }

  // Determine if the lock is held.  When the lock is held by the invoking
  // thread, true will always be returned. Intended to be used as
  // CHECK(lock.IsHeld()).
  inline bool IsHeld() const {
    return (lockword_.load(std::memory_order_relaxed) & kSpinLockHeld) != 0;
  }

 protected:
  // These should not be exported except for testing.

  // Store number of cycles between wait_start_time and wait_end_time in a
  // lock value.
  static uint32_t EncodeWaitCycles(int64_t wait_start_time,
                                   int64_t wait_end_time);

  // Extract number of wait cycles in a lock value.
  static uint64_t DecodeWaitCycles(uint32_t lock_value);

  // Provide access to protected method above.  Use for testing only.
  friend struct SpinLockTest;

 private:
  // lockword_ is used to store the following:
  //
  // bit[0] encodes whether a lock is being held.
  // bit[1] encodes whether a lock uses cooperative scheduling.
  // bit[2] encodes whether a lock disables scheduling.
  // bit[3:31] encodes time a lock spent on waiting as a 29-bit unsigned int.
  enum { kSpinLockHeld = 1 };
  enum { kSpinLockCooperative = 2 };
  enum { kSpinLockDisabledScheduling = 4 };
  enum { kSpinLockSleeper = 8 };
  enum { kWaitTimeMask =                      // Includes kSpinLockSleeper.
    ~(kSpinLockHeld | kSpinLockCooperative | kSpinLockDisabledScheduling) };

  uint32_t TryLockInternal(uint32_t lock_value, uint32_t wait_cycles);
  void InitLinkerInitializedAndCooperative();
  void SlowLock() ABSL_ATTRIBUTE_COLD;
  void SlowUnlock(uint32_t lock_value) ABSL_ATTRIBUTE_COLD;
  uint32_t SpinLoop(int64_t initial_wait_timestamp, uint32_t* wait_cycles);

  inline bool TryLockImpl() {
    uint32_t lock_value = lockword_.load(std::memory_order_relaxed);
    return (TryLockInternal(lock_value, 0) & kSpinLockHeld) == 0;
  }

  std::atomic<uint32_t> lockword_;

  SpinLock(const SpinLock&) = delete;
  SpinLock& operator=(const SpinLock&) = delete;
};

// Corresponding locker object that arranges to acquire a spinlock for
// the duration of a C++ scope.
class SCOPED_LOCKABLE SpinLockHolder {
 public:
  inline explicit SpinLockHolder(SpinLock* l) EXCLUSIVE_LOCK_FUNCTION(l)
      : lock_(l) {
    l->Lock();
  }
  inline ~SpinLockHolder() UNLOCK_FUNCTION() { lock_->Unlock(); }

  SpinLockHolder(const SpinLockHolder&) = delete;
  SpinLockHolder& operator=(const SpinLockHolder&) = delete;

 private:
  SpinLock* lock_;
};

// Register a hook for profiling support.
//
// The function pointer registered here will be called whenever a spinlock is
// contended.  The callback is given an opaque handle to the contended spinlock
// and the number of wait cycles.  This is thread-safe, but only a single
// profiler can be registered.  It is an error to call this function multiple
// times with different arguments.
void RegisterSpinLockProfiler(void (*fn)(const void* lock,
                                         int64_t wait_cycles));

//------------------------------------------------------------------------------
// Public interface ends here.
//------------------------------------------------------------------------------

// If (result & kSpinLockHeld) == 0, then *this was successfully locked.
// Otherwise, returns last observed value for lockword_.
inline uint32_t SpinLock::TryLockInternal(uint32_t lock_value,
                                          uint32_t wait_cycles) {
  if ((lock_value & kSpinLockHeld) != 0) {
    return lock_value;
  }

  uint32_t sched_disabled_bit = 0;
  if ((lock_value & kSpinLockCooperative) == 0) {
    // For non-cooperative locks we must make sure we mark ourselves as
    // non-reschedulable before we attempt to CompareAndSwap.
    if (base_internal::SchedulingGuard::DisableRescheduling()) {
      sched_disabled_bit = kSpinLockDisabledScheduling;
    }
  }

  if (lockword_.compare_exchange_strong(
          lock_value,
          kSpinLockHeld | lock_value | wait_cycles | sched_disabled_bit,
          std::memory_order_acquire, std::memory_order_relaxed)) {
  } else {
    base_internal::SchedulingGuard::EnableRescheduling(sched_disabled_bit);
  }

  return lock_value;
}

}  // namespace base_internal
}  // namespace absl

#endif  // ABSL_BASE_INTERNAL_SPINLOCK_H_
