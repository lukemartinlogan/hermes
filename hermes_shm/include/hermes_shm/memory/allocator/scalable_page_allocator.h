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


#ifndef HERMES_MEMORY_ALLOCATOR_SCALABLE_PAGE_ALLOCATOR_H
#define HERMES_MEMORY_ALLOCATOR_SCALABLE_PAGE_ALLOCATOR_H

#include "allocator.h"
#include "hermes_shm/thread/lock.h"
#include "hermes_shm/data_structures/ipc/pair.h"
#include "hermes_shm/data_structures/ipc/vector.h"
#include "hermes_shm/data_structures/ipc/list.h"
#include "hermes_shm/data_structures/ipc/pair.h"
#include "hermes_shm/data_structures/ipc/split_ticket_queue.h"
#include <hermes_shm/memory/allocator/stack_allocator.h>
#include "mp_page.h"

namespace hshm::ipc {

typedef split_ticket_queue<size_t> SPA_PAGE_CACHE;

struct ScalablePageAllocatorHeader : public AllocatorHeader {
  ShmArchive<vector<SPA_PAGE_CACHE>> page_caches_;
  std::atomic<size_t> total_alloc_;
  size_t coalesce_trigger_;
  size_t coalesce_window_;
  RwLock coalesce_lock_;

  ScalablePageAllocatorHeader() = default;

  void Configure(allocator_id_t alloc_id,
                 size_t custom_header_size,
                 Allocator *alloc,
                 size_t buffer_size,
                 RealNumber coalesce_trigger,
                 size_t coalesce_window) {
    AllocatorHeader::Configure(alloc_id,
                               AllocatorType::kScalablePageAllocator,
                               custom_header_size);
    HSHM_MAKE_AR0(page_caches_, alloc)
    total_alloc_ = 0;
    coalesce_trigger_ = (coalesce_trigger * buffer_size).as_int();
    coalesce_window_ = coalesce_window;
    coalesce_lock_.Init();
  }
};

class ScalablePageAllocator : public Allocator {
 private:
  ScalablePageAllocatorHeader *header_;
  vector<SPA_PAGE_CACHE> *page_caches_;
  HeapAllocator *heap_;
  StackAllocator alloc_;
  /** The power-of-two exponent of the minimum size that can be cached */
  static const size_t min_cached_size_exp_ = 6;
  /** The minimum size that can be cached directly (64 bytes) */
  static const size_t min_cached_size_ = (1 << min_cached_size_exp_);
  /** The power-of-two exponent of the minimum size that can be cached */
  static const size_t max_cached_size_exp_ = 22;
  /** The maximum size that can be cached directly (4MB) */
  static const size_t max_cached_size_ = (1 << max_cached_size_exp_);
  /** Cache every size between 64 (2^6) BYTES and 4MB (2^22): (17 entries) */
  static const size_t num_caches_ = max_cached_size_exp_ -
                                    min_cached_size_exp_ + 1;
  /**
   * The last free list stores sizes larger than 4MB or sizes which are
   * not exactly powers-of-two.
   * */
  static const size_t num_page_caches_ = num_caches_ + 1;

 public:
  /**
   * Default Constructor
   * */
  HSHM_ALWAYS_INLINE ScalablePageAllocator()
  : header_(nullptr) {}

  /**
   * Destructor
   * */
  virtual ~ScalablePageAllocator() {
    HERMES_MEMORY_REGISTRY_REF.UnregisterAllocator(alloc_.GetId());
  }

  /**
   * Get the ID of this allocator from shared memory
   * */
  allocator_id_t &GetId() override {
    return header_->allocator_id_;
  }

  /**
   * Initialize the allocator in shared memory
   * */
  void shm_init(allocator_id_t id,
                size_t custom_header_size,
                char *buffer,
                size_t buffer_size,
                RealNumber coalesce_trigger = RealNumber(1, 5),
                size_t coalesce_window = MEGABYTES(1));

  /**
   * Attach an existing allocator from shared memory
   * */
  void shm_deserialize(char *buffer,
                       size_t buffer_size) override;

  /**
   * Allocate a memory of \a size size. The page allocator cannot allocate
   * memory larger than the page size.
   * */
  OffsetPointer AllocateOffset(size_t size) override;

 private:
  /** Allocate a page from the heap */
  HSHM_ALWAYS_INLINE MpPage* AllocateFromHeap(size_t size_mp,
                                              size_t exp,
                                              OffsetPointer &p_mp) {
    MpPage *page;

    // Get number of pages to allocate from the heap
    size_t expand = 32;
    if (size_mp > max_cached_size_) {
      expand = 1;
    }
    do {
      ExpandFromHeap(size_mp, exp, expand);
      page = CheckLocalCaches(size_mp, exp, p_mp);
    } while (page == nullptr);
    return page;
  }

  /** Allocate a page from the heap */
  HSHM_ALWAYS_INLINE MpPage* ExpandFromHeap(size_t size_mp,
                                            size_t exp,
                                            size_t expand) {
    // Make one large allocation to the heap
    size_t expand_size = expand * size_mp;
    OffsetPointer p_mp = heap_->AllocateOffset(expand_size);

    // Divide allocation into pages
    if (!p_mp.IsNull()) {
      // Store the first page
      SPA_PAGE_CACHE &pages = (*page_caches_)[exp];
      auto *page = alloc_.Convert<MpPage>(p_mp);
      p_mp = Convert<MpPage, OffsetPointer>(page);
      size_t p_mp_cur = p_mp.load();
      MpPage *page_cur = page;
      for (size_t i = 0; i < expand; ++i) {
        p_mp_cur += size_mp;
        page_cur = (MpPage*)((char*)page_cur + size_mp);
        page_cur->page_size_ = size_mp;
        page_cur->off_ = 0;
        pages.emplace(p_mp_cur);
      }
    }
    return nullptr;
  }

  /** Check if a cached page on this core can be re-used */
  HSHM_ALWAYS_INLINE MpPage* CheckLocalCaches(size_t size_mp,
                                              size_t exp,
                                              OffsetPointer &p_mp) {
    SPA_PAGE_CACHE &pages = (*page_caches_)[exp];
    if (exp < num_caches_) {
      size_t page_off;
      qtok_t qtok = pages.pop(page_off);
      if (qtok.IsNull()) {
        return nullptr;
      }
      p_mp.exchange(page_off);
      auto page = Convert<MpPage>(p_mp);
      page->page_size_ = size_mp;
      page->off_ = 0;
      return page;
    } else {
      return FindFirstFit(size_mp, pages, p_mp);
    }
  }

  /**
   * Find the first fit of an element in a free list
   * @param size_mp size of the memory region, including the header
   * @param pages the set of cached pages
   * @param p_mp the offset of the MpPage from beginning of SHM region
   * */
  HSHM_ALWAYS_INLINE MpPage* FindFirstFit(size_t size_mp,
                                          SPA_PAGE_CACHE &pages,
                                          OffsetPointer &p_mp) {
    size_t min_divide_size = size_mp + sizeof(MpPage) + 256;
    // TODO(llogan): Use pages.size()
    for (size_t i = 0; i < 1024; ++i) {
      size_t page_off;
      qtok_t qtok = pages.pop(page_off);
      if (qtok.IsNull()) {
        return nullptr;
      }
      p_mp.exchange(page_off);
      auto page = Convert<MpPage>(p_mp);
      if (page->page_size_ > min_divide_size) {
        // Page was larger than the size being checked, must divide
        DividePage(pages, page_off, page, size_mp);
        return page;
      } else if (page->page_size_ >= size_mp) {
        // Page was nearly the size being checked, return
        return page;
      } else {
        // Page was smaller than the size being checked
        pages.emplace(page_off);
      }
    }
    return nullptr;
  }

  /**
   * Divide a page into smaller pages and cache them
   * */
  HSHM_ALWAYS_INLINE static void DividePage(SPA_PAGE_CACHE &pages,
                                            size_t fit_page_off,
                                            MpPage *fit_page,
                                            size_t size_mp)  {
    size_t total_size = fit_page->page_size_;
    auto *new_page = (MpPage*)((char*)fit_page + size_mp);
    fit_page->page_size_ = size_mp;
    fit_page->off_ = 0;
    new_page->page_size_ = total_size - size_mp;
    new_page->off_ = 0;
    fit_page_off += (size_t)size_mp;
    pages.emplace(fit_page_off);
  }

 public:
  /**
   * Allocate a memory of \a size size, which is aligned to \a
   * alignment.
   * */
  OffsetPointer AlignedAllocateOffset(size_t size, size_t alignment) override;

  /**
   * Reallocate \a p pointer to \a new_size new size.
   *
   * @return whether or not the pointer p was changed
   * */
  OffsetPointer ReallocateOffsetNoNullCheck(
    OffsetPointer p, size_t new_size) override;

  /**
   * Free \a ptr pointer. Null check is performed elsewhere.
   * */
  void FreeOffsetNoNullCheck(OffsetPointer p) override;

  /**
   * Get the current amount of data allocated. Can be used for leak
   * checking.
   * */
  size_t GetCurrentlyAllocatedSize() override;

 private:
  /** Get the page size corresponding to exp */
  HSHM_ALWAYS_INLINE size_t GetPageSize(size_t exp) {
    size_t round;
    round = 1 << (exp + min_cached_size_exp_);
    round += sizeof(MpPage);
    return round;
  }

  /** Round a number up to the nearest page size. */
  HSHM_ALWAYS_INLINE size_t RoundUp(size_t num, size_t &exp) {
    size_t round;
    for (exp = 0; exp < num_caches_; ++exp) {
      round = GetPageSize(exp);
      if (num <= round) {
        return round;
      }
    }
    return num;
  }
};

}  // namespace hshm::ipc

#endif  // HERMES_MEMORY_ALLOCATOR_SCALABLE_PAGE_ALLOCATOR_H
