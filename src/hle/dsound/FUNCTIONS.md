# DSOUND section (XDK 5028) — HLE coverage

Section `DSOUND` 0x1444E0..0x14E404, 352 lifted entries (`dsound_entries.inc`).
Only 24 are called from outside the section (Ghidra caller lists of `.text`,
`XMV`; `PlayMovie` 0x18D40). All 24 are replaced; every other entry is
guarded (logs once + runs the original; `HP2_DSOUND_STRICT=1` stops), except the
4 CRT static initialisers listed in the XBE initialiser table at 0x18E720.

Identification: each public entry is a stdcall C wrapper that converts the
interface pointer to the C++ object (`-8` = CDirectSound, `-0x1C` =
CDirectSoundBuffer) and calls the method; names come from argument count
(`ret N`), the method body and the callers' use. Confidence: H = body + callers
unambiguous, M = consistent but not proven. XbSymbolDatabase 5028 OOVPAs not
matched byte-wise yet (TODO; names agree with the XDK API).

| va | name | args | conf | evidence | status |
|---|---|---|---|---|---|
| 0x14613F | DirectSoundCreate | 3 | H | 0x117940 passes &[0x54B5BC]; writes obj+8 to *pp | HLE (singleton) |
| 0x146186 | DirectSoundCreateBuffer | 2 | H | DSBUFFERDESC with wfx at +0xC (0x117160); calls CreateSoundBuffer 0x145EBE | HLE |
| 0x1461DD | DirectSoundCreateStream | 2 | H | XMV 0x14EBC0: DSSTREAMDESC, callback 0x14EB6F, 2 packets | HLE |
| 0x145390 | DirectSoundDoWork | 0 | H | no args, XMV wait loops | HLE: delivers packet completions |
| 0x144641 | IDirectSound_Release | 1 | H | vtable+8 on obj-8 | HLE |
| 0x145210 | IDirectSound_DownloadEffectsImage | 5 | H | 0x117940: image 0x18A468, 0x3360 bytes | HLE: no DSP; empty desc |
| 0x145237 | IDirectSound_SynchPlayback | 1 | H | 0x1470C3 starts every voice flagged 0x40 (set by Pause mode 2); XMV calls it after Pause(2) | HLE |
| 0x145C3F | IDirectSound_SetI3DL2Listener | 3 | H | copies 12 dwords (DSI3DL2LISTENER) | HLE: no reverb |
| 0x145F75 | IDirectSound_CreateSoundBuffer | 4 | H | 0x117310 creates 6 PCM ring buffers | HLE |
| 0x144657 | IDirectSoundBuffer_Release | 1 | H | vtable+8 on obj-0x1C | HLE |
| 0x14524F | IDirectSoundBuffer_SetVolume | 2 | H | voice[+0x1C] = vol - headroom[+0x20] | HLE |
| 0x14526B | IDirectSoundBuffer_SetFilter | 2 | M | copies 6 dwords (DSFILTERDESC) into filter regs | no-op |
| 0x145287 | IDirectSoundBuffer_SetMixBins | 2 | H | DSMIXBINS {count, pairs} (0x117160) | HLE |
| 0x1452A3 | IDirectSoundBuffer_SetMixBinVolumes | 2 | H | writes vol per bin [+0x30+bin*4] | HLE |
| 0x1452BF | IDirectSoundBuffer_Play | 4 | H | (0,0,flags), flags 1 = LOOPING | HLE |
| 0x1452E3 | IDirectSoundBuffer_Stop | 1 | H | | HLE |
| 0x1452FB | IDirectSoundBuffer_SetPlayRegion | 3 | H | checks start+len <= size [+0xC4] | HLE |
| 0x14531B | IDirectSoundBuffer_GetStatus | 2 | H | writes PLAYING/LOOPING bits | HLE |
| 0x145337 | IDirectSoundBuffer_GetCurrentPosition | 3 | H | play/write cursors | HLE (real-time) |
| 0x145357 | IDirectSoundBuffer_SetCurrentPosition | 2 | H | | HLE |
| 0x14569F | IDirectSoundBuffer_SetFrequency | 2 | H | 0x117FE0 clamps 188..191983 Hz | HLE |
| 0x145C5F | IDirectSoundBuffer_SetBufferData | 3 | H | 0x117310 (ptr, bytes) | HLE |
| 0x145373 | IDirectSoundStream_Pause | 2 | H | thunk → 0x145009 → 0x148371 modes 0..3 | HLE |
| 0x145378 | IDirectSoundStream_FlushEx | 4 | M | (stream, rt64, flags); flags 0 → flush now | HLE |

Stream vtable (XMediaObject, as in DSOUND's own at 0x18A2B8): AddRef, Release,
GetInfo, GetStatus (+0xC, XMV checks 0x10000 PLAYING), Process, Discontinuity,
Flush — all HLE. Other vtable slots of HLE objects stop with a message.

Internal functions read for behaviour (not HLE'd, documented in NOTES.md):
0x145942 voice init (default headroom 600 / 0), 0x144841 mixbin routing with
default tables 0x14D404 / 0x14D944, 0x14495C volume register = vol − headroom.
