// gx2 : API graphique de la Wii U. Pas encore de rendu (GX2 -> Vulkan est le prochain
// gros chantier) : les commandes de dessin sont ignorées, mais tout ce que le jeu lit
// en retour (tailles de surfaces, état des swaps, vsync) est cohérent pour que la
// boucle de jeu avance.
#include "kernel.h"

#include <chrono>

using namespace rt;

namespace
{

// Octets par élément (pixel ou bloc 4x4 compressé) selon le format matériel (6 bits bas).
uint32_t bytesPerElement(uint32_t format, bool& compressed)
{
    compressed = false;
    switch (format & 0x3F)
    {
    case 0x01: case 0x02: return 1;                         // R8, R4G4
    case 0x05: case 0x07: case 0x08: case 0x0A: case 0x0B:  // R16, R8G8, R5G6B5, RGB5A1, RGBA4
    case 0x0C: return 2;
    case 0x1D: case 0x1E: case 0x1F: case 0x20: return 8;   // R32G32, R16G16B16A16
    case 0x22: case 0x23: return 16;                        // R32G32B32A32
    case 0x31: case 0x34: compressed = true; return 8;      // BC1, BC4
    case 0x32: case 0x33: case 0x35: compressed = true; return 16; // BC2, BC3, BC5
    default: return 4;
    }
}

uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// GX2Surface : dim +0, width +4, height +8, depth +0xC, mipLevels +0x10, format +0x14,
// aa +0x18, use +0x1C, imageSize +0x20, image +0x24, mipmapSize +0x28, mipmaps +0x2C,
// tileMode +0x30, swizzle +0x34, alignment +0x38, pitch +0x3C, mipLevelOffset[13] +0x40
void calcSurface(uint32_t s)
{
    bool compressed;
    const uint32_t bpe = bytesPerElement(load32(s + 0x14), compressed);
    const uint32_t width = std::max(load32(s + 0x4), 1u), height = std::max(load32(s + 0x8), 1u);
    const uint32_t depth = std::max(load32(s + 0xC), 1u);
    const uint32_t aa = 1u << std::min(load32(s + 0x18), 3u);
    uint32_t levels = std::max(load32(s + 0x10), 1u);
    uint32_t maxLevels = 1;
    for (uint32_t d = std::max(width, height); d > 1; d >>= 1)
        maxLevels++;
    levels = std::min(levels, maxLevels);
    store32(s + 0x10, levels);
    if (load32(s + 0x30) == 0)
        store32(s + 0x30, (load32(s + 0x0) || load32(s + 0x18) || (load32(s + 0x1C) & 4)) ? 4 : 1);

    auto levelSize = [&](uint32_t level, uint32_t& pitch) {
        uint32_t w = std::max(width >> level, 1u), h = std::max(height >> level, 1u);
        if (compressed)
        {
            w = (w + 3) / 4;
            h = (h + 3) / 4;
        }
        pitch = alignUp(w, 64);
        return alignUp(pitch * alignUp(h, 64) * bpe * depth * aa, 0x2000);
    };

    uint32_t pitch0;
    const uint32_t imageSize = levelSize(0, pitch0);
    uint32_t mipSize = 0;
    for (uint32_t l = 1; l < 14; l++)
        store32(s + 0x40 + (l - 1) * 4, 0);
    for (uint32_t l = 1; l < levels; l++)
    {
        uint32_t p;
        if (l > 1)
            store32(s + 0x40 + (l - 1) * 4, mipSize);
        mipSize += levelSize(l, p);
    }
    store32(s + 0x40, levels > 1 ? imageSize : 0);
    store32(s + 0x20, imageSize);
    store32(s + 0x28, mipSize);
    store32(s + 0x38, 0x2000);
    store32(s + 0x3C, compressed ? pitch0 * 4 : pitch0);
}

uint32_t s_swapCount = 0;
uint64_t s_lastFlip = 0, s_lastVsync = 0;
auto s_nextVsync = std::chrono::steady_clock::now();

} // namespace

HLE_FUNC(gx2, GX2Init) { RT_INFO("GX2Init (rendu non implémenté : écran noir)"); }
HLE_FUNC(gx2, GX2TempGetGPUVersion) { RET_U32(2); }
HLE_FUNC(gx2, GX2CalcSurfaceSizeAndAlignment) { calcSurface(ARG_U32(3)); }

// void GX2CalcTVSize(renderMode, format, bufferingMode, uint32_t* size, BOOL* scaleNeeded)
HLE_FUNC(gx2, GX2CalcTVSize)
{
    store32(ARG_U32(6), 1920 * 1080 * 4 * std::max(ARG_U32(5), 2u));
    store32(ARG_U32(7), 0);
}
HLE_FUNC(gx2, GX2CalcDRCSize)
{
    store32(ARG_U32(6), 854 * 480 * 4 * std::max(ARG_U32(5), 2u));
    store32(ARG_U32(7), 0);
}

// void GX2CalcColorBufferAuxInfo(GX2ColorBuffer*, uint32_t* size, uint32_t* align)
HLE_FUNC(gx2, GX2CalcColorBufferAuxInfo)
{
    store32(ARG_U32(4), 0x10000);
    store32(ARG_U32(5), 0x800);
}
HLE_FUNC(gx2, GX2CalcDepthBufferHiZInfo)
{
    store32(ARG_U32(4), 0x10000);
    store32(ARG_U32(5), 0x800);
}

HLE_FUNC(gx2, GX2CalcFetchShaderSizeEx) { RET_U32(0x40 + ARG_U32(3) * 0x10); }
// void GX2InitFetchShaderEx(GX2FetchShader*, void* program, uint32_t count, attribs*, type, tess)
HLE_FUNC(gx2, GX2InitFetchShaderEx)
{
    const uint32_t fs = ARG_U32(3);
    store32(fs + 0x0, ARG_U32(7));
    store32(fs + 0x8, 0x40 + ARG_U32(5) * 0x10);
    store32(fs + 0xC, ARG_U32(4));
    store32(fs + 0x10, ARG_U32(5));
}
HLE_FUNC(gx2, GX2CalcGeometryShaderInputRingBufferSize) { RET_U32(0x40000); }
HLE_FUNC(gx2, GX2CalcGeometryShaderOutputRingBufferSize) { RET_U32(0x40000); }

HLE_FUNC(gx2, GX2DrawDone) { RET_U32(1); }

HLE_FUNC(gx2, GX2SwapScanBuffers)
{
    if (++s_swapCount % 60 == 1)
        RT_INFO("image %u présentée", s_swapCount);
    s_lastFlip = kernel::systemTicks();
}

HLE_FUNC(gx2, GX2WaitForVsync)
{
    s_nextVsync += std::chrono::microseconds(16667);
    const auto now = std::chrono::steady_clock::now();
    if (s_nextVsync < now)
        s_nextVsync = now;
    std::this_thread::sleep_until(s_nextVsync);
    s_lastVsync = kernel::systemTicks();
}

// void GX2GetSwapStatus(uint32_t* swapCount, uint32_t* flipCount, OSTime* lastFlip, OSTime* lastVsync)
HLE_FUNC(gx2, GX2GetSwapStatus)
{
    if (ARG_U32(3)) store32(ARG_U32(3), s_swapCount);
    if (ARG_U32(4)) store32(ARG_U32(4), s_swapCount);
    if (ARG_U32(5)) store64(ARG_U32(5), s_lastFlip);
    if (ARG_U32(6)) store64(ARG_U32(6), s_lastVsync);
}

HLE_FUNC(gx2, GX2SampleTopGPUCycle) { store64(ARG_U32(3), kernel::systemTicks()); }
HLE_FUNC(gx2, GX2SampleBottomGPUCycle) { store64(ARG_U32(3), kernel::systemTicks()); }
HLE_FUNC(gx2, GX2GPUTimeToCPUTime) { RET_U64((uint64_t(ARG_U32(3)) << 32) | ARG_U32(4)); }

// Listes d'affichage : le jeu enregistre ses commandes dans des tampons ; sans GPU on
// ne fait que suivre le tampon courant.
namespace
{
uint32_t s_dlBuffer = 0, s_dlSize = 0;
}
HLE_FUNC(gx2, GX2BeginDisplayListEx)
{
    s_dlBuffer = ARG_U32(3);
    s_dlSize = ARG_U32(4);
}
HLE_FUNC(gx2, GX2EndDisplayList)
{
    s_dlBuffer = 0;
    RET_U32(0);
}
// BOOL GX2GetCurrentDisplayList(void** list, uint32_t* size)
HLE_FUNC(gx2, GX2GetCurrentDisplayList)
{
    if (ARG_U32(3)) store32(ARG_U32(3), s_dlBuffer);
    if (ARG_U32(4)) store32(ARG_U32(4), s_dlSize);
    RET_U32(s_dlBuffer ? 1 : 0);
}

// Appels « set state » / dessin : sans effet tant qu'il n'y a pas de rendu.
#define GX2_NOP(name) HLE_FUNC(gx2, name) {}
GX2_NOP(GX2CallDisplayList)
GX2_NOP(GX2DirectCallDisplayList)
GX2_NOP(GX2ClearBuffersEx)
GX2_NOP(GX2ClearColor)
GX2_NOP(GX2ClearDepthStencilEx)
GX2_NOP(GX2CopyColorBufferToScanBuffer)
GX2_NOP(GX2CopySurface)
GX2_NOP(GX2DrawEx)
GX2_NOP(GX2DrawIndexedEx)
GX2_NOP(GX2ExpandDepthBuffer)
GX2_NOP(GX2Flush)
GX2_NOP(GX2Invalidate)
GX2_NOP(GX2SetContextState)
GX2_NOP(GX2SetupContextStateEx)
GX2_NOP(GX2SetTVBuffer)
GX2_NOP(GX2SetTVEnable)
GX2_NOP(GX2SetTVScale)
GX2_NOP(GX2SetDRCBuffer)
GX2_NOP(GX2SetDRCEnable)
GX2_NOP(GX2SetDRCScale)
GX2_NOP(GX2SetSwapInterval)
GX2_NOP(GX2SetDebugMode)
#undef GX2_NOP
