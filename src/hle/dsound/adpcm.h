#pragma once
// Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM 0x69): IMA ADPCM, 36-byte blocks per
// channel, 64 samples per channel per block.

#include <cstdint>

namespace hp2ds {

// Decodes one block of `ch` channels (36*ch bytes) into 64*ch interleaved
// samples scaled to [-1, 1).
void adpcm_decode_block(const uint8_t* block, int ch, float* out);

}  // namespace hp2ds
