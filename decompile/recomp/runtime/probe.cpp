// Sondes de diagnostic : interceptent des fonctions précises du jeu pour comparer leur résultat
// recompilé à une référence calculée à part. Les fonctions recompilées sont des symboles faibles
// (`sub_XXXX` appelle `__imp__sub_XXXX`) ; une définition forte ici les remplace.
//
// Activation : `--probe`. Sans l'option, l'appel est transmis tel quel.
#include "runtime.h"

#include <atomic>
#include <cmath>
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
