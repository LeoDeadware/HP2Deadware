// Boot: guest arena, XBE mapping, dispatch, kernel imports, main thread.

#include <pthread.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

#include "runtime/hp2_internal.h"
#include "runtime/xbe_image.h"

namespace hp2rt {

void Boot(const std::string& xbe_path) {
  if (!g_mem.Reserve(32)) hp2_fatal("cannot reserve the 4 GiB guest arena");
  recomp::g_guest_base = g_mem.base();
  recomp::PeImage img;
  recomp::XbeInfo xi;
  std::string err;
  if (!recomp::LoadXbe(xbe_path, g_mem, &img, &xi, &err))
    hp2_fatal("%s: %s", xbe_path.c_str(), err.c_str());
  static std::string path_copy = xbe_path;
  g_xbe = Hp2XbeInfo{};
  g_xbe.base = uint32_t(img.image_base);
  g_xbe.image_size = img.image_size;
  g_xbe.headers_size = uint32_t(xi.headers_size);
  g_xbe.entry = uint32_t(img.entry);
  g_xbe.kernel_thunk = uint32_t(xi.kernel_thunk);
  g_xbe.cert_va = uint32_t(xi.cert);
  g_xbe.tls_va = img.has_tls ? uint32_t(xi.tls_dir) : 0;
  g_xbe.stack_commit = xi.stack_commit;
  g_xbe.heap_reserve = xi.heap_reserve;
  g_xbe.heap_commit = xi.heap_commit;
  g_xbe.tls_raw_start = uint32_t(img.tls.raw_start);
  g_xbe.tls_raw_end = uint32_t(img.tls.raw_end);
  g_xbe.tls_zero_fill = img.tls.zero_fill;
  g_xbe.tls_index_va = uint32_t(img.tls.index_va);
  g_xbe.code_lo = uint32_t(xi.code_lo);
  g_xbe.code_hi = uint32_t(xi.code_hi);
  g_xbe.path = path_copy.c_str();
  for (const auto& s : img.sections) g_sections.push_back({s.name, uint32_t(s.va), s.vsize});
  hp2_log("hp2: %s: base %08X size %08X entry %08X kernel thunk %08X (%zu imports), "
          "code %08X..%08X, stack %X, TLS %08X (%u bytes)",
          xbe_path.c_str(), g_xbe.base, g_xbe.image_size, g_xbe.entry, g_xbe.kernel_thunk,
          img.imports.size(), g_xbe.code_lo, g_xbe.code_hi, g_xbe.stack_commit, g_xbe.tls_va,
          TlsDataSize());
  InstallFaultHandler();
  InitDispatch();
  BindKernelImports();
}

}  // namespace hp2rt

// Provided by the D3D8 HLE (src/hle/d3d8/window.mm); absent in headless builds.
extern "C" __attribute__((weak)) void hp2_host_main_loop();

namespace hp2rt {

void RunMain() {
  RunOnBigStack(
      [](void*) {
        Thread* t = NewMainThread();
        BindThread(t);
        pthread_setname_np("guest-main");
        hp2_log("hp2: main thread: esp %08X fs %08X KTHREAD %08X TlsData %08X stack %08X..%08X",
                t->ctx.esp, t->ctx.fs_base, t->info.kthread, t->info.tls_data,
                t->info.stack_lo, t->info.stack_hi);
        // HP2_CALL=va:arg,arg,... (dev/test switch): call one guest function
        // (stdcall or cdecl) instead of the entry, log eax and exit 0. An
        // argument `buf` is a zeroed 4 KiB guest buffer (the same one each
        // time), anything else is hex.
        if (std::string spec = Env("HP2_CALL"); !spec.empty()) {
          uint32_t va = uint32_t(std::strtoul(spec.c_str(), nullptr, 16)), buf = 0;
          std::vector<uint32_t> args;
          for (size_t pos = spec.find(':'); pos != std::string::npos;) {
            size_t end = spec.find(',', pos + 1);
            std::string a = spec.substr(pos + 1, end == std::string::npos ? end : end - pos - 1);
            if (a == "buf") {
              if (!buf) buf = hp2_hle_alloc(0x1000, 16);
              args.push_back(buf);
            } else if (!a.empty()) {
              args.push_back(uint32_t(std::strtoul(a.c_str(), nullptr, 16)));
            }
            pos = end;
          }
          hp2_log("hp2: HP2_CALL %08X with %zu args", va, args.size());
          uint32_t r = hp2_call_guest(t->ctx, va, args.data(), int(args.size()));
          hp2_log("hp2: HP2_CALL %08X returned %08X (buf %08X: \"%.32s\")", va, r, buf,
                  buf ? static_cast<const char*>(hp2_ptr(buf)) : "");
          hp2_exit(0, "HP2_CALL done");
        }
        hp2_log("hp2: calling the XBE entry %08X", g_xbe.entry);
        hp2_call_guest(t->ctx, g_xbe.entry);
        // XAPI's startup runs the title on a thread of its own
        // (PsCreateSystemThreadEx) and returns: only this thread ends; the
        // run stops at HalReturnToFirmware, a fatal error or HP2_STOP_AT.
        hp2_log("hp2: the XBE entry returned (launch thread ends)");
      },
      nullptr, true);
  // The process main thread is free now (the game runs on its own threads).
  // macOS windowing must run here: the D3D8 HLE's window/event loop takes it
  // when linked (weak; returns at once for silent HP2_BACKGROUND=1 runs).
  if (hp2_host_main_loop) hp2_host_main_loop();
  for (;;) pause();
}

}  // namespace hp2rt
