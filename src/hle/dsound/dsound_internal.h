#pragma once
// DSound HLE internals (owner: Audio). The game sees XDK 5028 DirectSound
// objects as guest addresses; their real state lives here, on the host.
// dsound.cpp implements the API (guest calling conventions), mixer.cpp
// renders every playing voice into one stereo 48 kHz stream (SDL3 audio
// and/or a WAV file). One mutex (g_ds.mu) guards all voice state.

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "runtime/hp2_runtime.h"

namespace hp2ds {

// XDK formats (WAVEFORMATEX wFormatTag).
inline constexpr uint16_t kFmtPcm = 1;
inline constexpr uint16_t kFmtXboxAdpcm = 0x69;
inline constexpr uint32_t kAdpcmBlockBytes = 36;    // per channel
inline constexpr uint32_t kAdpcmBlockSamples = 64;  // per channel

inline constexpr uint32_t kOutRate = 48000;
inline constexpr int kMixBins = 32;  // DSMIXBIN_* ids (0..5 speakers, 6..9 XTLK, 10 I3DL2, FX sends)

// XMEDIAPACKET status values.
inline constexpr uint32_t kPacketSuccess = 0;
inline constexpr uint32_t kPacketPending = 0x8000000A;  // E_PENDING
inline constexpr uint32_t kPacketFlushed = 0x80004004;  // E_ABORT

struct Format {
  uint16_t tag = kFmtPcm;
  uint16_t channels = 1;
  uint32_t rate = 48000;
  uint16_t bits = 16;
  uint16_t block_align = 2;  // bytes per block (PCM: per frame)

  uint32_t frames_per_block() const { return tag == kFmtXboxAdpcm ? kAdpcmBlockSamples : 1; }
  // Byte offset <-> frame (sample per channel) conversions, block-granular.
  uint32_t bytes_to_frames(uint32_t b) const {
    return block_align ? b / block_align * frames_per_block() : 0;
  }
  uint32_t frames_to_bytes(uint32_t f) const { return f / frames_per_block() * block_align; }
};

struct Packet {  // XMEDIAPACKET as submitted to IDirectSoundStream::Process
  uint32_t buffer_va = 0, bytes = 0;
  uint32_t completed_size_va = 0, status_va = 0;
  uint32_t context = 0;  // hCompletionEvent, or pContext when the stream has a callback
};

struct Voice {
  uint32_t va = 0;  // guest object handed to the game
  uint64_t serial = 0;  // unique per object (VAs are recycled)
  bool stream = false;
  int refs = 1;
  Format fmt;
  uint32_t flags = 0;  // DSBUFFERDESC/DSSTREAMDESC dwFlags

  // Mix parameters, as the XDK programs the APU voice (mB / Hz). Every
  // (bin, channel) path gets volume - headroom + bin_vol[bin]; headroom
  // defaults to 600 for 2D voices, 0 for 3D / mix-in ones (0x145942).
  int32_t volume = 0;
  int32_t headroom = 0;
  uint32_t freq = 0;  // 0 = the format's rate
  // Mixbin routing: bins[i] receives channel i % channels (XDK order).
  int nbins = 0;
  uint8_t bins[8] = {};
  int32_t bin_vol[kMixBins] = {};  // by mixbin id

  // Buffer: data in guest memory (SetBufferData or our own allocation).
  uint32_t data_va = 0, data_bytes = 0;
  uint32_t region_start = 0, region_len = 0;  // play region, bytes (len 0 = to end)
  double pos = 0;  // frames from the start of the data
  bool playing = false, looping = false;
  bool sync_wait = false;  // Play/Pause with SYNCHPLAYBACK: starts at IDirectSound_SynchPlayback

  // Stream.
  uint32_t callback = 0, cb_context = 0, max_packets = 0;
  std::deque<Packet> packets;
  std::vector<float> pkt_pcm;  // decoded head packet, interleaved
  double pkt_pos = 0;          // frames into pkt_pcm
  bool pkt_decoded = false;
  bool paused = false;
  bool started = false;  // ever had data (STARVED vs idle)
  // Stats (logged at release): frames played, wall time of the first frame.
  uint64_t poll_frame = 0;  // mixer frame of the last GetCurrentPosition (stall check)
  uint64_t played_frames = 0;  // source frames consumed
  double first_play_s = -1, last_play_s = -1;

  // Level of the source (before gains), for the mixer's stats log.
  double src_sumsq = 0;
  uint64_t src_frames = 0;

  // ADPCM decode cache (buffers).
  int64_t cache_block = -1;
  std::vector<float> cache;  // one decoded block, interleaved
};

struct Completion {  // delivered to the guest by the worker thread
  uint32_t voice_va = 0;
  uint64_t voice_serial = 0;  // object VAs are reused; this identifies the stream
  Packet pkt;
  uint32_t status = kPacketSuccess;
  uint32_t completed = 0;
  uint32_t callback = 0, cb_context = 0;
};

struct State {
  std::mutex mu;
  std::map<uint32_t, Voice*> voices;  // by guest VA
  std::deque<Completion> done;        // guarded by mu
  uint64_t next_serial = 1;           // guarded by mu
  // Held while a completion is written into guest packet memory and its
  // callback/event runs, and by Release: once Release returns, nothing of that
  // stream touches guest memory again. Recursive: a callback may Release.
  std::recursive_mutex deliver_mu;
  uint64_t mixed_frames = 0;
};
extern State g_ds;

// HP2_TEST_CLOCK=frames (kernel test clock): the mixer runs 800 frames per
// vblank from a vblank callback instead of the wall clock, and packet
// completions are delivered there too; no worker thread.
bool test_clock_frames();

// dsound.cpp: writes finished packets' status into guest memory and runs
// their callbacks/events, on guest thread c.
void deliver_completions(Hp2Ctx& c);

// mixer.cpp
void mixer_start();  // idempotent; starts SDL audio / the mixer clock
// Decodes `bytes` of Xbox ADPCM / PCM at host pointer src into interleaved floats.
void decode(const Format& f, const uint8_t* src, uint32_t bytes, std::vector<float>& out);
// Drops the stream's queued packets, completing them as flushed (needs mu).
void flush_stream(Voice& v);

}  // namespace hp2ds
