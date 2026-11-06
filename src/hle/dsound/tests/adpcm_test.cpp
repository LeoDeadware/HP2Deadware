// Host test: hp2ds::adpcm_decode_block vs ffmpeg's adpcm_ima_xbox, sample-exact.
//
//   tests/adpcm_test.sh   (extracts warner.xmv's audio packets + ffmpeg's decode)
//   adpcm_test <packets.bin> <ffmpeg.s16le> <channels>

#include <cmath>
#include <cstdio>
#include <vector>

#include "hle/dsound/adpcm.h"

static std::vector<unsigned char> slurp(const char* p) {
  std::vector<unsigned char> v;
  if (FILE* f = fopen(p, "rb")) {
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f);
  }
  return v;
}

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  auto adpcm = slurp(argv[1]);
  auto ref = slurp(argv[2]);
  int ch = atoi(argv[3]);
  size_t block = 36 * ch, blocks = adpcm.size() / block;
  const int16_t* r = reinterpret_cast<const int16_t*>(ref.data());
  size_t nref = ref.size() / 2, n = 0, bad = 0;
  std::vector<float> out(64 * ch);
  for (size_t b = 0; b < blocks; ++b) {
    hp2ds::adpcm_decode_block(&adpcm[b * block], ch, out.data());
    for (int i = 0; i < 64 * ch && n < nref; ++i, ++n) {
      int s = int(std::lrint(out[i] * 32768.f));
      if (s != r[n] && bad++ < 5)
        printf("mismatch at sample %zu (block %zu): ours %d, ffmpeg %d\n", n, b, s, r[n]);
    }
  }
  printf("adpcm_test: %zu blocks, %zu samples compared (ffmpeg %zu), %zu mismatches -> %s\n", blocks,
         n, nref, bad, bad == 0 && n > 0 ? "PASS" : "FAIL");
  return bad == 0 && n > 0 ? 0 : 1;
}
