#pragma once
// HP2Port runtime: the public API for HLE roles (kernel, XAPI, D3D8,
// DSound, polish). Owned by Lifter/Runtime; other roles include this and
// link `hp2_runtime`. Everything here is stable across lifts: a new
// lift_id never changes this header.
//
// Model (see CLAUDE.md "Build architecture"):
// - The XBE is mapped at its real VAs in a 4 GiB guest arena
//   (host address = hp2_guest_base() + guest VA).
// - Every guest call (direct, tail, indirect) goes through a dispatch slot
//   per lifted function. hp2_override() swaps a slot at startup, so an HLE
//   replacement needs no re-lift.
// - Kernel imports: the loader asks the Kernel HLE (kernel_exports.h) for
//   each ordinal of the thunk table at 0x17DA80 and patches the slot with a
//   thunk VA (functions) or the export's guest VA (data).
//
// Calling conventions for a HostFn (replacement of a guest function or a
// kernel export): on entry [esp] = return address, [esp+4] = first stack
// argument, ecx/edx hold fastcall/thiscall arguments. A HostFn must leave
// the stack as the guest `ret` would: hp2_ret_stdcall/hp2_ret_cdecl below.

#include <cstdint>
#include <cstring>

#include "runtime/ctx.h"  // recomp::Ctx (x86recomp): guest registers

using Hp2Ctx = recomp::Ctx;
using HostFn = void (*)(Hp2Ctx& c);

// ---- guest memory -----------------------------------------------------
namespace recomp {
extern uint8_t* g_guest_base;  // set once at boot, before any guest code
}
// Inline: HLE draw paths call it per vertex stream / per constant.
inline uint8_t* hp2_guest_base() { return recomp::g_guest_base; }

inline void* hp2_ptr(uint32_t va) { return hp2_guest_base() + va; }
inline uint32_t hp2_rd32(uint32_t va) {
  uint32_t v;
  std::memcpy(&v, hp2_guest_base() + va, 4);
  return v;
}
inline uint16_t hp2_rd16(uint32_t va) {
  uint16_t v;
  std::memcpy(&v, hp2_guest_base() + va, 2);
  return v;
}
inline uint8_t hp2_rd8(uint32_t va) { return hp2_guest_base()[va]; }
inline void hp2_wr32(uint32_t va, uint32_t v) { std::memcpy(hp2_guest_base() + va, &v, 4); }
inline void hp2_wr16(uint32_t va, uint16_t v) { std::memcpy(hp2_guest_base() + va, &v, 2); }
inline void hp2_wr8(uint32_t va, uint8_t v) { hp2_guest_base()[va] = v; }

// Makes [va, va+size) accessible (zero-filled where newly committed).
// Guest pages are 4 KiB. Returns false if the range is outside the arena.
bool hp2_commit(uint32_t va, uint32_t size);
void hp2_decommit(uint32_t va, uint32_t size);
bool hp2_is_committed(uint32_t va);

// Guest memory for HLE-owned objects (kernel data exports, KTHREADs,
// device objects...): a bump allocator in a reserved region (see
// hp2_layout below), zero-filled, never freed.
uint32_t hp2_hle_alloc(uint32_t size, uint32_t align = 16);

// Guest address space layout (fixed; documented for Kernel HLE's
// NtAllocateVirtualMemory / MmAllocateContiguousMemory).
namespace hp2_layout {
inline constexpr uint32_t kImageBase = 0x00010000;  // XBE headers + sections
inline constexpr uint32_t kStackBase = 0xFB000000;  // runtime-made guest stacks
inline constexpr uint32_t kStackSize = 0x02000000;  //   (hp2_spawn_thread fallback)
inline constexpr uint32_t kHleBase = 0xFD000000;    // hp2_hle_alloc region
inline constexpr uint32_t kHleSize = 0x00F00000;
inline constexpr uint32_t kThunkBase = 0xFE000000;  // kernel export thunks:
inline constexpr uint32_t kThunkStride = 0x10;      // kThunkBase + ord*stride
inline constexpr uint32_t kThunkSize = 0x00010000;
inline constexpr uint32_t kHostThunkBase = 0xFE010000;  // hp2_host_thunk()
inline constexpr uint32_t kHostThunkSize = 0x00010000;
// Everything else below kStackBase (after the image) is free for the
// Kernel HLE's allocators.
}  // namespace hp2_layout

// ---- the XBE ------------------------------------------------------------
struct Hp2XbeInfo {
  uint32_t base;           // 0x10000
  uint32_t image_size;
  uint32_t headers_size;
  uint32_t entry;          // decoded (retail key)
  uint32_t kernel_thunk;   // decoded
  uint32_t cert_va;        // XBE certificate (title id at +8)
  uint32_t tls_va;         // XBE TLS directory, 0 if none
  uint32_t stack_commit;   // header "size of stack commit"
  uint32_t heap_reserve, heap_commit;
  uint32_t tls_raw_start, tls_raw_end, tls_zero_fill, tls_index_va;
  uint32_t code_lo, code_hi;  // [lo, hi): .text .. XPP (lifted code)
  const char* path;        // host path of default.xbe
};
const Hp2XbeInfo& hp2_xbe();

// Guest VA range of an XBE section by name ("D3D", "DSOUND", ...).
bool hp2_section(const char* name, uint32_t* va, uint32_t* size);

// ---- function overrides (dispatch slots) --------------------------------
// Replaces the guest function at `guest_va` for every caller (direct,
// tail and indirect calls). Call at startup, before hp2_run(); usually from
// a static initialiser via HP2_OVERRIDE. Returns false if `guest_va` is not
// a lifted function entry (then nothing changes).
bool hp2_override(uint32_t guest_va, HostFn fn);
// The lifted body of `guest_va` (unaffected by overrides), or null.
HostFn hp2_original(uint32_t guest_va);
// True if `guest_va` is a lifted function entry.
bool hp2_is_function(uint32_t guest_va);

// Registers fn at startup: HP2_OVERRIDE(0x130720, MyPush1);
#define HP2_OVERRIDE(va, fn)                                     \
  static const bool HP2_CAT_(hp2_ovr_, __LINE__) =               \
      (hp2_register_override_at_start(uint32_t(va), fn, #fn), true)
#define HP2_CAT2_(a, b) a##b
#define HP2_CAT_(a, b) HP2_CAT2_(a, b)
// Queued and applied by the runtime once the lifted table is loaded.
void hp2_register_override_at_start(uint32_t guest_va, HostFn fn, const char* name);

// A guest-callable address for a host function (callbacks the game stores
// and calls indirectly, vtables of HLE objects). Calls to it dispatch to fn.
uint32_t hp2_host_thunk(HostFn fn, const char* name);

// ---- calling helpers ----------------------------------------------------
// i-th stack argument of a HostFn (0-based; [esp] is the return address).
inline uint32_t hp2_arg(const Hp2Ctx& c, int i) { return hp2_rd32(c.esp + 4 + 4u * i); }
inline float hp2_argf(const Hp2Ctx& c, int i) {
  uint32_t v = hp2_arg(c, i);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
// Return as `ret 4*nargs` (stdcall/thiscall/fastcall stack args).
inline void hp2_ret_stdcall(Hp2Ctx& c, uint32_t eax, int nargs) {
  c.eax = eax;
  c.esp += 4 + 4u * nargs;
}
// Return as plain `ret` (cdecl: the caller pops its arguments).
inline void hp2_ret_cdecl(Hp2Ctx& c, uint32_t eax) {
  c.eax = eax;
  c.esp += 4;
}
// 64-bit result in edx:eax.
inline void hp2_ret_stdcall64(Hp2Ctx& c, uint64_t v, int nargs) {
  c.edx = uint32_t(v >> 32);
  hp2_ret_stdcall(c, uint32_t(v), nargs);
}

// Calls guest code at `va` from the host on the current guest thread:
// pushes args (right to left) and a return address, dispatches, restores
// esp (so stdcall and cdecl both work) and returns eax.
uint32_t hp2_call_guest(Hp2Ctx& c, uint32_t va, const uint32_t* args, int nargs);
inline uint32_t hp2_call_guest(Hp2Ctx& c, uint32_t va) { return hp2_call_guest(c, va, nullptr, 0); }

// Indirect dispatch as lifted code does it (no stack changes): the caller
// has already pushed arguments and a return address.
void hp2_dispatch(Hp2Ctx& c, uint32_t va);

// ---- guest threads ------------------------------------------------------
// Xbox per-thread state the runtime builds for every guest thread:
// fs:[0x00] ExceptionList, fs:[0x04] StackBase, fs:[0x08] StackLimit,
// fs:[0x18] Self (NT_TIB), fs:[0x1C] SelfPcr, fs:[0x20] Prcb,
// fs:[0x28] Prcb.CurrentThread -> KTHREAD (hp2_kthread_size bytes, zeroed;
// KTHREAD+0x28 TlsData -> this thread's copy of the XBE TLS template).
inline constexpr uint32_t hp2_kthread_size = 0x200;
struct Hp2ThreadInfo {
  uint32_t kpcr;       // fs base
  uint32_t kthread;    // CurrentThread
  uint32_t tls_data;   // KTHREAD.TlsData
  uint32_t stack_lo, stack_hi;
  uint32_t id;         // runtime thread id (1 = main)
};
// The calling host thread's guest thread, or null on a non-guest thread.
Hp2Ctx* hp2_current_ctx();
const Hp2ThreadInfo* hp2_current_thread();

// Starts a new host thread running a guest thread: a fresh Ctx with its own
// guest stack (stack_size bytes, 0 = XBE default), KPCR/KTHREAD and TLS
// block, then calls body(ctx, user) on it. The host stack is large (lifted
// recursion mirrors the guest's). Returns the runtime thread id, 0 on error.
using Hp2ThreadBody = void (*)(Hp2Ctx& c, void* user);
uint32_t hp2_spawn_thread(uint32_t stack_size, Hp2ThreadBody body, void* user);

// ---- logging / stopping -------------------------------------------------
// One line to the run log (stderr + $HP2_LOG_DIR/runtime.log).
void hp2_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Logs the message with guest state and exits with `code` (non-zero).
[[noreturn]] void hp2_fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Clean stop (milestone reached, test finished): flushes logs, exits.
[[noreturn]] void hp2_exit(int code, const char* why);
// Name of a kernel ordinal ("NtCreateFile"), "?" if unknown.
const char* hp2_kernel_name(uint32_t ordinal);
// True when kernel calls are traced (HP2_KTRACE=1; default off since 2026-11-06).
bool hp2_ktrace_enabled();
