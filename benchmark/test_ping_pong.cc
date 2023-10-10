//
// Created by lukemartinlogan on 10/10/23.
//

#include <mpi.h>
#include <iostream>
#include "hermes_shm/data_structures/ipc/string.h"
#include "hermes_shm/data_structures/ipc/mpsc_queue.h"
#include "hermes_shm/util/error.h"
#include "hermes_shm/util/affinity.h"
#include "hermes_shm/data_structures/data_structure.h"

using hshm::ipc::PosixShmMmap;
using hshm::ipc::MemoryBackendType;
using hshm::ipc::MemoryBackend;
using hshm::ipc::allocator_id_t;
using hshm::ipc::AllocatorType;
using hshm::ipc::Allocator;
using hshm::ipc::Pointer;

using hshm::ipc::MemoryBackendType;
using hshm::ipc::MemoryBackend;
using hshm::ipc::allocator_id_t;
using hshm::ipc::AllocatorType;
using hshm::ipc::Allocator;
using hshm::ipc::MemoryManager;
using hshm::ipc::Pointer;

Allocator *alloc_g;

template<typename AllocT>
void PretestRank0() {
  std::string shm_url = "test_allocators";
  allocator_id_t alloc_id(0, 1);
  auto mem_mngr = HERMES_MEMORY_MANAGER;
  mem_mngr->UnregisterAllocator(alloc_id);
  mem_mngr->UnregisterBackend(shm_url);
  mem_mngr->CreateBackend<PosixShmMmap>(
      MEGABYTES(100), shm_url);
  mem_mngr->CreateAllocator<AllocT>(shm_url, alloc_id, sizeof(Pointer));
  alloc_g = mem_mngr->GetAllocator(alloc_id);
}

void PretestRankN() {
  std::string shm_url = "test_allocators";
  allocator_id_t alloc_id(0, 1);
  auto mem_mngr = HERMES_MEMORY_MANAGER;
  mem_mngr->AttachBackend(MemoryBackendType::kPosixShmMmap, shm_url);
  alloc_g = mem_mngr->GetAllocator(alloc_id);
}

void MainPretest() {
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  if (rank == 0) {
    // PretestRank0<hipc::StackAllocator>();
    PretestRank0<hipc::ScalablePageAllocator>();
  }
  MPI_Barrier(MPI_COMM_WORLD);
  if (rank != 0) {
    PretestRankN();
  }
}

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);
  MainPretest();
  
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  // The allocator was initialized in test_init.c
  // we are getting the "header" of the allocator
  hipc::Allocator *alloc = alloc_g;
  hipc::Pointer *header = alloc->GetCustomHeader<hipc::Pointer>();
  HIPRINT("Rank: {} has begun", rank)

  // Make the queue uptr
  hipc::uptr<hipc::mpsc_queue<hipc::Pointer>> queue_;
  if (rank == 0) {
    // Rank 0 create the pointer queue
    queue_ = hipc::make_uptr<hipc::mpsc_queue<hipc::Pointer>>(alloc, 256);
    queue_ >> (*header);
    // Affine to CPU 0
    ProcessAffiner::SetCpuAffinity(getpid(), 0);
  }
  MPI_Barrier(MPI_COMM_WORLD);
  if (rank != 0) {
    // Rank 1 gets the pointer queue
    queue_ << (*header);
    // Affine to CPU 1
    ProcessAffiner::SetCpuAffinity(getpid(), 1);
  }
  HIPRINT("Rank: {} has started", rank)

  size_t ops = (1 << 16);
  if (rank == 0) {
    // Emplace values into the queue
    hshm::Timer t;
    t.Resume();
    for (int i = 0; i < ops; ++i) {
      hipc::LPointer<int> obj = alloc->AllocateLocalPtr<int>(1);
      (*obj.ptr_) = 0;
      queue_->emplace(obj.shm_);
      do {
        sched_yield();
      } while(!(*obj.ptr_));
      queue_->pop();
      alloc->FreeLocalPtr(obj);
    }
    t.Pause();
  } else {
    // Pop entries from the queue
    hipc::Pointer *x;
    int count = 0;
    while (!queue_->peek(x, 0).IsNull() && count < ops) {
      int *ptr = alloc->Convert<int>(*x);
      (*ptr) = 1;
      ++count;
    }
  }

  // The barrier is necessary so that
  // Rank 0 doesn't exit before Rank 1
  // The uptr frees data when rank 0 exits.
  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Finalize();
}