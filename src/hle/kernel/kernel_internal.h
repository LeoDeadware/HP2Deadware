#pragma once
// Shared internals of the Xbox kernel HLE: guest memory access, the export
// registry, the guest address map and the handle table.

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "hle/kernel/kernel_exports.h"
#include "runtime/ctx.h"

namespace hp2k {

using recomp::Ctx;

extern Hp2KernelHost g_host;
extern Hp2KernelConfig g_cfg;

// ---- guest memory ------------------------------------------------------------
inline uint8_t* GP(uint32_t va) { return g_host.guest_base + va; }
inline uint32_t Rd32(uint32_t va) { uint32_t v; std::memcpy(&v, GP(va), 4); return v; }
inline uint16_t Rd16(uint32_t va) { uint16_t v; std::memcpy(&v, GP(va), 2); return v; }
inline uint8_t Rd8(uint32_t va) { return *GP(va); }
inline uint64_t Rd64(uint32_t va) { uint64_t v; std::memcpy(&v, GP(va), 8); return v; }
inline void Wr32(uint32_t va, uint32_t v) { std::memcpy(GP(va), &v, 4); }
inline void Wr16(uint32_t va, uint16_t v) { std::memcpy(GP(va), &v, 2); }
inline void Wr8(uint32_t va, uint8_t v) { *GP(va) = v; }
inline void Wr64(uint32_t va, uint64_t v) { std::memcpy(GP(va), &v, 8); }
// Argument i of the stdcall function being called (esp -> return address).
inline uint32_t Arg(const Ctx& c, int i) { return Rd32(c.esp + 4 + 4 * i); }
std::string GStr(uint32_t va);                      // NUL-terminated
std::string AnsiString(uint32_t pstring);           // ANSI_STRING/OBJECT_STRING*

// ---- logging (stderr; HP2_KTRACE=1 traces every call) -------------------------
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void Fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern bool g_trace;

// ---- exports ------------------------------------------------------------------
// Impl returns eax, or edx:eax when ret64. nargs = stdcall dwords popped;
// fastcall exports (Kf*, Iof*, Obf*) take ecx/edx and pop nothing.
using Impl = uint64_t (*)(Ctx& c);
struct Export {
  const char* name = nullptr;
  Impl fn = nullptr;
  int nargs = 0;
  bool ret64 = false;
};
struct Registrar {
  Registrar(uint32_t ord, const char* name, int nargs, Impl fn, bool ret64 = false);
};
// Data export: `size` bytes in the kernel data page, set up by init(va).
struct DataRegistrar {
  DataRegistrar(uint32_t ord, const char* name, uint32_t size, void (*init)(uint32_t va));
};
const Export* FindExport(uint32_t ord);

#define HP2K_CAT2(a, b) a##b
#define HP2K_CAT(a, b) HP2K_CAT2(a, b)
// KEXPORT(255, PsCreateSystemThreadEx, 10) { ... return status; }
#define KEXPORT_X(ord, name, nargs, r64)                                     \
  static uint64_t HP2K_CAT(k_, name)(::hp2k::Ctx & c);                         \
  static ::hp2k::Registrar HP2K_CAT(reg_, name){ord, #name, nargs,             \
                                                &HP2K_CAT(k_, name), r64};     \
  static uint64_t HP2K_CAT(k_, name)([[maybe_unused]] ::hp2k::Ctx & c)
#define KEXPORT(ord, name, nargs) KEXPORT_X(ord, name, nargs, false)
#define KEXPORT64(ord, name, nargs) KEXPORT_X(ord, name, nargs, true)
#define KDATA(ord, name, size, init) \
  static ::hp2k::DataRegistrar HP2K_CAT(dreg_, name){ord, #name, size, init}

// ---- NTSTATUS -------------------------------------------------------------------
constexpr uint32_t kSuccess = 0, kTimeout = 0x102, kPending = 0x103,
                   kObjectNameExists = 0x40000000, kBufferOverflow = 0x80000005,
                   kNoMoreFiles = 0x80000006, kUnsuccessful = 0xC0000001,
                   kNotImplemented = 0xC0000002, kInvalidHandle = 0xC0000008,
                   kInvalidParameter = 0xC000000D, kNoSuchFile = 0xC000000F,
                   kEndOfFile = 0xC0000011, kNoMemory = 0xC0000017,
                   kAccessDenied = 0xC0000022, kBufferTooSmall = 0xC0000023,
                   kObjectTypeMismatch = 0xC0000024,
                   kObjectNameInvalid = 0xC0000033, kObjectNameNotFound = 0xC0000034,
                   kObjectNameCollision = 0xC0000035, kObjectPathNotFound = 0xC000003A,
                   kMediaWriteProtected = 0xC00000A2, kNotADirectory = 0xC0000103,
                   kFileIsADirectory = 0xC00000BA, kNotSupported = 0xC00000BB,
                   kInvalidDeviceRequest = 0xC0000010, kDirectoryNotEmpty = 0xC0000101,
                   kCannotDelete = 0xC0000121, kConflictingAddresses = 0xC0000018,
                   kInvalidPageProtection = 0xC0000045, kMemoryNotAllocated = 0xC00000A0,
                   kFreeVmNotAtBase = 0xC000009F, kInsufficientResources = 0xC000009A;

// ---- guest address map (see IMPORTS.md) ------------------------------------------
constexpr uint32_t kUserLo = 0x00600000, kUserHi = 0x7EF00000;   // NtAllocateVirtualMemory
constexpr uint32_t kPhysVa = 0x80000000;                         // "physical" RAM (contiguous)
constexpr uint32_t kPhysSize = 0x08000000;                       // 128 MB
constexpr uint32_t kKernelDataVa = 0x8FF00000;                   // data exports, 64 KB
constexpr uint32_t kPoolLo = 0x90000000, kPoolHi = 0x98000000;   // ExAllocatePool, KPCR/KTHREAD

// ---- allocators (memory.cpp) ---------------------------------------------------------
// Range allocator over [lo, hi): first fit, aligned; tracks allocation sizes.
class RangeAlloc {
 public:
  RangeAlloc(uint32_t lo, uint32_t hi, uint32_t gran);
  // 0 on failure. Within [lo_limit, hi_limit] if given.
  uint32_t Alloc(uint32_t size, uint32_t align, uint32_t lo_limit = 0, uint32_t hi_limit = 0xFFFFFFFF,
                 bool top_down = false);
  bool Reserve(uint32_t va, uint32_t size);  // a fixed range; false if taken
  uint32_t Free(uint32_t va);                // returns size freed, 0 if not a block start
  uint32_t SizeOf(uint32_t va) const;        // block size if va is a block start
  // Block containing va: start/size; false if none.
  bool Find(uint32_t va, uint32_t* start, uint32_t* size) const;
  uint32_t lo() const { return lo_; }
  uint32_t hi() const { return hi_; }
 private:
  uint32_t lo_, hi_, gran_;
  std::map<uint32_t, uint32_t> used_;  // start -> size
};
// Kernel pool (ExAllocatePool etc.): committed, zeroed, 16-byte aligned.
uint32_t PoolAlloc(uint32_t size);
void PoolFree(uint32_t va);
uint32_t PoolSize(uint32_t va);
uint32_t ContiguousAlloc(uint32_t size, uint32_t lo_phys, uint32_t hi_phys, uint32_t align);
uint32_t UserAlloc(uint32_t size, uint32_t align);  // committed RW (stacks)
void MemoryInit();

// ---- objects & handles (objects.cpp) ----------------------------------------------
struct KObject {
  virtual ~KObject() = default;
  // Guest dispatcher object to wait on (KEVENT, KTHREAD, ...); 0 if none.
  uint32_t disp_va = 0;
  virtual const char* Kind() const = 0;
};
uint32_t NewHandle(std::shared_ptr<KObject> o);
std::shared_ptr<KObject> GetHandle(uint32_t h);
bool CloseHandle(uint32_t h);
constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;

// ---- dispatcher (sync.cpp) ---------------------------------------------------------
// Type codes of DISPATCHER_HEADER.Type.
enum : uint8_t {
  kEventNotification = 0, kEventSynchronization = 1, kMutant = 2, kSemaphore = 5,
  kThreadObj = 6, kTimerNotification = 8, kTimerSynchronization = 9,
};
std::mutex& DispatcherLock();
void SignalChanged();  // under DispatcherLock: wake waiters
// Waits on guest dispatcher objects. timeout100ns: nullptr = infinite, else an
// NT LARGE_INTEGER (negative = relative, positive = absolute system time).
uint32_t WaitObjects(const uint32_t* objs, uint32_t n, bool wait_all, const int64_t* timeout);
void InitDispatcherHeader(uint32_t va, uint8_t type, uint8_t size_dw, int32_t signal);
int32_t SetEventVa(uint32_t ev);   // returns previous state
void QueueDpc(uint32_t dpc, uint32_t a1, uint32_t a2);  // KeInsertQueueDpc semantics
void SyncInit();

// ---- threads (thread.cpp) --------------------------------------------------------------
uint32_t CurrentKThread();   // KTHREAD of the calling thread (0 if not a guest thread)
uint32_t CurrentKpcr();
Ctx* CurrentCtx();
// A guest context for host service threads (DPC, timers): stack + KPCR.
void RunServiceThread(const char* name, std::function<void(Ctx&)> body);
uint32_t CallGuest(Ctx& c, uint32_t va, std::initializer_list<uint32_t> args);
struct ThreadExit { uint32_t status; };

// ---- time (time.cpp) ----------------------------------------------------------------------
uint64_t InterruptTime100ns();     // since kernel init
uint64_t SystemTime100ns();        // FILETIME (since 1601)
void TimeInit();
void SleepUntil100ns(uint64_t interrupt_time);  // absolute, interrupt-time base
bool TestClock();                  // HP2_TEST_CLOCK=frames (time.cpp)
// Test clock: number of running (not blocked) counted threads. A counted
// thread (guest threads, the timer and DPC service threads) calls
// RunningAdd(-1) when it blocks in the kernel and +1 when it runs again; a
// thread that wakes another one adds +1 on its behalf (WakeCheckLocked, CS and
// DPC hand-offs), so the Swap barrier never sees 0 while work is pending.
// All no-ops without the test clock.
void RunningAdd(int d);
extern thread_local bool t_counted;
void SetCounted();                 // t_counted = true; uncounted again when the thread ends
void WakeOnTimeAdvance();          // sync.cpp: deadlines may have passed
std::string LanguageKey();         // en/uk/uk_text (file.cpp)
bool HybridSlotAvailable();        // the mods have _UKT\text.bin (file.cpp)
extern std::string g_data_dir;     // profile root (file.cpp)
void CrashNote(const char* line);  // crash.cpp: log tail for reports

// runtime_glue.cpp (only with the Lifter's hp2_runtime.h): fills the host
// interface from the runtime API.
}  // namespace hp2k
bool hp2k_runtime_glue(Hp2KernelHost* host, Hp2KernelConfig* cfg);  // false without the runtime
namespace hp2k {
void AutoInit();  // hp2_kernel_init from the runtime glue, once

// ---- file system (vfs.cpp / file.cpp) -------------------------------------------------------
void FileInit();
// misc.cpp: the language picker lists English (US) + Ukrainian only.
void LanguagePickerInit();
// misc.cpp: "language=<key>" into <profile>/hp2.ini (the in-game picker's choice).
void WriteLanguageToIni(const std::string& key);
void VblankInit(double hz);

}  // namespace hp2k
