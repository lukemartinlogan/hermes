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

#ifndef HRUN_hermes_traits_H_
#define HRUN_hermes_traits_H_

#include "hermes_traits_tasks.h"

namespace hermes::traits {

/** Create hermes_traits requests */
class Client : public TaskLibClient {

 public:
  /** Default constructor */
  Client() = default;

  /** Destructor */
  ~Client() = default;

  /** Async create a task state */
  HSHM_ALWAYS_INLINE
  LPointer<ConstructTask> AsyncCreate(const TaskNode &task_node,
                                      const DomainId &domain_id,
                                      const std::string &state_name) {
    id_ = TaskStateId::GetNull();
    QueueManagerInfo &qm = HRUN_CLIENT->server_config_.queue_manager_;
    std::vector<PriorityInfo> queue_info;
    return HRUN_ADMIN->AsyncCreateTaskState<ConstructTask>(
        task_node, domain_id, state_name, id_, queue_info);
  }
  HRUN_TASK_NODE_ROOT(AsyncCreate)
  template<typename ...Args>
  HSHM_ALWAYS_INLINE
  void CreateRoot(Args&& ...args) {
    LPointer<ConstructTask> task =
        AsyncCreateRoot(std::forward<Args>(args)...);
    task->Wait();
    Init(task->id_, HRUN_ADMIN->queue_id_);
    HRUN_CLIENT->DelTask(task);
  }

  /** Destroy task state + queue */
  HSHM_ALWAYS_INLINE
  void DestroyRoot(const DomainId &domain_id) {
    HRUN_ADMIN->DestroyTaskStateRoot(domain_id, id_);
  }

  /** Encode a blob */
  HSHM_ALWAYS_INLINE
  void AsyncEncodeConstruct(EncodeTask *task,
                            const TaskNode &task_node,
                            const DomainId &domain_id,
                            BlobInfo &blob_info,
                            blob_mdm::PutBlobTask *put_blob_task,
                            blob_mdm::Server *blob_mdm_state) {
    HRUN_CLIENT->ConstructTask<EncodeTask>(
        task, task_node, domain_id, id_,
        blob_info, put_blob_task, blob_mdm_state);
  }
  HSHM_ALWAYS_INLINE
  void EncodeRoot(const DomainId &domain_id,
                  BlobInfo &blob_info,
                  blob_mdm::PutBlobTask *put_blob_task,
                  blob_mdm::Server *blob_mdm_state) {
    LPointer<hrunpq::TypedPushTask<EncodeTask>> task =
        AsyncEncodeRoot(domain_id, blob_info, put_blob_task, blob_mdm_state);
    task.ptr_->Wait();
  }
  HRUN_TASK_NODE_PUSH_ROOT(Encode);

  /** Encode a blob */
  HSHM_ALWAYS_INLINE
  void AsyncDecodeConstruct(DecodeTask *task,
                            const TaskNode &task_node,
                            const DomainId &domain_id,
                            BlobInfo &blob_info,
                            blob_mdm::GetBlobTask *get_blob_task,
                            blob_mdm::Server *blob_mdm_state) {
    HRUN_CLIENT->ConstructTask<DecodeTask>(
        task, task_node, domain_id, id_,
        blob_info, get_blob_task, blob_mdm_state);
  }
  HSHM_ALWAYS_INLINE
  void DecodeRoot(const DomainId &domain_id,
                  BlobInfo &blob_info,
                  blob_mdm::GetBlobTask *get_blob_task,
                  blob_mdm::Server *blob_mdm_state) {
    LPointer<hrunpq::TypedPushTask<DecodeTask>> task =
        AsyncDecodeRoot(domain_id, blob_info, get_blob_task,
                        blob_mdm_state);
    task.ptr_->Wait();
  }
  HRUN_TASK_NODE_PUSH_ROOT(Decode);
};

}  // namespace hrun

#endif  // HRUN_hermes_traits_H_
