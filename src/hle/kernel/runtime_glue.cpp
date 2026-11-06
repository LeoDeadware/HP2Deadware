// Binds the kernel HLE to the Lifter's runtime API (src/runtime/hp2_runtime.h).
// Without that header, or in the standalone unit-test build (HP2K_NO_RUNTIME_GLUE:
// hp2_runtime is not linked there), it reports "no runtime".

#if __has_include("runtime/hp2_runtime.h") && !defined(HP2K_NO_RUNTIME_GLUE)
#include "runtime/hp2_runtime.h"

#include "hle/kernel/kernel_internal.h"

namespace {

bool Commit(uint32_t va, uint32_t size, bool) { return hp2_commit(va, size); }
void Decommit(uint32_t va, uint32_t size) { hp2_decommit(va, size); }
uint32_t CallGuest(recomp::Ctx& c, uint32_t va, const uint32_t* args, int n) {
  return hp2_call_guest(c, va, args, n);
}
// The runtime builds its own stack/KPCR for the thread; the kernel then puts
// the thread on the stack/KPCR it laid out (hp2k::Prepare/Apply), which
// matches XAPI's TLS layout.
void Body(recomp::Ctx& c, void* user) {
  auto* fn = static_cast<std::function<void(recomp::Ctx&)>*>(user);
  (*fn)(c);
  delete fn;
}
void StartThread(std::function<void(recomp::Ctx&)> body) {
  auto* fn = new std::function<void(recomp::Ctx&)>(std::move(body));
  if (!hp2_spawn_thread(0x4000, Body, fn)) hp2k::Fatal("hp2_spawn_thread failed");
}

}  // namespace

bool hp2k_runtime_glue(Hp2KernelHost* host, Hp2KernelConfig* cfg) {
  host->guest_base = hp2_guest_base();
  host->commit = Commit;
  host->decommit = Decommit;
  host->call_guest = CallGuest;
  host->start_thread = StartThread;
  host->current_ctx = hp2_current_ctx;
  host->code_lo = hp2_xbe().code_lo;
  host->code_hi = hp2_xbe().code_hi;
  (void)cfg;  // defaults + HP2_* environment
  return host->guest_base != nullptr;
}
#else
#include "hle/kernel/kernel_exports.h"
bool hp2k_runtime_glue(Hp2KernelHost*, Hp2KernelConfig*) { return false; }
#endif
