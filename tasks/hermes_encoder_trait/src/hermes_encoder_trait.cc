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
#include "hermes_encoder_trait/hermes_encoder_trait.h"
#include "hermes_blob_mdm/hermes_blob_mdm_tasks.h"
#include "hermes_blob_mdm/hermes_blob_mdm_server.h"
#include "hermes/traits/mod_in_place_blob.h"
#include "hermes/traits/encoded_blob.h"
#include "hermes/dpe/dpe_factory.h"
#include "hermes_shm/util/compress/compress_factory.h"

namespace hermes::traits::encoder_trait {

class Encoder {
 public:
  static void Encode(std::vector<BufferInfo> &buffers,
                     size_t real_blob_off,
                     hapi::Blob &encoded_blob,
                     hapi::Blob &decoded_blob) {
    // Encode the blob
    hshm::Bzip2 lib;
    size_t cmpr_size = encoded_blob.size();
    lib.Compress(encoded_blob.data(), cmpr_size,
                 decoded_blob.data(), decoded_blob.size());
    encoded_blob.resize(cmpr_size);
  }

  static void Decode(std::vector<BufferInfo> &buffers,
                     size_t real_blob_off,
                     hapi::Blob &decoded_blob,
                     hapi::Blob &encoded_blob) {
    // Decode the blob
    hshm::Bzip2 lib;
    size_t raw_size = decoded_blob.size();
    lib.Decompress(decoded_blob.data(), raw_size,
                   encoded_blob.data(), encoded_blob.size());
    decoded_blob.resize(raw_size);
  }
};

class Server : public TaskLib {
 public:
  Server() = default;

  /** Construct hermes_encoder_trait */
  void Construct(ConstructTask *task, RunContext &rctx) {
    task->SetModuleComplete();
  }
  void MonitorConstruct(u32 mode, ConstructTask *task, RunContext &rctx) {
  }

  /** Destroy hermes_encoder_trait */
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

    // Subtract bkt size if replacing the blob
    hapi::Blob blob(HRUN_CLIENT->GetDataPointer(put_task->data_),
                    put_task->data_size_);
    Encoder encoder;
    EncodeBlob<Encoder>::Encode(task,
                                blob_info,
                                blob,
                                put_task->blob_off_,
                                put_task->flags_,
                                put_task->score_,
                                blob_mdm,
                                encoder);
    task->SetModuleComplete();
  }
  void MonitorEncode(u32 mode, EncodeTask *task, RunContext &rctx) {
  }

  /** A decoding method */
  void Decode(DecodeTask *task, RunContext &rctx) {
    BlobInfo &blob_info = *task->blob_info_;
    blob_mdm::GetBlobTask *get_task = task->get_blob_task_;
    blob_mdm::Server *blob_mdm = task->blob_mdm_state_;

    hapi::Blob blob(HRUN_CLIENT->GetDataPointer(get_task->data_),
                    blob_info.logical_blob_size_);
    Encoder encoder;
    EncodeBlob<Encoder>::Decode(task,
                                blob_info,
                                blob,
                                get_task->blob_off_,
                                get_task->flags_,
                                blob_mdm,
                                encoder);
    task->SetModuleComplete();
  }
  void MonitorDecode(u32 mode, DecodeTask *task, RunContext &rctx) {
  }
 public:
#include "hermes_encoder_trait/hermes_encoder_trait_lib_exec.h"
};

}  // namespace hermes::traits::encoder_trait

HRUN_TASK_CC(hermes::traits::encoder_trait::Server, "hermes_encoder_trait");
