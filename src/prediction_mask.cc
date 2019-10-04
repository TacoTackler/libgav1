// Copyright 2019 The libgav1 Authors
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

#include "src/prediction_mask.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "src/utils/array_2d.h"
#include "src/utils/bit_mask_set.h"
#include "src/utils/common.h"
#include "src/utils/constants.h"
#include "src/utils/logging.h"
#include "src/utils/memory.h"

namespace libgav1 {
namespace {

constexpr int kWedgeDirectionTypes = 16;

enum kWedgeDirection : uint8_t {
  kWedgeHorizontal,
  kWedgeVertical,
  kWedgeOblique27,
  kWedgeOblique63,
  kWedgeOblique117,
  kWedgeOblique153,
};

constexpr uint8_t kWedgeCodebook[3][16][3] = {{{kWedgeOblique27, 4, 4},
                                               {kWedgeOblique63, 4, 4},
                                               {kWedgeOblique117, 4, 4},
                                               {kWedgeOblique153, 4, 4},
                                               {kWedgeHorizontal, 4, 2},
                                               {kWedgeHorizontal, 4, 4},
                                               {kWedgeHorizontal, 4, 6},
                                               {kWedgeVertical, 4, 4},
                                               {kWedgeOblique27, 4, 2},
                                               {kWedgeOblique27, 4, 6},
                                               {kWedgeOblique153, 4, 2},
                                               {kWedgeOblique153, 4, 6},
                                               {kWedgeOblique63, 2, 4},
                                               {kWedgeOblique63, 6, 4},
                                               {kWedgeOblique117, 2, 4},
                                               {kWedgeOblique117, 6, 4}},
                                              {{kWedgeOblique27, 4, 4},
                                               {kWedgeOblique63, 4, 4},
                                               {kWedgeOblique117, 4, 4},
                                               {kWedgeOblique153, 4, 4},
                                               {kWedgeVertical, 2, 4},
                                               {kWedgeVertical, 4, 4},
                                               {kWedgeVertical, 6, 4},
                                               {kWedgeHorizontal, 4, 4},
                                               {kWedgeOblique27, 4, 2},
                                               {kWedgeOblique27, 4, 6},
                                               {kWedgeOblique153, 4, 2},
                                               {kWedgeOblique153, 4, 6},
                                               {kWedgeOblique63, 2, 4},
                                               {kWedgeOblique63, 6, 4},
                                               {kWedgeOblique117, 2, 4},
                                               {kWedgeOblique117, 6, 4}},
                                              {{kWedgeOblique27, 4, 4},
                                               {kWedgeOblique63, 4, 4},
                                               {kWedgeOblique117, 4, 4},
                                               {kWedgeOblique153, 4, 4},
                                               {kWedgeHorizontal, 4, 2},
                                               {kWedgeHorizontal, 4, 6},
                                               {kWedgeVertical, 2, 4},
                                               {kWedgeVertical, 6, 4},
                                               {kWedgeOblique27, 4, 2},
                                               {kWedgeOblique27, 4, 6},
                                               {kWedgeOblique153, 4, 2},
                                               {kWedgeOblique153, 4, 6},
                                               {kWedgeOblique63, 2, 4},
                                               {kWedgeOblique63, 6, 4},
                                               {kWedgeOblique117, 2, 4},
                                               {kWedgeOblique117, 6, 4}}};

constexpr BitMaskSet kWedgeFlipSignMasks[9] = {
    BitMaskSet(0xBBFF),  // kBlock8x8
    BitMaskSet(0xBBEF),  // kBlock8x16
    BitMaskSet(0xBAEF),  // kBlock8x32
    BitMaskSet(0xBBEF),  // kBlock16x8
    BitMaskSet(0xBBFF),  // kBlock16x16
    BitMaskSet(0xBBEF),  // kBlock16x32
    BitMaskSet(0xABEF),  // kBlock32x8
    BitMaskSet(0xBBEF),  // kBlock32x16
    BitMaskSet(0xBBFF)   // kBlock32x32
};

// This table (and the one below) contains a few leading zeros and trailing 64s
// to avoid some additional memcpys where it is actually used.
constexpr uint8_t kWedgeMasterObliqueOdd[kWedgeMaskMasterSize * 3 / 2] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  6,  18, 37,
    53, 60, 63, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64};

constexpr uint8_t kWedgeMasterObliqueEven[kWedgeMaskMasterSize * 3 / 2] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  4,  11, 27,
    46, 58, 62, 63, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64};

constexpr uint8_t kWedgeMasterVertical[kWedgeMaskMasterSize] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  2,  7,  21,
    43, 57, 62, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64};

int BlockShape(BlockSize block_size) {
  const int width = kNum4x4BlocksWide[block_size];
  const int height = kNum4x4BlocksHigh[block_size];
  if (height > width) return 0;
  if (height < width) return 1;
  return 2;
}

uint8_t GetWedgeDirection(BlockSize block_size, int index) {
  return kWedgeCodebook[BlockShape(block_size)][index][0];
}

uint8_t GetWedgeOffsetX(BlockSize block_size, int index) {
  return kWedgeCodebook[BlockShape(block_size)][index][1];
}

uint8_t GetWedgeOffsetY(BlockSize block_size, int index) {
  return kWedgeCodebook[BlockShape(block_size)][index][2];
}

}  // namespace

bool GenerateWedgeMask(uint8_t* const wedge_master_mask_data,
                       WedgeMaskArray* const wedge_masks) {
  // Generate master masks.
  Array2DView<uint8_t> master_mask(6, kMaxMaskBlockSize,
                                   wedge_master_mask_data);
  uint8_t* master_mask_row = master_mask[kWedgeVertical];
  const int stride = kWedgeMaskMasterSize;
  for (int y = 0; y < kWedgeMaskMasterSize; ++y) {
    memcpy(master_mask_row, kWedgeMasterVertical, kWedgeMaskMasterSize);
    master_mask_row += stride;
  }

  master_mask_row = master_mask[kWedgeOblique63];
  for (int y = 0, shift = 0; y < kWedgeMaskMasterSize; y += 2, ++shift) {
    memcpy(master_mask_row, kWedgeMasterObliqueEven + shift,
           kWedgeMaskMasterSize);
    master_mask_row += stride;
    memcpy(master_mask_row, kWedgeMasterObliqueOdd + shift,
           kWedgeMaskMasterSize);
    master_mask_row += stride;
  }

  uint8_t* const master_mask_horizontal = master_mask[kWedgeHorizontal];
  uint8_t* master_mask_vertical = master_mask[kWedgeVertical];
  uint8_t* const master_mask_oblique_27 = master_mask[kWedgeOblique27];
  uint8_t* master_mask_oblique_63 = master_mask[kWedgeOblique63];
  uint8_t* master_mask_oblique_117 = master_mask[kWedgeOblique117];
  uint8_t* const master_mask_oblique_153 = master_mask[kWedgeOblique153];
  for (int y = 0; y < kWedgeMaskMasterSize; ++y) {
    for (int x = 0; x < kWedgeMaskMasterSize; ++x) {
      const uint8_t mask_value = master_mask_oblique_63[x];
      master_mask_horizontal[x * stride + y] = master_mask_vertical[x];
      master_mask_oblique_27[x * stride + y] = mask_value;
      master_mask_oblique_117[kWedgeMaskMasterSize - 1 - x] = 64 - mask_value;
      master_mask_oblique_153[(kWedgeMaskMasterSize - 1 - x) * stride + y] =
          64 - mask_value;
    }
    master_mask_vertical += stride;
    master_mask_oblique_63 += stride;
    master_mask_oblique_117 += stride;
  }

  // Generate wedge masks.
  int block_size_index = 0;
  for (int size = kBlock8x8; size <= kBlock32x32; ++size) {
    if (!kIsWedgeCompoundModeAllowed.Contains(size)) continue;

    const int width = kBlockWidthPixels[size];
    const int height = kBlockHeightPixels[size];
    assert(width >= 8);
    assert(width <= 32);
    assert(height >= 8);
    assert(height <= 32);

    const auto block_size = static_cast<BlockSize>(size);
    for (int wedge_index = 0; wedge_index < kWedgeDirectionTypes;
         ++wedge_index) {
      const uint8_t direction = GetWedgeDirection(block_size, wedge_index);
      const uint8_t offset_x =
          DivideBy2(kWedgeMaskMasterSize) -
          ((GetWedgeOffsetX(block_size, wedge_index) * width) >> 3);
      const uint8_t offset_y =
          DivideBy2(kWedgeMaskMasterSize) -
          ((GetWedgeOffsetY(block_size, wedge_index) * height) >> 3);

      // Allocate the 2d array.
      for (int flip_sign = 0; flip_sign < 2; ++flip_sign) {
        if (!((*wedge_masks)[block_size_index][flip_sign][wedge_index].Reset(
                height, width, /*zero_initialize=*/false))) {
          LIBGAV1_DLOG(ERROR, "Failed to allocate memory for wedge masks.");
          return false;
        }
      }

      const auto flip_sign = static_cast<uint8_t>(
          kWedgeFlipSignMasks[block_size_index].Contains(wedge_index));
      uint8_t* wedge_masks_row =
          (*wedge_masks)[block_size_index][flip_sign][wedge_index][0];
      uint8_t* wedge_masks_row_flip =
          (*wedge_masks)[block_size_index][1 - flip_sign][wedge_index][0];
      master_mask_row = &master_mask[direction][offset_y * stride + offset_x];
      for (int y = 0; y < height; ++y) {
        memcpy(wedge_masks_row, master_mask_row, width);
        for (int x = 0; x < width; ++x) {
          wedge_masks_row_flip[x] = 64 - wedge_masks_row[x];
        }
        wedge_masks_row += width;
        wedge_masks_row_flip += width;
        master_mask_row += stride;
      }
    }

    block_size_index++;
  }
  return true;
}

void GenerateWeightMask(const uint16_t* prediction_1, const ptrdiff_t stride_1,
                        const uint16_t* prediction_2, const ptrdiff_t stride_2,
                        const bool mask_is_inverse, const int width,
                        const int height, const int bitdepth, uint8_t* mask,
                        const ptrdiff_t mask_stride) {
#if LIBGAV1_MAX_BITDEPTH == 12
  const int inter_post_round_bits = (bitdepth == 12) ? 2 : 4;
#else
  constexpr int inter_post_round_bits = 4;
#endif
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int rounding_bits = bitdepth - 8 + inter_post_round_bits;
      const int difference = RightShiftWithRounding(
          std::abs(prediction_1[x] - prediction_2[x]), rounding_bits);
      const auto mask_value =
          static_cast<uint8_t>(std::min(DivideBy16(difference) + 38, 64));
      mask[x] = mask_is_inverse ? 64 - mask_value : mask_value;
    }
    prediction_1 += stride_1;
    prediction_2 += stride_2;
    mask += mask_stride;
  }
}

}  // namespace libgav1
