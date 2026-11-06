// Xbox ADPCM decoder (owner: Audio). No dependencies: also built by the host
// test (tests/adpcm_test.cpp) that checks it sample-exact against ffmpeg.

#include "hle/dsound/adpcm.h"

namespace hp2ds {

namespace {

// ---- Xbox ADPCM (IMA ADPCM, 36-byte blocks per channel, 64 samples) ----------

const int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

struct ImaState {
  int pred, index;
  int step(int nibble) {
    // Exact product form (ffmpeg's IMA WAV/Xbox expansion), not the shift sum.
    int diff = ((2 * (nibble & 7) + 1) * kStepTable[index]) >> 3;
    pred += (nibble & 8) ? -diff : diff;
    pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
    index += kIndexTable[nibble];
    index = index < 0 ? 0 : index > 88 ? 88 : index;
    return pred;
  }
};

}  // namespace

// One block: per channel a 4-byte header {int16 sample, u8 step index, u8 0},
// then 8 groups of 4 bytes per channel (8 nibbles each, low nibble first).
// The header sample is output sample 0; nibbles 0..62 give samples 1..63 and
// the 64th nibble is unused (as ffmpeg's adpcm_ima_xbox, checked sample-exact
// by tests/adpcm_test.sh).
void adpcm_decode_block(const uint8_t* b, int ch, float* out) {
  ImaState st[8];
  for (int c = 0; c < ch; ++c) {
    st[c].pred = int16_t(b[4 * c] | b[4 * c + 1] << 8);
    st[c].index = b[4 * c + 2] > 88 ? 88 : b[4 * c + 2];
    out[c] = st[c].pred / 32768.f;
  }
  const uint8_t* d = b + 4 * ch;
  for (int g = 0; g < 8; ++g) {
    for (int c = 0; c < ch; ++c) {
      for (int k = 0; k < 4; ++k) {
        uint8_t byte = *d++;
        int s = 1 + g * 8 + k * 2;
        out[s * ch + c] = st[c].step(byte & 15) / 32768.f;
        if (s + 1 < 64) out[(s + 1) * ch + c] = st[c].step(byte >> 4) / 32768.f;
      }
    }
  }
}

}  // namespace hp2ds
