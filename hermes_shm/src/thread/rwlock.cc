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


#include "hermes_shm/thread/lock/rwlock.h"
#include "hermes_shm/thread/thread_model_manager.h"
#include "hermes_shm/util/logging.h"

namespace hshm {

/**====================================
 * Rw Lock
 * ===================================*/

/**
 * Acquire the read lock
 * */
void RwLock::ReadLock(uint32_t owner) {
  RwLockMode mode;

  // Increment # readers. Check if in read mode.
  readers_.fetch_add(1);

  // Wait until we are in read mode
  do {
    UpdateMode(mode);
    if (mode == RwLockMode::kRead) {
      return;
    }
    if (mode == RwLockMode::kNone) {
      bool ret = mode_.compare_exchange_weak(mode, RwLockMode::kRead);
      if (ret) {
#ifdef HERMES_DEBUG_LOCK
        owner_ = owner;
        HILOG(kDebug, "Acquired read lock for {}", owner);
#endif
        return;
      }
    }
    HERMES_THREAD_MODEL->Yield();
  } while (true);
}

/**
 * Release the read lock
 * */
void RwLock::ReadUnlock() {
  readers_.fetch_sub(1);
}

/**
 * Acquire the write lock
 * */
void RwLock::WriteLock(uint32_t owner) {
  RwLockMode mode;
  uint32_t cur_writer;

  // Increment # writers & get ticket
  writers_.fetch_add(1);
  uint64_t tkt = ticket_.fetch_add(1);

  // Wait until we are in read mode
  do {
    UpdateMode(mode);
    if (mode == RwLockMode::kNone) {
      mode_.compare_exchange_weak(mode, RwLockMode::kWrite);
      mode = mode_.load();
    }
    if (mode == RwLockMode::kWrite) {
      cur_writer = cur_writer_.load();
      if (cur_writer == tkt) {
#ifdef HERMES_DEBUG_LOCK
        owner_ = owner;
        HILOG(kDebug, "Acquired write lock for {}", owner);
#endif
        return;
      }
    }
    HERMES_THREAD_MODEL->Yield();
  } while (true);
}

/**
 * Release the write lock
 * */
void RwLock::WriteUnlock() {
  writers_.fetch_sub(1);
  cur_writer_.fetch_add(1);
}

/**====================================
 * ScopedRwReadLock
 * ===================================*/

/**
 * Constructor
 * */
ScopedRwReadLock::ScopedRwReadLock(RwLock &lock, uint32_t owner)
: lock_(lock), is_locked_(false) {
  Lock(owner);
}

/**
 * Release the read lock
 * */
ScopedRwReadLock::~ScopedRwReadLock() {
  Unlock();
}

/**
 * Acquire the read lock
 * */
void ScopedRwReadLock::Lock(uint32_t owner) {
  if (!is_locked_) {
    lock_.ReadLock(owner);
    is_locked_ = true;
  }
}

/**
 * Release the read lock
 * */
void ScopedRwReadLock::Unlock() {
  if (is_locked_) {
    lock_.ReadUnlock();
    is_locked_ = false;
  }
}

/**====================================
 * ScopedRwWriteLock
 * ===================================*/

/**
 * Constructor
 * */
ScopedRwWriteLock::ScopedRwWriteLock(RwLock &lock, uint32_t owner)
: lock_(lock), is_locked_(false) {
  Lock(owner);
}

/**
 * Release the write lock
 * */
ScopedRwWriteLock::~ScopedRwWriteLock() {
  Unlock();
}

/**
 * Acquire the write lock
 * */
void ScopedRwWriteLock::Lock(uint32_t owner) {
  if (!is_locked_) {
    lock_.WriteLock(owner);
    is_locked_ = true;
  }
}

/**
 * Release the write lock
 * */
void ScopedRwWriteLock::Unlock() {
  if (is_locked_) {
    lock_.WriteUnlock();
    is_locked_ = false;
  }
}

}  // namespace hshm
