#pragma once
// Xbox D3D8 (XDK 5028) data layouts and enums the HLE reads from guest
// memory. Sources: the game's own D3D section (disassembly, see
// FUNCTIONS.md) and the 5028 tables documented by Cxbx-Reloaded/XbSymbolDatabase
// (render-state ordinals: 4627+ layout without D3DRS_MULTISAMPLETYPE).

#include <cstdint>

namespace hp2d3d {

// ---- D3D lib globals in the D3D section (5028 build of this XBE) ---------
inline constexpr uint32_t kRenderStateVa = 0x1405F0;   // D3D__RenderState[166]
inline constexpr uint32_t kTextureStateVa = 0x1403F0;  // D3D__TextureState[4][32]
inline constexpr uint32_t kTextureStageStride = 0x80;
inline constexpr uint32_t kDirtyFlagsVa = 0x1403E8;    // D3D__DirtyFlags
inline constexpr uint32_t kIndexDataVa = 0x1403E4;     // D3D__IndexData (read by the game)
inline constexpr uint32_t kDeviceVa = 0x140890;        // g_Device (CDevice)
inline constexpr uint32_t kDevicePtrVa = 0x140888;     // g_pDevice

// ---- render states (index into D3D__RenderState) ---------------------------
enum RS : uint32_t {
  RS_PSALPHAINPUTS0 = 0,
  RS_PSFINALCOMBINERINPUTSABCD = 8,
  RS_PSFINALCOMBINERINPUTSEFG = 9,
  RS_PSCONSTANT0_0 = 10,
  RS_PSCONSTANT1_0 = 18,
  RS_PSALPHAOUTPUTS0 = 26,
  RS_PSRGBINPUTS0 = 34,
  RS_PSCOMPAREMODE = 42,
  RS_PSFINALCOMBINERCONSTANT0 = 43,
  RS_PSFINALCOMBINERCONSTANT1 = 44,
  RS_PSRGBOUTPUTS0 = 45,
  RS_PSCOMBINERCOUNT = 53,
  RS_PSTEXTUREMODES_RESERVED = 54,
  RS_PSDOTMAPPING = 55,
  RS_PSINPUTTEXTURE = 56,
  RS_ZFUNC = 57,
  RS_ALPHAFUNC = 58,
  RS_ALPHABLENDENABLE = 59,
  RS_ALPHATESTENABLE = 60,
  RS_ALPHAREF = 61,
  RS_SRCBLEND = 62,
  RS_DESTBLEND = 63,
  RS_ZWRITEENABLE = 64,
  RS_DITHERENABLE = 65,
  RS_SHADEMODE = 66,
  RS_COLORWRITEENABLE = 67,
  RS_STENCILZFAIL = 68,
  RS_STENCILPASS = 69,
  RS_STENCILFUNC = 70,
  RS_STENCILREF = 71,
  RS_STENCILMASK = 72,
  RS_STENCILWRITEMASK = 73,
  RS_BLENDOP = 74,
  RS_BLENDCOLOR = 75,
  RS_SWATHWIDTH = 76,
  RS_POLYGONOFFSETZSLOPESCALE = 77,
  RS_POLYGONOFFSETZOFFSET = 78,
  RS_POINTOFFSETENABLE = 79,
  RS_WIREFRAMEOFFSETENABLE = 80,
  RS_SOLIDOFFSETENABLE = 81,
  RS_DEPTHCLIPCONTROL = 82,
  RS_STIPPLEENABLE = 83,
  RS_FOGENABLE = 92,
  RS_FOGTABLEMODE = 93,
  RS_FOGSTART = 94,
  RS_FOGEND = 95,
  RS_FOGDENSITY = 96,
  RS_RANGEFOGENABLE = 97,
  RS_WRAP0 = 98,
  RS_LIGHTING = 102,
  RS_SPECULARENABLE = 103,
  RS_LOCALVIEWER = 104,
  RS_COLORVERTEX = 105,
  RS_SPECULARMATERIALSOURCE = 110,
  RS_DIFFUSEMATERIALSOURCE = 111,
  RS_AMBIENTMATERIALSOURCE = 112,
  RS_EMISSIVEMATERIALSOURCE = 113,
  RS_BACKAMBIENT = 114,
  RS_AMBIENT = 115,
  RS_POINTSIZE = 116,
  RS_POINTSIZE_MIN = 117,
  RS_POINTSPRITEENABLE = 118,
  RS_POINTSCALEENABLE = 119,
  RS_POINTSIZE_MAX = 123,
  RS_SWAPFILTER = 126,
  RS_PRESENTATIONINTERVAL = 127,
  RS_PSTEXTUREMODES = 136,
  RS_VERTEXBLEND = 137,
  RS_FOGCOLOR = 138,
  RS_FILLMODE = 139,
  RS_BACKFILLMODE = 140,
  RS_TWOSIDEDLIGHTING = 141,
  RS_NORMALIZENORMALS = 142,
  RS_ZENABLE = 143,
  RS_STENCILENABLE = 144,
  RS_STENCILFAIL = 145,
  RS_FRONTFACE = 146,
  RS_CULLMODE = 147,
  RS_TEXTUREFACTOR = 148,
  RS_ZBIAS = 149,
  RS_LOGICOP = 150,
  RS_EDGEANTIALIAS = 151,
  RS_MULTISAMPLEANTIALIAS = 152,
  RS_MULTISAMPLEMASK = 153,
  RS_MULTISAMPLEMODE = 154,
  RS_MULTISAMPLERENDERTARGETMODE = 155,
  RS_SHADOWFUNC = 156,
  RS_LINEWIDTH = 157,
  RS_SAMPLEALPHA = 158,
  RS_DXT1NOISEENABLE = 159,
  RS_YUVENABLE = 160,
  RS_OCCLUSIONCULLENABLE = 161,
  RS_STENCILCULLENABLE = 162,
  RS_ROPZCMPALWAYSREAD = 163,
  RS_ROPZREAD = 164,
  RS_DONOTCULLUNCOMPRESSED = 165,
  RS_COUNT = 166,
};

// ---- texture stage states (index into a stage's 32 dwords) ----------------
enum TSS : uint32_t {
  TSS_ADDRESSU = 0,
  TSS_ADDRESSV = 1,
  TSS_ADDRESSW = 2,
  TSS_MAGFILTER = 3,
  TSS_MINFILTER = 4,
  TSS_MIPFILTER = 5,
  TSS_MIPMAPLODBIAS = 6,
  TSS_MAXMIPLEVEL = 7,
  TSS_MAXANISOTROPY = 8,
  TSS_COLORKEYOP = 9,
  TSS_COLORSIGN = 10,
  TSS_ALPHAKILL = 11,
  TSS_COLOROP = 12,
  TSS_COLORARG0 = 13,
  TSS_COLORARG1 = 14,
  TSS_COLORARG2 = 15,
  TSS_ALPHAOP = 16,
  TSS_ALPHAARG0 = 17,
  TSS_ALPHAARG1 = 18,
  TSS_ALPHAARG2 = 19,
  TSS_RESULTARG = 20,
  TSS_TEXTURETRANSFORMFLAGS = 21,
  TSS_BUMPENVMAT00 = 22,
  TSS_BUMPENVMAT01 = 23,
  TSS_BUMPENVMAT11 = 24,
  TSS_BUMPENVMAT10 = 25,
  TSS_BUMPENVLSCALE = 26,
  TSS_BUMPENVLOFFSET = 27,
  TSS_TEXCOORDINDEX = 28,
  TSS_BORDERCOLOR = 29,
  TSS_COLORKEYCOLOR = 30,
  TSS_COUNT = 32,
};

// D3DTEXTUREOP (same numbers as PC D3D8)
enum TOP : uint32_t {
  TOP_DISABLE = 1, TOP_SELECTARG1, TOP_SELECTARG2, TOP_MODULATE, TOP_MODULATE2X,
  TOP_MODULATE4X, TOP_ADD, TOP_ADDSIGNED, TOP_ADDSIGNED2X, TOP_SUBTRACT, TOP_ADDSMOOTH,
  TOP_BLENDDIFFUSEALPHA, TOP_BLENDCURRENTALPHA, TOP_BLENDTEXTUREALPHA, TOP_BLENDFACTORALPHA,
  TOP_BLENDTEXTUREALPHAPM, TOP_PREMODULATE, TOP_MODULATEALPHA_ADDCOLOR,
  TOP_MODULATECOLOR_ADDALPHA, TOP_MODULATEINVALPHA_ADDCOLOR, TOP_MODULATEINVCOLOR_ADDALPHA,
  TOP_DOTPRODUCT3, TOP_MULTIPLYADD, TOP_LERP, TOP_BUMPENVMAP, TOP_BUMPENVMAPLUMINANCE,
};
// D3DTA_*
inline constexpr uint32_t TA_SELECTMASK = 0xF, TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2,
                          TA_TFACTOR = 3, TA_SPECULAR = 4, TA_TEMP = 5, TA_COMPLEMENT = 0x10,
                          TA_ALPHAREPLICATE = 0x20;

// ---- primitive types --------------------------------------------------------
enum Prim : uint32_t {
  PT_POINTLIST = 1, PT_LINELIST, PT_LINELOOP, PT_LINESTRIP, PT_TRIANGLELIST,
  PT_TRIANGLESTRIP, PT_TRIANGLEFAN, PT_QUADLIST, PT_QUADSTRIP, PT_POLYGON,
};

// ---- D3DResource header in guest memory ------------------------------------
// Common +0 (refcount 0..15, type 16..18, flags), Data +4 (physical address
// for D3D-created resources; CPU address = Data | 0x80000000), Lock +8;
// pixel containers add Format +0xC and Size +0x10; surfaces Parent +0x14.
inline constexpr uint32_t COMMON_REFCOUNT_MASK = 0xFFFF;
inline constexpr uint32_t COMMON_TYPE_MASK = 0x00070000;
inline constexpr uint32_t COMMON_TYPE_VERTEXBUFFER = 0x00000000;
inline constexpr uint32_t COMMON_TYPE_INDEXBUFFER = 0x00010000;
inline constexpr uint32_t COMMON_TYPE_PUSHBUFFER = 0x00020000;
inline constexpr uint32_t COMMON_TYPE_PALETTE = 0x00030000;
inline constexpr uint32_t COMMON_TYPE_TEXTURE = 0x00040000;
inline constexpr uint32_t COMMON_TYPE_SURFACE = 0x00050000;
inline constexpr uint32_t COMMON_TYPE_FIXUP = 0x00060000;
inline constexpr uint32_t COMMON_D3DCREATED = 0x01000000;
inline constexpr uint32_t COMMON_ISLOCKED = 0x02000010;  // Xbox D3DCOMMON_ISLOCKED

// Format dword of a pixel container.
inline constexpr uint32_t FMT_DMA_MASK = 0x3;
inline constexpr uint32_t FMT_CUBEMAP = 0x4;
inline constexpr uint32_t FMT_BORDER = 0x8;
inline uint32_t FmtDims(uint32_t f) { return (f >> 4) & 0xF; }
inline uint32_t FmtFormat(uint32_t f) { return (f >> 8) & 0xFF; }
inline uint32_t FmtLevels(uint32_t f) { return (f >> 16) & 0xF; }
inline uint32_t FmtUSizeLog(uint32_t f) { return (f >> 20) & 0xF; }
inline uint32_t FmtVSizeLog(uint32_t f) { return (f >> 24) & 0xF; }
inline uint32_t FmtPSizeLog(uint32_t f) { return (f >> 28) & 0xF; }
// Size dword (linear textures/surfaces only; 0 for swizzled ones).
inline uint32_t SizeWidth(uint32_t s) { return (s & 0xFFF) + 1; }
inline uint32_t SizeHeight(uint32_t s) { return ((s >> 12) & 0xFFF) + 1; }
inline uint32_t SizePitch(uint32_t s) { return (((s >> 24) & 0xFF) + 1) * 64; }

// X_D3DFORMAT values (bits 8..15 of the Format dword).
enum XFmt : uint32_t {
  XF_L8 = 0x00, XF_AL8 = 0x01, XF_A1R5G5B5 = 0x02, XF_X1R5G5B5 = 0x03, XF_A4R4G4B4 = 0x04,
  XF_R5G6B5 = 0x05, XF_A8R8G8B8 = 0x06, XF_X8R8G8B8 = 0x07, XF_P8 = 0x0B, XF_DXT1 = 0x0C,
  XF_DXT3 = 0x0E, XF_DXT5 = 0x0F, XF_LIN_A1R5G5B5 = 0x10, XF_LIN_R5G6B5 = 0x11,
  XF_LIN_A8R8G8B8 = 0x12, XF_LIN_L8 = 0x13, XF_LIN_R8B8 = 0x16, XF_LIN_G8B8 = 0x17,
  XF_A8 = 0x19, XF_A8L8 = 0x1A, XF_LIN_AL8 = 0x1B, XF_LIN_X1R5G5B5 = 0x1C,
  XF_LIN_A4R4G4B4 = 0x1D, XF_LIN_X8R8G8B8 = 0x1E, XF_LIN_A8 = 0x1F, XF_LIN_A8L8 = 0x20,
  XF_YUY2 = 0x24, XF_UYVY = 0x25, XF_L6V5U5 = 0x27, XF_V8U8 = 0x28, XF_R8B8 = 0x29,
  XF_D24S8 = 0x2A, XF_F24S8 = 0x2B, XF_D16 = 0x2C, XF_F16 = 0x2D, XF_LIN_D24S8 = 0x2E,
  XF_LIN_F24S8 = 0x2F, XF_LIN_D16 = 0x30, XF_LIN_F16 = 0x31, XF_L16 = 0x32,
  XF_V16U16 = 0x33, XF_LIN_L16 = 0x35, XF_LIN_V16U16 = 0x36, XF_LIN_L6V5U5 = 0x37,
  XF_R5G5B5A1 = 0x38, XF_R4G4B4A4 = 0x39, XF_A8B8G8R8 = 0x3A, XF_B8G8R8A8 = 0x3B,
  XF_R8G8B8A8 = 0x3C, XF_LIN_R5G5B5A1 = 0x3D, XF_LIN_R4G4B4A4 = 0x3E,
  XF_LIN_A8B8G8R8 = 0x3F, XF_LIN_B8G8R8A8 = 0x40, XF_LIN_R8G8B8A8 = 0x41,
};

struct XFmtInfo {
  uint8_t bpp;      // bits per pixel (4 for DXT1)
  bool linear;      // LIN_* (unnormalised texture coordinates on NV2A)
  bool compressed;  // DXTn (4x4 blocks)
  bool depth;
  const char* name;
};
const XFmtInfo& FormatInfo(uint32_t xfmt);

// ---- vertex shader declaration tokens (D3DVSD_*) ---------------------------
inline constexpr uint32_t VSD_TOKEN_NOP = 0, VSD_TOKEN_STREAM = 1, VSD_TOKEN_STREAMDATA = 2,
                          VSD_TOKEN_TESSELLATOR = 3, VSD_TOKEN_CONSTMEM = 4, VSD_TOKEN_EXT = 5,
                          VSD_TOKEN_END = 7;
// D3DVSDT_* vertex element types.
enum VSDT : uint32_t {
  VSDT_FLOAT1 = 0x12, VSDT_FLOAT2 = 0x22, VSDT_FLOAT3 = 0x32, VSDT_FLOAT4 = 0x42,
  VSDT_D3DCOLOR = 0x40, VSDT_SHORT2 = 0x25, VSDT_SHORT4 = 0x45, VSDT_NORMSHORT1 = 0x11,
  VSDT_NORMSHORT2 = 0x21, VSDT_NORMSHORT3 = 0x31, VSDT_NORMSHORT4 = 0x41,
  VSDT_NORMPACKED3 = 0x16, VSDT_SHORT1 = 0x15, VSDT_SHORT3 = 0x35, VSDT_PBYTE1 = 0x14,
  VSDT_PBYTE2 = 0x24, VSDT_PBYTE3 = 0x34, VSDT_PBYTE4 = 0x44, VSDT_FLOAT2H = 0x72,
  VSDT_NONE = 0x02,
};
// Bytes one element of type t takes in a vertex (0 = unknown).
uint32_t VsdtSize(uint32_t t);

// ---- NV2A / Xbox-GL enums stored in the render-state array -----------------
inline constexpr uint32_t CMP_NEVER = 0x200;  // ..ALWAYS 0x207
inline constexpr uint32_t STENCILOP_KEEP = 0x1E00, STENCILOP_ZERO = 0, STENCILOP_REPLACE = 0x1E01,
                          STENCILOP_INCRSAT = 0x1E02, STENCILOP_DECRSAT = 0x1E03,
                          STENCILOP_INVERT = 0x150A, STENCILOP_INCR = 0x8507,
                          STENCILOP_DECR = 0x8508;
inline constexpr uint32_t CULL_NONE = 0, CULL_CW = 0x900, CULL_CCW = 0x901;
inline constexpr uint32_t FILL_POINT = 0x1B00, FILL_WIREFRAME = 0x1B01, FILL_SOLID = 0x1B02;
inline constexpr uint32_t BLENDOP_ADD = 0x8006, BLENDOP_MIN = 0x8007, BLENDOP_MAX = 0x8008,
                          BLENDOP_SUBTRACT = 0x800A, BLENDOP_REVSUBTRACT = 0x800B;

}  // namespace hp2d3d
