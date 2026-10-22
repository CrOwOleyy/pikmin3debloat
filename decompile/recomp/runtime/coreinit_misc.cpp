// coreinit : journal (OSReport…), chargement dynamique, configuration système, sortie.
#include "kernel.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <unordered_map>
#include <vector>

using namespace rt;

namespace rt
{
uint32_t findImportAddress(const char* lib, const char* sym);
}

static void report(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    RT_INFO("[jeu] %s", text.c_str());
}

HLE_FUNC(coreinit, OSReport) { report(formatGuest(ctx, ARG_U32(3), 4)); }
HLE_FUNC(coreinit, OSVReport) { report(formatGuestVaList(ARG_U32(3), ARG_U32(4))); }
HLE_FUNC(coreinit, OSConsoleWrite) { report(std::string(reinterpret_cast<const char*>(ptr(ARG_U32(3))), ARG_U32(4))); }

// void OSPanic(const char* file, int line, const char* fmt, ...)
HLE_FUNC(coreinit, OSPanic)
{
    fatal("OSPanic %s:%d : %s", readString(ARG_U32(3)).c_str(), ARG_S32(4), formatGuest(ctx, ARG_U32(5), 6).c_str());
}

HLE_FUNC(coreinit, exit)
{
    RT_INFO("le jeu appelle exit(%d)", ARG_S32(3));
    ExitProcess(ARG_U32(3));
}
HLE_FUNC(coreinit, _Exit)
{
    RT_INFO("le jeu appelle _Exit(%d)", ARG_S32(3));
    ExitProcess(ARG_U32(3));
}
HLE_FUNC(coreinit, __gh_set_errno) {}

// ================================================================= OSDynLoad
// Les modules « chargés » dynamiquement (erreula, swkbd…) sont nos bibliothèques HLE.
// Un export est résolu, dans l'ordre : import statique connu du RPX, HLE_FUNC dédié,
// sinon stub générique qui renvoie 0 (et le signale une fois), comme un import non
// implémenté. Les exports de données reçoivent un bloc mis à zéro.
namespace
{
std::mutex s_dynMutex;
std::vector<std::string> s_modules;
std::unordered_map<std::string, uint32_t> s_dynExports;   // "module::nom" -> adresse invitée
std::unordered_map<uint32_t, std::string> s_dynStubNames; // adresse -> "module::nom"
std::unordered_map<uint32_t, HleFn> s_dynImpls;           // adresse -> implémentation

PPC_FUNC(dynamicExport)
{
    const uint32_t addr = t_indirectTarget;
    HleFn impl = nullptr;
    std::string name;
    bool first = false;
    {
        std::lock_guard lock(s_dynMutex);
        if (auto it = s_dynImpls.find(addr); it != s_dynImpls.end())
            impl = it->second;
        else if (auto n = s_dynStubNames.find(addr); n != s_dynStubNames.end())
        {
            name = n->second;
            first = !name.empty() && name[0] != '!';
            if (first)
                n->second = "!" + name; // déjà signalé
        }
    }
    if (impl)
    {
        impl(ctx, base);
        return;
    }
    if (first)
        RT_WARN("export dynamique non implémenté : %s(%08X, %08X, %08X) lr=%08X -> 0", name.c_str(),
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
}
} // namespace

// int OSDynLoad_Acquire(const char* name, OSDynLoad_Module* out)
HLE_FUNC(coreinit, OSDynLoad_Acquire)
{
    std::string name = readString(ARG_U32(3));
    if (auto dot = name.rfind(".rpl"); dot != std::string::npos)
        name.resize(dot);
    std::lock_guard lock(s_dynMutex);
    s_modules.push_back(name);
    store32(ARG_U32(4), uint32_t(s_modules.size()));
    RT_INFO("OSDynLoad_Acquire(%s)", name.c_str());
    RET_U32(0);
}

// int OSDynLoad_FindExport(module, BOOL isData, const char* name, void** out)
HLE_FUNC(coreinit, OSDynLoad_FindExport)
{
    std::string module;
    {
        std::lock_guard lock(s_dynMutex);
        const uint32_t id = ARG_U32(3);
        module = id && id <= s_modules.size() ? s_modules[id - 1] : "";
    }
    const bool isData = ARG_U32(4) != 0;
    const std::string name = readString(ARG_U32(5));
    if (const uint32_t addr = findImportAddress(module.c_str(), name.c_str()))
    {
        store32(ARG_U32(6), addr);
        RET_U32(0);
        return;
    }
    const std::string key = module + "::" + name;
    std::lock_guard lock(s_dynMutex);
    uint32_t& addr = s_dynExports[key];
    if (!addr)
    {
        if (isData)
            addr = sysAlloc(0x1000);
        else
        {
            addr = exportHostFunction(dynamicExport);
            if (HleFn impl = findDynamicHle(module, name))
                s_dynImpls[addr] = impl;
            else
                s_dynStubNames[addr] = key;
        }
    }
    store32(ARG_U32(6), addr);
    RET_U32(0);
}

HLE_FUNC(coreinit, OSDynLoad_Release) {}
HLE_FUNC(coreinit, OSDynLoad_SetAllocator) { RET_U32(0); }

// ================================================================= système
HLE_FUNC(coreinit, ENVGetEnvironmentVariable)
{
    if (ARG_U32(4) && ARG_U32(5))
        store8(ARG_U32(4), 0);
    RET_U32(1); // non trouvée
}

HLE_FUNC(coreinit, bspGetHardwareVersion)
{
    store32(ARG_U32(3), 0x26100028); // Wii U grand public
    RET_U32(0);
}

HLE_FUNC(coreinit, OSIsDebuggerInitialized) { RET_U32(0); }
HLE_FUNC(coreinit, OSIsDebuggerPresent) { RET_U32(0); }
HLE_FUNC(coreinit, OSDriver_Register) { RET_U32(0); }
HLE_FUNC(coreinit, OSSavesDone_ReadyToRelease) {}
HLE_FUNC(coreinit, OSEnableHomeButtonMenu) { RET_U32(1); }
HLE_FUNC(coreinit, OSIsHomeButtonMenuEnabled) { RET_U32(1); }
HLE_FUNC(coreinit, IMDisableDim) { RET_U32(0); }
HLE_FUNC(coreinit, IMEnableDim) { RET_U32(0); }
HLE_FUNC(coreinit, IMIsDimEnabled)
{
    store32(ARG_U32(3), 0);
    RET_U32(0);
}
HLE_FUNC(coreinit, OSReadRegister16) { RET_U32(0); }
HLE_FUNC(coreinit, OSWriteRegister16) {}
HLE_FUNC(coreinit, __OSReadRegister32Ex) { RET_U32(0); }
HLE_FUNC(coreinit, __OSWriteRegister32Ex) {}

// MCP : réglages produit (région, numéro de série…)
HLE_FUNC(coreinit, MCP_Open) { RET_U32(1); }
HLE_FUNC(coreinit, MCP_Close) { RET_U32(0); }
// MCPSysProdSettings (decaf ios_mcp_config.h) : product_area u32 +0x00, eeprom_version
// u16 +0x04, game_region u32 +0x08, ntsc_pal char[5] +0x10… (taille 0x46).
// MCPRegion : 1 = Japon, 2 = USA, 4 = Europe.
HLE_FUNC(coreinit, MCP_GetSysProdSettings)
{
    const uint32_t s = ARG_U32(4);
    memset(ptr(s), 0, 0x46);
    store32(s + 0x00, 4);
    store32(s + 0x08, 4);
    memcpy(ptr(s + 0x10), "PAL", 4);
    RET_U32(0);
}

// UC : configuration utilisateur (langue, pays, contrôle parental…)
// UCSysConfig { char name[64]; u32 access; u32 dataType; s32 error; u32 dataSize; void* data; }
HLE_FUNC(coreinit, UCOpen) { RET_U32(1); }
HLE_FUNC(coreinit, UCClose) { RET_U32(0); }
HLE_FUNC(coreinit, UCReadSysConfig)
{
    const uint32_t count = ARG_U32(4), settings = ARG_U32(5);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t e = settings + i * 0x54;
        const std::string name = readString(e, 64);
        const uint32_t size = load32(e + 0x4C), data = load32(e + 0x50);
        uint32_t value = 0;
        if (name == "cafe.language")
            value = 2; // français
        else if (name == "cafe.cntry_reg")
            value = 77; // France
        else if (name == "cafe.initial_launch")
            value = 2;
        if (data && size)
        {
            memset(ptr(data), 0, size);
            if (size == 1)
                store8(data, uint8_t(value));
            else if (size == 2)
                store16(data, uint16_t(value));
            else if (size >= 4)
                store32(data, value);
        }
        store32(e + 0x48, 0);
        RT_INFO("UCReadSysConfig : %s = %u", name.c_str(), value);
    }
    RET_U32(0);
}
