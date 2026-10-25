// Table des fonctions invitées, appels indirects, traps et appels runtime -> jeu.
#include "runtime.h"

#include <string>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

extern PPCFuncMapping PPCFuncMappings[];

namespace rt
{

static std::vector<PPCFunc*> s_funcTable; // indexée par (adresse - kCodeBase) / 4
static thread_local PPCContext* t_ctx = nullptr;
thread_local uint32_t t_indirectTarget = 0;
static std::mutex s_exportMutex;
static uint32_t s_nextExport = kHostExportBase;

void registerFunction(uint32_t address, PPCFunc* fn)
{
    if (address < kCodeBase || address >= kImportEnd || (address & 3))
        fatal("fonction hors table : 0x%08X", address);
    s_funcTable[(address - kCodeBase) / 4] = fn;
}

PPCFunc* findFunction(uint32_t address)
{
    if (address < kCodeBase || address >= kImportEnd || (address & 3))
        return nullptr;
    return s_funcTable[(address - kCodeBase) / 4];
}

static std::unordered_map<uint32_t, std::pair<HleFnEx, void*>> s_exportsEx;

static PPC_FUNC(exportExTrampoline)
{
    std::pair<HleFnEx, void*> target;
    {
        std::lock_guard lock(s_exportMutex);
        target = s_exportsEx.at(t_indirectTarget);
    }
    target.first(ctx, base, target.second);
}

uint32_t exportHostFunction(HleFnEx fn, void* user)
{
    const uint32_t addr = exportHostFunction(exportExTrampoline);
    std::lock_guard lock(s_exportMutex);
    s_exportsEx[addr] = { fn, user };
    return addr;
}

uint32_t exportHostFunction(PPCFunc* fn)
{
    std::lock_guard lock(s_exportMutex);
    uint32_t addr = s_nextExport;
    if (addr >= kImportEnd)
        fatal("plus de place pour les exports hôtes");
    s_nextExport += 4;
    registerFunction(addr, fn);
    return addr;
}

void cpuInit()
{
    s_funcTable.assign((kImportEnd - kCodeBase) / 4, nullptr);
    size_t n = 0;
    for (auto* m = PPCFuncMappings; m->host; ++m, ++n)
        registerFunction(uint32_t(m->guest), m->host);
    RT_INFO("%zu fonctions recompilées / imports enregistrés", n);
}

PPCContext* currentContext() { return t_ctx; }
void setCurrentContext(PPCContext* ctx) { t_ctx = ctx; }

uint32_t callGuest(PPCContext& ctx, uint32_t address, std::initializer_list<uint32_t> args)
{
    // Le code invité ne connaît que son ABI : on sauvegarde tout le contexte autour
    // de l'appel et on lui donne un cadre de pile propre.
    PPCContext saved = ctx;
    ctx.r1.u64 = (ctx.r1.u32 - 0x40) & ~0xFu;
    store32(ctx.r1.u32, saved.r1.u32); // back-chain
    PPCRegister* gpr[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
    size_t i = 0;
    for (uint32_t a : args)
        gpr[i++]->u64 = a;
    PPCCallIndirect(ctx, g_base, address);
    uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
}

} // namespace rt

void PPCCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t address)
{
    PPCFunc* fn = rt::findFunction(address);
    if (!fn)
    {
        // Contexte pour le diagnostic : chaîne des appelants invités (pile : [r1] = cadre
        // précédent, LR sauvegardé en +4 du cadre de l'appelant) et pile hôte.
        std::string chain;
        uint32_t sp = ctx.r1.u32;
        for (int depth = 0; depth < 16 && sp >= 0x10000 && sp < 0xF0000000u; depth++)
        {
            const uint32_t next = rt::load32(sp);
            if (next <= sp)
                break;
            char v[16];
            snprintf(v, sizeof(v), " %08X", rt::load32(next + 4));
            chain += v;
            sp = next;
        }
        RT_ERROR("appel indirect nul : r1=%08X r30=%08X r31=%08X, appelants invités :%s", ctx.r1.u32, ctx.r30.u32,
            ctx.r31.u32, chain.c_str());
        RT_ERROR("pile hôte : %s", rt::hostStack().c_str());
        rt::fatal("appel indirect vers 0x%08X : aucune fonction (lr=0x%08X, r3=0x%08X)",
            address, uint32_t(ctx.lr), ctx.r3.u32);
    }
    rt::t_indirectTarget = address;
    fn(ctx, base);
}

void PPCTrap(PPCContext& ctx, uint8_t* base, uint32_t address)
{
    rt::fatal("trap (tw) du jeu en 0x%08X - lr=0x%08X r3=0x%08X r4=0x%08X", address,
        uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32);
}
