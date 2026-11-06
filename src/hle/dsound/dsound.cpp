// DirectSound HLE (owner: Audio): XDK 5028 DSOUND at API level.
//
// The statically linked DSOUND library (section DSOUND, 0x1444E0..) drives
// the MCPX APU (MMIO 0xFE800000), the AC97 codec (0xFEC00000) and APU
// memory directly. None of it runs here: every entry the game and XMV call
// is replaced (table kApi below, identities in FUNCTIONS.md), objects are
// small guest blocks whose state lives in hp2ds::Voice, and mixer.cpp plays
// them. Every other DSOUND entry is guarded: a call logs once and runs the
// original (HP2_DSOUND_STRICT=1: stops instead), so a missed entry point
// shows up by name instead of as a fault on APU registers.
//
// Calling conventions: the C API wrappers are stdcall; their first argument
// is the interface pointer we handed out. XMV calls streams through the
// XMediaObject vtable (AddRef, Release, GetInfo, GetStatus, Process,
// Discontinuity, Flush), so streams carry a real vtable of host thunks.
//
// Env: HP2_DSLOG=1 logs every API call.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>

#include "hle/dsound/dsound_internal.h"
#include "hle/kernel/kernel_exports.h"
#include "runtime/hp2_runtime.h"

namespace hp2ds {
State g_ds;

bool test_clock_frames() {
  static const bool on = [] {
    const char* v = getenv("HP2_TEST_CLOCK");
    return v && std::string(v) == "frames";
  }();
  return on;
}
}  // namespace hp2ds

using namespace hp2ds;

namespace {

constexpr uint32_t S_OK = 0;
constexpr uint32_t E_FAIL = 0x80004005;
constexpr uint32_t DSERR_INVALIDCALL = 0x88780032;

constexpr uint32_t kOrdNtSetEvent = 225;

constexpr uint32_t kMagic = 0x53443248;  // "H2DS" at object +4
constexpr uint32_t kObjSize = 0x20;

bool env_on(const char* n) {
  const char* v = getenv(n);
  return v && atoi(v) != 0;
}
const bool g_trace = env_on("HP2_DSLOG");
// Under HP2_TEST_CLOCK=frames each line also carries the vblank and the
// guest return address of the API call (to diff two runs' call streams).
#define DSLOG(fmt, ...)                                                          \
  do {                                                                           \
    if (g_trace) {                                                               \
      if (hp2ds::test_clock_frames()) {                                          \
        Hp2Ctx* lc = hp2_current_ctx();                                          \
        hp2_log("ds: v%llu ret %08x " fmt, (unsigned long long)hp2_vblank_count(), \
                lc ? hp2_rd32(lc->esp) : 0u, ##__VA_ARGS__);                     \
      } else {                                                                   \
        hp2_log("ds: " fmt, ##__VA_ARGS__);                                      \
      }                                                                          \
    }                                                                            \
  } while (0)

// ---- guest objects ------------------------------------------------------

uint32_t g_ds_obj = 0;  // the IDirectSound singleton
int g_ds_refs = 0;
uint32_t g_vt_ds = 0, g_vt_buffer = 0, g_vt_stream = 0;
std::vector<uint32_t> g_free_objs;  // released object blocks (guarded by mu)

template <int I>
void VtTrap(Hp2Ctx& c) {
  hp2_fatal("dsound: unexpected call through an HLE object's vtable, slot %d (+0x%x), "
            "this=0x%08x ret=0x%08x", I, I * 4, hp2_arg(c, 0), hp2_rd32(c.esp));
}
constexpr HostFn kVtTraps[16] = {VtTrap<0>,  VtTrap<1>,  VtTrap<2>,  VtTrap<3>,
                                 VtTrap<4>,  VtTrap<5>,  VtTrap<6>,  VtTrap<7>,
                                 VtTrap<8>,  VtTrap<9>,  VtTrap<10>, VtTrap<11>,
                                 VtTrap<12>, VtTrap<13>, VtTrap<14>, VtTrap<15>};

uint32_t make_vtable(const HostFn* fns, int n, const char* what) {
  uint32_t vt = hp2_hle_alloc(16 * 4);
  for (int i = 0; i < 16; ++i) {
    HostFn f = i < n && fns[i] ? fns[i] : kVtTraps[i];
    hp2_wr32(vt + 4 * i, hp2_host_thunk(f, what));
  }
  return vt;
}

uint32_t new_object(uint32_t vtable, uint32_t kind) {  // needs mu
  uint32_t va;
  if (!g_free_objs.empty()) {
    va = g_free_objs.back();
    g_free_objs.pop_back();
    std::memset(hp2_ptr(va), 0, kObjSize);
  } else {
    va = hp2_hle_alloc(kObjSize);
  }
  hp2_wr32(va, vtable);
  hp2_wr32(va + 4, kMagic);
  hp2_wr32(va + 8, kind);
  return va;
}

Voice* voice(uint32_t va) {  // needs mu
  auto it = g_ds.voices.find(va);
  return it == g_ds.voices.end() ? nullptr : it->second;
}

Format read_wfx(uint32_t wfx) {
  Format f;
  if (!wfx) return f;
  f.tag = hp2_rd16(wfx);
  f.channels = hp2_rd16(wfx + 2) ? hp2_rd16(wfx + 2) : 1;
  f.rate = hp2_rd32(wfx + 4) ? hp2_rd32(wfx + 4) : 48000;
  f.block_align = hp2_rd16(wfx + 12);
  f.bits = hp2_rd16(wfx + 14);
  if (f.tag == kFmtXboxAdpcm) f.block_align = uint16_t(kAdpcmBlockBytes * f.channels);
  if (f.tag == kFmtPcm && f.block_align == 0) f.block_align = uint16_t(f.channels * f.bits / 8);
  return f;
}

// DSMIXBINS {dwMixBinCount, DSMIXBINVOLUMEPAIR* {dwMixBin, lVolume}}.
// routing (SetMixBins / desc): the pairs become the voice's bins, in order;
// otherwise (SetMixBinVolumes) only the volumes of those bins change.
void apply_mixbins(Voice& v, uint32_t mb, bool routing) {
  if (!mb) return;
  uint32_t n = hp2_rd32(mb), pairs = hp2_rd32(mb + 4);
  if (!pairs || n > 64) return;
  if (routing) v.nbins = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t bin = hp2_rd32(pairs + 8 * i);
    int32_t vol = int32_t(hp2_rd32(pairs + 8 * i + 4));
    if (bin >= uint32_t(kMixBins)) continue;
    v.bin_vol[bin] = vol < -10000 ? -10000 : vol > 0 ? 0 : vol;
    if (routing && v.nbins < 8) v.bins[v.nbins++] = uint8_t(bin);
  }
}

// The library's defaults when a desc has no DSMIXBINS (0x144841, tables at
// 0x14D404 by channel count and 0x14D944 for 3D voices, all at 0 mB), and
// its default headroom (0x145942).
void init_mix_defaults(Voice& v) {
  static const uint8_t k2d[3][6] = {{0, 1}, {0, 1, 4, 5}, {0, 1, 2, 3, 4, 5}};
  static const uint8_t k3d[5] = {6, 8, 7, 9, 10};
  bool is3d = v.flags & 0x10;  // DSBCAPS_CTRL3D
  const uint8_t* b = is3d ? k3d : k2d[v.fmt.channels <= 2 ? 0 : v.fmt.channels <= 4 ? 1 : 2];
  v.nbins = is3d ? 5 : v.fmt.channels <= 2 ? 2 : v.fmt.channels <= 4 ? 4 : 6;
  for (int i = 0; i < v.nbins; ++i) v.bins[i] = b[i];
  for (int32_t& x : v.bin_vol) x = 0;
  v.headroom = (is3d || (v.flags & 0x182000)) ? 0 : 600;
}

// ---- completions (packets done) ------------------------------------------
// The mixer (host thread) queues finished packets; a guest worker thread
// and DirectSoundDoWork deliver them: status + size, then the stream's
// callback or the packet's completion event, as the XDK DPC would.

}  // namespace

void hp2ds::deliver_completions(Hp2Ctx& c) {
  for (;;) {
    std::lock_guard<std::recursive_mutex> dl(g_ds.deliver_mu);
    Completion d;
    {
      std::lock_guard<std::mutex> lk(g_ds.mu);
      if (g_ds.done.empty()) return;
      d = g_ds.done.front();
      g_ds.done.pop_front();
      // Release completes its own pending entries; a stale one never writes.
      Voice* v = voice(d.voice_va);
      if (!v || v->serial != d.voice_serial) continue;
    }
    if (d.pkt.completed_size_va) hp2_wr32(d.pkt.completed_size_va, d.completed);
    if (d.pkt.status_va) hp2_wr32(d.pkt.status_va, d.status);
    if (d.callback) {
      uint32_t args[3] = {d.cb_context, d.pkt.context, d.status};
      hp2_call_guest(c, d.callback, args, 3);
    } else if (d.pkt.context) {
      uint32_t args[2] = {d.pkt.context, 0};
      hp2_call_guest(c, hp2_kernel_thunk_va(kOrdNtSetEvent), args, 2);
    }
  }
}

namespace {

void worker_body(Hp2Ctx& c, void*) {
  hp2_log("dsound: completion worker running (guest thread %u)", hp2_current_thread()->id);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    deliver_completions(c);
  }
}

extern const HostFn kStreamVtbl[7];

void ensure_started() {
  static std::once_flag once;
  std::call_once(once, [] {
    static const HostFn ds_fns[1] = {};
    g_vt_ds = make_vtable(ds_fns, 0, "dsound_vt_ds");
    g_vt_buffer = make_vtable(ds_fns, 0, "dsound_vt_buffer");
    g_vt_stream = make_vtable(kStreamVtbl, 7, "dsound_vt_stream");
    if (!test_clock_frames()) hp2_spawn_thread(0x10000, worker_body, nullptr);
    mixer_start();
  });
}

// ---- creation ------------------------------------------------------------

uint32_t create_buffer(uint32_t desc, uint32_t* out) {
  if (!desc || !out) return DSERR_INVALIDCALL;
  ensure_started();
  uint32_t flags = hp2_rd32(desc + 4), bytes = hp2_rd32(desc + 8);
  uint32_t wfx = hp2_rd32(desc + 12), mixbins = hp2_rd32(desc + 16);
  auto* v = new Voice;
  v->flags = flags;
  v->fmt = read_wfx(wfx);
  init_mix_defaults(*v);
  apply_mixbins(*v, mixbins, true);
  if (bytes) {  // the library allocates the data; SetBufferData may replace it
    v->data_va = hp2_hle_alloc(bytes, 32);
    v->data_bytes = bytes;
    hp2_log("dsound: buffer with %u own bytes at 0x%08x", bytes, v->data_va);
  }
  std::lock_guard<std::mutex> lk(g_ds.mu);
  v->va = new_object(g_vt_buffer, 1);
  v->serial = g_ds.next_serial++;
  g_ds.voices[v->va] = v;
  *out = v->va;
  DSLOG("CreateSoundBuffer flags=0x%x bytes=%u fmt=0x%x ch=%u rate=%u -> 0x%08x", flags, bytes,
        v->fmt.tag, v->fmt.channels, v->fmt.rate, v->va);
  return S_OK;
}

uint32_t create_stream(uint32_t desc, uint32_t* out) {
  if (!desc || !out) return DSERR_INVALIDCALL;
  ensure_started();
  auto* v = new Voice;
  v->stream = true;
  v->flags = hp2_rd32(desc);
  v->max_packets = hp2_rd32(desc + 4);
  v->fmt = read_wfx(hp2_rd32(desc + 8));
  v->callback = hp2_rd32(desc + 12);
  v->cb_context = hp2_rd32(desc + 16);
  init_mix_defaults(*v);
  apply_mixbins(*v, hp2_rd32(desc + 20), true);
  std::lock_guard<std::mutex> lk(g_ds.mu);
  v->va = new_object(g_vt_stream, 2);
  v->serial = g_ds.next_serial++;
  g_ds.voices[v->va] = v;
  *out = v->va;
  hp2_log("dsound: CreateSoundStream fmt=0x%x ch=%u rate=%u packets=%u cb=0x%08x -> 0x%08x",
          v->fmt.tag, v->fmt.channels, v->fmt.rate, v->max_packets, v->callback, v->va);
  return S_OK;
}

uint32_t release_voice(uint32_t va) {
  // Waits for a completion of this stream being delivered right now.
  std::lock_guard<std::recursive_mutex> dl(g_ds.deliver_mu);
  std::lock_guard<std::mutex> lk(g_ds.mu);
  Voice* v = voice(va);
  if (!v) return 0;
  if (--v->refs > 0) return uint32_t(v->refs);
  // A destroyed stream's packets end here, synchronously and without
  // callbacks (the owner is tearing down and may already have freed the
  // context): finished ones keep their status, queued ones are flushed.
  // Afterwards nothing touches their memory.
  for (auto it = g_ds.done.begin(); it != g_ds.done.end();) {
    if (it->voice_va == va && it->voice_serial == v->serial) {
      if (it->pkt.completed_size_va) hp2_wr32(it->pkt.completed_size_va, it->completed);
      if (it->pkt.status_va) hp2_wr32(it->pkt.status_va, it->status);
      it = g_ds.done.erase(it);
    } else {
      ++it;
    }
  }
  for (const Packet& p : v->packets) {
    if (p.completed_size_va) hp2_wr32(p.completed_size_va, 0);
    if (p.status_va) hp2_wr32(p.status_va, kPacketFlushed);
  }
  if (v->stream && v->started)
    hp2_log("dsound: stream 0x%08x released: %.3f s of audio played over %.3f s of mixer time "
            "(t %.3f..%.3f), %zu packets dropped",
            va, double(v->played_frames) / v->fmt.rate, v->last_play_s - v->first_play_s,
            v->first_play_s, v->last_play_s, v->packets.size());
  g_ds.voices.erase(va);
  DSLOG("release 0x%08x", va);
  delete v;
  hp2_wr32(va + 4, 0);
  g_free_objs.push_back(va);
  return 0;
}

// ---- API: DirectSound ------------------------------------------------------
// Each: [esp+4..] stdcall arguments.

// HRESULT DirectSoundCreate(LPGUID, LPDIRECTSOUND8* ppDS, LPUNKNOWN)
void DirectSoundCreate(Hp2Ctx& c) {
  uint32_t pp = hp2_arg(c, 1);
  ensure_started();
  {
    std::lock_guard<std::mutex> lk(g_ds.mu);
    if (!g_ds_obj) {
      g_ds_obj = new_object(g_vt_ds, 0);
      hp2_log("dsound: DirectSoundCreate -> 0x%08x (HLE, no APU)", g_ds_obj);
    }
    ++g_ds_refs;
  }
  if (pp) hp2_wr32(pp, g_ds_obj);
  hp2_ret_stdcall(c, S_OK, 3);
}

// ULONG IDirectSound_Release(LPDIRECTSOUND8)
void IDirectSound_Release(Hp2Ctx& c) {
  std::lock_guard<std::mutex> lk(g_ds.mu);
  if (g_ds_refs > 0) --g_ds_refs;
  DSLOG("IDirectSound_Release -> %d", g_ds_refs);
  // The device stays (other owners may still hold buffers made from it).
  hp2_ret_stdcall(c, uint32_t(g_ds_refs), 1);
}

// HRESULT IDirectSound_DownloadEffectsImage(pDS, pvImage, dwSize, pImageLoc, ppImageDesc)
void IDirectSound_DownloadEffectsImage(Hp2Ctx& c) {
  uint32_t size = hp2_arg(c, 2), ppdesc = hp2_arg(c, 4);
  hp2_log("dsound: DownloadEffectsImage %u bytes (DSP effects not emulated)", size);
  if (ppdesc) {
    static uint32_t desc = hp2_hle_alloc(0x200);  // DSEFFECTIMAGEDESC, no effects
    hp2_wr32(ppdesc, desc);
  }
  hp2_ret_stdcall(c, S_OK, 5);
}

// HRESULT IDirectSound_SynchPlayback(pDS): starts together every buffer and
// stream waiting with DSBPLAY_SYNCHPLAYBACK / DSSTREAMPAUSE_SYNCHPLAYBACK
// (0x1470C3 walks the voices flagged 0x40 and starts them in one APU write).
void IDirectSound_SynchPlayback(Hp2Ctx& c) {
  std::lock_guard<std::mutex> lk(g_ds.mu);
  int n = 0;
  for (auto& [va, v] : g_ds.voices) {
    if (!v->sync_wait) continue;
    v->sync_wait = false;
    if (v->stream) v->paused = false;
    else v->playing = true;
    ++n;
  }
  DSLOG("SynchPlayback: %d voices", n);
  hp2_ret_stdcall(c, S_OK, 1);
}

// HRESULT IDirectSound_SetI3DL2Listener(pDS, LPCDSI3DL2LISTENER, dwApply)
void IDirectSound_SetI3DL2Listener(Hp2Ctx& c) {
  // DSI3DL2LISTENER: lRoom, lRoomHF, flRoomRolloffFactor, flDecayTime,
  // flDecayHFRatio, lReflections, flReflectionsDelay, lReverb, flReverbDelay,
  // flDiffusion, flDensity, flHFReference.
  if (uint32_t p = hp2_arg(c, 1)) {
    auto f = [&](int i) {
      uint32_t u = hp2_rd32(p + 4 * i);
      float x;
      std::memcpy(&x, &u, 4);
      return x;
    };
    auto l = [&](int i) { return int32_t(hp2_rd32(p + 4 * i)); };
    hp2_log("dsound: SetI3DL2Listener room %d roomHF %d rolloff %.2f decay %.2f s hfratio %.2f "
            "refl %d delay %.3f reverb %d delay %.3f diffusion %.0f density %.0f hfref %.0f "
            "(reverb not emulated)",
            l(0), l(1), f(2), f(3), f(4), l(5), f(6), l(7), f(8), f(9), f(10), f(11));
  }
  hp2_ret_stdcall(c, S_OK, 3);
}

// HRESULT IDirectSound_CreateSoundBuffer(pDS, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN)
void IDirectSound_CreateSoundBuffer(Hp2Ctx& c) {
  uint32_t out = 0, pp = hp2_arg(c, 2);
  uint32_t hr = create_buffer(hp2_arg(c, 1), &out);
  if (pp) hp2_wr32(pp, out);
  hp2_ret_stdcall(c, hr, 4);
}

// HRESULT DirectSoundCreateBuffer(LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*)
void DirectSoundCreateBuffer(Hp2Ctx& c) {
  uint32_t out = 0, pp = hp2_arg(c, 1);
  uint32_t hr = create_buffer(hp2_arg(c, 0), &out);
  if (pp) hp2_wr32(pp, out);
  hp2_ret_stdcall(c, hr, 2);
}

// HRESULT DirectSoundCreateStream(LPCDSSTREAMDESC, LPDIRECTSOUNDSTREAM*)
void DirectSoundCreateStream(Hp2Ctx& c) {
  uint32_t out = 0, pp = hp2_arg(c, 1);
  uint32_t hr = create_stream(hp2_arg(c, 0), &out);
  if (pp) hp2_wr32(pp, out);
  hp2_ret_stdcall(c, hr, 2);
}

// void DirectSoundDoWork(void)
void DirectSoundDoWork(Hp2Ctx& c) {
  deliver_completions(c);
  hp2_ret_cdecl(c, 0);
}

// ---- API: IDirectSoundBuffer -------------------------------------------------

// Runs fn on the buffer under the lock; unknown objects get E_FAIL.
template <typename F>
uint32_t with_voice(uint32_t va, const char* what, F fn) {
  std::lock_guard<std::mutex> lk(g_ds.mu);
  Voice* v = voice(va);
  if (!v) {
    hp2_log("dsound: %s on unknown object 0x%08x", what, va);
    return E_FAIL;
  }
  return fn(*v);
}

void IDirectSoundBuffer_Release(Hp2Ctx& c) {
  hp2_ret_stdcall(c, release_voice(hp2_arg(c, 0)), 1);
}

void IDirectSoundBuffer_SetVolume(Hp2Ctx& c) {
  int32_t vol = int32_t(hp2_arg(c, 1));
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "SetVolume", [&](Voice& v) {
                    v.volume = vol < -10000 ? -10000 : vol > 0 ? 0 : vol;
                    DSLOG("SetVolume 0x%08x %d", v.va, v.volume);
                    return S_OK;
                  }), 2);
}

void IDirectSoundBuffer_SetFrequency(Hp2Ctx& c) {
  uint32_t hz = hp2_arg(c, 1);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "SetFrequency", [&](Voice& v) {
                    v.freq = hz;
                    DSLOG("SetFrequency 0x%08x %u", v.va, hz);
                    return S_OK;
                  }), 2);
}

// HRESULT SetFilter(pBuffer, LPCDSFILTERDESC): the per-voice DSP filter.
void IDirectSoundBuffer_SetFilter(Hp2Ctx& c) {
  uint32_t d = hp2_arg(c, 1);
  if (d)
    DSLOG("SetFilter 0x%08x mode %u q %u coef %08x %08x %08x %08x (not emulated)", hp2_arg(c, 0),
          hp2_rd32(d), hp2_rd32(d + 4), hp2_rd32(d + 8), hp2_rd32(d + 12), hp2_rd32(d + 16),
          hp2_rd32(d + 20));
  hp2_ret_stdcall(c, S_OK, 2);
}

void IDirectSoundBuffer_SetMixBins(Hp2Ctx& c) {
  uint32_t mb = hp2_arg(c, 1);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "SetMixBins", [&](Voice& v) {
                    apply_mixbins(v, mb, true);
                    DSLOG("SetMixBins 0x%08x: %d bins, first %u", v.va, v.nbins, v.bins[0]);
                    return S_OK;
                  }), 2);
}

void IDirectSoundBuffer_SetMixBinVolumes(Hp2Ctx& c) {
  uint32_t mb = hp2_arg(c, 1);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "SetMixBinVolumes", [&](Voice& v) {
                    apply_mixbins(v, mb, false);
                    DSLOG("SetMixBinVolumes 0x%08x FL %d FR %d C %d BL %d BR %d", v.va, v.bin_vol[0],
                          v.bin_vol[1], v.bin_vol[2], v.bin_vol[4], v.bin_vol[5]);
                    return S_OK;
                  }), 2);
}

uint32_t region_end(const Voice& v) {
  uint32_t end = v.region_len ? v.region_start + v.region_len : v.data_bytes;
  return end > v.data_bytes ? v.data_bytes : end;
}

// HRESULT Play(pBuffer, dwReserved1, dwReserved2, dwFlags)
void IDirectSoundBuffer_Play(Hp2Ctx& c) {
  uint32_t flags = hp2_arg(c, 3);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "Play", [&](Voice& v) {
                    v.looping = flags & 1;  // DSBPLAY_LOOPING
                    double start = v.fmt.bytes_to_frames(v.region_start);
                    double end = v.fmt.bytes_to_frames(region_end(v));
                    if ((flags & 2) || v.pos < start || v.pos >= end) v.pos = start;  // FROMSTART
                    if (test_clock_frames() && (flags & 1) && !v.playing)
                      hp2_log("dsound: test clock: looping Play 0x%08x at vblank %llu, mixed %llu",
                              v.va, (unsigned long long)hp2_vblank_count(),
                              (unsigned long long)g_ds.mixed_frames);
                    v.sync_wait = flags & 4;  // DSBPLAY_SYNCHPLAYBACK
                    v.playing = !v.sync_wait;
                    DSLOG("Play 0x%08x flags=%u pos=%.0f data=%u", v.va, flags, v.pos, v.data_bytes);
                    return S_OK;
                  }), 4);
}

void IDirectSoundBuffer_Stop(Hp2Ctx& c) {
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "Stop", [&](Voice& v) {
                    v.playing = v.sync_wait = false;
                    return S_OK;
                  }), 1);
}

// HRESULT SetPlayRegion(pBuffer, dwPlayStart, dwPlayLength) (bytes; 0 = to the end)
void IDirectSoundBuffer_SetPlayRegion(Hp2Ctx& c) {
  uint32_t start = hp2_arg(c, 1), len = hp2_arg(c, 2);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "SetPlayRegion", [&](Voice& v) {
                    if (len ? start + len > v.data_bytes : start > v.data_bytes)
                      return DSERR_INVALIDCALL;
                    v.region_start = start;
                    v.region_len = len;
                    return S_OK;
                  }), 3);
}

// HRESULT GetStatus(pBuffer, LPDWORD): DSBSTATUS_PLAYING 1 | DSBSTATUS_LOOPING 4.
void IDirectSoundBuffer_GetStatus(Hp2Ctx& c) {
  uint32_t out = hp2_arg(c, 1);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "GetStatus", [&](Voice& v) {
                    if (out) hp2_wr32(out, v.playing ? (v.looping ? 5u : 1u) : 0u);
                    return S_OK;
                  }), 2);
}

// HRESULT GetCurrentPosition(pBuffer, LPDWORD pdwPlay, LPDWORD pdwWrite) (bytes)
void IDirectSoundBuffer_GetCurrentPosition(Hp2Ctx& c) {
  uint32_t pplay = hp2_arg(c, 1), pwrite = hp2_arg(c, 2);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "GetCurrentPosition", [&](Voice& v) {
                    // The game feeds its looping rings from a 10 ms poll: a long gap
                    // between polls of a playing ring means its feeder stalled
                    // (the ring is only ~50 ms).
                    if (v.playing && v.looping) {
                      uint64_t now = g_ds.mixed_frames;
                      if (v.poll_frame && now - v.poll_frame > kOutRate / 25)
                        hp2_log("dsound: ring 0x%08x not polled for %.0f ms (at %.2f s, ring %.0f ms)",
                                v.va, double(now - v.poll_frame) * 1000 / kOutRate,
                                double(now) / kOutRate,
                                double(v.fmt.bytes_to_frames(v.data_bytes)) * 1000 / v.fmt.rate);
                      v.poll_frame = now;
                    }
                    uint32_t play = v.fmt.frames_to_bytes(uint32_t(v.pos));
                    uint32_t write = play;
                    if (v.playing && v.data_bytes) {
                      uint32_t lead = v.fmt.frames_to_bytes(
                          std::max<uint32_t>(32, v.fmt.frames_per_block()));
                      write = (play + lead) % v.data_bytes;
                    }
                    if (pplay) hp2_wr32(pplay, play);
                    if (pwrite) hp2_wr32(pwrite, write);
                    DSLOG("GetCurrentPosition 0x%08x play %u write %u", v.va, play, write);
                    return S_OK;
                  }), 3);
}

void IDirectSoundBuffer_SetCurrentPosition(Hp2Ctx& c) {
  uint32_t bytes = hp2_arg(c, 1);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "SetCurrentPosition", [&](Voice& v) {
                    v.pos = v.fmt.bytes_to_frames(bytes > v.data_bytes ? 0 : bytes);
                    return S_OK;
                  }), 2);
}

// HRESULT SetBufferData(pBuffer, LPVOID pvBufferData, DWORD dwBufferBytes)
void IDirectSoundBuffer_SetBufferData(Hp2Ctx& c) {
  uint32_t data = hp2_arg(c, 1), bytes = hp2_arg(c, 2);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "SetBufferData", [&](Voice& v) {
                    v.data_va = data;
                    v.data_bytes = data ? bytes : 0;
                    v.region_start = v.region_len = 0;
                    if (v.pos >= v.fmt.bytes_to_frames(v.data_bytes)) v.pos = 0;
                    v.cache_block = -1;
                    if (!v.data_bytes) v.playing = false;
                    DSLOG("SetBufferData 0x%08x data=0x%08x bytes=%u", v.va, data, bytes);
                    return S_OK;
                  }), 3);
}

// ---- API: IDirectSoundStream -------------------------------------------------

// HRESULT IDirectSoundStream_Pause(pStream, DWORD dwPause): 0 RESUME, 1 PAUSE,
// 2 SYNCHPLAYBACK (paused until IDirectSound_SynchPlayback), 3 PAUSENOACTIVATE.
void IDirectSoundStream_Pause(Hp2Ctx& c) {
  uint32_t mode = hp2_arg(c, 1);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "Stream_Pause", [&](Voice& v) {
                    v.paused = mode != 0;
                    v.sync_wait = mode == 2;
                    DSLOG("Stream_Pause 0x%08x mode=%u", v.va, mode);
                    return S_OK;
                  }), 2);
}

// HRESULT IDirectSoundStream_FlushEx(pStream, REFERENCE_TIME rt, DWORD dwFlags)
// Flushed packets complete (FLUSHED, callbacks/events) before the call
// returns, as in the XDK; after that the stream holds no game packet.
void IDirectSoundStream_FlushEx(Hp2Ctx& c) {
  uint32_t hr = with_voice(hp2_arg(c, 0), "Stream_FlushEx", [&](Voice& v) {
    flush_stream(v);
    return S_OK;
  });
  deliver_completions(c);
  hp2_ret_stdcall(c, hr, 4);
}

// XMediaObject vtable (this in [esp+4]).
void Stream_AddRef(Hp2Ctx& c) {
  uint32_t r = with_voice(hp2_arg(c, 0), "Stream_AddRef", [&](Voice& v) { return uint32_t(++v.refs); });
  hp2_ret_stdcall(c, r, 1);
}
void Stream_Release(Hp2Ctx& c) { hp2_ret_stdcall(c, release_voice(hp2_arg(c, 0)), 1); }

// HRESULT GetInfo(this, LPXMEDIAINFO {dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead})
void Stream_GetInfo(Hp2Ctx& c) {
  uint32_t info = hp2_arg(c, 1);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "Stream_GetInfo", [&](Voice& v) {
                    if (info) {
                      hp2_wr32(info, 0x1 | 0x4);  // FIXED_SAMPLE_SIZE | INPUT_ASYNC
                      hp2_wr32(info + 4, v.fmt.block_align);
                      hp2_wr32(info + 8, 0);
                      hp2_wr32(info + 12, 0x1000);
                    }
                    return S_OK;
                  }), 2);
}

// HRESULT GetStatus(this, LPDWORD): DSSTREAMSTATUS_READY 1, PLAYING 0x10000,
// PAUSED 0x20000, STARVED 0x40000.
void Stream_GetStatus(Hp2Ctx& c) {
  uint32_t out = hp2_arg(c, 1);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "Stream_GetStatus", [&](Voice& v) {
                    uint32_t s = 0;
                    if (v.packets.size() < v.max_packets) s |= 1;
                    if (v.paused) s |= 0x20000;
                    else if (!v.packets.empty()) s |= 0x10000;
                    else if (v.started) s |= 0x40000;
                    if (out) hp2_wr32(out, s);
                    return S_OK;
                  }), 2);
}

// HRESULT Process(this, LPCXMEDIAPACKET pInput, LPCXMEDIAPACKET pOutput)
// XMEDIAPACKET {pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus,
//               hCompletionEvent|pContext, prtTimestamp}
void Stream_Process(Hp2Ctx& c) {
  uint32_t in = hp2_arg(c, 1);
  hp2_ret_stdcall(c, with_voice(hp2_arg(c, 0), "Stream_Process", [&](Voice& v) {
                    if (!in) return DSERR_INVALIDCALL;
                    Packet p;
                    p.buffer_va = hp2_rd32(in);
                    p.bytes = hp2_rd32(in + 4);
                    p.completed_size_va = hp2_rd32(in + 8);
                    p.status_va = hp2_rd32(in + 12);
                    p.context = hp2_rd32(in + 16);
                    if (p.status_va) hp2_wr32(p.status_va, kPacketPending);
                    if (p.completed_size_va) hp2_wr32(p.completed_size_va, 0);
                    v.packets.push_back(p);
                    v.started = true;
                    DSLOG("Stream_Process 0x%08x buf=0x%08x bytes=%u queued=%zu", v.va, p.buffer_va,
                          p.bytes, v.packets.size());
                    return S_OK;
                  }), 3);
}

// HRESULT Discontinuity(this): end of data; the queue just drains.
void Stream_Discontinuity(Hp2Ctx& c) { hp2_ret_stdcall(c, S_OK, 1); }

void Stream_Flush(Hp2Ctx& c) {
  uint32_t hr = with_voice(hp2_arg(c, 0), "Stream_Flush", [&](Voice& v) {
    flush_stream(v);
    return S_OK;
  });
  deliver_completions(c);
  hp2_ret_stdcall(c, hr, 1);
}

// ---- registration ------------------------------------------------------------

struct Api {
  uint32_t va;
  HostFn fn;
  const char* name;
};
// Identities and evidence: FUNCTIONS.md.
const Api kApi[] = {
    {0x14613F, DirectSoundCreate, "DirectSoundCreate"},
    {0x146186, DirectSoundCreateBuffer, "DirectSoundCreateBuffer"},
    {0x1461DD, DirectSoundCreateStream, "DirectSoundCreateStream"},
    {0x145390, DirectSoundDoWork, "DirectSoundDoWork"},
    {0x144641, IDirectSound_Release, "IDirectSound_Release"},
    {0x145210, IDirectSound_DownloadEffectsImage, "IDirectSound_DownloadEffectsImage"},
    {0x145237, IDirectSound_SynchPlayback, "IDirectSound_SynchPlayback"},
    {0x145C3F, IDirectSound_SetI3DL2Listener, "IDirectSound_SetI3DL2Listener"},
    {0x145F75, IDirectSound_CreateSoundBuffer, "IDirectSound_CreateSoundBuffer"},
    {0x144657, IDirectSoundBuffer_Release, "IDirectSoundBuffer_Release"},
    {0x14524F, IDirectSoundBuffer_SetVolume, "IDirectSoundBuffer_SetVolume"},
    {0x14526B, IDirectSoundBuffer_SetFilter, "IDirectSoundBuffer_SetFilter"},
    {0x145287, IDirectSoundBuffer_SetMixBins, "IDirectSoundBuffer_SetMixBins"},
    {0x1452A3, IDirectSoundBuffer_SetMixBinVolumes, "IDirectSoundBuffer_SetMixBinVolumes"},
    {0x1452BF, IDirectSoundBuffer_Play, "IDirectSoundBuffer_Play"},
    {0x1452E3, IDirectSoundBuffer_Stop, "IDirectSoundBuffer_Stop"},
    {0x1452FB, IDirectSoundBuffer_SetPlayRegion, "IDirectSoundBuffer_SetPlayRegion"},
    {0x14531B, IDirectSoundBuffer_GetStatus, "IDirectSoundBuffer_GetStatus"},
    {0x145337, IDirectSoundBuffer_GetCurrentPosition, "IDirectSoundBuffer_GetCurrentPosition"},
    {0x145357, IDirectSoundBuffer_SetCurrentPosition, "IDirectSoundBuffer_SetCurrentPosition"},
    {0x14569F, IDirectSoundBuffer_SetFrequency, "IDirectSoundBuffer_SetFrequency"},
    {0x145C5F, IDirectSoundBuffer_SetBufferData, "IDirectSoundBuffer_SetBufferData"},
    {0x145373, IDirectSoundStream_Pause, "IDirectSoundStream_Pause"},
    {0x145378, IDirectSoundStream_FlushEx, "IDirectSoundStream_FlushEx"},
};

// DSOUND entries that may run as-is: CRT static initialisers of DSOUND
// globals (listed in the XBE's initialiser table at 0x18E720).
const uint32_t kHarmless[] = {0x146472, 0x14647D, 0x14AE88, 0x14AE93};

template <uint32_t VA>
void Guard(Hp2Ctx& c) {
  static std::atomic<bool> seen{false};
  static const bool strict = env_on("HP2_DSOUND_STRICT");
  if (!seen.exchange(true) || strict) {
    if (strict)
      hp2_fatal("dsound: DSOUND entry 0x%06x is not HLE'd (called from 0x%08x)", VA,
                hp2_rd32(c.esp));
    hp2_log("dsound: WARNING: DSOUND entry 0x%06x is not HLE'd (called from 0x%08x); "
            "running the original (it may touch APU hardware)", VA, hp2_rd32(c.esp));
  }
  hp2_original(VA)(c);
}

struct Registrar {
  Registrar() {
    std::set<uint32_t> done;
    for (const Api& a : kApi) {
      hp2_register_override_at_start(a.va, a.fn, a.name);
      done.insert(a.va);
    }
    for (uint32_t va : kHarmless) done.insert(va);
#define DS_ENTRY(va) \
  if (!done.count(va)) hp2_register_override_at_start(va, Guard<va>, "dsound_guard");
#include "hle/dsound/dsound_entries.inc"
#undef DS_ENTRY
  }
} g_registrar;

const HostFn kStreamVtbl[7] = {Stream_AddRef, Stream_Release, Stream_GetInfo, Stream_GetStatus,
                               Stream_Process, Stream_Discontinuity, Stream_Flush};

}  // namespace
