//
// Created by lukemartinlogan on 8/11/23.
//

#ifndef LABSTOR_TASKS_TASK_TEMPL_INCLUDE_hermes_adapters_hermes_adapters_TASKS_H_
#define LABSTOR_TASKS_TASK_TEMPL_INCLUDE_hermes_adapters_hermes_adapters_TASKS_H_

#include "labstor/api/labstor_client.h"
#include "labstor/task_registry/task_lib.h"
#include "labstor_admin/labstor_admin.h"
#include "labstor/queue_manager/queue_manager_client.h"
#include "proc_queue/proc_queue.h"

namespace labstor::hermes_adapters {

#include "hermes_adapters_methods.h"
#include "labstor/labstor_namespace.h"
using labstor::proc_queue::TypedPushTask;
using labstor::proc_queue::PushTask;

/**
 * A task to create hermes_adapters
 * */
using labstor::Admin::CreateTaskStateTask;
struct ConstructTask : public CreateTaskStateTask {
  /** SHM default constructor */
  HSHM_ALWAYS_INLINE explicit
  ConstructTask(hipc::Allocator *alloc)
  : CreateTaskStateTask(alloc) {}

  /** Emplace constructor */
  HSHM_ALWAYS_INLINE explicit
  ConstructTask(hipc::Allocator *alloc,
                const TaskNode &task_node,
                const DomainId &domain_id,
                const std::string &state_name,
                const TaskStateId &id,
                const std::vector<PriorityInfo> &queue_info)
      : CreateTaskStateTask(alloc, task_node, domain_id, state_name,
                            "hermes_adapters", id, queue_info) {
    // Custom params
  }

  HSHM_ALWAYS_INLINE
  ~ConstructTask() {
    // Custom params
  }
};

/** A task to destroy hermes_adapters */
using labstor::Admin::DestroyTaskStateTask;
struct DestructTask : public DestroyTaskStateTask {
  /** SHM default constructor */
  HSHM_ALWAYS_INLINE explicit
  DestructTask(hipc::Allocator *alloc)
  : DestroyTaskStateTask(alloc) {}

  /** Emplace constructor */
  HSHM_ALWAYS_INLINE explicit
  DestructTask(hipc::Allocator *alloc,
               const TaskNode &task_node,
               const DomainId &domain_id,
               TaskStateId &state_id)
  : DestroyTaskStateTask(alloc, task_node, domain_id, state_id) {}

  /** Create group */
  HSHM_ALWAYS_INLINE
  u32 GetGroup(hshm::charbuf &group) {
    return TASK_UNORDERED;
  }
};

/**
 * A custom task in hermes_adapters
 * */
struct CustomTask : public Task, TaskFlags<TF_SRL_SYM> {
  /** SHM default constructor */
  HSHM_ALWAYS_INLINE explicit
  CustomTask(hipc::Allocator *alloc) : Task(alloc) {}

  /** Emplace constructor */
  HSHM_ALWAYS_INLINE explicit
  CustomTask(hipc::Allocator *alloc,
             const TaskNode &task_node,
             const DomainId &domain_id,
             const TaskStateId &state_id) : Task(alloc) {
    // Initialize task
    task_node_ = task_node;
    lane_hash_ = 0;
    prio_ = TaskPrio::kLowLatency;
    task_state_ = state_id;
    method_ = Method::kCustom;
    task_flags_.SetBits(0);
    domain_id_ = domain_id;

    // Custom params
  }

  /** (De)serialize message call */
  template<typename Ar>
  void SerializeStart(Ar &ar) {
    task_serialize<Ar>(ar);
  }

  /** (De)serialize message return */
  template<typename Ar>
  void SerializeEnd(u32 replica, Ar &ar) {
  }

  /** Create group */
  HSHM_ALWAYS_INLINE
  u32 GetGroup(hshm::charbuf &group) {
    return TASK_UNORDERED;
  }
};

}  // namespace labstor::hermes_adapters

#endif  // LABSTOR_TASKS_TASK_TEMPL_INCLUDE_hermes_adapters_hermes_adapters_TASKS_H_