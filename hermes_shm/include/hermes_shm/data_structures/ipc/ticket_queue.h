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

#ifndef HERMES_SHM_INCLUDE_HERMES_SHM_DATA_STRUCTURES_IPC_TICKET_QUEUE_H_
#define HERMES_SHM_INCLUDE_HERMES_SHM_DATA_STRUCTURES_IPC_TICKET_QUEUE_H_

#include "hermes_shm/data_structures/ipc/internal/shm_internal.h"
#include "hermes_shm/thread/lock.h"
#include "vector.h"
#include "_queue.h"

namespace hshm::ipc {

/** Forward declaration of ticket_queue */
template<typename T>
class ticket_queue;

/**
 * MACROS used to simplify the ticket_queue namespace
 * Used as inputs to the SHM_CONTAINER_TEMPLATE
 * */
#define CLASS_NAME ticket_queue
#define TYPED_CLASS ticket_queue<T>
#define TYPED_HEADER ShmHeader<ticket_queue<T>>

#define MARK_FIRST_BIT (((T)1) << (8 * sizeof(T) - 1))
#define MARK_TICKET(tkt) ((tkt) | MARK_FIRST_BIT)
#define IS_MARKED(tkt) ((tkt) & MARK_FIRST_BIT)
#define UNMARK_TICKET(tkt) ((tkt) & ~MARK_FIRST_BIT)

/**
 * A MPMC queue for allocating tickets. Handles concurrency
 * without blocking.
 * */
template<typename T>
class ticket_queue : public ShmContainer {
 public:
  SHM_CONTAINER_TEMPLATE((CLASS_NAME), (TYPED_CLASS))
  ShmArchive<vector<T>> queue_;
  std::atomic<_qtok_t> head_, tail_;

 public:
  /**====================================
   * Default Constructor
   * ===================================*/

  /** SHM constructor. Default. */
  explicit ticket_queue(Allocator *alloc,
                        size_t depth = 1024) {
    shm_init_container(alloc);
    HSHM_MAKE_AR(queue_, GetAllocator(), depth, 0);
    SetNull();
  }

  /**====================================
   * Copy Constructors
   * ===================================*/

  /** SHM copy constructor */
  explicit ticket_queue(Allocator *alloc,
                        const ticket_queue &other) {
    shm_init_container(alloc);
    SetNull();
    shm_strong_copy_construct_and_op(other);
  }

  /** SHM copy assignment operator */
  ticket_queue& operator=(const ticket_queue &other) {
    if (this != &other) {
      shm_destroy();
      shm_strong_copy_construct_and_op(other);
    }
    return *this;
  }

  /** SHM copy constructor + operator main */
  void shm_strong_copy_construct_and_op(const ticket_queue &other) {
    head_ = other.head_.load();
    tail_ = other.tail_.load();
    (*queue_) = (*other.queue_);
  }

  /**====================================
   * Move Constructors
   * ===================================*/

  /** SHM move constructor. */
  ticket_queue(Allocator *alloc,
               ticket_queue &&other) noexcept {
    shm_init_container(alloc);
    if (GetAllocator() == other.GetAllocator()) {
      head_ = other.head_.load();
      tail_ = other.tail_.load();
      (*queue_) = std::move(*other.queue_);
      other.SetNull();
    } else {
      shm_strong_copy_construct_and_op(other);
      other.shm_destroy();
    }
  }

  /** SHM move assignment operator. */
  ticket_queue& operator=(ticket_queue &&other) noexcept {
    if (this != &other) {
      shm_destroy();
      if (GetAllocator() == other.GetAllocator()) {
        head_ = other.head_.load();
        tail_ = other.tail_.load();
        (*queue_) = std::move(*other.queue_);
        other.SetNull();
      } else {
        shm_strong_copy_construct_and_op(other);
        other.shm_destroy();
      }
    }
    return *this;
  }

  /**====================================
   * Destructor
   * ===================================*/

  /** SHM destructor.  */
  void shm_destroy_main() {
    (*queue_).shm_destroy();
  }

  /** Check if the list is empty */
  bool IsNull() const {
    return (*queue_).IsNull();
  }

  /** Sets this list as empty */
  void SetNull() {
    head_ = 0;
    tail_ = 0;
  }

  /**====================================
   * ticket Queue Methods
   * ===================================*/

  /** Construct an element at \a pos position in the queue */
  template<typename ...Args>
  qtok_t emplace(T &tkt) {
    auto &queue = *queue_;
    do {
      // Get the current tail
      _qtok_t entry_tok = tail_.load();
      _qtok_t tail = entry_tok + 1;

      // Verify tail exists
      uint32_t idx = entry_tok % queue.size();
      auto &entry = queue[idx];
      if (IS_MARKED(entry)) {
        return qtok_t::GetNull();
      }

      // Claim the tail
      bool ret = tail_.compare_exchange_weak(entry_tok, tail);
      if (!ret) {
        continue;
      }

      // Update the tail
      entry = MARK_TICKET(tkt);
      return qtok_t(entry_tok);
    } while (true);
  }

 public:
  /** Pop an element from the queue */
  qtok_t pop(T &tkt) {
    auto &queue = *queue_;
    do {
      // Get the current head
      _qtok_t entry_tok = head_.load();
      _qtok_t head = entry_tok + 1;

      // Verify head is marked
      uint32_t idx = entry_tok % queue.size();
      auto &entry = queue[idx];
      if (!IS_MARKED(entry)) {
        return qtok_t::GetNull();
      }

      // Claim the head
      bool ret = head_.compare_exchange_weak(entry_tok, head);
      if (!ret) {
        continue;
      }

      // Update the head
      tkt = UNMARK_TICKET(entry);
      entry = 0;
      return qtok_t(entry_tok);
    } while (true);
  }
};

}  // namespace hshm::ipc

#undef TYPED_HEADER
#undef TYPED_CLASS
#undef CLASS_NAME

#endif  // HERMES_SHM_INCLUDE_HERMES_SHM_DATA_STRUCTURES_IPC_TICKET_QUEUE_H_
