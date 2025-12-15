#pragma once

#include <cstdint>

namespace PRNM::Signs {

struct RleImage {
  uint16_t w, h;
  const uint32_t* row_offs;
  const uint8_t* data;
};

void Initialize();
const RleImage* Next();

void decode_rle_row_1bpp(
  const RleImage& img,
  uint16_t y,
  uint8_t* row_data,
  uint16_t row_bytes);

}
