/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Distributed under BSD 3-Clause license.                                   *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Illinois Institute of Technology.                        *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of Hermes. The full Hermes copyright notice, including  *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the top directory. If you do not  *
 * have access to the file, you may request a copy from help@hdfgroup.org.   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#ifndef HERMES_THREAD_RWLOCK_H_
#define HERMES_THREAD_RWLOCK_H_

#include <atomic>
#include <hermes_shm/constants/macros.h>

namespace hshm {

enum class RwLockMode {
  kNone,
  kWrite,
  kRead,
};

/** A reader-writer lock implementation */
struct RwLock {
  std::atomic<uint32_t> readers_;
  std::atomic<uint32_t> writers_;
  std::atomic<uint64_t> ticket_;
  std::atomic<RwLockMode> mode_;
  std::atomic<uint32_t> cur_writer_;
#ifdef HERMES_DEBUG_LOCK
  uint32_t owner_;
#endif

  /** Default constructor */
  RwLock()
  : readers_(0),
    writers_(0),
    ticket_(0),
    mode_(RwLockMode::kNone),
    cur_writer_(0) {}

  /** Explicit constructor */
  void Init() {
    readers_ = 0;
    writers_ = 0;
    ticket_ = 0;
    mode_ = RwLockMode::kNone;
    cur_writer_ = 0;
  }

  /** Delete copy constructor */
  RwLock(const RwLock &other) = delete;

  /** Move constructor */
  RwLock(RwLock &&other) noexcept
  : readers_(other.readers_.load()),
    writers_(other.writers_.load()),
    ticket_(other.ticket_.load()),
    mode_(other.mode_.load()),
    cur_writer_(other.cur_writer_.load()) {}

  /** Move assignment operator */
  RwLock& operator=(RwLock &&other) noexcept {
    if (this != &other) {
      readers_ = other.readers_.load();
      writers_ = other.writers_.load();
      ticket_ = other.ticket_.load();
      mode_ = other.mode_.load();
      cur_writer_ = other.cur_writer_.load();
    }
    return *this;
  }

  /** Acquire read lock */
  void ReadLock(uint32_t owner);

  /** Release read lock */
  void ReadUnlock();

  /** Acquire write lock */
  void WriteLock(uint32_t owner);

  /** Release write lock */
  void WriteUnlock();

 private:
  /** Update the mode of the lock */
  HSHM_ALWAYS_INLINE void UpdateMode(RwLockMode &mode) {
    // When # readers is 0, there is a lag to when the mode is updated
    // When # writers is 0, there is a lag to when the mode is updated
    mode = mode_.load();
    if ((readers_.load() == 0 && mode == RwLockMode::kRead) ||
        (writers_.load() == 0 && mode == RwLockMode::kWrite)) {
      mode_.compare_exchange_weak(mode, RwLockMode::kNone);
    }
  }
};

/** Acquire the read lock in a scope */
struct ScopedRwReadLock {
  RwLock &lock_;
  bool is_locked_;

  /** Acquire the read lock */
  explicit ScopedRwReadLock(RwLock &lock, uint32_t owner);

  /** Release the read lock */
  ~ScopedRwReadLock();

  /** Explicitly acquire read lock */
  void Lock(uint32_t owner);

  /** Explicitly release read lock */
  void Unlock();
};

/** Acquire scoped write lock */
struct ScopedRwWriteLock {
  RwLock &lock_;
  bool is_locked_;

  /** Acquire the write lock */
  explicit ScopedRwWriteLock(RwLock &lock, uint32_t owner);

  /** Release the write lock */
  ~ScopedRwWriteLock();

  /** Explicity acquire the write lock */
  void Lock(uint32_t owner);

  /** Explicitly release the write lock */
  void Unlock();
};

}  // namespace hshm

#endif  // HERMES_THREAD_RWLOCK_H_
