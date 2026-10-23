// Pont entre notre runtime et le code HLE de decaf-emu (GX2) :
//
//  * chaque fonction exportée par la bibliothèque GX2 de decaf remplace l'import gx2::*
//    correspondant ; l'appel convertit notre PPCContext en cpu::Core (la structure de
//    registres de decaf), laisse l'enveloppe typée de decaf lire ses arguments, puis
//    recopie les registres de retour ;
//  * quand decaf rappelle du code du jeu (cafe::invoke -> cpu::this_core::executeSub),
//    le cpu::Core est recopié dans le PPCContext du thread et la fonction recompilée
//    est appelée ;
//  * les données internes de GX2 (RegisterDataInternal) sont allouées en mémoire invitée.
#include "kernel.h"

#include <cafe/libraries/cafe_hle_library.h>
#include <cafe/libraries/gx2/gx2.h>
#include <cafe/libraries/gx2/gx2_shaders.h>
#include <libgpu/latte/latte_disassembler.h>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <libcpu/cpu_control.h>
#include <libcpu/state.h>

#include <atomic>
#include <vector>
#include <cstring>
#include <unordered_map>

namespace rt
{
void rdocCaptureSpan(uint32_t ms);
}

namespace
{

thread_local cpu::Core* t_core = nullptr;

void contextToCore(const PPCContext& ctx, cpu::Core& core)
{
    const PPCRegister* gpr[32] = { &ctx.r0, &ctx.r1, &ctx.r2, &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7,
        &ctx.r8, &ctx.r9, &ctx.r10, &ctx.r11, &ctx.r12, &ctx.r13, &ctx.r14, &ctx.r15, &ctx.r16, &ctx.r17,
        &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22, &ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27,
        &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31 };
    const PPCRegister* fpr[32] = { &ctx.f0, &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7,
        &ctx.f8, &ctx.f9, &ctx.f10, &ctx.f11, &ctx.f12, &ctx.f13, &ctx.f14, &ctx.f15, &ctx.f16, &ctx.f17,
        &ctx.f18, &ctx.f19, &ctx.f20, &ctx.f21, &ctx.f22, &ctx.f23, &ctx.f24, &ctx.f25, &ctx.f26, &ctx.f27,
        &ctx.f28, &ctx.f29, &ctx.f30, &ctx.f31 };
    for (int i = 0; i < 32; i++)
    {
        core.gpr[i] = gpr[i]->u32;
        core.fpr[i].paired0 = fpr[i]->f64;
        core.fpr[i].paired1 = ctx.ps1[i].f64;
    }
    core.lr = uint32_t(ctx.lr);
    core.ctr = ctx.ctr.u32;
    const PPCCRRegister* cr[8] = { &ctx.cr0, &ctx.cr1, &ctx.cr2, &ctx.cr3, &ctx.cr4, &ctx.cr5, &ctx.cr6, &ctx.cr7 };
    uint32_t crValue = 0;
    for (int i = 0; i < 8; i++)
        crValue |= ((cr[i]->lt << 3) | (cr[i]->gt << 2) | (cr[i]->eq << 1) | cr[i]->so) << (28 - 4 * i);
    core.cr.value = crValue;
    core.xer.value = (uint32_t(ctx.xer.so) << 31) | (uint32_t(ctx.xer.ov) << 30) | (uint32_t(ctx.xer.ca) << 29);
    for (int i = 0; i < 8; i++)
        core.gqr[i].value = ctx.gqr[i];
}

void coreToContext(const cpu::Core& core, PPCContext& ctx)
{
    PPCRegister* gpr[32] = { &ctx.r0, &ctx.r1, &ctx.r2, &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7,
        &ctx.r8, &ctx.r9, &ctx.r10, &ctx.r11, &ctx.r12, &ctx.r13, &ctx.r14, &ctx.r15, &ctx.r16, &ctx.r17,
        &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22, &ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27,
        &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31 };
    PPCRegister* fpr[32] = { &ctx.f0, &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7,
        &ctx.f8, &ctx.f9, &ctx.f10, &ctx.f11, &ctx.f12, &ctx.f13, &ctx.f14, &ctx.f15, &ctx.f16, &ctx.f17,
        &ctx.f18, &ctx.f19, &ctx.f20, &ctx.f21, &ctx.f22, &ctx.f23, &ctx.f24, &ctx.f25, &ctx.f26, &ctx.f27,
        &ctx.f28, &ctx.f29, &ctx.f30, &ctx.f31 };
    for (int i = 0; i < 32; i++)
    {
        gpr[i]->u64 = core.gpr[i];
        fpr[i]->f64 = core.fpr[i].paired0;
        ctx.ps1[i].f64 = core.fpr[i].paired1;
    }
    ctx.lr = core.lr;
    ctx.ctr.u64 = core.ctr;
    PPCCRRegister* cr[8] = { &ctx.cr0, &ctx.cr1, &ctx.cr2, &ctx.cr3, &ctx.cr4, &ctx.cr5, &ctx.cr6, &ctx.cr7 };
    for (int i = 0; i < 8; i++)
    {
        const uint32_t field = (core.cr.value >> (28 - 4 * i)) & 0xF;
        cr[i]->lt = (field >> 3) & 1;
        cr[i]->gt = (field >> 2) & 1;
        cr[i]->eq = (field >> 1) & 1;
        cr[i]->so = field & 1;
    }
    ctx.xer.so = (core.xer.value >> 31) & 1;
    ctx.xer.ov = (core.xer.value >> 30) & 1;
    ctx.xer.ca = (core.xer.value >> 29) & 1;
    for (int i = 0; i < 8; i++)
        ctx.gqr[i] = core.gqr[i].value;
}

// Garde-fous devant certaines fonctions GX2 : renvoie vrai pour sauter l'appel (le résultat
// est alors 0). Sert à tolérer des données de jeu manquantes ou abîmées - voir ci-dessous.
using Guard = bool (*)(PPCContext&);

struct BridgeFunction
{
    cafe::hle::LibraryFunction* fn;
    Guard guard;
    void (*after)(PPCContext&) = nullptr; // après l'appel, cœur decaf encore actif
};

// Un shader dont le programme est absent (fichier GSH illisible : le chargeur du jeu laisse la
// structure à zéro) ferait échouer une assertion de decaf dans GX2Set*Shader, puis planter
// libgpu au premier dessin. On signale, on saute le réglage, et on saute aussi les dessins
// tant que le jeu croit avoir un shader manquant lié (état approximé globalement).
std::atomic<bool> s_vertexShaderMissing { false };
std::atomic<bool> s_pixelShaderMissing { false };
std::atomic<uint32_t> s_skippedDraws { 0 };

template<typename Shader>
bool shaderHasProgram(uint32_t address)
{
    if (!address)
        return false;
    auto shader = virt_cast<Shader *>(virt_addr { address });
    auto program = virt_cast<virt_addr>(shader->data).getAddress();
    uint32_t size = shader->size;
    if (!program)
    {
        program = virt_cast<virt_addr>(shader->gx2rData.buffer).getAddress();
        size = shader->gx2rData.elemCount * shader->gx2rData.elemSize;
    }
    return program && size;
}

void warnOnce(std::atomic<bool>& flag, const char* what)
{
    if (!flag.exchange(true))
        RT_WARN("GX2 : %s sans programme (shader illisible, fichier GSH abîmé ?) : ignoré", what);
}

// Diagnostic (--probe) : désassemble les premiers shaders de sommets distincts.
void probeDumpVertexShader(uint32_t address)
{
    static std::set<uint32_t> seen;
    static std::mutex lock;
    std::lock_guard guard(lock);
    if (seen.size() >= 4 || !seen.insert(address).second)
        return;
    auto shader = virt_cast<cafe::gx2::GX2VertexShader *>(virt_addr { address });
    auto code = virt_cast<virt_addr>(shader->data).getAddress();
    if (!code)
        return;
    const uint32_t size = shader->size;
    const std::string text = latte::disassemble(gsl::span<const uint8_t>(rt::ptr(code), size));
    RT_INFO("SONDE shader de sommets 0x%08X (%u octets) :\n%s", address, size, text.c_str());
}

std::atomic<uint32_t> s_probeProgram { 0 }; // ringItemsize du shader de sommets lié (--probe)

bool guardSetVertexShader(PPCContext& ctx)
{
    if (rt::g_probe && ctx.r3.u32)
    {
        s_probeProgram = rt::load32(ctx.r3.u32 + 0x10C);
        static std::mutex lock;
        static std::set<uint32_t> seen;
        std::lock_guard guard(lock);
        if (seen.insert(ctx.r3.u32).second && seen.size() <= 64)
            RT_INFO("SONDE shader de sommets lié 0x%08X marqueur=%u (lr=0x%08X)", ctx.r3.u32, s_probeProgram.load(),
                uint32_t(ctx.lr));
    }
    static std::atomic<bool> warned { false };
    const bool ok = shaderHasProgram<cafe::gx2::GX2VertexShader>(ctx.r3.u32);
    if (ok && rt::g_probe)
        probeDumpVertexShader(ctx.r3.u32);
    s_vertexShaderMissing = !ok;
    if (!ok)
        warnOnce(warned, "GX2SetVertexShader");
    return !ok;
}

bool guardSetPixelShader(PPCContext& ctx)
{
    static std::atomic<bool> warned { false };
    const bool ok = shaderHasProgram<cafe::gx2::GX2PixelShader>(ctx.r3.u32);
    s_pixelShaderMissing = !ok;
    if (!ok)
        warnOnce(warned, "GX2SetPixelShader");
    return !ok;
}

bool guardSetGeometryShader(PPCContext& ctx)
{
    static std::atomic<bool> warned { false };
    const bool ok = shaderHasProgram<cafe::gx2::GX2GeometryShader>(ctx.r3.u32);
    if (!ok)
        warnOnce(warned, "GX2SetGeometryShader");
    return !ok;
}

std::atomic<uint32_t> s_probeTarget { 0 }; // image de la cible de couleur 0 liée (--probe)

bool guardDraw(PPCContext& ctx)
{
    if (rt::g_probe)
    {
        static std::mutex lock;
        static std::map<uint32_t, uint32_t> counts;
        static uint32_t total = 0;
        std::lock_guard guard(lock);
        counts[s_probeTarget.load()]++;
        if (++total % 200000 == 0)
            for (auto& [image, n] : counts)
                RT_INFO("SONDE draws cible 0x%08X : %u", image, n);
    }
    if (!s_vertexShaderMissing && !s_pixelShaderMissing)
        return false;
    const uint32_t n = ++s_skippedDraws;
    if (n == 1 || n % 5000 == 0)
        RT_WARN("GX2 : %u dessins ignorés (shader manquant)", n);
    return true;
}

// Emplacement (échantillonneur, texture, bloc uniforme) tiré de la structure d'un shader : si
// le shader est vide, le jeu n'y trouve rien et passe un identifiant invalide (-1…) que decaf
// refuse par assertion. On ignore l'appel, en le signalant une fois par fonction.
#define SLOT_GUARD(NAME, REG, LIMIT)                                                                  bool guard##NAME(PPCContext& ctx)                                                                {                                                                                                     static std::atomic<bool> warned { false };                                                        if (ctx.REG.u32 < (LIMIT))                                                                            return false;                                                                                 if (!warned.exchange(true))                                                                           RT_WARN("GX2 : %s avec un emplacement invalide (%u) : ignoré", #NAME, ctx.REG.u32);           return true;                                                                                  }
SLOT_GUARD(SetPixelSampler, r4, 0x12)
SLOT_GUARD(SetVertexSampler, r4, 0x12)
SLOT_GUARD(SetGeometrySampler, r4, 0x12)
SLOT_GUARD(SetPixelTexture, r4, 0x12)
SLOT_GUARD(SetVertexTexture, r4, 0x12)
SLOT_GUARD(SetGeometryTexture, r4, 0x12)
SLOT_GUARD(SetPixelSamplerBorderColor, r3, 0x12)
SLOT_GUARD(SetVertexSamplerBorderColor, r3, 0x12)
SLOT_GUARD(SetGeometrySamplerBorderColor, r3, 0x12)
SLOT_GUARD(SetPixelUniformBlock, r3, 0x10)
SLOT_GUARD(SetVertexUniformBlock, r3, 0x10)
SLOT_GUARD(SetGeometryUniformBlock, r3, 0x10)
#undef SLOT_GUARD

// GX2Set{Vertex,Pixel}UniformReg(offset, count, data) : `offset` compte des vecteurs de 4 mots ;
// les 16 bits hauts désignent une constante de boucle (32 au plus). Chaque étage dispose de
// 256 vecteurs (1024 mots).
// --probe : journalise une fenêtre d'envois de constantes (étage, décalage en mots, valeurs).
void probeUniforms(const char* stage, PPCContext& ctx)
{
    static std::atomic<uint32_t> calls { 0 };
    const uint32_t n = calls++;
    // uTextureMode (PS, offset 0) : journalisé hors fenêtre, une fois par couple (valeur, lr).
    static std::set<uint64_t> modes;
    const bool mode = stage[0] == 'P' && ctx.r3.u32 == 0 && ctx.r5.u32
        && modes.insert(uint64_t(rt::load32(ctx.r5.u32)) << 32 | uint32_t(ctx.lr)).second;
    // uProjection des layouts (VS, offset 0, 16 valeurs, appelé depuis nw::lyt) : chaque matrice distincte.
    static std::set<uint64_t> projections;
    if (stage[0] == 'V' && (ctx.r3.u32 == 0 || ctx.r3.u32 == 16) && ctx.r5.u32 && (uint32_t(ctx.lr) >> 20) == 0x034
        && projections.size() < 60)
    {
        uint64_t h = 0;
        for (int i = 0; i < 16; i++)
            h = h * 31 + rt::load32(ctx.r5.u32 + i * 4);
        if (projections.insert(h).second)
        {
            float m[16] = {};
            for (uint32_t i = 0; i < ctx.r4.u32 && i < 16; i++)
            {
                const uint32_t bits = rt::load32(ctx.r5.u32 + i * 4);
                memcpy(&m[i], &bits, 4);
            }
            RT_INFO("SONDE matrice offset=%u lr=0x%08X : %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g",
                ctx.r3.u32, uint32_t(ctx.lr), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12],
                m[13], m[14], m[15]);
        }
    }
    if (!mode && (n < 3000 || n >= 4500 || !ctx.r5.u32))
        return;
    float v[4];
    for (int i = 0; i < 4; i++)
    {
        const uint32_t bits = rt::load32(ctx.r5.u32 + i * 4);
        memcpy(&v[i], &bits, 4);
    }
    RT_INFO("SONDE prog=%u %s offset=%d count=%u lr=0x%08X : %g %g %g %g", s_probeProgram.load(), stage, int32_t(ctx.r3.u32),
        ctx.r4.u32, uint32_t(ctx.lr), v[0], v[1], v[2], v[3]);
}

bool guardSetUniformReg(PPCContext& ctx)
{
    static std::atomic<bool> warned { false };
    const uint32_t offset = ctx.r3.u32, count = ctx.r4.u32;
    const uint32_t loop = offset >> 16;
    const uint32_t low = loop ? (offset & 0x7FFF) : offset;
    // offset et count en mots de 32 bits ; 256 vec4 (1024 mots) par étage.
    if (loop < 32 && uint64_t(low) + count <= 1024)
        return false;
    if (!warned.exchange(true))
        RT_WARN("GX2 : GX2Set*UniformReg avec un décalage invalide (0x%X, %u mots) : ignoré", offset, count);
    return true;
}

bool guardSetVertexUniformReg(PPCContext& ctx)
{
    if (rt::g_probe)
        probeUniforms("VS", ctx);
    return guardSetUniformReg(ctx);
}

bool guardSetPixelUniformReg(PPCContext& ctx)
{
    if (rt::g_probe)
        probeUniforms("PS", ctx);
    return guardSetUniformReg(ctx);
}

// GX2SetShaders(fetch, vertex, pixel) : on note le programme lié pour les sondes.
bool guardSetShaders(PPCContext& ctx)
{
    if (rt::g_probe && ctx.r4.u32)
        s_probeProgram = rt::load32(ctx.r4.u32 + 0x10C);
    return false;
}

// --probe : viewports distincts (x, y, largeur, hauteur) et appelant.
bool guardSetViewport(PPCContext& ctx)
{
    static std::mutex lock;
    static std::set<std::string> seen;
    if (!rt::g_probe)
        return false;
    char text[160];
    snprintf(text, sizeof(text), "%g %g %g %g lr=0x%08X", ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64,
        uint32_t(ctx.lr));
    std::lock_guard guard(lock);
    if (seen.insert(text).second)
        RT_INFO("SONDE GX2SetViewport %s", text);
    return false;
}

// --probe : formats d'attributs de sommets distincts (GX2AttribStream, 0x20 octets :
// location, buffer, offset, format, type, aluDivisor, mask, endianSwap).
bool guardInitFetchShader(PPCContext& ctx)
{
    static std::mutex lock;
    static std::set<std::string> seen;
    if (!rt::g_probe)
        return false;
    std::lock_guard guard(lock);
    for (uint32_t i = 0; i < ctx.r5.u32 && i < 32; i++)
    {
        const uint32_t a = ctx.r6.u32 + i * 0x20;
        char text[160];
        snprintf(text, sizeof(text), "format=0x%X type=%u mask=0x%08X endian=%u", rt::load32(a + 12),
            rt::load32(a + 16), rt::load32(a + 24), rt::load32(a + 28));
        if (seen.insert(text).second)
            RT_INFO("SONDE attribut %s (loc %u, lr=0x%08X)", text, rt::load32(a), uint32_t(ctx.lr));
    }
    return false;
}

// --probe : textures de pixels distinctes par (dimension, format, mode de tuile, compSel).
bool guardSetPixelTextureProbe(PPCContext& ctx)
{
    if (guardSetPixelTexture(ctx))
        return true;
    if (!rt::g_probe || !ctx.r3.u32)
        return false;
    static std::mutex lock;
    static std::set<std::string> seen;
    const uint32_t t = ctx.r3.u32;
    char text[160];
    snprintf(text, sizeof(text), "dim=%u format=0x%X tile=%u compSel=0x%08X", rt::load32(t), rt::load32(t + 0x14),
        rt::load32(t + 0x30), rt::load32(t + 0x84));
    std::lock_guard guard(lock);
    if (seen.insert(text).second)
        RT_INFO("SONDE texture %s (%ux%u, mips %u, slot %u)", text, rt::load32(t + 4), rt::load32(t + 8),
            rt::load32(t + 0x10), ctx.r4.u32);
    return false;
}

// --probe : contenu distinct du bloc uniforme 2 des sommets (brouillard du décor : vec4 2 à 4).
bool guardSetVertexUniformBlockProbe(PPCContext& ctx)
{
    if (guardSetVertexUniformBlock(ctx))
        return true;
    if (!rt::g_probe || ctx.r3.u32 != 2 || !ctx.r5.u32 || ctx.r4.u32 < 80)
        return false;
    static std::mutex lock;
    static std::set<std::string> seen;
    std::string text;
    for (uint32_t i = 8; i < 20; i++)
    {
        const uint32_t bits = rt::load32(ctx.r5.u32 + i * 4);
        float f;
        memcpy(&f, &bits, 4);
        char v[24];
        snprintf(v, sizeof(v), i % 4 == 3 ? "%g | " : "%g ", f);
        text += v;
    }
    std::lock_guard guard(lock);
    if (seen.size() < 40 && seen.insert(text).second)
        RT_INFO("SONDE bloc VS 2 (taille %u, lr=0x%08X) : %s", ctx.r4.u32, uint32_t(ctx.lr), text.c_str());
    return false;
}

// Mémoire des cibles de couleur (GX2Surface : imageSize +0x20, image +0x24). Le vrai
// lyt_BuildinShader retourne V pour les textures rendues par le GPU (convention des cibles de
// rendu) ; notre shader reconstruit lit ce choix dans le registre C255 (lyt.psh).
std::mutex s_targetLock;
std::map<uint32_t, uint32_t> s_targets; // début -> fin

void noteColorTarget(uint32_t surface)
{
    const uint32_t image = rt::load32(surface + 0x24), size = rt::load32(surface + 0x20);
    if (!image || !size)
        return;
    std::lock_guard guard(s_targetLock);
    s_targets[image] = image + size;
}

bool isColorTarget(uint32_t image)
{
    std::lock_guard guard(s_targetLock);
    auto it = s_targets.upper_bound(image);
    if (it == s_targets.begin())
        return false;
    --it;
    return image < it->second;
}

void afterSetPixelTexture(PPCContext& ctx)
{
    if (ctx.r4.u32 != 0 || !ctx.r3.u32)
        return;
    static thread_local uint32_t buffer = rt::sysAlloc(16);
    const bool target = isColorTarget(rt::load32(ctx.r3.u32 + 0x24));
    const float values[4] = { target ? 1.0f : 0.0f, target ? -1.0f : 1.0f, 0.0f, 0.0f };
    for (int i = 0; i < 4; i++)
    {
        uint32_t bits;
        memcpy(&bits, &values[i], 4);
        rt::store32(buffer + i * 4, bits);
    }
    cafe::gx2::GX2SetPixelUniformReg(255 * 4, 4, virt_cast<uint32_t *>(virt_addr { buffer }));
}

// --probe : cibles de couleur distinctes (GX2ColorBuffer : surface.dim +0, width +4, height +8,
// depth +0xC, format +0x14, image +0x24, tileMode +0x30 ; viewMip +0x74, viewFirstSlice +0x78).
bool guardSetColorBufferProbe(PPCContext& ctx)
{
    if (ctx.r3.u32)
        noteColorTarget(ctx.r3.u32);
    // DECAF_RDOC_ON_CUBE : capture RenderDoc au premier rendu dans une cube map (sondes de lumière).
    static const bool rdocOnCube = getenv("DECAF_RDOC_ON_CUBE") != nullptr;
    // DECAF_RDOC_ON_SIZE=<l>x<h> : idem au premier rendu dans une cible de cette taille.
    static const char* rdocOnSize = getenv("DECAF_RDOC_ON_SIZE");
    if (rdocOnSize && ctx.r3.u32)
    {
        char size[32];
        snprintf(size, sizeof(size), "%ux%u", rt::load32(ctx.r3.u32 + 4), rt::load32(ctx.r3.u32 + 8));
        static std::atomic<bool> done { false };
        if (strcmp(size, rdocOnSize) == 0 && !done.exchange(true))
            rt::rdocCaptureSpan(300);
    }
    if (rdocOnCube && ctx.r3.u32 && rt::load32(ctx.r3.u32) == 3)
    {
        static std::atomic<bool> done { false };
        if (!done.exchange(true))
            rt::rdocCaptureSpan(1500);
    }
    if (!rt::g_probe || !ctx.r3.u32)
        return false;
    static std::mutex lock;
    static std::set<std::string> seen;
    const uint32_t b = ctx.r3.u32;
    if (ctx.r4.u32 == 0)
        s_probeTarget = rt::load32(b + 0x24) + rt::load32(b + 0x78);
    char text[200];
    snprintf(text, sizeof(text), "%ux%ux%u dim=%u fmt=0x%X tile=%u image=0x%08X slice=%u target=%u",
        rt::load32(b + 4), rt::load32(b + 8), rt::load32(b + 0xC), rt::load32(b), rt::load32(b + 0x14),
        rt::load32(b + 0x30), rt::load32(b + 0x24), rt::load32(b + 0x78), ctx.r4.u32);
    std::lock_guard guard(lock);
    if (seen.insert(text).second)
        RT_INFO("SONDE cible %s (lr=0x%08X)", text, uint32_t(ctx.lr));
    return false;
}

// Images par seconde côté jeu (appels à GX2SwapScanBuffers), journalisées toutes les 5 s.
bool guardSwapScanBuffers(PPCContext&)
{
    static auto start = std::chrono::steady_clock::now();
    static uint32_t frames = 0;
    ++frames;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - start).count();
    if (seconds >= 5.0)
    {
        RT_INFO("%.1f images/s", frames / seconds);
        frames = 0;
        start = now;
    }
    return false;
}

const std::unordered_map<std::string, Guard> kGuards = {
    { "GX2SwapScanBuffers", guardSwapScanBuffers },
    { "GX2SetColorBuffer", guardSetColorBufferProbe },
    { "GX2InitFetchShaderEx", guardInitFetchShader },
    { "GX2SetViewport", guardSetViewport },
    { "GX2SetShaders", guardSetShaders },
    { "GX2SetVertexUniformReg", guardSetVertexUniformReg },
    { "GX2SetPixelUniformReg", guardSetPixelUniformReg },
    { "GX2SetPixelSampler", guardSetPixelSampler },
    { "GX2SetVertexSampler", guardSetVertexSampler },
    { "GX2SetGeometrySampler", guardSetGeometrySampler },
    { "GX2SetPixelTexture", guardSetPixelTextureProbe },
    { "GX2SetVertexTexture", guardSetVertexTexture },
    { "GX2SetGeometryTexture", guardSetGeometryTexture },
    { "GX2SetPixelSamplerBorderColor", guardSetPixelSamplerBorderColor },
    { "GX2SetVertexSamplerBorderColor", guardSetVertexSamplerBorderColor },
    { "GX2SetGeometrySamplerBorderColor", guardSetGeometrySamplerBorderColor },
    { "GX2SetPixelUniformBlock", guardSetPixelUniformBlock },
    { "GX2SetVertexUniformBlock", guardSetVertexUniformBlockProbe },
    { "GX2SetGeometryUniformBlock", guardSetGeometryUniformBlock },
    { "GX2SetVertexShader", guardSetVertexShader },
    { "GX2SetPixelShader", guardSetPixelShader },
    { "GX2SetGeometryShader", guardSetGeometryShader },
    { "GX2DrawEx", guardDraw },
    { "GX2DrawEx2", guardDraw },
    { "GX2DrawIndexedEx", guardDraw },
    { "GX2DrawIndexedEx2", guardDraw },
    { "GX2DrawIndexedImmediateEx", guardDraw },
};

// Appel d'une fonction HLE de decaf depuis le code recompilé.
void invokeDecafFunction(PPCContext& ctx, uint8_t*, void* user)
{
    auto* bridge = static_cast<BridgeFunction*>(user);
    if (bridge->guard && bridge->guard(ctx))
    {
        ctx.r3.u64 = 0;
        return;
    }
    auto* fn = bridge->fn;
    cpu::Core core {};
    contextToCore(ctx, core);
    cpu::Core* previous = t_core;
    t_core = &core;
    cpu::Core* result = fn->invokeHandler(&core, 0);
    if (bridge->after)
        bridge->after(ctx);
    t_core = previous;
    coreToContext(*result, ctx);
}

// Expose registerSymbols(), protégée dans cafe::hle::Library.
struct Gx2Library : cafe::gx2::Library
{
    using cafe::gx2::Library::registerSymbols;
};

} // namespace

// ---------------------------------------------------------------- libcpu côté decaf
namespace cpu::this_core
{

cpu::Core* state()
{
    return t_core;
}

// Exécute du code invité à core->nia (cafe::invoke) sur le contexte du thread courant.
void executeSub()
{
    cpu::Core* core = t_core;
    PPCContext& ctx = *rt::currentContext();
    coreToContext(*core, ctx);
    PPCCallIndirect(ctx, rt::g_base, core->nia);
    contextToCore(ctx, *core);
}

// Cœur Cafe OS du thread courant (0, 1 ou 2) : GX2 tient un tampon de commandes actif par cœur.
uint32_t id()
{
    const auto* thread = rt::kernel::current();
    return thread ? static_cast<uint32_t>(thread->core) : 1;
}

} // namespace cpu::this_core

// ---------------------------------------------------------------- enregistrement
namespace
{
BridgeFunction* s_gx2Entry = nullptr;
}

namespace rt
{

// Appelle l'initialisation du module gx2.rpl (rpl_entry) sur le thread invité courant.
void decafGx2RunEntry()
{
    if (!s_gx2Entry)
        return;
    PPCContext& ctx = *currentContext();
    PPCContext saved = ctx;
    ctx.r3.u64 = 0;
    ctx.r4.u64 = 1; // OSDynLoad_EntryReason::Loaded
    invokeDecafFunction(ctx, g_base, s_gx2Entry);
    ctx = saved;
}

void decafGx2Register()
{
    static Gx2Library library;
    library.registerSymbols();
    size_t functions = 0, data = 0, internals = 0;
    for (auto& [name, symbol] : library.getSymbolMap())
    {
        if (symbol->type == cafe::hle::LibrarySymbol::Function)
        {
            auto* fn = static_cast<cafe::hle::LibraryFunction*>(symbol.get());
            auto guard = kGuards.find(name);
            auto* bridge = new BridgeFunction { fn, guard == kGuards.end() ? nullptr : guard->second };
            if (name == "GX2SetPixelTexture")
                bridge->after = afterSetPixelTexture;
            if (name == "rpl_entry")
                s_gx2Entry = bridge;
            else if (fn->hostPtr)
            {
                // Fonction interne passée au jeu comme pointeur (callbacks…)
                *fn->hostPtr = virt_cast<void*>(virt_addr { exportHostFunction(invokeDecafFunction, bridge) });
                internals++;
            }
            else if (hleSetImplementation("gx2", name, invokeDecafFunction, bridge))
                functions++;
        }
        else if (symbol->type == cafe::hle::LibrarySymbol::Data)
        {
            auto* d = static_cast<cafe::hle::LibraryData*>(symbol.get());
            const uint32_t addr = sysAlloc(std::max<uint32_t>(d->size, 4), std::max<uint32_t>(d->align, 16));
            if (d->constructor)
                d->constructor(ptr(addr));
            if (d->hostPointer)
                *d->hostPointer = virt_cast<void*>(virt_addr { addr });
            data++;
        }
    }
    RT_INFO("GX2 de decaf : %zu fonctions branchées, %zu internes, %zu données", functions, internals, data);
    for (const auto& name : hleListWithoutEx("gx2"))
        RT_INFO("  import gx2 non couvert par decaf : %s", name.c_str());
}

} // namespace rt
