// Mémoire invitée : 4 Gio d'espace d'adressage réservé, pages engagées par régions
// selon la carte mémoire Cafe OS (voir runtime.h).
#include "runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <mutex>

namespace rt
{

uint8_t* g_base = nullptr;

static std::mutex s_sysMutex;
static uint32_t s_sysNext = kSysBase;

static LONG CALLBACK guestFaultHandler(EXCEPTION_POINTERS* info)
{
    auto* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto host = static_cast<uint8_t*>(reinterpret_cast<void*>(rec->ExceptionInformation[1]));
    if (host < g_base || host >= g_base + 0x100000000ull)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t guest = uint32_t(host - g_base);
    const char* kind = rec->ExceptionInformation[0] == 1 ? "écriture" : "lecture";
    PPCContext* ctx = currentContext();
    fatal("accès mémoire invitée invalide (%s) en 0x%08X - lr=0x%08X r1=0x%08X r3=0x%08X",
        kind, guest, ctx ? uint32_t(ctx->lr) : 0, ctx ? ctx->r1.u32 : 0, ctx ? ctx->r3.u32 : 0);
}

void memCommit(uint32_t addr, uint32_t size)
{
    if (!VirtualAlloc(g_base + addr, size, MEM_COMMIT, PAGE_READWRITE))
        fatal("impossible d'engager 0x%08X..0x%08X (erreur %lu)", addr, addr + size, GetLastError());
}

void memInit()
{
    g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
    if (!g_base)
        fatal("impossible de réserver 4 Gio d'espace d'adressage invité");

    // Page 0 lisible et nulle : le crt0 lit `*(&symbole_faible)` sur des symboles absents.
    memCommit(0, 0x10000);
    memCommit(kCodeBase, kImportEnd - kCodeBase);
    memCommit(kMem2Base, kMem2End - kMem2Base);
    memCommit(kSysBase, kSysSize);
    memCommit(kFgBucketBase, kFgBucketSize);
    memCommit(kMem1Base, kMem1Size);

    AddVectoredExceptionHandler(1, guestFaultHandler);
    RT_INFO("mémoire invitée réservée à %p", g_base);
}

uint32_t sysAlloc(uint32_t size, uint32_t align)
{
    std::lock_guard lock(s_sysMutex);
    uint32_t addr = (s_sysNext + align - 1) & ~(align - 1);
    if (addr + size > kSysBase + kSysSize)
        fatal("zone système du runtime épuisée (%u octets demandés)", size);
    s_sysNext = addr + size;
    memset(g_base + addr, 0, size);
    return addr;
}

} // namespace rt
