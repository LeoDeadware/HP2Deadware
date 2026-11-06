#pragma once
// Runtime internals shared by src/runtime and src/loader (not for HLE roles:
// they use hp2_runtime.h only).

#include <string>

#include "runtime/guest_memory.h"
#include "runtime/hp2_runtime.h"
#include "runtime/lifted.h"

namespace hp2rt {

extern recomp::GuestMemory g_mem;
extern Hp2XbeInfo g_xbe;
struct Section {
  std::string name;
  uint32_t va, size;
};
extern std::vector<Section> g_sections;

// hp2_dispatch.cpp: builds the dispatch table from the lifted function
// table and applies the overrides queued by HP2_OVERRIDE.
void InitDispatch();
// Patches the kernel thunk table (each slot: data VA or thunk VA).
void BindKernelImports();
// Installs SIGSEGV/SIGBUS reporting.
void InstallFaultHandler();

// hp2_thread.cpp
struct Thread {
  Hp2Ctx ctx{};
  Hp2ThreadInfo info{};
};
Thread* NewThread(uint32_t stack_size);  // guest stack + KPCR/KTHREAD + TLS
Thread* NewMainThread();  // laid out by the kernel HLE when it is linked
void BindThread(Thread* t);              // current host thread runs t
// Runs body on a new host thread with a big host stack; joins if wait.
void RunOnBigStack(void (*body)(void*), void* arg, bool wait);
// TlsDataSize the way XAPI computes it from the XBE TLS directory.
uint32_t TlsDataSize();

// hp2_boot.cpp: reserves the arena, maps the XBE, builds dispatch, binds
// kernel imports. Then RunMain runs the entry on the main guest thread.
void Boot(const std::string& xbe_path);
[[noreturn]] void RunMain();

void OpenLog();
// Value of $name, or def.
std::string Env(const char* name, const char* def = "");

}  // namespace hp2rt
