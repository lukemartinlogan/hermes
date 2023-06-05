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


#include <hermes_shm/memory/allocator/scalable_page_allocator.h>
#include <hermes_shm/memory/allocator/mp_page.h>

namespace hshm::ipc {

void ScalablePageAllocator::shm_init(allocator_id_t id,
                                     size_t custom_header_size,
                                     char *buffer,
                                     size_t buffer_size,
                                     RealNumber coalesce_trigger,
                                     size_t coalesce_window) {
  buffer_ = buffer;
  buffer_size_ = buffer_size;
  header_ = reinterpret_cast<ScalablePageAllocatorHeader*>(buffer_);
  custom_header_ = reinterpret_cast<char*>(header_ + 1);
  size_t region_off = (custom_header_ - buffer_) + custom_header_size;
  size_t region_size = buffer_size_ - region_off;
  allocator_id_t sub_id(id.bits_.major_, id.bits_.minor_ + 1);
  alloc_.shm_init(sub_id, 0, buffer + region_off, region_size);
  HERMES_MEMORY_REGISTRY_REF.RegisterAllocator(&alloc_);
  header_->Configure(id, custom_header_size, &alloc_,
                     buffer_size, coalesce_trigger, coalesce_window);
  heap_ = alloc_.heap_;
  page_caches_ = header_->page_caches_.get();
  // Cache every power-of-two between 64B and 16MB
  size_t splits = HERMES_SYSTEM_INFO->ncpu_;
  page_caches_->reserve(num_page_caches_);
  for (size_t exp = 0; exp < num_page_caches_; ++exp) {
    size_t depth_per_split = MEGABYTES(1) / sizeof(uint32_t) / splits;
    page_caches_->emplace_back(depth_per_split, splits);
    size_t size_mp = GetPageSize(exp);
    size_t expand = 64;
    if (expand * size_mp <= MEGABYTES(1)) {
      expand = MEGABYTES(1) / size_mp;
    }
    ExpandFromHeap(size_mp, exp, expand);
  }
}

void ScalablePageAllocator::shm_deserialize(char *buffer,
                                            size_t buffer_size) {
  buffer_ = buffer;
  buffer_size_ = buffer_size;
  header_ = reinterpret_cast<ScalablePageAllocatorHeader*>(buffer_);
  custom_header_ = reinterpret_cast<char*>(header_ + 1);
  size_t region_off = (custom_header_ - buffer_) + header_->custom_header_size_;
  size_t region_size = buffer_size_ - region_off;
  alloc_.shm_deserialize(buffer + region_off, region_size);
  HERMES_MEMORY_REGISTRY_REF.RegisterAllocator(&alloc_);
  page_caches_ = header_->page_caches_.get();
}

size_t ScalablePageAllocator::GetCurrentlyAllocatedSize() {
  return header_->total_alloc_;
}

OffsetPointer ScalablePageAllocator::AllocateOffset(size_t size) {
  MpPage *page;
  size_t exp;
  size_t size_mp = RoundUp(size + sizeof(MpPage), exp);
  OffsetPointer p_mp;

  // Case 1: Can we re-use an existing page?
  page = CheckLocalCaches(size_mp, exp, p_mp);

  // Case 2: Check if enough space is wasted
  // TODO(llogan): keep track of amount freed

  // Case 3: Allocate from stack if no page found
  if (page == nullptr) {
    AllocateFromHeap(size_mp, exp, p_mp);
  }

  // Mark as allocated
  auto p = p_mp + sizeof(MpPage);
  return p;
}

OffsetPointer ScalablePageAllocator::AlignedAllocateOffset(size_t size,
                                                           size_t alignment) {
  throw ALIGNED_ALLOC_NOT_SUPPORTED.format();
}

OffsetPointer ScalablePageAllocator::ReallocateOffsetNoNullCheck(
  OffsetPointer p, size_t new_size) {
  OffsetPointer p_mp = p - sizeof(MpPage);
  auto *page = Convert<MpPage>(p_mp);
  size_t size_mp = new_size + sizeof(MpPage);
  size_t old_size_mp = page->page_size_ - page->off_;
  if (old_size_mp >= size_mp) {
    return p;
  }
  size_t old_size = old_size_mp - sizeof(MpPage);
  OffsetPointer new_p = AllocateOffset(new_size);
  auto *old_ptr = (char*)(page + 1);
  auto *new_ptr = Convert<char>(new_p);
  memcpy(new_ptr, old_ptr, old_size);
  FreeOffsetNoNullCheck(p);
  return new_p;
}

void ScalablePageAllocator::FreeOffsetNoNullCheck(OffsetPointer p) {
  OffsetPointer p_mp = p - sizeof(MpPage);
  auto *page = Convert<MpPage>(p_mp);
  size_t exp;
  RoundUp(page->page_size_, exp);
  SPA_PAGE_CACHE &pages = (*page_caches_)[exp];
  size_t page_off = p_mp.load();
  if (pages.emplace(page_off).IsNull()) {
    throw OUT_OF_CACHE.format();
  }
}

}  // namespace hshm::ipc
