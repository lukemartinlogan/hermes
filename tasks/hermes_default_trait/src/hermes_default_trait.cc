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

#include "hrun_admin/hrun_admin.h"
#include "hrun/api/hrun_runtime.h"
#include "hermes_default_trait/hermes_default_trait.h"
#include "hermes/hermes_types.h"
#include "hermes_blob_mdm/hermes_blob_mdm_tasks.h"
#include "hermes/dpe/dpe_factory.h"
#include "tasks/hermes_blob_mdm/include/hermes_blob_mdm/hermes_blob_mdm_server.h"

namespace hermes::traits::default_trait {

class Server : public TaskLib {
 public:
  Server() = default;

  /** Construct hermes_default_trait */
  void Construct(ConstructTask *task, RunContext &rctx) {
    task->SetModuleComplete();
  }
  void MonitorConstruct(u32 mode, ConstructTask *task, RunContext &rctx) {
  }

  /** Destroy hermes_default_trait */
  void Destruct(DestructTask *task, RunContext &rctx) {
    task->SetModuleComplete();
  }
  void MonitorDestruct(u32 mode, DestructTask *task, RunContext &rctx) {
  }

  /** A encoding method */
  void Encode(EncodeTask *task, RunContext &rctx) {
    BlobInfo &blob_info = *task->blob_info_;
    blob_mdm::PutBlobTask *put_task = task->put_blob_task_;
    blob_mdm::Server *blob_mdm = task->blob_mdm_state_;
    size_t bkt_size_diff = 0;

    // Determine amount of additional buffering space needed
    size_t needed_space = put_task->blob_off_ + put_task->data_size_;
    size_t size_diff = 0;
    if (needed_space > blob_info.max_blob_size_) {
      size_diff = needed_space - blob_info.max_blob_size_;
    }
    size_t min_blob_size = put_task->blob_off_ + put_task->data_size_;
    if (min_blob_size > blob_info.blob_size_) {
      blob_info.blob_size_ = put_task->blob_off_ + put_task->data_size_;
    }
    bkt_size_diff += (ssize_t)size_diff;
    HILOG(kDebug, "The size diff is {} bytes (bkt diff {})",
          size_diff, bkt_size_diff)

    // Use DPE
    std::vector<PlacementSchema> schema_vec;
    if (size_diff > 0) {
      Context ctx;
      auto *dpe = DpeFactory::Get(ctx.dpe_);
      ctx.blob_score_ = put_task->score_;
      dpe->Placement({size_diff}, blob_mdm->targets_, ctx, schema_vec);
    }

    // Allocate blob buffers
    for (PlacementSchema &schema : schema_vec) {
      schema.plcmnts_.emplace_back(0, blob_mdm->fallback_target_->id_);
      for (size_t sub_idx = 0; sub_idx < schema.plcmnts_.size(); ++sub_idx) {
        SubPlacement &placement = schema.plcmnts_[sub_idx];
        TargetInfo &bdev = *blob_mdm->target_map_[placement.tid_];
        LPointer<bdev::AllocateTask> alloc_task =
            bdev.AsyncAllocate(put_task->task_node_ + 1,
                               blob_info.score_,
                               placement.size_,
                               blob_info.buffers_);
        alloc_task->Wait<TASK_YIELD_CO>(task);
//        HILOG(kInfo, "(node {}) Placing {}/{} bytes in target {} of bw {}",
//              HRUN_CLIENT->node_id_,
//              alloc_put_task->alloc_size_, put_task->data_size_,
//              placement.tid_, bdev.bandwidth_)
        if (alloc_task->alloc_size_ < alloc_task->size_) {
          SubPlacement &next_placement = schema.plcmnts_[sub_idx + 1];
          size_t diff = alloc_task->size_ - alloc_task->alloc_size_;
          next_placement.size_ += diff;
        }
        // bdev.monitor_task_->rem_cap_ -= alloc_put_task->alloc_size_;
        HRUN_CLIENT->DelTask(alloc_task);
      }
    }

    // Place blob in buffers
    std::vector<LPointer<bdev::WriteTask>> write_tasks;
    write_tasks.reserve(blob_info.buffers_.size());
    size_t blob_off = put_task->blob_off_, buf_off = 0;
    size_t buf_left = 0, buf_right = 0;
    size_t blob_right = put_task->blob_off_ + put_task->data_size_;
    char *blob_buf = HRUN_CLIENT->GetDataPointer(put_task->data_);
    HILOG(kDebug, "Number of buffers {}", blob_info.buffers_.size());
    bool found_left = false;
    for (BufferInfo &buf : blob_info.buffers_) {
      buf_right = buf_left + buf.t_size_;
      if (blob_off >= blob_right) {
        break;
      }
      if (buf_left <= blob_off && blob_off < buf_right) {
        found_left = true;
      }
      if (found_left) {
        size_t rel_off = blob_off - buf_left;
        size_t tgt_off = buf.t_off_ + rel_off;
        size_t buf_size = buf.t_size_ - rel_off;
        if (buf_right > blob_right) {
          buf_size = blob_right - (buf_left + rel_off);
        }
        HILOG(kDebug, "Writing {} bytes at off {} from target {}", buf_size, tgt_off, buf.tid_)
        TargetInfo &target = *blob_mdm->target_map_[buf.tid_];
        LPointer<bdev::WriteTask> write_task =
            target.AsyncWrite(put_task->task_node_ + 1,
                              blob_buf + buf_off,
                              tgt_off, buf_size);
        write_tasks.emplace_back(write_task);
        buf_off += buf_size;
        blob_off = buf_right;
      }
      buf_left += buf.t_size_;
    }
    blob_info.max_blob_size_ = blob_off;

    // Wait for the placements to complete
    for (LPointer<bdev::WriteTask> &write_task : write_tasks) {
      write_task->Wait<TASK_YIELD_CO>(task);
      HRUN_CLIENT->DelTask(write_task);
    }

    // Update information
    if (put_task->flags_.Any(HERMES_SHOULD_STAGE)) {
      blob_mdm->stager_mdm_.AsyncUpdateSize(put_task->task_node_ + 1,
                                            put_task->tag_id_,
                                            blob_info.name_,
                                            put_task->blob_off_,
                                            put_task->data_size_, 0);
    } else {
      blob_mdm->bkt_mdm_.AsyncUpdateSize(put_task->task_node_ + 1,
                                         put_task->tag_id_,
                                         bkt_size_diff,
                                         bucket_mdm::UpdateSizeMode::kAdd);
    }
    if (put_task->flags_.Any(HERMES_BLOB_DID_CREATE)) {
      blob_mdm->bkt_mdm_.AsyncTagAddBlob(put_task->task_node_ + 1,
                                         put_task->tag_id_,
                                         put_task->blob_id_);
    }
    if (put_task->flags_.Any(HERMES_HAS_DERIVED)) {
      blob_mdm->op_mdm_.AsyncRegisterData(put_task->task_node_ + 1,
                                          put_task->tag_id_,
                                          put_task->blob_name_->str(),
                                          put_task->blob_id_,
                                          put_task->blob_off_,
                                          put_task->data_size_);
    }
    put_task->SetModuleComplete();
  }
  void MonitorEncode(u32 mode, EncodeTask *task, RunContext &rctx) {
  }

  /** A decoding method */
  void Decode(DecodeTask *task, RunContext &rctx) {
    BlobInfo &blob_info = *task->blob_info_;
    GetBlobTask *get_task = task->get_blob_task_;
    blob_mdm::Server *blob_mdm = task->blob_mdm_state_;

    // Read blob from buffers
    std::vector<bdev::ReadTask*> read_tasks;
    read_tasks.reserve(blob_info.buffers_.size());
    HILOG(kDebug, "Getting blob {} of size {} starting at offset {} (total_blob_size={}, buffers={})",
          get_task->blob_id_, get_task->data_size_, get_task->blob_off_, blob_info.blob_size_, blob_info.buffers_.size());
    size_t blob_off = get_task->blob_off_;
    size_t buf_left = 0, buf_right = 0;
    size_t buf_off = 0;
    size_t blob_right = get_task->blob_off_ + get_task->data_size_;
    bool found_left = false;
    char *blob_buf = HRUN_CLIENT->GetDataPointer(get_task->data_);
    for (BufferInfo &buf : blob_info.buffers_) {
      buf_right = buf_left + buf.t_size_;
      if (blob_off >= blob_right) {
        break;
      }
      if (buf_left <= blob_off && blob_off < buf_right) {
        found_left = true;
      }
      if (found_left) {
        size_t rel_off = blob_off - buf_left;
        size_t tgt_off = buf.t_off_ + rel_off;
        size_t buf_size = buf.t_size_ - rel_off;
        if (buf_right > blob_right) {
          buf_size = blob_right - (buf_left + rel_off);
        }
        HILOG(kDebug, "Loading {} bytes at off {} from target {}",
              buf_size, tgt_off, buf.tid_)
        TargetInfo &target = *blob_mdm->target_map_[buf.tid_];
        bdev::ReadTask *read_task = target.AsyncRead(task->task_node_ + 1,
                                                     blob_buf + buf_off,
                                                     tgt_off, buf_size).ptr_;
        read_tasks.emplace_back(read_task);
        buf_off += buf_size;
        blob_off = buf_right;
      }
      buf_left += buf.t_size_;
    }
    for (bdev::ReadTask *&read_task : read_tasks) {
      read_task->Wait<TASK_YIELD_CO>(task);
      HRUN_CLIENT->DelTask(read_task);
    }
    get_task->data_size_ = buf_off;

    task->SetModuleComplete();
  }
  void MonitorDecode(u32 mode, DecodeTask *task, RunContext &rctx) {
  }

 public:
#include "hermes_default_trait/hermes_default_trait_lib_exec.h"
};

}  // namespace hermes::traits::default_trait

HRUN_TASK_CC(hermes::traits::default_trait::Server, "hermes_default_trait");
