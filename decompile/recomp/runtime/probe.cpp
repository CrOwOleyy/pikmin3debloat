// Sondes de diagnostic : interceptent des fonctions précises du jeu pour comparer leur résultat
// recompilé à une référence calculée à part. Les fonctions recompilées sont des symboles faibles
// (`sub_XXXX` appelle `__imp__sub_XXXX`) ; une définition forte ici les remplace.
//
// Activation : `--probe`. Sans l'option, l'appel est transmis tel quel.
#include "runtime.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <cstring>

namespace rt
{
bool g_probe = false;
}

extern "C" void __imp__sub_3463540(PPCContext& __restrict ctx, uint8_t* base);

namespace
{

float guestFloat(uint32_t addr)
{
    const uint32_t bits = rt::load32(addr);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

void readMatrix(uint32_t addr, float m[16])
{
    for (int i = 0; i < 16; i++)
        m[i] = guestFloat(addr + i * 4);
}

std::string row(const float* m)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%10.5g %10.5g %10.5g %10.5g", m[0], m[1], m[2], m[3]);
    return buf;
}

} // namespace

// MTX44Concat(a, b, out) : out = a x b, matrices 4x4 stockées par lignes.
void sub_3463540(PPCContext& __restrict ctx, uint8_t* base)
{
    // Seuls nous intéressent les deux appels qui construisent la matrice envoyée au GPU par
    // l'appelant 0x03130084 (retours 0x031300D4 et 0x031300E4).
    static std::atomic<uint32_t> calls { 0 };
    const uint32_t lr = uint32_t(ctx.lr);
    if (!rt::g_probe || (lr != 0x031300D4 && lr != 0x031300E4))
        return __imp__sub_3463540(ctx, base);
    const uint32_t n = calls++;
    if (n < 3000 || n >= 3012) // régime établi : on saute l'animation d'apparition
        return __imp__sub_3463540(ctx, base);

    const uint32_t pa = ctx.r3.u32, pb = ctx.r4.u32, pout = ctx.r5.u32;
    float a[16], b[16];
    readMatrix(pa, a);
    readMatrix(pb, b); // lus avant l'appel : `out` peut recouvrir `a` ou `b`

    __imp__sub_3463540(ctx, base);

    float out[16], ref[16];
    readMatrix(pout, out);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            double sum = 0;
            for (int k = 0; k < 4; k++)
                sum += double(a[i * 4 + k]) * double(b[k * 4 + j]);
            ref[i * 4 + j] = float(sum);
        }
    double maxErr = 0;
    for (int i = 0; i < 16; i++)
        maxErr = std::fmax(maxErr, std::fabs(double(out[i]) - double(ref[i])));
    RT_INFO("SONDE MTX44Concat lr=%08X (a=%08X b=%08X out=%08X) erreur max vs produit exact = %g", lr, pa, pb, pout, maxErr);
    for (int i = 0; i < 4; i++)
        RT_INFO("   a[%d] %s | b[%d] %s | out[%d] %s | ref %s", i, row(a + i * 4).c_str(), i,
            row(b + i * 4).c_str(), i, row(out + i * 4).c_str(), row(ref + i * 4).c_str());
}

extern "C" void __imp__sub_34536F4(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_3458580(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_3456B48(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_3451D18(PPCContext& __restrict ctx, uint8_t* base);

// DECAF_PROBE_PANE_ALPHA : suit un pane de texte dont l'opacité globale (+0x46) n'est pas 255 au
// dessin (texte des dialogues qui clignote) et journalise, dans l'ordre, ses calculs
// (sub_34536F4, Pane::CalculateMtx) et ses dessins (sub_3458580), avec la pile (thread).
namespace
{
std::atomic<uint32_t> sTracked { 0 }, sEvents { 0 };
uint64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// valeur de DECAF_PROBE_PANE_ALPHA = secondes d'attente avant de choisir le pane suivi
bool trackingOpen()
{
    static const uint64_t start = nowMs();
    static const uint64_t delay = strtoull(getenv("DECAF_PROBE_PANE_ALPHA"), nullptr, 10) * 1000;
    return nowMs() - start >= delay;
}
}

void sub_34536F4(PPCContext& __restrict ctx, uint8_t* base)
{
    static const bool enabled = getenv("DECAF_PROBE_PANE_ALPHA") != nullptr;
    if (!enabled)
        return __imp__sub_34536F4(ctx, base);
    const uint32_t pane = ctx.r3.u32, info = ctx.r4.u32, sp = ctx.r1.u32;
    const float factor = guestFloat(info + 0x10);
    const uint8_t f15 = rt::load8(info + 0x15), f17 = rt::load8(info + 0x17);
    __imp__sub_34536F4(ctx, base);
    const uint32_t tr = sTracked;
    const uint32_t p1 = tr ? rt::load32(tr + 0xC) : 0, p2 = p1 ? rt::load32(p1 + 0xC) : 0, p3 = p2 ? rt::load32(p2 + 0xC) : 0;
    if (tr && (pane == tr || pane == p1 || pane == p2 || pane == p3) && sEvents++ < 600)
        RT_INFO("SONDE t=%llu calc pane %08X alpha=%u glb=%u info=%08X facteur=%.4f f15=%u f17=%u flags44=%02X sp=%08X",
            (unsigned long long)nowMs(), pane, rt::load8(pane + 0x45), rt::load8(pane + 0x46), info, factor, f15, f17,
            rt::load8(pane + 0x44), sp);
}

void sub_3458580(PPCContext& __restrict ctx, uint8_t* base)
{
    static const bool enabled = getenv("DECAF_PROBE_PANE_ALPHA") != nullptr;
    if (enabled)
    {
        const uint32_t pane = ctx.r3.u32;
        if (false)
            sTracked = pane;
        if (pane == sTracked && sEvents++ < 400)
            RT_INFO("SONDE t=%llu draw pane %08X alpha=%u glb=%u info=%08X lr=%08X sp=%08X", (unsigned long long)nowMs(), pane,
                rt::load8(pane + 0x45), rt::load8(pane + 0x46), ctx.r4.u32, uint32_t(ctx.lr), ctx.r1.u32);
    }
    __imp__sub_3458580(ctx, base);
}

void sub_3456B48(PPCContext& __restrict ctx, uint8_t* base)
{
    static const bool enabled = getenv("DECAF_PROBE_PANE_ALPHA") != nullptr;
    if (enabled)
    {
        const uint32_t pane = ctx.r3.u32;
        if (false)
            sTracked = pane;
        if (pane == sTracked && sEvents++ < 400)
            RT_INFO("SONDE t=%llu draw2 pane %08X alpha=%u glb=%u info=%08X lr=%08X sp=%08X", (unsigned long long)nowMs(), pane,
                rt::load8(pane + 0x45), rt::load8(pane + 0x46), ctx.r4.u32, uint32_t(ctx.lr), ctx.r1.u32);
    }
    __imp__sub_3456B48(ctx, base);
}

void sub_3451D18(PPCContext& __restrict ctx, uint8_t* base)
{
    static const bool enabled = getenv("DECAF_PROBE_PANE_ALPHA") != nullptr;
    if (enabled)
    {
        const uint32_t pane = ctx.r3.u32;
        const uint8_t glb = rt::load8(pane + 0x46);
        if (!sTracked && (glb == 240 || glb == 255) && rt::load32(pane + 0xa4) && trackingOpen())
        {
            // pane avec matériau dont l'opacité globale alterne 240/255 : on attend de le voir aux deux
            static std::mutex lock;
            static std::unordered_map<uint32_t, uint8_t> seen;
            std::lock_guard guard(lock);
            auto [it, fresh] = seen.try_emplace(pane, glb);
            if (!fresh && it->second != glb)
                sTracked = pane;
        }
        if (pane == sTracked && sEvents++ < 400)
            RT_INFO("SONDE t=%llu draw3 pane %08X alpha=%u glb=%u info=%08X lr=%08X sp=%08X", (unsigned long long)nowMs(), pane,
                rt::load8(pane + 0x45), glb, ctx.r4.u32, uint32_t(ctx.lr), ctx.r1.u32);
    }
    __imp__sub_3451D18(ctx, base);
}
