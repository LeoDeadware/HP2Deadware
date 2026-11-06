// Unit test of x86recomp's MMX lane helpers (runtime/mmx.h, from
// patches/x86recomp-xbe.patch) against values worked out from the Intel SDM
// definitions. No x86 host is needed (Apple Silicon has no MMX to compare
// against), so each case is chosen to hit an edge: wrap vs saturation,
// signed vs unsigned lanes, shift counts at and past the lane width.
//
//   out/src/runtime/hp2_mmx_test

#include <cstdio>

#include "runtime/mmx.h"

namespace {
int g_fail = 0, g_n = 0;
void Check(const char* what, uint64_t got, uint64_t want) {
  g_n++;
  if (got != want) {
    g_fail++;
    std::printf("FAIL %-28s got %016llX want %016llX\n", what, (unsigned long long)got,
                (unsigned long long)want);
  }
}
}  // namespace

int main() {
  using namespace recomp::mmx;
  // Wrapping adds/subs.
  Check("paddb wrap", Add<uint8_t>(0x00FF7F80'01020304ull, 0x0001'0180'FFFF'FFFFull),
        0x0000'8000'0001'0203ull);  // byte 6: FF+01 wraps
  Check("paddw", Add<uint16_t>(0xFFFF'0001'8000'7FFFull, 0x0001'0001'8000'0001ull),
        0x0000'0002'0000'8000ull);
  Check("paddd", Add<uint32_t>(0xFFFFFFFF'00000001ull, 0x00000001'00000002ull),
        0x00000000'00000003ull);
  Check("psubb", Sub<uint8_t>(0x0000000000000000ull, 0x0000000000000001ull),
        0x00000000000000FFull);
  // Saturation: signed bytes 0x7F+1 -> 0x7F, 0x80-1 -> 0x80; unsigned 0xFF+1 -> 0xFF, 0-1 -> 0.
  Check("paddsb", AddSat<int8_t>(0x0000'0000'0000'807Full, 0x0000'0000'0000'FF01ull),
        0x0000'0000'0000'807Full);
  Check("paddusb", AddSat<uint8_t>(0x00000000000000FFull, 0x0000000000000001ull),
        0x00000000000000FFull);
  Check("psubusb", SubSat<uint8_t>(0x0000000000000000ull, 0x0000000000000001ull), 0);
  Check("psubsw", SubSat<int16_t>(0x0000'0000'0000'8000ull, 0x0000'0000'0000'0001ull),
        0x0000'0000'0000'8000ull);
  Check("paddsw", AddSat<int16_t>(0x0000'0000'0000'7FFFull, 0x0000'0000'0000'0001ull),
        0x0000'0000'0000'7FFFull);
  // Multiplies: 0x7FFF*2 = 0xFFFE (lo), hi 0; -2*3 = -6 (lo FFFA, hi FFFF); unsigned hi of FFFF*FFFF = FFFE.
  Check("pmullw", MulLo16(0x0000'0000'FFFE'7FFFull, 0x0000'0000'0003'0002ull),
        0x0000'0000'FFFA'FFFEull);
  Check("pmulhw", MulHi16(0x0000'0000'FFFE'7FFFull, 0x0000'0000'0003'0002ull),
        0x0000'0000'FFFF'0000ull);
  Check("pmulhuw", MulHiU16(0x000000000000FFFFull, 0x000000000000FFFFull), 0xFFFEull);
  // pmaddwd: [1,2,3,4]*[5,6,7,8] -> [1*5+2*6, 3*7+4*8] = [17, 53]; 0x8000*0x8000*2 wraps to 0x80000000.
  Check("pmaddwd", MAddWD(0x0004'0003'0002'0001ull, 0x0008'0007'0006'0005ull),
        0x00000035'00000011ull);
  Check("pmaddwd wrap", MAddWD(0x0000'0000'8000'8000ull, 0x0000'0000'8000'8000ull),
        0x00000000'80000000ull);
  // Compares.
  Check("pcmpeqw", CmpEq<uint16_t>(0x1111'2222'3333'4444ull, 0x1111'0000'3333'0000ull),
        0xFFFF'0000'FFFF'0000ull);
  Check("pcmpgtb signed", CmpGt<int8_t>(0x00000000'0000017Full, 0x00000000'0000FF80ull),
        0x00000000'0000FFFFull);
  Check("pcmpgtd", CmpGt<int32_t>(0x00000001'80000000ull, 0x00000000'00000000ull),
        0xFFFFFFFF'00000000ull);
  // Shifts: by count, at the width (0 / sign fill) and past it.
  Check("psllw 4", Shl<uint16_t>(0x0000'0000'8001'0F0Full, 4), 0x0000'0000'0010'F0F0ull);
  Check("psllw 16", Shl<uint16_t>(0xFFFF'FFFF'FFFF'FFFFull, 16), 0);
  Check("psrlq 64", Shr<uint64_t>(0xFFFFFFFFFFFFFFFFull, 64), 0);
  Check("psrld 1", Shr<uint32_t>(0x80000000'00000003ull, 1), 0x40000000'00000001ull);
  Check("psraw 1", Sar<int16_t>(0x0000'0000'8000'7FFEull, 1), 0x0000'0000'C000'3FFFull);
  Check("psraw 99", Sar<int16_t>(0x0000'0000'8000'7FFFull, 99), 0x0000'0000'FFFF'0000ull);
  Check("psrad 31", Sar<int32_t>(0x80000000'7FFFFFFFull, 31), 0xFFFFFFFF'00000000ull);
  // Unpack: low bytes of a interleaved with low bytes of b (a first).
  Check("punpcklbw", Unpack<uint8_t>(0x00000000'44332211ull, 0x00000000'DDCCBBAAull, false),
        0xDD44CC33'BB22AA11ull);
  Check("punpckhwd", Unpack<uint16_t>(0x4444'3333'0000'0000ull, 0xDDDD'CCCC'0000'0000ull, true),
        0xDDDD'4444'CCCC'3333ull);
  Check("punpckldq", Unpack<uint32_t>(0x00000000'11111111ull, 0x00000000'22222222ull, false),
        0x22222222'11111111ull);
  // Packs: words 0x0100 -> 0xFF (us), 0xFFFF(-1) -> 0x00 (us), 0x0080 -> 0x7F (ss).
  Check("packuswb", Pack<int16_t, uint8_t>(0x0001'FFFF'0100'007Full, 0x0000'0000'0000'0000ull),
        0x00000000'0100FF7Full);
  Check("packsswb", Pack<int16_t, int8_t>(0x0000'0000'FF7F'0080ull, 0x0000'0000'0000'8000ull),
        0x00000080'0000807Full);
  Check("packssdw", Pack<int32_t, int16_t>(0xFFFF0000'00010000ull, 0x00000001'FFFFFFFFull),
        0x0001'FFFF'8000'7FFFull);
  // Others.
  Check("pavgb", Avg<uint8_t>(0x00000000'0000FF01ull, 0x00000000'0000FF02ull),
        0x00000000'0000FF02ull);
  Check("pshufw 1B", ShufW(0x4444'3333'2222'1111ull, 0x1B), 0x1111'2222'3333'4444ull);
  Check("pmovmskb", MovMskB(0x80000080'00FF7F01ull), 0x94ull);
  Check("psadbw", SadBW(0x00000000'000000FFull, 0x00000000'00000001ull), 0xFEull);
  Check("pminsw", Min<int16_t>(0x0000'0000'8000'0001ull, 0x0000'0000'0000'0002ull),
        0x0000'0000'8000'0001ull);
  Check("pmaxub", Max<uint8_t>(0x00000000'000080FFull, 0x00000000'00007F01ull),
        0x00000000'000080FFull);

  // x87 aliasing: MMX state, register writes, EMMS.
  recomp::Ctx c{};
  c.fpu.top = 5;
  c.fpu.empty = 0xFF;
  Enter(c);
  Set(c, 3, 0x1122334455667788ull);
  Check("Enter top", c.fpu.top, 0);
  Check("Enter tags valid", c.fpu.empty, 0);
  Check("mm3 = R3.sig", c.fpu.r[3].sig, 0x1122334455667788ull);
  Check("mm write exp", c.fpu.r[3].se, 0xFFFF);
  Emms(c);
  Check("emms empty", c.fpu.empty, 0xFF);

  std::printf("%s: %d/%d MMX checks pass\n", g_fail ? "FAIL" : "PASS", g_n - g_fail, g_n);
  return g_fail ? 1 : 0;
}
