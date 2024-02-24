//
// Created by lukemartinlogan on 2/13/24.
//

#ifndef HERMES_INCLUDE_HERMES_TRAITS_MOD_IN_PLACE_BLOB_H_
#define HERMES_INCLUDE_HERMES_TRAITS_MOD_IN_PLACE_BLOB_H_

namespace hermes {

class ModInPlaceBlob {
 public:
  static size_t GetNewLogicalSize(BlobInfo &blob_info,
                                  size_t logical_off,
                                  size_t logical_size,
                                  size_t &size_diff) {
    size_t max_size = logical_off + logical_size;
    if (max_size > blob_info.max_blob_size_) {
      size_diff = max_size - blob_info.max_blob_size_;
      return max_size;
    } else {
      size_diff = 0;
      return blob_info.max_blob_size_;
    }
  }

  static void AllocateBlobBuffers(Task *task,
                                  BlobInfo &blob_info,
                                  std::vector<PlacementSchema> &schema_vec,
                                  blob_mdm::Server *blob_mdm) {
    for (PlacementSchema &schema : schema_vec) {
      schema.plcmnts_.emplace_back(0, blob_mdm->fallback_target_->id_);
      for (size_t sub_idx = 0; sub_idx < schema.plcmnts_.size(); ++sub_idx) {
        SubPlacement &placement = schema.plcmnts_[sub_idx];
        TargetInfo &bdev = *blob_mdm->target_map_[placement.tid_];
        LPointer<bdev::AllocateTask> alloc_task =
            bdev.AsyncAllocate(task->task_node_ + 1,
                               blob_info.score_,
                               placement.size_,
                               blob_info.buffers_);
        alloc_task->Wait<TASK_YIELD_CO>(task);
//        HILOG(kInfo, "(node {}) Placing {}/{} bytes in target {} of bw {}",
//              HRUN_CLIENT->node_id_,
//              alloc_put_task->alloc_size_, blob.size(),
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
  }

  static void WriteToBlob(Task *task,
                          BlobInfo &blob_info,
                          Blob &blob,
                          size_t real_blob_off,
                          ssize_t bkt_size_diff,
                          blob_mdm::Server *blob_mdm,
                          bitfield32_t &flags) {
    // Place blob in buffers
    std::vector<LPointer<bdev::WriteTask>> write_tasks;
    write_tasks.reserve(blob_info.buffers_.size());
    size_t blob_off = real_blob_off, buf_off = 0;
    size_t buf_left = 0, buf_right = 0;
    size_t blob_right = real_blob_off + blob.size();
    char *blob_buf = blob.data();
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
            target.AsyncWrite(task->task_node_ + 1,
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
    if (flags.Any(HERMES_SHOULD_STAGE)) {
      blob_mdm->stager_mdm_.AsyncUpdateSize(task->task_node_ + 1,
                                            blob_info.tag_id_,
                                            blob_info.name_,
                                            real_blob_off,
                                            blob.size(), 0);
    } else {
      blob_mdm->bkt_mdm_.AsyncUpdateSize(task->task_node_ + 1,
                                         blob_info.tag_id_,
                                         bkt_size_diff,
                                         UpdateSizeMode::kAdd);
    }
    if (flags.Any(HERMES_BLOB_DID_CREATE)) {
      blob_mdm->bkt_mdm_.AsyncTagAddBlob(task->task_node_ + 1,
                                         blob_info.tag_id_,
                                         blob_info.blob_id_);
    }
    if (flags.Any(HERMES_HAS_DERIVED)) {
      blob_mdm->op_mdm_.AsyncRegisterData(task->task_node_ + 1,
                                          blob_info.tag_id_,
                                          blob_info.name_.str(),
                                          blob_info.blob_id_,
                                          real_blob_off,
                                          blob.size());
    }
  }

  static size_t ReadFromBlob(Task *task,
                             BlobInfo &blob_info,
                             Blob &blob,
                             size_t real_blob_off,
                             blob_mdm::Server *blob_mdm,
                             bitfield32_t &flags) {
    // Read blob from buffers
    std::vector<bdev::ReadTask*> read_tasks;
    read_tasks.reserve(blob_info.buffers_.size());
    HILOG(kDebug, "Getting blob {} of size {} starting at offset {} (total_blob_size={}, buffers={})",
          blob_info.blob_id_, blob.size(), real_blob_off, blob_info.blob_size_, blob_info.buffers_.size());
    size_t blob_off = real_blob_off;
    size_t buf_left = 0, buf_right = 0;
    size_t buf_off = 0;
    size_t blob_right = real_blob_off + blob.size();
    bool found_left = false;
    char *blob_buf = blob.data();
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
    return buf_off;
  }
};

}  // namespace hermes

#endif  // HERMES_INCLUDE_HERMES_TRAITS_MOD_IN_PLACE_BLOB_H_
