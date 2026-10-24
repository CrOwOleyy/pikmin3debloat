// Sondes de diagnostic : interceptent des fonctions précises du jeu pour comparer leur résultat
// recompilé à une référence calculée à part. Les fonctions recompilées sont des symboles faibles
// (`sub_XXXX` appelle `__imp__sub_XXXX`) ; une définition forte ici les remplace.
//
// Activation : `--probe`. Sans l'option, l'appel est transmis tel quel.
#include "runtime.h"

#include <atomic>
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

// nw::lyt::Pane::CalculateMtx(DrawInfo&) : calcule l'opacité globale du pane (octet +0x46) =
// opacité propre (+0x45) × facteur de DrawInfo (+0x10, flottant) selon ses drapeaux (+0x15,
// +0x17). DECAF_PROBE_PANE_ALPHA : suit le premier pane dont l'opacité globale vaut 20 (texte
// des dialogues qui clignote).
void sub_34536F4(PPCContext& __restrict ctx, uint8_t* base)
{
    static const bool enabled = getenv("DECAF_PROBE_PANE_ALPHA") != nullptr;
    if (!enabled)
        return __imp__sub_34536F4(ctx, base);
    const uint32_t pane = ctx.r3.u32, info = ctx.r4.u32;
    const uint8_t before = rt::load8(pane + 0x46);
    const float factor = guestFloat(info + 0x10);
    const uint8_t f15 = rt::load8(info + 0x15), f17 = rt::load8(info + 0x17);
    __imp__sub_34536F4(ctx, base);
    static std::atomic<uint32_t> count { 0 };
    static std::mutex lock;
    static std::unordered_map<uint32_t, std::pair<int, int>> history; // pane -> (dernière opacité, sauts)
    const uint8_t after = rt::load8(pane + 0x46);
    bool alternating = false;
    {
        std::lock_guard guard(lock);
        auto [it, fresh] = history.try_emplace(pane, int(after), 0);
        if (!fresh && std::abs(int(after) - it->second.first) > 100)
            alternating = ++it->second.second >= 4;
        it->second.first = after;
    }
    if (alternating && count++ < 300)
    {
        char name[25] = {};
        for (int i = 0; i < 24; i++)
            name[i] = char(rt::load8(pane + 0x18 + i));
        for (auto& c : name)
            if (c && (c < 32 || c > 126)) c = '.';
        RT_INFO("SONDE pane %08X '%s' alpha=%u glb %u->%u info=%08X facteur=%.4f f15=%u f17=%u parent=%08X flags44=%02X",
            pane, name, rt::load8(pane + 0x45), before, after, info, factor, f15, f17, rt::load32(pane + 0xC), rt::load8(pane + 0x44));
    }
}
