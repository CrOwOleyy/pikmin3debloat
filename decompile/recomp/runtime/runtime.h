// Runtime de Pikmin 3 Recomp : ce qui entoure le code recompilé par XenonRecomp.
//
// Le code généré (generated/ppc_recomp.*.cpp) manipule un PPCContext et une mémoire
// invitée big-endian de 4 Gio (`base`). Le runtime fournit cette mémoire, charge
// l'image du jeu, résout les appels indirects et implémente en HLE les bibliothèques
// Cafe OS importées (coreinit, gx2, snd_core…).
#pragma once

#include "ppc_config.h"
#include "ppc_context.h"

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace rt
{

// ---------------------------------------------------------------- mémoire invitée
extern uint8_t* g_base;

// Carte mémoire invitée (adresses Cafe OS).
constexpr uint32_t kCodeBase = 0x02000000;     // .text du RPX
constexpr uint32_t kImportBase = 0x03500000;   // imports relogés (scripts/link_rpl.py)
constexpr uint32_t kImportEnd = 0x03520000;
constexpr uint32_t kHostExportBase = 0x0351F000; // fonctions hôtes exposées au jeu
constexpr uint32_t kMem2Base = 0x10000000;     // MEM2 : données du RPX puis tas par défaut
constexpr uint32_t kMem2End = 0x50000000;
constexpr uint32_t kSysBase = 0x60000000;      // allocations internes au runtime
constexpr uint32_t kSysSize = 0x08000000;
constexpr uint32_t kFgBucketBase = 0xE0000000; // « foreground bucket »
constexpr uint32_t kFgBucketSize = 0x04000000;
constexpr uint32_t kMem1Base = 0xF4000000;     // MEM1 (32 Mio)
constexpr uint32_t kMem1Size = 0x02000000;

void memInit();
void memCommit(uint32_t addr, uint32_t size);
// Allocation « système » (structures du runtime visibles par le jeu : OSThread du
// thread principal, piles, chaînes…). Jamais libérée.
uint32_t sysAlloc(uint32_t size, uint32_t align = 16);

inline uint8_t* ptr(uint32_t a) { return g_base + a; }
inline uint8_t load8(uint32_t a) { return g_base[a]; }
inline uint16_t load16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
inline uint32_t load32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
inline uint64_t load64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
inline void store8(uint32_t a, uint8_t v) { g_base[a] = v; }
inline void store16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
inline void store32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
inline void store64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }
inline std::string readString(uint32_t a, size_t max = 4096)
{
    if (!a)
        return "(null)";
    const char* s = reinterpret_cast<const char*>(g_base + a);
    return std::string(s, strnlen(s, max));
}
inline std::string pathToUtf8(const std::filesystem::path& p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
inline void writeString(uint32_t a, const std::string& s)
{
    memcpy(g_base + a, s.c_str(), s.size() + 1);
}

// ---------------------------------------------------------------- CPU invité
// Table adresse invitée -> fonction native (code recompilé, imports, exports hôtes).
void cpuInit();
void registerFunction(uint32_t address, PPCFunc* fn);
PPCFunc* findFunction(uint32_t address);
// Expose une fonction hôte au jeu sous une adresse invitée (pointeurs de fonction
// que le jeu appelle, ex. MEMAllocFromDefaultHeap).
uint32_t exportHostFunction(PPCFunc* fn);
// Implémentation avec contexte (ex. une fonction HLE de decaf et son enveloppe d'appel).
using HleFnEx = void (*)(PPCContext& ctx, uint8_t* base, void* user);
uint32_t exportHostFunction(HleFnEx fn, void* user);

// Dernière adresse appelée indirectement sur ce thread (permet à une fonction hôte
// partagée entre plusieurs adresses invitées de savoir laquelle a été appelée).
extern thread_local uint32_t t_indirectTarget;

// Contexte CPU du thread courant (un thread hôte par thread Cafe OS).
PPCContext* currentContext();
void setCurrentContext(PPCContext* ctx);

// Appelle une fonction invitée depuis le runtime (callbacks : entrée de thread,
// alarmes, atexit…). Les registres non volatils sont préservés.
uint32_t callGuest(PPCContext& ctx, uint32_t address, std::initializer_list<uint32_t> args);

// ---------------------------------------------------------------- journal
enum class LogLevel { Trace, Info, Warn, Error };
void logf(LogLevel level, const char* fmt, ...);
#define RT_TRACE(...) ::rt::logf(::rt::LogLevel::Trace, __VA_ARGS__)
#define RT_INFO(...) ::rt::logf(::rt::LogLevel::Info, __VA_ARGS__)
#define RT_WARN(...) ::rt::logf(::rt::LogLevel::Warn, __VA_ARGS__)
#define RT_ERROR(...) ::rt::logf(::rt::LogLevel::Error, __VA_ARGS__)
[[noreturn]] void fatal(const char* fmt, ...);
extern bool g_traceHle;
extern bool g_probe; // --probe : sondes de diagnostic (probe.cpp, shaders…)
// Diagnostic : journalise la pile d'appels des écritures dans [addr, addr+size) (watch.cpp).
void watchRange(uint32_t addr, uint32_t size, uint32_t maxHits, const char* label);
std::string hostStack(); // pile d'appels hôte symbolisée (watch.cpp)
extern std::string g_traceFilter; // --trace-filter : ne trace que les imports contenant ce texte
extern bool g_recordHle;          // --record <filtre> : enregistre en mémoire, vidé par --dump
extern std::string g_recordFilter;
void dumpHleEvents(size_t count);

// ---------------------------------------------------------------- HLE
using HleFn = void (*)(PPCContext& ctx, uint8_t* base);

// Remplace l'implémentation d'un import ; faux si le RPX n'importe pas ce symbole.
// Noms des imports de `lib` qui n'ont pas d'implémentation « avec contexte » (decaf).
std::vector<std::string> hleListWithoutEx(const std::string& lib);
bool hleSetImplementation(const std::string& lib, const std::string& name, HleFnEx fn, void* user);

struct HleRegistrar
{
    HleRegistrar(const char* lib, const char* name, HleFn fn);
};

// Données importées (pointeurs MEMAllocFromDefaultHeap, _iob…) : initialisées au
// démarrage à leur adresse relogée.
using HleDataInit = void (*)(uint32_t addr);

struct HleDataRegistrar
{
    HleDataRegistrar(const char* lib, const char* name, HleDataInit fn);
};

void hleInit();
// Implémentation HLE enregistrée pour un export chargé dynamiquement (OSDynLoad),
// c'est-à-dire un HLE_FUNC qui ne correspond à aucun import statique du RPX.
HleFn findDynamicHle(const std::string& lib, const std::string& name);
// Appelle l'implémentation HLE d'un symbole (import ou export dynamique) avec ctx ; sert à
// la couche de compatibilité decaf, qui appelle coreinit depuis du code hôte.
void hleInvoke(const char* lib, const char* name, PPCContext& ctx);

// Tas MEM2 par défaut (MEMAllocFromDefaultHeapEx / MEMFreeToDefaultHeap).
uint32_t defaultHeapAlloc(uint32_t size, int32_t align);
void defaultHeapFree(uint32_t ptr);

// Formate une chaîne printf invitée (OSReport, vsnprintf…) : arguments entiers dans
// r[firstGpr..10], flottants dans f1..f8, puis sur la pile (va_list GHS si vaList).
std::string formatGuest(PPCContext& ctx, uint32_t fmtAddr, int firstGpr);
std::string formatGuestVaList(uint32_t fmtAddr, uint32_t vaList);

} // namespace rt

// Implémentation HLE d'un import : HLE_FUNC(coreinit, OSReport) { ... }
// Le nom est le symbole importé tel qu'il figure dans le RPX (y compris les noms C++
// décorés GHS, ex. GetAccountId__Q2_2nn3actFPc).
#define HLE_FUNC(lib, name)                                                          \
    static void hle_##lib##_##name(PPCContext& ctx, uint8_t* base);                 \
    static ::rt::HleRegistrar hle_reg_##lib##_##name(#lib, #name, hle_##lib##_##name); \
    static void hle_##lib##_##name([[maybe_unused]] PPCContext& ctx, [[maybe_unused]] uint8_t* base)

#define HLE_DATA(lib, name)                                                               \
    static void hle_data_##lib##_##name(uint32_t addr);                                   \
    static ::rt::HleDataRegistrar hle_dreg_##lib##_##name(#lib, #name, hle_data_##lib##_##name); \
    static void hle_data_##lib##_##name([[maybe_unused]] uint32_t addr)

// Accès aux arguments / valeur de retour selon l'ABI PowerPC EABI.
#define ARG_U32(n) (ctx.r##n.u32)
#define ARG_S32(n) (ctx.r##n.s32)
#define RET_U32(v) (ctx.r3.u64 = uint32_t(v))
#define RET_S32(v) (ctx.r3.s64 = int32_t(v))
#define RET_U64(v) do { uint64_t _v = (v); ctx.r3.u64 = uint32_t(_v >> 32); ctx.r4.u64 = uint32_t(_v); } while (0)
