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
#include "hermes_blob_mdm/hermes_blob_mdm_server.h"
#include "hermes/traits/mod_in_place_blob.h"

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
    ssize_t bkt_size_diff = 0;

    // Subtract bkt size if replacing the blob
    if (put_task->flags_.Any(HERMES_BLOB_REPLACE)) {
      bkt_size_diff -= blob_info.blob_size_;
    }

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

    // Write blob to buffers
    hapi::Blob blob(HRUN_CLIENT->GetDataPointer(put_task->data_),
                    put_task->data_size_);
    ModInPlaceBlob::AllocateBlobBuffers(task,
                                        blob_info,
                                        schema_vec,
                                        blob_mdm);
    ModInPlaceBlob::WriteToBlob(task,
                                blob_info,
                                blob,
                                put_task->blob_off_,
                                bkt_size_diff,
                                blob_mdm,
                                blob_info.flags_);
    task->SetModuleComplete();
  }
  void MonitorEncode(u32 mode, EncodeTask *task, RunContext &rctx) {
  }

  /** A decoding method */
  void Decode(DecodeTask *task, RunContext &rctx) {
    BlobInfo &blob_info = *task->blob_info_;
    GetBlobTask *get_task = task->get_blob_task_;
    blob_mdm::Server *blob_mdm = task->blob_mdm_state_;

    // Read blob from buffers
    hapi::Blob blob(HRUN_CLIENT->GetDataPointer(get_task->data_),
                    get_task->data_size_);
    get_task->data_size_ = ModInPlaceBlob::ReadFromBlob(task,
                                                        blob_info,
                                                        blob,
                                                        get_task->blob_off_,
                                                        blob_mdm,
                                                        blob_info.flags_);
    task->SetModuleComplete();
  }
  void MonitorDecode(u32 mode, DecodeTask *task, RunContext &rctx) {
  }

 public:
#include "hermes_default_trait/hermes_default_trait_lib_exec.h"
};

}  // namespace hermes::traits::default_trait

HRUN_TASK_CC(hermes::traits::default_trait::Server, "hermes_default_trait");
