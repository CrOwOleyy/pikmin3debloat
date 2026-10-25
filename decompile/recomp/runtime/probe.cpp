// Sondes de diagnostic : interceptent des fonctions précises du jeu pour comparer leur résultat
// recompilé à une référence calculée à part. Les fonctions recompilées sont des symboles faibles
// (`sub_XXXX` appelle `__imp__sub_XXXX`) ; une définition forte ici les remplace.
//
// Activation : `--probe`. Sans l'option, l'appel est transmis tel quel.
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
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

// DECAF_PROBE_SKIN=<adresse de palette> : entrées de sub_343E98C (palette d'os = matrice A de l'os
// × matrice B, écrite en petit-boutiste) pour le squelette qui écrit dans cette palette.
extern "C" void __imp__sub_343E98C(PPCContext& __restrict ctx, uint8_t* base);

void sub_343E98C(PPCContext& __restrict ctx, uint8_t* base)
{
    static const char* env = getenv("DECAF_PROBE_SKIN");
    const uint32_t self = ctx.r3.u32;
    __imp__sub_343E98C(ctx, base);
    if (!env)
        return;
    // Toutes les matrices monde (B) : échelle par axe comparée à celle de l'os 0
    {
        const uint32_t res = rt::load32(self), table = rt::load32(self + 0x10);
        const uint32_t count = rt::load32(res + 0x8) & 0xFFFF, listOff = rt::load32(res + 0x18);
        auto norm = [&](uint32_t m, int c) {
            const float x = guestFloat(m + c * 4), y = guestFloat(m + 16 + c * 4), z = guestFloat(m + 32 + c * 4);
            return std::sqrt(x * x + y * y + z * z);
        };
        static std::atomic<uint32_t> bad { 0 }, total { 0 };
        total++;
        const uint32_t m0 = table + (rt::load32(res + 0x18 + listOff) >> 16) * 0x30;
        const float ref = norm(m0, 0);
        for (uint32_t i = 0; i < count && i < 64; i++)
        {
            const uint32_t bone = rt::load32(res + 0x18 + listOff + i * 2) >> 16;
            const uint32_t m = table + bone * 0x30;
            const float n0 = norm(m, 0), n1 = norm(m, 1), n2 = norm(m, 2);
            if (std::fabs(n0 - ref) > 0.03f * ref || std::fabs(n1 - ref) > 0.03f * ref || std::fabs(n2 - ref) > 0.03f * ref)
            {
                if (bad++ % 2000 == 0)
                    RT_INFO("SKIN déformé : squelette %08X (%u os) os %u : échelles %.3f %.3f %.3f (réf %.3f) - %u/%u appels",
                        self, count, bone, n0, n1, n2, ref, bad.load(), total.load());
                break;
            }
        }
    }
    if (rt::load32(self + 0x18) != strtoul(env, nullptr, 16))
        return;
    static uint64_t last = 0;
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now - last < 1000)
        return;
    last = now;
    const uint32_t res = rt::load32(self);
    const uint32_t joints = rt::load32(self + 8), table = rt::load32(self + 0x10);
    const uint32_t count = rt::load32(res + 0x8) & 0xFFFF, listOff = rt::load32(res + 0x18);
    RT_INFO("SKIN %08X res %08X joints %08X table %08X locaux %08X n %u", self, res, joints, table, rt::load32(self + 0xC), count);
    for (uint32_t i = 0; i < count && i < 16; i++)
    {
        const uint32_t bone = rt::load32(res + 0x18 + listOff + i * 2) >> 16;
        float a[12], b[12];
        for (int k = 0; k < 12; k++)
        {
            a[k] = guestFloat(joints + bone * 0x70 + 0x40 + k * 4);
            b[k] = guestFloat(table + bone * 0x30 + k * 4);
        }
        RT_INFO("SKIN  os %u (%u) A %s | %s | %s   B %s | %s | %s", i, bone, row(a).c_str(), row(a + 4).c_str(),
            row(a + 8).c_str(), row(b).c_str(), row(b + 4).c_str(), row(b + 8).c_str());
        float c[12];
        for (int k = 0; k < 12; k++)
        {
            uint32_t bits;
            memcpy(&bits, base + rt::load32(self + 0x18) + i * 0x30 + k * 4, 4); // petit-boutiste
            memcpy(&c[k], &bits, 4);
        }
        RT_INFO("SKIN  os %u palette %s | %s | %s", i, row(c).c_str(), row(c + 4).c_str(), row(c + 8).c_str());
    }
    // Transformations locales (animation) : table *(self+0xC), 0x40 octets par os (drapeaux, échelle,
    // matrice 3x4 en +0x10) ; articulation j : +4 os, +6 parent.
    const uint32_t locals = rt::load32(self + 0xC);
    const uint32_t joints2 = rt::load32(self + 8);
    const uint32_t total = rt::load32(self + 0x2C) >> 16;
    for (uint32_t j = 0; j < total && j < 32; j++)
    {
        const uint32_t jb = joints2 + j * 0x70;
        const uint32_t bone = rt::load32(jb + 4) >> 16, parent = rt::load32(jb + 4) & 0xFFFF;
        const uint32_t l = locals + bone * 0x40;
        float m[12];
        for (int k = 0; k < 12; k++)
            m[k] = guestFloat(l + 0x10 + k * 4);
        RT_INFO("SKIN  art %u os %u parent %u drapeaux %08X échelle %g %g %g local %s | %s | %s", j, bone, parent,
            rt::load32(l), guestFloat(l + 4), guestFloat(l + 8), guestFloat(l + 12), row(m).c_str(), row(m + 4).c_str(),
            row(m + 8).c_str());
    }
}

// DECAF_PROBE_FRAME : boucle principale du framework (sub_31B3EB8, this = r3). Journalise une fois
// les méthodes virtuelles appelées (+0x30 : table ; entrées 0x6C, 0xDC, 0xE4, 0xEC, 0xF4).
extern "C" void __imp__sub_31B3EB8(PPCContext& __restrict ctx, uint8_t* base);

void sub_31B3EB8(PPCContext& __restrict ctx, uint8_t* base)
{
    static const bool enabled = getenv("DECAF_PROBE_FRAME") != nullptr;
    static bool logged = false;
    const uint32_t self = ctx.r3.u32;
    if (enabled && !logged)
    {
        logged = true;
        const uint32_t vt = rt::load32(self + 0x30);
        RT_INFO("FRAME this %08X vtable %08X : 6C %08X DC %08X E4 %08X EC %08X F4 %08X", self, vt,
            rt::load32(vt + 0x6C), rt::load32(vt + 0xDC), rt::load32(vt + 0xE4), rt::load32(vt + 0xEC), rt::load32(vt + 0xF4));
    }
    __imp__sub_31B3EB8(ctx, base);
}

// Garde des registres non volatils (r14-r31) autour des fonctions appelées par la boucle du
// framework : une fonction qui les rend modifiés a restauré une pile écrasée (plantage « appel
// indirect vers 0 », lr=0x031B3FF8). Actif par défaut (coût négligeable) ; journalise la première
// corruption avec la pile hôte et la pile invitée autour de r1.
namespace
{
void checkNonVolatile(const char* name, const uint32_t (&before)[18], PPCContext& ctx, uint32_t sp)
{
    static std::atomic<int> reports { 0 };
    const PPCRegister* regs[18] = { &ctx.r14, &ctx.r15, &ctx.r16, &ctx.r17, &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21,
        &ctx.r22, &ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31 };
    for (int i = 0; i < 18; i++)
        if (regs[i]->u32 != before[i] && reports++ < 4)
        {
            RT_ERROR("GARDE : %s a rendu r%d = %08X au lieu de %08X (r1 = %08X, ctx.r1 = %08X)", name, 14 + i,
                regs[i]->u32, before[i], sp, ctx.r1.u32);
            std::string dump;
            for (uint32_t a = sp - 0x100; a < sp + 0x40; a += 4)
            {
                char v[16];
                snprintf(v, sizeof(v), "%s%08X", (a - sp + 0x100) % 32 ? " " : "\n  ", rt::load32(a));
                dump += v;
            }
            RT_ERROR("GARDE : pile invitée %08X..%08X :%s", sp - 0x100, sp + 0x40, dump.c_str());
            RT_ERROR("GARDE : pile hôte %s", rt::hostStack().c_str());
            return;
        }
}
}

#define GUARDED(addr)                                                                        \
    extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);            \
    void sub_##addr(PPCContext& __restrict ctx, uint8_t* base)                               \
    {                                                                                        \
        const PPCRegister* regs[18] = { &ctx.r14, &ctx.r15, &ctx.r16, &ctx.r17, &ctx.r18,    \
            &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22, &ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26,  \
            &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31 };                              \
        uint32_t before[18];                                                                 \
        for (int i = 0; i < 18; i++)                                                         \
            before[i] = regs[i]->u32;                                                        \
        const uint32_t sp = ctx.r1.u32;                                                      \
        __imp__sub_##addr(ctx, base);                                                        \
        checkNonVolatile("sub_" #addr, before, ctx, sp);                                     \
    }

GUARDED(321222C)
GUARDED(31B4050)
GUARDED(31B41A4)
GUARDED(31B42FC)
GUARDED(3217280)
GUARDED(3238B64)
GUARDED(31B7864)
GUARDED(31B78D0)
GUARDED(31CFB74)

// DECAF_PROBE_SECTION : création d'une section du jeu par son nom (sub_317DA24, r4 = paramètres,
// nom de la section en SafeString à +36). Journalise les chaînes trouvées dans les paramètres.
extern "C" void __imp__sub_317DA24(PPCContext& __restrict ctx, uint8_t* base);

void sub_317DA24(PPCContext& __restrict ctx, uint8_t* base)
{
    static const bool enabled = getenv("DECAF_PROBE_SECTION") != nullptr;
    if (enabled)
    {
        const uint32_t args = ctx.r4.u32;
        std::string text;
        for (uint32_t off = 0; off < 0x100; off += 4)
        {
            const uint32_t v = rt::load32(args + off);
            char word[64];
            snprintf(word, sizeof(word), " +%02X=%08X", off, v);
            text += word;
            if (v >= 0x10000000 && v < 0x50000000 && base[v] >= 0x20 && base[v] < 0x7F)
                text += "\"" + std::string(reinterpret_cast<const char*>(base + v), strnlen(reinterpret_cast<const char*>(base + v), 40)) + "\"";
        }
        RT_INFO("SECTION r5=%08X lr=%08X%s", ctx.r5.u32, ctx.lr, text.c_str());
        const uint32_t param = rt::load32(args + 0x0C);
        std::string ptext;
        for (uint32_t off = 0; off < 0x200 && param; off += 4)
        {
            const uint32_t v = rt::load32(param + off);
            char word[32];
            snprintf(word, sizeof(word), " +%02X=%08X", off, v);
            ptext += word;
            if (base[param + off] >= 0x41 && base[param + off] < 0x7B)
                ptext += "'" + std::string(reinterpret_cast<const char*>(base + param + off), strnlen(reinterpret_cast<const char*>(base + param + off), 24)) + "'";
        }
        RT_INFO("SECTION param %08X%s", param, ptext.c_str());
    }
    __imp__sub_317DA24(ctx, base);
}

// DECAF_COUNT_CALLS : compte les appels de quelques fonctions (méthodes du layout RadarIcon_00,
// vtable 0x1032F970 ; méthodes héritées comptées pour cet objet seulement) et journalise aux
// puissances de 10.
std::atomic<uint32_t> g_radarThis { 0 }; // objet RadarIcon_00 (this vu par sub_2F393EC)

#define COUNTED_IF_ALWAYS(addr, cond, always)                                                              \
    extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);            \
    void sub_##addr(PPCContext& __restrict ctx, uint8_t* base)                               \
    {                                                                                        \
        static const bool enabled = getenv("DECAF_COUNT_CALLS") != nullptr || always;               \
        static std::atomic<uint32_t> calls { 0 };                                            \
        if (enabled && (cond))                                                               \
        {                                                                                    \
            const uint32_t n = ++calls;                                                      \
            if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000)                    \
                RT_INFO("APPELS sub_" #addr " : %u (this %08X, lr %08X, vtable+48 %08X)", n, ctx.r3.u32, uint32_t(ctx.lr), rt::load32(ctx.r3.u32 + 48)); \
        }                                                                                    \
        __imp__sub_##addr(ctx, base);                                                        \
    }
#define COUNTED_IF(addr, cond) COUNTED_IF_ALWAYS(addr, cond, false)
#define COUNTED(addr) COUNTED_IF(addr, true)
#define COUNTED_THIS(addr) COUNTED_IF(addr, ctx.r3.u32 == g_radarThis.load())

COUNTED(2F36C08)
COUNTED(2F36C60)
COUNTED(2F39344)
COUNTED(2F40E30)
COUNTED(2F40E34)
COUNTED(2F3DDE4)
COUNTED(2F41308)
COUNTED_IF(2F393EC, (g_radarThis.store(ctx.r3.u32), true))
COUNTED_THIS(30B47C4)
COUNTED_THIS(30B432C)
COUNTED_THIS(30B335C)
COUNTED_THIS(30B339C)
COUNTED_THIS(30B2BD0)
COUNTED_THIS(30B2D68)
COUNTED_THIS(30B2EB4)
COUNTED_THIS(30B2FA4)
COUNTED_THIS(30B528C)
COUNTED_THIS(30B3008)
COUNTED_THIS(30B33F8)
COUNTED_THIS(30B5290)
COUNTED_THIS(30B3614)
COUNTED_THIS(30B3798)
COUNTED_THIS(30B37F0)
COUNTED_THIS(30B3800)
COUNTED_THIS(30B3810)
COUNTED_THIS(30B38AC)
COUNTED_THIS(30B5298)
COUNTED_THIS(30B38BC)
COUNTED_THIS(30B393C)
COUNTED_THIS(30B39BC)
COUNTED_THIS(30B3A3C)
COUNTED_THIS(30B3AEC)
COUNTED_THIS(30B3B38)
COUNTED_THIS(30B3B84)
COUNTED_THIS(30B3BC8)
COUNTED_THIS(30B3C0C)
COUNTED_THIS(30B3C8C)

// Carte du GamePad : F11 (ou DECAF_PROBE_RADAR=<s>, <s> secondes après le démarrage) journalise,
// pendant le prochain dessin de la carte (sub_2F3DAE4), chaque panneau lyt visité par
// nw::lyt::Pane::Draw (sub_3454128) : profondeur, nom, drapeaux (+68, bit 0 = visible), alpha
// (+69, +70) et 16 flottants (position, échelle, taille).
namespace
{
std::atomic<bool> s_radarLogged { false };
std::atomic<bool> s_radarLogging { false };
std::atomic<bool> s_radarRequest { false };
std::atomic<int> s_paneDepth { 0 };
std::atomic<int> s_fogLog { 0 };       // tests de brouillard restant à journaliser (F11)
std::atomic<bool> s_ignoreFog { false }; // F10 : icônes de la carte affichées même sous le brouillard
}

namespace rt
{
void toggleRadarFog() // F10 (gpu_window.cpp), expérimental
{
    s_ignoreFog = !s_ignoreFog;
    RT_INFO("RADAR : test de brouillard des icônes %s", s_ignoreFog ? "ignoré (F10)" : "normal");
}

void requestRadarProbe() // F11 (gpu_window.cpp)
{
    s_radarRequest = true;
    s_fogLog = 12;
    RT_INFO("RADAR : journal demandé pour le prochain dessin de la carte du GamePad");
}
}

extern "C" void __imp__sub_2F3DAE4(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_3454128(PPCContext& __restrict ctx, uint8_t* base);

void sub_2F3DAE4(PPCContext& __restrict ctx, uint8_t* base)
{
    static const char* env = getenv("DECAF_PROBE_RADAR");
    static const auto start = std::chrono::steady_clock::now() + std::chrono::seconds(env ? atoi(env) : 0);
    const bool log = s_radarRequest.exchange(false) || (env && !s_radarLogged && std::chrono::steady_clock::now() >= start);
    if (log)
        s_radarLogging = true;
    __imp__sub_2F3DAE4(ctx, base);
    if (log)
    {
        s_radarLogging = false;
        s_radarLogged = true;
    }
}

void sub_3454128(PPCContext& __restrict ctx, uint8_t* base)
{
    if (s_radarLogging)
    {
        const uint32_t pane = ctx.r3.u32;
        std::string name;
        for (uint32_t off = 0x40; off < 0x120 && name.empty(); off++)
        {
            const char* p = reinterpret_cast<const char*>(base + pane + off);
            size_t n = 0;
            while (n < 24 && p[n] >= 0x30 && p[n] < 0x7B)
                n++;
            if (n >= 4 && p[n] == 0)
                name = std::string(p, n) + "@+" + std::to_string(off);
        }
        std::string floats;
        for (uint32_t off = 0x18; off < 0x58; off += 4)
        {
            uint32_t u = rt::load32(pane + off);
            float f;
            memcpy(&f, &u, 4);
            char v[24];
            snprintf(v, sizeof(v), " %g", f);
            floats += v;
        }
        RT_INFO("RADAR %*s%08X %-28s drap %02X alpha %u/%u f :%s", s_paneDepth.load() * 2, "", pane, name.c_str(),
            base[pane + 68], base[pane + 69], base[pane + 70], floats.c_str());
    }
    s_paneDepth++;
    __imp__sub_3454128(ctx, base);
    s_paneDepth--;
}

// Carte du GamePad : panneau de carte (sub_2F40904, this = écran radar, vtable en +48) ; la
// méthode virtuelle en +780 est appelée après le cercle découvert. Journalisée avec F11.
extern "C" void __imp__sub_2F40904(PPCContext& __restrict ctx, uint8_t* base);

void sub_2F40904(PPCContext& __restrict ctx, uint8_t* base)
{
    if (s_radarLogging)
    {
        const uint32_t self = ctx.r3.u32, vt = rt::load32(self + 48);
        RT_INFO("RADAR panneau de carte : this %08X vtable %08X méthode +780 = %08X", self, vt, rt::load32(vt + 780));
        auto dump = [&](const char* label, uint32_t addr, uint32_t words) {
            std::string text;
            for (uint32_t i = 0; i < words; i++)
            {
                char v[12];
                snprintf(v, sizeof(v), " %08X", rt::load32(addr + i * 4));
                text += v;
            }
            RT_INFO("RADAR %s @%08X :%s", label, addr, text.c_str());
        };
        dump("this+1360", self + 1360, 8);
        if (const uint32_t fog = rt::load32(0x103F5900))
        {
            const uint32_t sampler = fog + 352;
            const uint32_t mask = rt::load32(sampler + 1480);
            const uint32_t data = mask ? rt::load32(mask + 40) : 0;
            const uint32_t w = rt::load32(sampler + 344), h = rt::load32(sampler + 348);
            uint32_t nonZero = 0, total = 0, maxValue = 0;
            if (data >= 0x10000000 && data < 0x50000000 && w && h && w * h <= 4096 * 4096)
                for (uint32_t i = 0; i < w * h; i++, total++)
                {
                    nonZero += base[data + i] != 0;
                    maxValue = std::max<uint32_t>(maxValue, base[data + i]);
                }
            RT_INFO("RADAR brouillard : objet %08X masque %08X données %08X %ux%u : %u/%u octets non nuls, max %u", fog,
                mask, data, w, h, nonZero, total, maxValue);
            if (mask)
                dump("masque (info)", mask, 32);
        }
        const uint32_t replay = rt::load32(self + 3700);
        RT_INFO("RADAR replayMgr %08X : en replay (+364) = %u, carte visible (+3452) = %u, +1364 = %u", replay,
            replay ? base[replay + 364] : 0, base[self + 3452], base[self + 1364]);
        for (uint32_t off : { 3904u, 8524u, 10632u })
        {
            const uint32_t pool = rt::load32(self + off);
            if (pool < 0x10000000 || pool >= 0x50000000)
                continue;
            const uint32_t entries = rt::load32(pool + 284);
            RT_INFO("RADAR réserve +%u : %u tampon(s), courant %u, éléments du tampon 0 = %u", off, rt::load32(pool + 280),
                rt::load32(pool + 316), entries ? rt::load32(entries + 12) : 0);
        }
        {
            std::map<uint32_t, uint32_t> kinds; // fonction « visible ? » (vtable+60) -> nombre de cibles
            std::string first;
            for (uint32_t i = 0; i < 100; i++)
            {
                const uint32_t target = rt::load32(self + 9708 + i * 4);
                if (target < 0x10000000 || target >= 0x50000000)
                {
                    kinds[0]++;
                    continue;
                }
                const uint32_t tvt = rt::load32(target);
                kinds[rt::load32(tvt + 60)]++;
                if (i < 12)
                {
                    char v[48];
                    snprintf(v, sizeof(v), " %08X(vt %08X)", target, tvt);
                    first += v;
                }
            }
            std::string text;
            for (auto& [fn, n] : kinds)
            {
                char v[32];
                snprintf(v, sizeof(v), " %08X x%u", fn, n);
                text += v;
            }
            RT_INFO("RADAR cibles (+9708) : visible?(vt+60) :%s ; premières :%s", text.c_str(), first.c_str());
        }
        for (uint32_t i = 0; i < 4; i++)
        {
            const uint32_t pane = rt::load32(self + 1364 + i * 4);
            if (pane < 0x10000000 || pane >= 0x50000000)
                continue;
            const uint32_t pvt = rt::load32(pane + 8);
            uint32_t children = 0;
            for (uint32_t node = rt::load32(pane + 20); node != pane + 20 && node && children < 10000;
                 node = rt::load32(node))
                children++;
            RT_INFO("RADAR calque %u : pane %08X vtable %08X DrawSelf(+148) %08X Draw(+140) %08X enfants %u", i, pane, pvt,
                rt::load32(pvt + 148), rt::load32(pvt + 140), children);
            dump("calque", pane, 64);
        }
        dump("this+2300", self + 2300, 32);
        dump("this+2944", self + 2944, 8);
        dump("this+3672", self + 3672, 16);
        for (uint32_t off : { 3904u, 8524u, 10632u })
        {
            const uint32_t pool = rt::load32(self + off);
            char label[32];
            snprintf(label, sizeof(label), "réserve +%u", off);
            if (pool >= 0x10000000 && pool < 0x50000000)
                dump(label, pool, 80);
        }
    }
    __imp__sub_2F40904(ctx, base);
}
COUNTED(2F3D76C)
COUNTED(2F3972C)
COUNTED(2F3A094)
COUNTED(2F3A4DC)
COUNTED(2F3AC5C)
COUNTED(2F3ACFC)
COUNTED(2F3B58C)
COUNTED(2F3C43C)
COUNTED(2F3A840)
COUNTED(2F3C6A0)
COUNTED(2F3D6A0)
COUNTED(2F36548)
COUNTED(2F36534)
COUNTED(2F392A0)

// Ajout d'une icône dans la réserve de la carte du GamePad : compté en permanence (diagnostic)
COUNTED_IF_ALWAYS(2F3C0B0, true, true)

// Test « icône sous le brouillard » de la carte du GamePad (sub_2F2F518 : r3 = 1 si la valeur
// du masque à cette position est sous 0,85, l'icône est alors cachée). F10 l'ignore (essai) ;
// F11 journalise les 12 résultats suivants.
extern "C" void __imp__sub_2F2F518(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_2F2F4B4(PPCContext& __restrict ctx, uint8_t* base);

void sub_2F2F518(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t pos = ctx.r4.u32;
    if (s_fogLog > 0)
    {
        const uint32_t self = ctx.r3.u32;
        __imp__sub_2F2F4B4(ctx, base); // valeur du masque (f1)
        const double value = ctx.f1.f64;
        ctx.r3.u64 = self;
        ctx.r4.u64 = pos;
        __imp__sub_2F2F518(ctx, base);
        uint32_t x = rt::load32(pos), z = rt::load32(pos + 4);
        float fx, fz;
        memcpy(&fx, &x, 4);
        memcpy(&fz, &z, 4);
        RT_INFO("RADAR test de brouillard : position (%g, %g) valeur %g -> %s", fx, fz, value, ctx.r3.u32 ? "cachée" : "visible");
        s_fogLog--;
    }
    else
        __imp__sub_2F2F518(ctx, base);
    if (s_ignoreFog)
        ctx.r3.u64 = 0;
}
