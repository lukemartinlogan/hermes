//
// Created by lukemartinlogan on 2/13/24.
//

#ifndef HERMES_INCLUDE_HERMES_TRAITS_ENCODED_BLOB_H_
#define HERMES_INCLUDE_HERMES_TRAITS_ENCODED_BLOB_H_

#include "mod_in_place_blob.h"

namespace hermes {

template <typename EncoderT>
class EncodeBlob {
 public:
  static void Encode(Task *task,
                     BlobInfo &blob_info,
                     hapi::Blob &blob,
                     size_t logical_blob_off,
                     bitfield32_t &flags,
                     float score,
                     blob_mdm::Server *blob_mdm,
                     EncoderT &encoder) {
    ssize_t bkt_size_diff = 0;

    // Subtract bkt size if replacing the blob
    if (flags.Any(HERMES_BLOB_REPLACE)) {
      bkt_size_diff -= blob_info.logical_blob_size_;
    }
    size_t size_diff;
    blob_info.logical_blob_size_ = ModInPlaceBlob::GetNewLogicalSize(
        blob_info,
        logical_blob_off,
        blob.size(),
        size_diff);

    // Create a blob that can hold encoded data
    hapi::Blob encoded_blob(HRUN_CLIENT->rdata_alloc_,
                            blob_info.logical_blob_size_);

    // Use DPE
    std::vector<PlacementSchema> schema_vec;
    if (size_diff > 0) {
      Context ctx;
      auto *dpe = DpeFactory::Get(ctx.dpe_);
      ctx.blob_score_ = score;
      dpe->Placement({size_diff}, blob_mdm->targets_, ctx, schema_vec);
    }

    // Allocate blob buffers
    ModInPlaceBlob::AllocateBlobBuffers(task,
                                        blob_info,
                                        schema_vec,
                                        blob_mdm);

    if (blob_info.blob_size_ > 0) {
      // Read blob
      ModInPlaceBlob::ReadFromBlob(task,
                                   blob_info,
                                   encoded_blob,
                                   0,
                                   blob_mdm,
                                   blob_info.flags_);

      // Decode & modify the blob
      hapi::Blob decoded_blob(HRUN_CLIENT->rdata_alloc_,
                              blob_info.logical_blob_size_);
      encoder.Decode(blob_info.buffers_, 0, decoded_blob, encoded_blob);
      memcpy(decoded_blob.data() + logical_blob_off,
             blob.data(), blob.size());
      encoder.Encode(blob_info.buffers_, 0, encoded_blob, decoded_blob);
    } else if (logical_blob_off > 0) {
      // Encode the blob
      hapi::Blob decoded_blob(HRUN_CLIENT->rdata_alloc_,
                              blob_info.logical_blob_size_);
      memcpy(decoded_blob.data() + logical_blob_off,
             blob.data(), blob.size());
      encoder.Encode(blob_info.buffers_, 0, encoded_blob, decoded_blob);
    } else {
      // Encode the blob
      encoder.Encode(blob_info.buffers_, 0, encoded_blob, blob);
    }

    // Write the encoded blob
    blob_info.blob_size_ = encoded_blob.size();
    ModInPlaceBlob::WriteToBlob(task,
                                blob_info,
                                encoded_blob,
                                0,
                                bkt_size_diff,
                                blob_mdm,
                                flags);
  }

  static void Decode(Task *task,
                     BlobInfo &blob_info,
                     hapi::Blob &blob,
                     size_t logical_blob_off,
                     bitfield32_t &flags,
                     blob_mdm::Server *blob_mdm,
                     EncoderT &encoder) {
    // Create a blob that can hold encoded data
    hapi::Blob encoded_blob(HRUN_CLIENT->rdata_alloc_,
                            blob_info.logical_blob_size_);

    // Read the entire blob
    ModInPlaceBlob::ReadFromBlob(task,
                                 blob_info,
                                 encoded_blob,
                                 0,
                                 blob_mdm,
                                 flags);

    // Read the part of the blob that matters
    if (logical_blob_off > 0 || blob.size() < blob_info.logical_blob_size_) {
      hapi::Blob decoded_blob(HRUN_CLIENT->rdata_alloc_,
                              blob_info.logical_blob_size_);
      encoder.Decode(blob_info.buffers_, 0, decoded_blob, encoded_blob);
      memcpy(blob.data(),
             decoded_blob.data() + logical_blob_off,
             blob.size());
    } else {
      encoder.Decode(blob_info.buffers_, 0, blob, encoded_blob);
    }
  }
};

}  // hermes

#endif  // HERMES_INCLUDE_HERMES_TRAITS_ENCODED_BLOB_H_
