#pragma once
// Contract between the Xbox kernel HLE (src/hle/kernel, owner: Kernel HLE)
// and the loader/runtime (owner: Lifter).
//
// default.xbe imports 115 xboxkrnl.exe exports by ordinal (thunk table at
// 0x17DA80). Each is either a function or a data export:
//   - function: the loader binds the slot to a guest address of its choice
//     and routes calls to it to hp2_kernel_lookup(ordinal) (hp2_override);
//   - data: the loader writes hp2_kernel_data_va(ordinal) into the slot (the
//     game dereferences the slot to reach the variable: KeTickCount,
//     XboxHardwareInfo, LaunchDataPage, ...).
//
// Calling convention of every HostFn: exactly the x86 function it stands for.
// It reads its arguments from the guest stack (esp+4, esp+8, ...), returns in
// eax (edx:eax for 64-bit results) and pops the return address plus its
// stdcall arguments (cdecl ones, DbgPrint: only the return address), so the
// lifted caller simply continues after the call. Same as recomp::GuestFn.

#include <cstdint>
#include <functional>

namespace recomp {
struct Ctx;
}

#ifndef HP2_HOSTFN_DEFINED
#define HP2_HOSTFN_DEFINED
using HostFn = void (*)(recomp::Ctx&);
#endif

// ---- What the kernel needs from the runtime (filled by the loader) ----------
struct Hp2KernelHost {
  // Host address of guest VA 0 (the 4 GiB guest arena).
  uint8_t* guest_base = nullptr;
  // Make [va, va+size) accessible (zero-filled on first commit) / release it.
  // The kernel owns the address-space layout of everything it allocates
  // (see "Guest address map" in IMPORTS.md); it only asks the runtime to back it.
  bool (*commit)(uint32_t va, uint32_t size, bool writable) = nullptr;
  void (*decommit)(uint32_t va, uint32_t size) = nullptr;
  // Calls guest function `va` with stdcall arguments on context `c` (its esp
  // must point into a valid guest stack) and returns eax.
  uint32_t (*call_guest)(recomp::Ctx& c, uint32_t va, const uint32_t* args,
                         int nargs) = nullptr;
  // Starts a new host thread with a fresh guest context registered with the
  // runtime and runs body(ctx) on it. The kernel sets up esp, fs_base and
  // the Xbox KPCR/KTHREAD itself (hp2_kernel_thread_setup). The thread ends
  // when body returns.
  void (*start_thread)(std::function<void(recomp::Ctx&)> body) = nullptr;
  // Context of the guest thread running on the calling host thread.
  recomp::Ctx* (*current_ctx)() = nullptr;
  // Lifted code range [code_lo, code_hi) (crash reports: guest call chains).
  uint32_t code_lo = 0, code_hi = 0;
};

// ---- Configuration (defaults are fine for a normal run) ---------------------
struct Hp2KernelConfig {
  const char* disc_dir = nullptr;   // D:\  (default: $HP2_DATA/game/extracted)
  const char* mods_dirs = nullptr;  // ':'-separated override roots, first wins
  const char* data_dir = nullptr;   // per-profile root for T:\ U:\ Z:\ (default:
                                    // ~/Library/Application Support/HP2Port/<profile>)
  const char* language = nullptr;   // "en"(USA, default) | "uk"(_UK slot) | "fr" | "de" | "es" | "it"
  double vblank_hz = 60.0;          // NTSC-M; never PAL
};

// Call once after the guest arena is reserved and the XBE is mapped, before
// any guest code runs. With the Lifter's runtime (hp2_runtime.h) linked in,
// the first hp2_kernel_data_va/lookup call does this itself from the
// runtime API (runtime_glue.cpp). Places the data exports, starts KeTickCount (1 ms) and
// the vblank pacer. Returns false on a fatal setup error (logged).
bool hp2_kernel_init(const Hp2KernelHost& host, const Hp2KernelConfig& cfg = {});

// Function export `ordinal`; nullptr if it is a data export or not provided.
HostFn hp2_kernel_lookup(uint32_t ordinal);
// Guest VA of data export `ordinal`; 0 if it is a function or not provided.
// Valid after hp2_kernel_init.
uint32_t hp2_kernel_data_va(uint32_t ordinal);
// "KeSetEvent" etc., for traces; "?" if unknown.
const char* hp2_kernel_name(uint32_t ordinal);
// Optional helper: a stable fake guest VA per function ordinal
// (0xFE000000 + ordinal * 16, the runtime's scheme) for the loader to put in the thunk table and
// register with hp2_override, if it has no address scheme of its own.
uint32_t hp2_kernel_thunk_va(uint32_t ordinal);

// Prepares context `c` to run as an Xbox kernel thread: commits a stack of
// `stack_size` bytes, places `tls_size` bytes of TLS data right below
// NtTib.StackBase (XAPI's TLS index is negative relative to it), builds the
// KPCR/KTHREAD and sets c.esp and c.fs_base. Used by PsCreateSystemThreadEx;
// the loader calls it for the main thread before running the XBE entry, with
// tls_size from the XBE TLS directory (hp2_kernel_tls_size).
void hp2_kernel_thread_setup(recomp::Ctx& c, uint32_t stack_size, uint32_t tls_size);
// TlsDataSize the way XAPI computes it from the XBE TLS directory at `tls_dir_va`.
uint32_t hp2_kernel_tls_size(uint32_t tls_dir_va);

// rdtsc for lifted code: a 733.33 MHz time-stamp counter (Pentium III), so
// QueryPerformanceFrequency-style math in XAPI/the game stays right.
uint64_t hp2_kernel_rdtsc();

// ---- 60 Hz vblank pacer (for Render's D3D Swap/Present and the XMV player) --
// A host thread ticks at exactly cfg.vblank_hz (absolute deadlines, no
// drift), independent of the host display.
uint64_t hp2_vblank_count();                  // vblanks since init
// Blocks until the counter exceeds `after` (pass hp2_vblank_count() to wait
// for the next vblank); returns the new count.
uint64_t hp2_vblank_wait(uint64_t after);
// Host time (seconds, steady clock) of vblank `n`.
double hp2_vblank_time(uint64_t n);
void hp2_vblank_set_rate(double hz);          // 0 = free-running (no wait)
// Called on the pacer thread at every vblank (keep it short). Returns an id.
int hp2_vblank_add_callback(void (*cb)(uint64_t count, void* user), void* user);
void hp2_vblank_remove_callback(int id);

// ---- Crash reports (crash.cpp) ----------------------------------------------
// Keeps the last 300 log lines (the kernel's own, plus whatever the runtime
// feeds with hp2_crash_note) and, on a fatal error, writes a readable report
// to ~/Library/Logs/hp2/hp2-crash-<time>.txt ($HP2_CRASH_DIR overrides):
// reason, settings (profile, language, HP2_* environment), frame, the guest
// thread's registers and call chain (return addresses into game code), the
// host backtrace and the log tail. Returns the report path ("" if it could
// not be written) and prints it to stderr. Call it before exiting on FATAL.
void hp2_crash_note(const char* line);
const char* hp2_crash_report(const char* reason);

// `hp2 --help` text: usage, keyboard controls, profile paths, HP2_* options.
const char* hp2_help_text();
