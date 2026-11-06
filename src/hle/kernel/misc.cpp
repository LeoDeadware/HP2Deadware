// HAL/AV, console settings (EEPROM), Rtl helpers, SHA-1, XBE sections,
// SEH, debug output, the driver-model stubs and the remaining data exports.

#include <cstdio>
#include <cstdlib>

#include "hle/kernel/kernel_internal.h"

namespace hp2k {

namespace {

// ---- console settings ----------------------------------------------------------
struct LangSetting {
  const char* key;
  uint32_t language;     // XC_LANGUAGE
  uint32_t game_region;  // XC_FACTORY_GAME_REGION
};
// The game maps (language, region) to its data slot: English + North
// America -> _USA, English elsewhere -> _UK (FUN_00011290). "uk" selects
// the _UK slot, which the Ukrainian translation overrides through mods/uk;
// "uk_text" boots the same way (English movies) and then LanguagePickerInit
// moves the boot index to the hybrid row 2 (_UKT).
constexpr LangSetting kLangs[] = {{"en", 1, 1}, {"uk", 1, 4}, {"uk_text", 1, 4}};

const LangSetting& Lang() {
  std::string key = LanguageKey();  // file.cpp: HP2_LANG / config / hp2.ini
  for (auto& l : kLangs)
    if (key == l.key) return l;
  return kLangs[0];
}

uint32_t EnvU32(const char* name, uint32_t def) {
  const char* v = std::getenv(name);
  return v ? uint32_t(std::strtoul(v, nullptr, 0)) : def;
}

constexpr uint32_t kAvPackStandard = 6;
constexpr uint32_t kAvStandardNtscM = 0x00000100, kAvFlags60Hz = 0x00400000;

}  // namespace

// ExQueryNonVolatileSetting(ValueIndex, PULONG Type, PVOID Value, ValueLength,
//   PULONG ResultLength)
KEXPORT(24, ExQueryNonVolatileSetting, 5) {
  uint32_t idx = Arg(c, 0), ptype = Arg(c, 1), val = Arg(c, 2), len = Arg(c, 3), pres = Arg(c, 4);
  uint32_t v = 0;
  switch (idx) {
    case 0x7: v = Lang().language; break;                         // XC_LANGUAGE
    case 0x8: v = EnvU32("HP2_VIDEO_FLAGS", 0) << 16; break;       // XC_VIDEO_FLAGS
    case 0x9: v = 0; break;                                        // XC_AUDIO_FLAGS (stereo)
    case 0xA: v = 0; break;                                        // XC_PARENTAL_CONTROL_GAMES
    case 0x103: v = kAvStandardNtscM | kAvFlags60Hz; break;        // XC_FACTORY_AV_REGION
    case 0x104: v = Lang().game_region; break;                     // XC_FACTORY_GAME_REGION
    default:
      if (g_trace) Log("ExQueryNonVolatileSetting(%X): 0", idx);
      break;
  }
  if (ptype) Wr32(ptype, 4);  // REG_DWORD
  if (pres) Wr32(pres, 4);
  if (len < 4) return kBufferTooSmall;
  Wr32(val, v);
  return kSuccess;
}

// ---- data exports ------------------------------------------------------------------
// XBOX_HARDWARE_INFO {ULONG Flags; UCHAR GpuRevision, McpRevision, ...}
KDATA(322, XboxHardwareInfo, 8, [](uint32_t va) {
  Wr32(va, 0);
  Wr8(va + 4, 0xD2);
  Wr8(va + 5, 0xD4);
});
// XBOX_KRNL_VERSION {USHORT Major, Minor, Build, Qfe}
KDATA(324, XboxKrnlVersion, 8, [](uint32_t va) {
  Wr16(va, 1);
  Wr16(va + 2, 0);
  Wr16(va + 4, 5838);
  Wr16(va + 6, 1);
});
// Keys for save-game signatures: fixed per install, our own saves only.
KDATA(323, XboxHDKey, 16, [](uint32_t va) {
  for (int i = 0; i < 16; i++) Wr8(va + i, uint8_t(0x48 + i * 7));
});
KDATA(325, XboxSignatureKey, 16, [](uint32_t va) {
  for (int i = 0; i < 16; i++) Wr8(va + i, uint8_t(0x50 + i * 11));
});
KDATA(354, XboxAlternateSignatureKeys, 256, [](uint32_t va) {
  for (int i = 0; i < 256; i++) Wr8(va + i, uint8_t(0x33 + i * 13));
});
// PLAUNCH_DATA_PAGE LaunchDataPage: none (cold boot).
KDATA(164, LaunchDataPage, 4, [](uint32_t va) { Wr32(va, 0); });
// Cache partitions (Z:): XAPI picks one per title from a table in partition 0.
KDATA(40, HalDiskCachePartitionCount, 4, [](uint32_t va) { Wr32(va, 3); });
// AV pack detected by the SMC at boot; 6 = standard composite.
KDATA(356, HalBootSMCVideoMode, 4, [](uint32_t va) { Wr32(va, EnvU32("HP2_AV_PACK", kAvPackStandard)); });
KDATA(357, IdexChannelObject, 0x40, [](uint32_t va) { std::memset(GP(va), 0, 0x40); });

// ---- AV -----------------------------------------------------------------------------------
// D3D's own calls; Render replaces D3D at API level. Report NTSC-M 60 Hz.
KEXPORT(1, AvGetSavedDataAddress, 0) { return 0; }
KEXPORT(4, AvSetSavedDataAddress, 1) { return 0; }
// AvSendTVEncoderOption(RegisterBase, Option, Param, PULONG Result)
KEXPORT(2, AvSendTVEncoderOption, 4) {
  uint32_t opt = Arg(c, 1), res = Arg(c, 3);
  uint32_t v = 0;
  if (opt == 6)  // AV_QUERY_AV_CAPABILITIES
    v = EnvU32("HP2_AV_PACK", kAvPackStandard) | kAvStandardNtscM | kAvFlags60Hz;
  if (res) Wr32(res, v);
  return 0;
}
// AvSetDisplayMode(RegisterBase, Step, Mode, Format, Pitch, FrameBuffer)
KEXPORT(3, AvSetDisplayMode, 6) { return 0; }

// ---- HAL ------------------------------------------------------------------------------------
// HalReturnToFirmware(Routine): 0 halt, 1 reboot, 2 quick reboot (launch an
// XBE / the dashboard), 3 hard reset, 4 fatal error.
KEXPORT(49, HalReturnToFirmware, 1) {
  uint32_t r = Arg(c, 0);
  Log("HalReturnToFirmware(%u): the game asked to leave (%s); exiting", r,
      r == 2 ? "launch dashboard/XBE" : r == 4 ? "fatal error" : "reboot/halt");
  std::fflush(nullptr);
  std::_Exit(r == 4 ? 1 : 0);
}
KEXPORT(360, HalInitiateShutdown, 0) {
  Log("HalInitiateShutdown; exiting");
  std::fflush(nullptr);
  std::_Exit(0);
}
KEXPORT(358, HalIsResetOrShutdownPending, 0) { return 0; }
// HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN Register)
KEXPORT(47, HalRegisterShutdownNotification, 2) { return 0; }
// HalReadWritePCISpace(BusNumber, SlotNumber, RegisterNumber, Buffer, Length, WritePCISpace)
KEXPORT(46, HalReadWritePCISpace, 6) {
  if (!(Arg(c, 5) & 0xFF)) std::memset(GP(Arg(c, 3)), 0, Arg(c, 4));
  return 0;
}

KEXPORT(95, KeBugCheck, 1) { Fatal("KeBugCheck(%08X)", Arg(c, 0)); }

// x87/SSE state is per guest context already.
KEXPORT(142, KeSaveFloatingPointState, 1) { return kSuccess; }
KEXPORT(139, KeRestoreFloatingPointState, 1) { return kSuccess; }

// ---- DbgPrint (cdecl, varargs) ---------------------------------------------------------------
KEXPORT(8, DbgPrint, -1) {
  std::string fmt = GStr(Arg(c, 0)), out;
  int ai = 1;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    std::string spec = "%";
    size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0123456789.", fmt[j])) spec += fmt[j++];
    bool wide64 = false;
    while (j < fmt.size() && std::strchr("lhI6", fmt[j])) {
      if (fmt[j] == 'I' && fmt.compare(j, 3, "I64") == 0) {
        wide64 = true;
        j += 2;
      }
      j++;
    }
    if (j >= fmt.size()) break;
    char conv = fmt[j];
    char buf[256];
    if (conv == '%') {
      out += '%';
    } else if (conv == 's') {
      spec += 's';
      std::snprintf(buf, sizeof buf, spec.c_str(), GStr(Arg(c, ai++)).c_str());
      out += buf;
    } else if (wide64) {
      uint64_t v = uint64_t(Arg(c, ai)) | uint64_t(Arg(c, ai + 1)) << 32;
      ai += 2;
      spec += "ll";
      spec += conv;
      std::snprintf(buf, sizeof buf, spec.c_str(), (unsigned long long)v);
      out += buf;
    } else {
      spec += conv == 'p' ? 'X' : conv;
      std::snprintf(buf, sizeof buf, spec.c_str(), Arg(c, ai++));
      out += buf;
    }
    i = j;
  }
  while (!out.empty() && out.back() == '\n') out.pop_back();
  Log("DbgPrint: %s", out.c_str());
  return 0;
}

// ---- Rtl ----------------------------------------------------------------------------------------
// RtlInitAnsiString(PANSI_STRING Dest, PCSZ Source)
KEXPORT(289, RtlInitAnsiString, 2) {
  uint32_t d = Arg(c, 0), s = Arg(c, 1);
  uint32_t n = s ? uint32_t(std::strlen(reinterpret_cast<const char*>(GP(s)))) : 0;
  Wr16(d, uint16_t(n));
  Wr16(d + 2, uint16_t(s ? n + 1 : 0));
  Wr32(d + 4, s);
  return 0;
}

// BOOLEAN RtlEqualString(PSTRING, PSTRING, BOOLEAN CaseInsensitive)
KEXPORT(279, RtlEqualString, 3) {
  std::string a = AnsiString(Arg(c, 0)), b = AnsiString(Arg(c, 1));
  if (a.size() != b.size()) return 0;
  if (Arg(c, 2) & 0xFF) {
    for (size_t i = 0; i < a.size(); i++)
      if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
        return 0;
    return 1;
  }
  return a == b;
}

// SIZE_T RtlCompareMemoryUlong(Source, Length, Pattern): bytes that match.
KEXPORT(269, RtlCompareMemoryUlong, 3) {
  uint32_t src = Arg(c, 0), len = Arg(c, 1) & ~3u, pat = Arg(c, 2);
  uint32_t i = 0;
  while (i < len && Rd32(src + i) == pat) i += 4;
  return i;
}

KEXPORT(301, RtlNtStatusToDosError, 1) {
  uint32_t s = Arg(c, 0);
  switch (s) {
    case kSuccess: return 0;
    case kTimeout: return 258;
    case kPending: return 997;
    case kBufferOverflow: return 234;
    case kNoMoreFiles: return 18;
    case kUnsuccessful: return 31;
    case kNotImplemented: return 1;
    case kInvalidHandle: return 6;
    case kInvalidParameter: return 87;
    case kNoSuchFile: return 2;
    case kInvalidDeviceRequest: return 1;
    case kEndOfFile: return 38;
    case kNoMemory: return 8;
    case kAccessDenied: return 5;
    case kBufferTooSmall: return 122;
    case kObjectTypeMismatch: return 1784;
    case kObjectNameInvalid: return 123;
    case kObjectNameNotFound: return 2;
    case kObjectNameCollision: return 183;
    case kObjectPathNotFound: return 3;
    case kMediaWriteProtected: return 19;
    case kNotADirectory: return 267;
    case kFileIsADirectory: return 5;
    case kNotSupported: return 50;
    case kDirectoryNotEmpty: return 145;
    case kCannotDelete: return 5;
    case 0xC000007F: return 112;  // STATUS_DISK_FULL
    case kInsufficientResources: return 1450;
    case 0xC0000043: return 32;   // STATUS_SHARING_VIOLATION
    case 0xC000014F: return 1005; // STATUS_UNRECOGNIZED_VOLUME
    case 0xC0000184: return 21;   // STATUS_INVALID_DEVICE_STATE -> NOT_READY
    default:
      if ((s & 0xFFFF0000) == 0x80070000) return s & 0xFFFF;
      return 317;  // ERROR_MR_MID_NOT_FOUND
  }
}

// ---- SHA-1 (XcSHA*): state kept in the caller's context buffer --------------------------
namespace {
// Context layout (our own, fits A_SHA_CTX's 116 bytes): h[5] @0, count @20 (u64), buf[64] @28.
void ShaBlock(uint32_t h[5], const uint8_t* p) {
  auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  uint32_t w[80];
  for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) f = (b & cc) | (~b & d), k = 0x5A827999;
    else if (i < 40) f = b ^ cc ^ d, k = 0x6ED9EBA1;
    else if (i < 60) f = (b & cc) | (b & d) | (cc & d), k = 0x8F1BBCDC;
    else f = b ^ cc ^ d, k = 0xCA62C1D6;
    uint32_t t = rol(a, 5) + f + e + k + w[i];
    e = d, d = cc, cc = rol(b, 30), b = a, a = t;
  }
  h[0] += a, h[1] += b, h[2] += cc, h[3] += d, h[4] += e;
}

void ShaUpdate(uint32_t ctx, const uint8_t* data, uint32_t len) {
  uint32_t h[5];
  std::memcpy(h, GP(ctx), 20);
  uint64_t count = Rd64(ctx + 20);
  uint8_t* buf = GP(ctx + 28);
  for (uint32_t i = 0; i < len; i++) {
    buf[count % 64] = data[i];
    count++;
    if (count % 64 == 0) ShaBlock(h, buf);
  }
  std::memcpy(GP(ctx), h, 20);
  Wr64(ctx + 20, count);
}
}  // namespace

KEXPORT(335, XcSHAInit, 1) {
  uint32_t ctx = Arg(c, 0);
  const uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::memcpy(GP(ctx), h, 20);
  Wr64(ctx + 20, 0);
  return 0;
}
// XcSHAUpdate(Context, Input, Length)
KEXPORT(336, XcSHAUpdate, 3) {
  std::vector<uint8_t> tmp(GP(Arg(c, 1)), GP(Arg(c, 1)) + Arg(c, 2));
  ShaUpdate(Arg(c, 0), tmp.data(), uint32_t(tmp.size()));
  return 0;
}
// XcSHAFinal(Context, Digest[20])
KEXPORT(337, XcSHAFinal, 2) {
  uint32_t ctx = Arg(c, 0), out = Arg(c, 1);
  uint64_t bits = Rd64(ctx + 20) * 8;
  uint8_t pad = 0x80, zero = 0;
  ShaUpdate(ctx, &pad, 1);
  while (Rd64(ctx + 20) % 64 != 56) ShaUpdate(ctx, &zero, 1);
  uint8_t lenb[8];
  for (int i = 0; i < 8; i++) lenb[i] = uint8_t(bits >> (56 - 8 * i));
  ShaUpdate(ctx, lenb, 8);
  for (int i = 0; i < 5; i++) {
    uint32_t v = Rd32(ctx + 4 * i);
    for (int j = 0; j < 4; j++) Wr8(out + 4 * i + j, uint8_t(v >> (24 - 8 * j)));
  }
  return 0;
}

// ---- XBE sections ---------------------------------------------------------------------------------
// XBEIMAGE_SECTION: Flags @0, VirtualAddress @4, VirtualSize @8, ...,
// SectionReferenceCount @0x18. The loader maps every section (also the
// non-preloaded ones), so loading is reference counting only.
KEXPORT(327, XeLoadSection, 1) {
  uint32_t s = Arg(c, 0);
  Wr32(s + 0x18, Rd32(s + 0x18) + 1);
  return kSuccess;
}
KEXPORT(328, XeUnloadSection, 1) {
  uint32_t s = Arg(c, 0);
  uint32_t n = Rd32(s + 0x18);
  if (!n) return kInvalidParameter;
  Wr32(s + 0x18, n - 1);
  return kSuccess;
}

// ---- driver model (only the XDK's USB stack in XPP uses it; replaced by XInput HLE) ----------------
// IoCreateDevice(DriverObject, DeviceExtensionSize, DeviceName, DeviceType,
//   Exclusive, PDEVICE_OBJECT* DeviceObject)
KEXPORT(65, IoCreateDevice, 6) {
  uint32_t ext = Arg(c, 1);
  uint32_t dev = PoolAlloc(0x60 + ext);
  if (!dev) return kInsufficientResources;
  Wr16(dev, 3);  // IO_TYPE_DEVICE
  Wr32(dev + 4, Arg(c, 0));
  Wr32(dev + 0x18, ext ? dev + 0x60 : 0);  // DeviceExtension
  Wr32(Arg(c, 5), dev);
  Warn("IoCreateDevice: an XDK driver is initialising (should be replaced by HLE)");
  return kSuccess;
}
KEXPORT(74, IoInvalidDeviceRequest, 2) { return kInvalidDeviceRequest; }
KEXPORT(81, IoStartNextPacket, 1) { return 0; }
KEXPORT(83, IoStartPacket, 3) { return 0; }
// VOID FASTCALL IofCompleteRequest(PIRP Irp /* ecx */, CCHAR PriorityBoost /* dl */)
KEXPORT(87, IofCompleteRequest, 0) { return 0; }
KEXPORT(359, IoMarkIrpMustComplete, 1) { return 0; }

// ---- SEH (fs:[0] chain on the guest stack, as on Windows/Xbox) -------------------------------------
namespace {
constexpr uint32_t kExceptionNoncontinuable = 0x1, kExceptionUnwinding = 0x2,
                   kExceptionExitUnwind = 0x4, kEnd = 0xFFFFFFFF;
constexpr uint32_t kRecordSize = 0x50, kContextSize = 0x2CC;

void FillContext(uint32_t ctx, const Ctx& c, uint32_t eip, uint32_t esp) {
  std::memset(GP(ctx), 0, kContextSize);
  Wr32(ctx + 0x00, 0x0001003F);
  Wr32(ctx + 0x9C, c.edi);
  Wr32(ctx + 0xA0, c.esi);
  Wr32(ctx + 0xA4, c.ebx);
  Wr32(ctx + 0xA8, c.edx);
  Wr32(ctx + 0xAC, c.ecx);
  Wr32(ctx + 0xB0, c.eax);
  Wr32(ctx + 0xB4, c.ebp);
  Wr32(ctx + 0xB8, eip);
  Wr32(ctx + 0xBC, 0x1B);
  Wr32(ctx + 0xC0, c.eflags);
  Wr32(ctx + 0xC4, esp);
  Wr32(ctx + 0xC8, 0x23);
}

uint32_t g_dispatcher_frame_handler = 0;  // guest VA of a "continue search" handler

bool InStack(uint32_t pcr, uint32_t va) {
  return va >= Rd32(pcr + 8) && va < Rd32(pcr + 4);
}
}  // namespace

// RtlRaiseException(PEXCEPTION_RECORD): walks the SEH chain. A handler that
// runs an __except block unwinds past us (runtime resume sites); one that
// continues returns here and we return to the raiser.
KEXPORT(302, RtlRaiseException, 1) {
  uint32_t src = Arg(c, 0), pcr = c.fs_base;
  uint32_t saved_esp = c.esp;
  c.esp -= 0x800;
  uint32_t rec = c.esp + 0x10, ctx = rec + kRecordSize;
  std::memcpy(GP(rec), GP(src), kRecordSize);
  uint32_t eip = Rd32(saved_esp);
  if (!Rd32(rec + 0x0C)) Wr32(rec + 0x0C, eip);
  FillContext(ctx, c, eip, saved_esp + 8);
  for (uint32_t reg = Rd32(pcr); reg != kEnd && reg; reg = Rd32(reg)) {
    if (!InStack(pcr, reg)) Fatal("corrupt SEH chain: record %08X outside the stack", reg);
    uint32_t disp = CallGuest(c, Rd32(reg + 4), {rec, reg, ctx, 0});
    if (disp == 0) {  // ExceptionContinueExecution
      if (Rd32(rec + 4) & kExceptionNoncontinuable)
        Fatal("handler continued a noncontinuable exception %08X", Rd32(rec));
      c.edi = Rd32(ctx + 0x9C);
      c.esi = Rd32(ctx + 0xA0);
      c.ebx = Rd32(ctx + 0xA4);
      c.ebp = Rd32(ctx + 0xB4);
      c.esp = saved_esp;
      return Rd32(ctx + 0xB0);
    }
  }
  // EXCEPTION_RECORD: Code @0, Flags @4, Address @0x0C, NumberParameters @0x10,
  // Information[] @0x14 (access violation: 0 read / 1 write, data address).
  Fatal("unhandled guest exception %08X raised from %08X at %08X (params %u: %08X %08X), "
        "fs %08X esp %08X", Rd32(rec), eip, Rd32(rec + 0x0C), Rd32(rec + 0x10), Rd32(rec + 0x14),
        Rd32(rec + 0x18), pcr, saved_esp);
}

// RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): calls the
// handlers above TargetFrame with the unwinding flag and pops them.
KEXPORT(312, RtlUnwind, 4) {
  uint32_t target = Arg(c, 0), target_ip = Arg(c, 1), exc = Arg(c, 2), retval = Arg(c, 3);
  if (target_ip && target_ip != Rd32(c.esp))
    Fatal("RtlUnwind to %08X, not its return address %08X: unsupported", target_ip, Rd32(c.esp));
  uint32_t pcr = c.fs_base;
  uint32_t saved_esp = c.esp;
  c.esp -= 0x800;
  uint32_t rec = exc;
  if (!rec) {
    rec = c.esp + 0x10;
    std::memset(GP(rec), 0, kRecordSize);
    Wr32(rec, 0xC0000027);  // STATUS_UNWIND
  }
  uint32_t ctx = c.esp + 0x10 + kRecordSize;
  FillContext(ctx, c, Rd32(saved_esp), saved_esp + 4 + 16);
  Wr32(rec + 4, Rd32(rec + 4) | kExceptionUnwinding | (target ? 0 : kExceptionExitUnwind));
  for (uint32_t reg = Rd32(pcr); reg != kEnd && reg && reg != target; reg = Rd32(pcr)) {
    CallGuest(c, Rd32(reg + 4), {rec, reg, ctx, 0});
    Wr32(pcr, Rd32(reg));
  }
  c.esp = saved_esp;
  (void)g_dispatcher_frame_handler;
  return retval;
}

// The game's language picker (Options -> flags, FUN_000FF640) lists the rows
// of the 6-row table 0x20F348 {language index, mask, flag sprite} whose
// mask & 0x3F != 0: USA, UK, FRA, GER, SPA, ITA. The port ships English,
// Ukrainian (the _UK slot, overridden by mods/uk, whose titlemap.geo repaints
// the UK flag) and, when the mods have _UKT, the hybrid "Ukrainian text +
// English speech" in the FRA row 2. The other masks are zeroed before the
// game runs (the front end builds the list at start). Checked against the
// NTSC-US tables first; a different XBE is left alone.
namespace {
bool g_hybrid_row = false;  // row 2 is the hybrid _UKT slot
}  // namespace

void LanguagePickerInit() {
  if (!g_host.code_lo) return;  // no XBE mapped (unit tests on a fake host)
  constexpr uint32_t kTable = 0x0020F348, kRow = 12;
  for (uint32_t k = 2; k < 6; k++)
    if (Rd32(kTable + kRow * k) != k || Rd32(kTable + kRow * k + 4) != (1u << k)) {
      Warn("language picker table at %08X not as expected: all languages listed", kTable);
      return;
    }
  for (uint32_t k = 3; k < 6; k++) Wr32(kTable + kRow * k + 4, 0);
  // Hybrid = row 2 (French) turned into _UKT: masks 4 -> 1 (English title
  // banner, none of the French nudges), folder "fra" -> "ukt" (the only user
  // of the string is the language table's pointer 0x1F7A98), geos -> the
  // English panel.geo / inv_font.geo. Language table 0x1F7A68: rows {mask,
  // name, folder, panel geo, inv_font geo} of 0x14 bytes (docs/handoff/loc.md).
  struct Patch {
    uint32_t va, old_v, new_v;
  };
  static const Patch kHybrid[] = {
      {0x0020F364, 4, 1},                    // picker row 2 mask
      {0x001F7A90, 4, 1},                    // language row 2 mask
      {0x001F7A9C, 0x1D9, 0x1D8},            // panel_fra.geo -> panel.geo
      {0x001F7AA0, 0x1E2, 0x1E1},            // inv_font_fra.geo -> inv_font.geo
      {0x001887D8, 0x00617266, 0x00746B75},  // "fra" -> "ukt"
  };
  bool hybrid = HybridSlotAvailable();
  for (auto& p : kHybrid)
    if (hybrid && Rd32(p.va) != p.old_v) {
      Warn("language table at %08X not as expected: no Ukrainian text/English speech slot", p.va);
      hybrid = false;
    }
  if (hybrid) {
    for (auto& p : kHybrid) Wr32(p.va, p.new_v);
    // The boot language (FUN_0004A4B0) comes from the table 0x1E4388 by mask
    // & 0x51E06C; English outside N. America = row 1, whose index field is
    // 0x1E4394. uk_text boots into index 2 (movies stay English: they follow
    // 0x51E06C, not the index).
    constexpr uint32_t kBootIndex = 0x001E4394;
    if (LanguageKey() == "uk_text") {
      if (Rd32(kBootIndex) == 1) Wr32(kBootIndex, 2);
      else Warn("boot language table at %08X not as expected: uk_text boots as uk", kBootIndex);
    }
    Log("language picker: English (US) + Ukrainian (_UK) + Ukrainian text/English speech (_UKT)");
  } else {
    Wr32(kTable + kRow * 2 + 4, 0);
    Log("language picker: English (US) + Ukrainian (_UK)");
  }
  g_hybrid_row = hybrid;
  // The picker's choice is the language index 0x52FDA4 (row of the table at
  // 0x1F7A68: 0 USA, 1 UK slot, 2 the hybrid); the game keeps it only in
  // memory. Twice a second, a changed index that differs from the configured
  // language is written to the profile's hp2.ini, which LanguageKey reads at
  // the next start (HP2_LANG still wins). The first value seen is the boot value.
  hp2_vblank_add_callback(
      [](uint64_t n, void*) {
        static uint32_t last = ~0u;
        if (n % 30) return;
        uint32_t idx = Rd32(0x0052FDA4);
        if (idx == last) return;
        bool first = last == ~0u;
        last = idx;
        if (first || idx > (g_hybrid_row ? 2u : 1u)) return;
        static const char* const kKeys[] = {"en", "uk", "uk_text"};
        const char* key = kKeys[idx];
        if (LanguageKey() == key) return;
        WriteLanguageToIni(key);
      },
      nullptr);
}

// Sets "language=<key>" in <profile>/hp2.ini: replaces the active line (or
// the template's commented "#language=" one), else appends; other lines kept.
void WriteLanguageToIni(const std::string& key) {
  std::string path = g_data_dir + "/hp2.ini", text, out;
  if (FILE* f = std::fopen(path.c_str(), "r")) {
    char buf[512];
    while (std::fgets(buf, sizeof buf, f)) text += buf;
    std::fclose(f);
  }
  bool done = false;
  std::string line_new = "language=" + key + "\n";
  for (size_t at = 0; at < text.size();) {
    size_t nl = text.find('\n', at);
    size_t end = nl == std::string::npos ? text.size() : nl + 1;
    std::string line = text.substr(at, end - at);
    if (line.rfind("language=", 0) == 0 && !done) {
      out += line_new, done = true;
    } else if (line.rfind("language=", 0) == 0) {
      // a second active line would override ours: drop it
    } else {
      out += line;
    }
    at = end;
  }
  if (!done) {
    // After the template's "#language=" example, else at the end.
    size_t ex = out.find("#language=");
    if (ex != std::string::npos) {
      size_t eol = out.find('\n', ex);
      eol = eol == std::string::npos ? out.size() : eol + 1;
      out.insert(eol, line_new);
    } else {
      if (!out.empty() && out.back() != '\n') out += '\n';
      out += line_new;
    }
  }
  std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f || std::fputs(out.c_str(), f) < 0 || std::fclose(f) != 0 ||
      std::rename(tmp.c_str(), path.c_str()) != 0) {
    Warn("cannot write %s: the language choice is not kept", path.c_str());
    return;
  }
  Log("language %s chosen in game: saved to %s", key.c_str(), path.c_str());
}

}  // namespace hp2k

