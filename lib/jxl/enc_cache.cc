// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lib/jxl/enc_cache.h"

#include <stddef.h>
#include <stdint.h>

#include <type_traits>

#include "lib/jxl/ac_strategy.h"
#include "lib/jxl/aux_out.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/padded_bytes.h"
#include "lib/jxl/base/profiler.h"
#include "lib/jxl/base/span.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/common.h"
#include "lib/jxl/compressed_dc.h"
#include "lib/jxl/dct_scales.h"
#include "lib/jxl/dct_util.h"
#include "lib/jxl/dec_frame.h"
#include "lib/jxl/enc_frame.h"
#include "lib/jxl/enc_group.h"
#include "lib/jxl/enc_modular.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_bundle.h"
#include "lib/jxl/image_ops.h"
#include "lib/jxl/passes_state.h"
#include "lib/jxl/quantizer.h"

namespace jxl {

void InitializePassesEncoder(const Image3F& opsin, ThreadPool* pool,
                             PassesEncoderState* enc_state,
                             ModularFrameEncoder* modular_frame_encoder,
                             AuxOut* aux_out) {
  PROFILER_FUNC;

  PassesSharedState& JXL_RESTRICT shared = enc_state->shared;

  enc_state->histogram_idx.resize(shared.frame_dim.num_groups);

  if (shared.frame_header.color_transform == ColorTransform::kXYB) {
    enc_state->x_qm_multiplier =
        std::pow(2.0f, 0.5f * shared.frame_header.x_qm_scale - 0.5f);
  } else {
    enc_state->x_qm_multiplier = 1.0f;  // don't scale X quantization in YCbCr
  }

  if (enc_state->coeffs.size() != shared.frame_header.passes.num_passes) {
    enc_state->coeffs.resize(shared.frame_header.passes.num_passes);
    for (size_t i = 0; i < shared.frame_header.passes.num_passes; i++) {
      static_assert(std::is_same<float, ac_qcoeff_t>::value,
                    "float != ac_coeff_t");
      // Allocate enough coefficients for each group on every row.
      enc_state->coeffs[i] =
          ACImage3(kGroupDim * kGroupDim, shared.frame_dim.num_groups);
    }
  }

  Image3F dc(shared.frame_dim.xsize_blocks, shared.frame_dim.ysize_blocks);
  RunOnPool(
      pool, 0, shared.frame_dim.num_groups, ThreadPool::SkipInit(),
      [&](size_t group_idx, size_t _) {
        ComputeCoefficients(group_idx, enc_state, opsin, &dc);
      },
      "Compute coeffs");

  if (shared.frame_header.flags & FrameHeader::kUseDcFrame) {
    CompressParams cparams = enc_state->cparams;
    // Guess a distance that produces good initial results.
    cparams.butteraugli_distance =
        std::max(kMinButteraugliDistance,
                 enc_state->cparams.butteraugli_distance * 0.1f);
    cparams.dots = Override::kOff;
    cparams.patches = Override::kOff;
    cparams.gaborish = Override::kOff;
    cparams.epf = 0;
    cparams.max_error_mode = true;
    for (size_t c = 0; c < 3; c++) {
      cparams.max_error[c] = shared.quantizer.MulDC()[c];
    }
    FrameDimensions frame_dim;
    frame_dim.Set(
        enc_state->shared.frame_dim.xsize << (3 * shared.frame_header.dc_level),
        enc_state->shared.frame_dim.ysize << (3 * shared.frame_header.dc_level),
        shared.frame_header.group_size_shift,
        shared.frame_header.chroma_subsampling.MaxHShift(),
        shared.frame_header.chroma_subsampling.MaxVShift());
    cparams.progressive_dc--;
    // Use kVarDCT in max_error_mode for intermediate progressive DC,
    // and kModular for the smallest DC (first in the bitstream)
    if (cparams.progressive_dc == 0) {
      cparams.modular_mode = true;
      cparams.quality_pair.first = cparams.quality_pair.second =
          99.f - enc_state->cparams.butteraugli_distance * 0.2f;
    }
    ImageBundle ib(shared.metadata);
    // This is a lie - dc is in XYB
    // (but EncodeFrame will skip RGB->XYB conversion anyway)
    ib.SetFromImage(std::move(dc), ColorEncoding::LinearSRGB());
    PassesEncoderState state;
    enc_state->special_frames.emplace_back();
    FrameInfo dc_frame_info;
    dc_frame_info.frame_type = FrameType::kDCFrame;
    dc_frame_info.dc_level = shared.frame_header.dc_level + 1;
    dc_frame_info.ib_needs_color_transform = false;
    dc_frame_info.save_before_color_transform = true;  // Implicitly true
    JXL_CHECK(EncodeFrame(cparams, dc_frame_info, shared.metadata, ib, &state,
                          pool, &enc_state->special_frames.back(), nullptr));
    const Span<const uint8_t> encoded =
        enc_state->special_frames.back().GetSpan();
    BitReader br(encoded);
    ImageBundle decoded(shared.metadata);
    PassesDecoderState dec_state;
    JXL_CHECK(DecodeFrame({}, &dec_state, pool, &br, nullptr, &decoded));
    shared.dc_storage =
        CopyImage(dec_state.shared->dc_frames[shared.frame_header.dc_level]);
    ZeroFillImage(&shared.quant_dc);
    JXL_CHECK(br.Close());
  } else {
    auto compute_dc_coeffs = [&](int group_index, int /* thread */) {
      modular_frame_encoder->AddVarDCTDC(
          dc, group_index,
          enc_state->cparams.butteraugli_distance >= 2.0f &&
              enc_state->cparams.speed_tier != SpeedTier::kFalcon,
          enc_state);
    };
    RunOnPool(pool, 0, shared.frame_dim.num_dc_groups, ThreadPool::SkipInit(),
              compute_dc_coeffs, "Compute DC coeffs");
    // TODO(veluca): this is only useful in tests and if inspection is enabled.
    if (!(shared.frame_header.flags & FrameHeader::kSkipAdaptiveDCSmoothing)) {
      AdaptiveDCSmoothing(shared.quantizer.MulDC(), &shared.dc_storage, pool);
    }
  }
  auto compute_ac_meta = [&](int group_index, int /* thread */) {
    modular_frame_encoder->AddACMetadata(group_index, /*jpeg_transcode=*/false,
                                         enc_state);
  };
  RunOnPool(pool, 0, shared.frame_dim.num_dc_groups, ThreadPool::SkipInit(),
            compute_ac_meta, "Compute AC Metadata");

  if (aux_out != nullptr) {
    aux_out->InspectImage3F("compressed_image:InitializeFrameEncCache:dc_dec",
                            shared.dc_storage);
  }
}

void EncCache::InitOnce() {
  PROFILER_FUNC;

  if (num_nzeroes.xsize() == 0) {
    num_nzeroes = Image3I(kGroupDimInBlocks, kGroupDimInBlocks);
  }
}

}  // namespace jxl