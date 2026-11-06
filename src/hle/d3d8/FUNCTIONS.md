# D3D section functions replaced by the D3D8 HLE

48 entry points called from `.text` + 4 called by the XMV player = 52, all replaced
(`HP2_OVERRIDE` in `d3d8.cpp`). Nothing else in the D3D section is reachable: the
remaining ~180 lib functions are only called by these.

Confidence: **S** = XbSymbolDatabase 5028-era OOVPA signature matches at that address
(matcher: own script over the DB's pattern data, see `docs/handoff/render.md`), plus
disassembly; **D** = identified from disassembly + call sites (no DB pattern for this
build); calling convention always checked against the function's own `ret`.
Status: **run** = exercised by the phase-2/3 runs (boot, movies, menu, first level); **impl** = implemented,
not reached yet.

| VA | Name | Conv. | Conf. | Status | Notes |
|---|---|---|---|---|---|
| 0x133FA0 | Direct3DCreate8(SDKVersion) | std 1 | D | run | returns 1, like the lib |
| 0x130620 | D3D::SetPushBufferSize(size, kickoff) | std 2 | S | run | no push buffer: no-op |
| 0x130640 | Direct3D_CreateDevice(..., pPP, ppDevice) | std 6 | S | run | 640x480 LIN_A8R8G8B8 + D24S8; back/front/Z surfaces in contiguous memory; default render/texture states |
| 0x130720 | SetRenderState_Simple push stub | fastcall ecx=method, edx=value | D | run | 98 inlined sites; methods 0x300-0x35C mapped to their render state |
| 0x130B00 | SetRenderState_FogColor(v) | std 1 | S | run | RS[138] |
| 0x130B50 | SetRenderState_CullMode(v) | std 1 | D | run | RS[147]; cull = (mode != FRONTFACE) ? back : front |
| 0x130C00 | SetRenderState_NormalizeNormals(v) | std 1 | S | run | RS[142] |
| 0x1310F0 | SetTextureState_BumpEnv(stage, type, v) | std 3 | S | impl | TSS[stage][type] |
| 0x131160 | SetTextureState_BorderColor(stage, v) | std 2 | S | run | TSS[29] |
| 0x131BD0 | SetRenderState_ZEnable(v) | std 1 | S | run | RS[143] |
| 0x131C60 | SetRenderState_StencilEnable(v) | std 1 | D | run | RS[144] (stores 0x140830) |
| 0x132570 | CreateVertexShader(pDecl, pFunc, pHandle, usage) | std 4 | D | run | 264 calls; handle = HLE object \| 1; declaration parsed (streams, regs, skips, consts) |
| 0x1326B0 | SetVertexShaderConstant1 | fastcall ecx=reg(0..191), edx=data | S | run | |
| 0x132710 | SetVertexShaderConstant4 | fastcall | S | run | MMX in the lib (replaced, so no MMX needed) |
| 0x1328A0 | SetVertexShaderConstantNotInline | fastcall + std 1 (count) | S | run | count is in DWORDs (lib: rep movsd); game passes 8 for c9-c10 |
| 0x1328E0 | SetStreamSource(n, pVB, stride) | std 3 | D | run | |
| 0x132A50 | SetShaderConstantMode(mode) | std 1 | D | run | game sets 1 (192 constants, c-38/c-37 reserved) |
| 0x132CA0 | SetVertexShader(handle) | std 1 | S | run | game only uses programmable handles |
| 0x132E00 | CreateVertexBuffer2(length) | std 1 | S | run | contiguous memory |
| 0x132E50 | VertexBuffer_Lock2(pVB, flags) | std 2 | S | impl | Data \| 0x80000000 |
| 0x132EA0 | SetRenderTarget(pRT, pZ) | std 2 | S | run | GPU target keyed by surface Data |
| 0x132EC0 | SetTransform(state, pMatrix) | std 2 | D | run | stored (unused: no fixed-function vertex path in this game) |
| 0x132FD0 | SetMaterial(pMaterial) | std 1 | S | run | stored |
| 0x133000 | D3D::KickOffAndWaitForIdle() | thunk, no args | S | run | no-op |
| 0x133010 | BlockUntilVerticalBlank() | no args | D | run | `hp2_vblank_wait` (60 Hz pacer) |
| 0x133330 | GetBackBuffer2(index) | std 1 | D | impl | -1 = front buffer (game aliases its Data into texture 0x396DC0 every frame) |
| 0x133380 | GetRenderTarget2() | no args | D | run | AddRef'd |
| 0x1333A0 | GetDepthStencilSurface2() | no args | D | run | AddRef'd or 0 |
| 0x133520 | SetTexture(stage, pTex) | std 2 | D | run | |
| 0x1336D0 | SetIndices(pIndexBuffer, baseVertex) | std 2 | S | run | publishes IB Data in D3D__IndexData 0x1403E4: the game reads it inline to build pIndexData |
| 0x133A80 | CreatePixelShader(pDef, pHandle) | std 2 | S | run | 7 defs; object {1, 1, pDef, copy} like the lib |
| 0x133AD0 | SetPixelShader(handle) | std 1 | D | run | copies the def into RS[0..56] + RS[136] like the lib |
| 0x133CD0 | SetPixelShaderConstant(reg, data, n) | std 3 | S | run | packs to D3DCOLOR via PSC0/PSC1/final mappings |
| 0x134160 | Resource_Release(p) | std 1 | S | run | frees D3D-created data 4 frames later |
| 0x134240 | Resource_Register(p, base) | std 2 | S | run | Data += base (masked physical) |
| 0x134270 | Resource_BlockUntilNotBusy(p) | std 1 (thunk) | S | impl | draws copy their data: nothing to wait for |
| 0x134D70 | Swap(flags) | std 1 | S | run | back/front Data swap, frame submit, 60.000 Hz pacing, screenshots |
| 0x134F10 | CreateTexture2(w, h, d, levels, usage, fmt, type) | std 7 | S | run | Format/Size dwords as D3D builds them; type 4 volume, 5 cube |
| 0x134FD0 | Texture_GetSurfaceLevel2(tex, level) | std 2 | S | run | surface header with Parent |
| 0x135020 | Texture_LockRect(tex, level, pLocked, pRect, flags) | std 5 | S | run | |
| 0x135050 | Clear(n, rects, flags, color, z, stencil) | std 6 | S | run | |
| 0x135760 | InsertCallback(type, cb, ctx) | std 3 | S | run | callback runs at once (cdecl cb(ctx)) |
| 0x135E60 | DrawVerticesUP(prim, count, data, stride) | std 4 | D | run | |
| 0x135FC0 | DrawIndexedVerticesUP(prim, count, idx, data, stride) | std 5 | D | run | |
| 0x136100 | DrawVertices(prim, start, count) | std 3 | D | impl | |
| 0x1361A0 | DrawIndexedVertices(prim, count, idx) | std 3 | D | run | |
| 0x136520 | UpdateOverlay(pSurface, SrcRect, DstRect, EnableColorKey, ColorKey) | std 5 | D | run | XMV frames (YUY2) drawn into the displayed buffer; writes PVIDEO regs 0x8704/0x8920 in the lib. First identified as CopyRects: the movie loop's call (surface, &rect, &rect, 0, 0) and the PVIDEO writes settle it |
| 0x1366A0 | EnableOverlay(enable) | std 1 | S | run | no-op (XMV) |
| 0x133260 | GetDisplayMode(pMode) | std 1 | S | run | XMV |
| 0x133790 | GetDisplayFieldStatus(pStatus) | std 1 | S | run | XMV; progressive, vblank count |
| 0x13A5E0 | Surface_GetDesc(p, pDesc) | std 2 | S | run | XMV |
| 0x13A600 | Surface_LockRect(p, pLocked, pRect, flags) | std 4 | S | run | XMV |

Also overridden (game code, `movies.cpp`): **0x18D40 PlayMovie(path, skippable)** (cdecl) — skipped
with `HP2_SKIP_MOVIES=1`, otherwise the lifted original runs.

## Lib globals read at draw time
- `D3D__RenderState` 0x1405F0 (166 dwords, 5028 order: `xbox_d3d.h` enum RS), written by the
  game directly (inlined `SetRenderState_Simple` + store) and by the setters above.
- `D3D__TextureState` 0x1403F0, 4 stages x 0x80 bytes (enum TSS).
- `D3D__DirtyFlags` 0x1403E8: ignored (every draw reads the full state).
- `g_pDevice` 0x140888 = 0x140890 (set by CreateDevice; nothing reads the CDevice).
