//
// Created by lukemartinlogan on 8/11/23.
//

#ifndef HRUN_TASKS_TASK_TEMPL_INCLUDE_hermes_traits_hermes_traits_TASKS_H_
#define HRUN_TASKS_TASK_TEMPL_INCLUDE_hermes_traits_hermes_traits_TASKS_H_

#include "hrun/api/hrun_client.h"
#include "hrun/task_registry/task_lib.h"
#include "hrun_admin/hrun_admin.h"
#include "hrun/queue_manager/queue_manager_client.h"
#include "proc_queue/proc_queue.h"

#include "hermes_blob_mdm/hermes_blob_mdm.h"

namespace hermes::blob_mdm {
class Server;
}  // namespace hermes::blob_mdm

namespace hermes::traits {

#include "hermes_traits_methods.h"
#include "hrun/hrun_namespace.h"
using hrun::proc_queue::TypedPushTask;
using hrun::proc_queue::PushTask;

/**
 * A task to create hermes_traits
 * */
using hrun::Admin::CreateTaskStateTask;
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
                            "hermes_traits", id, queue_info) {
    // Custom params
  }

  HSHM_ALWAYS_INLINE
  ~ConstructTask() {
    // Custom params
  }
};

/** A task to destroy hermes_traits */
using hrun::Admin::DestroyTaskStateTask;
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
 * A task to encode data
 * */
struct EncodeTask : public Task, TaskFlags<TF_LOCAL> {
  IN BlobInfo *blob_info_;
  IN blob_mdm::PutBlobTask *put_blob_task_;
  IN blob_mdm::Server *blob_mdm_state_;

  /** SHM default constructor */
  HSHM_ALWAYS_INLINE explicit
  EncodeTask(hipc::Allocator *alloc) : Task(alloc) {}

  /** Emplace constructor */
  HSHM_ALWAYS_INLINE explicit
  EncodeTask(hipc::Allocator *alloc,
             const TaskNode &task_node,
             const DomainId &domain_id,
             const TaskStateId &state_id,
             BlobInfo &blob_info,
             blob_mdm::PutBlobTask *put_blob_task,
             blob_mdm::Server *blob_mdm_state) : Task(alloc) {
    // Initialize task
    task_node_ = task_node;
    lane_hash_ = 0;
    prio_ = TaskPrio::kLowLatency;
    task_state_ = state_id;
    method_ = Method::kEncode;
    task_flags_.SetBits(0);
    domain_id_ = domain_id;

    // Custom params
    blob_info_ = &blob_info;
    put_blob_task_ = put_blob_task;
    blob_mdm_state_ = blob_mdm_state;
  }

  /** Create group */
  HSHM_ALWAYS_INLINE
  u32 GetGroup(hshm::charbuf &group) {
    return TASK_UNORDERED;
  }
};

/**
 * A task to decode data
 * */
struct DecodeTask : public Task, TaskFlags<TF_LOCAL> {
  IN BlobInfo *blob_info_;
  IN blob_mdm::GetBlobTask *get_blob_task_;
  IN blob_mdm::Server *blob_mdm_state_;

  /** SHM default constructor */
  HSHM_ALWAYS_INLINE explicit
  DecodeTask(hipc::Allocator *alloc) : Task(alloc) {}

  /** Emplace constructor */
  HSHM_ALWAYS_INLINE explicit
  DecodeTask(hipc::Allocator *alloc,
             const TaskNode &task_node,
             const DomainId &domain_id,
             const TaskStateId &state_id,
             BlobInfo &blob_info,
             blob_mdm::GetBlobTask *get_blob_task,
             blob_mdm::Server *blob_mdm_state) : Task(alloc) {
    // Initialize task
    task_node_ = task_node;
    lane_hash_ = 0;
    prio_ = TaskPrio::kLowLatency;
    task_state_ = state_id;
    method_ = Method::kEncode;
    task_flags_.SetBits(0);
    domain_id_ = domain_id;

    // Custom params
    blob_info_ = &blob_info;
    get_blob_task_ = get_blob_task;
    blob_mdm_state_ = blob_mdm_state;
  }

  /** Create group */
  HSHM_ALWAYS_INLINE
  u32 GetGroup(hshm::charbuf &group) {
    return TASK_UNORDERED;
  }
};

}  // namespace hermes::traits

#endif  // HRUN_TASKS_TASK_TEMPL_INCLUDE_hermes_traits_hermes_traits_TASKS_H_
