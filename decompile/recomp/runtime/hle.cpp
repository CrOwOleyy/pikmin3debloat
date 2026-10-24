// Dispatch des imports Cafe OS : chaque import du RPX (generated/imports.inc) devient
// une fonction appelée par le code recompilé, qui renvoie vers l'implémentation HLE
// enregistrée par HLE_FUNC, ou signale l'import comme non implémenté.
#include <cstring>
#include <chrono>
#include <thread>
#include "kernel.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{

enum ImportId
{
#define CAFE_IMPORT_FUNC(lib, sym, cname, addr) IMP_##cname,
#define CAFE_IMPORT_DATA(lib, sym, cname, addr, size)
#include "generated/imports.inc"
#undef CAFE_IMPORT_FUNC
#undef CAFE_IMPORT_DATA
    IMP_COUNT
};

struct ImportInfo
{
    const char* lib;
    const char* sym;
    uint32_t addr;
};

const ImportInfo kFuncImports[] = {
#define CAFE_IMPORT_FUNC(lib, sym, cname, addr) { #lib, sym, addr },
#define CAFE_IMPORT_DATA(lib, sym, cname, addr, size)
#include "generated/imports.inc"
#undef CAFE_IMPORT_FUNC
#undef CAFE_IMPORT_DATA
};

const ImportInfo kDataImports[] = {
#define CAFE_IMPORT_FUNC(lib, sym, cname, addr)
#define CAFE_IMPORT_DATA(lib, sym, cname, addr, size) { #lib, sym, addr },
#include "generated/imports.inc"
#undef CAFE_IMPORT_FUNC
#undef CAFE_IMPORT_DATA
};

rt::HleFn g_impl[IMP_COUNT];
struct HleImplEx
{
    rt::HleFnEx fn;
    void* user;
};
HleImplEx g_implEx[IMP_COUNT];
std::atomic<uint32_t> g_calls[IMP_COUNT];

std::unordered_map<std::string, rt::HleFn>& funcRegistry()
{
    static std::unordered_map<std::string, rt::HleFn> r;
    return r;
}

std::unordered_map<std::string, rt::HleDataInit>& dataRegistry()
{
    static std::unordered_map<std::string, rt::HleDataInit> r;
    return r;
}

// Enregistreur d'appels HLE en mémoire (--record) : quasiment sans coût, contrairement à
// --trace qui écrit chaque ligne sur disque et modifie le timing des threads.
struct HleEvent
{
    uint32_t thread;
    int id;
    uint32_t r3, r4, r5, lr, ret;
};
constexpr size_t kEventRing = 1 << 14;
HleEvent g_events[kEventRing];
std::atomic<uint64_t> g_eventPos{ 0 };

void hleCall(PPCContext& ctx, uint8_t* base, int id)
{
    const uint32_t n = g_calls[id]++;
    HleEvent* ev = nullptr;
    if (rt::g_recordHle && (rt::g_recordFilter.empty() || strstr(kFuncImports[id].sym, rt::g_recordFilter.c_str())))
    {
        const auto* t = rt::kernel::current();
        ev = &g_events[g_eventPos++ % kEventRing];
        *ev = { t ? t->guest : 0, id, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr), 0xDEADDEAD };
    }
    const bool trace = rt::g_traceHle &&
        (rt::g_traceFilter.empty() || strstr(kFuncImports[id].sym, rt::g_traceFilter.c_str()));
    if (trace)
    {
        const auto* t = rt::kernel::current();
        RT_TRACE("[%s] %s::%s(%08X, %08X, %08X, %08X) lr=%08X",
            t ? rt::readString(rt::load32(t->guest + rt::kernel::kThreadName)).c_str() : "?",
            kFuncImports[id].lib, kFuncImports[id].sym, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
            uint32_t(ctx.lr));
    }
    if (const HleImplEx& ex = g_implEx[id]; ex.fn)
    {
        ex.fn(ctx, base, ex.user);
        if (ev)
            ev->ret = ctx.r3.u32;
        return;
    }
    if (rt::HleFn fn = g_impl[id])
    {
        fn(ctx, base);
        if (trace)
            RT_TRACE("    %s -> %08X", kFuncImports[id].sym, ctx.r3.u32);
        if (ev)
            ev->ret = ctx.r3.u32;
        return;
    }
    if (n == 0)
        RT_WARN("HLE non implémenté : %s::%s(%08X, %08X, %08X, %08X) lr=%08X -> 0", kFuncImports[id].lib,
            kFuncImports[id].sym, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
    // SpotPass (nn_boss) absent : le thread BossMgr du jeu interroge ces fonctions en boucle sans
    // attendre (un cœur à 100 %, ce qui, sur un processeur à GPU intégré, prend de la puissance
    // au GPU). Une courte pause en fait une attente polie sans changer les réponses.
    if (strcmp(kFuncImports[id].lib, "nn_boss") == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ctx.r3.u64 = 0;
}

} // namespace

// Fonctions appelées par le code recompilé, une par import.
#define CAFE_IMPORT_FUNC(lib, sym, cname, addr) \
    PPC_FUNC(cname) { hleCall(ctx, base, IMP_##cname); }
#define CAFE_IMPORT_DATA(lib, sym, cname, addr, size)
#include "generated/imports.inc"
#undef CAFE_IMPORT_FUNC
#undef CAFE_IMPORT_DATA

namespace rt
{

bool g_traceHle = false;
std::string g_traceFilter;
bool g_recordHle = false;
std::string g_recordFilter;

void dumpHleEvents(size_t count)
{
    const uint64_t end = g_eventPos.load();
    const uint64_t begin = end > count ? end - count : 0;
    RT_INFO("---- %llu derniers appels HLE enregistrés ----", (unsigned long long)(end - begin));
    for (uint64_t i = begin; i < end; i++)
    {
        const HleEvent& e = g_events[i % kEventRing];
        const std::string name = e.thread ? readString(load32(e.thread + kernel::kThreadName)) : "?";
        RT_INFO("  [%s] %s(%08X, %08X, %08X) lr=%08X -> %08X", name.c_str(), kFuncImports[e.id].sym, e.r3, e.r4,
            e.r5, e.lr, e.ret);
    }
}
static std::mutex s_logMutex;
static FILE* s_logFile = nullptr;

HleRegistrar::HleRegistrar(const char* lib, const char* name, HleFn fn)
{
    funcRegistry()[std::string(lib) + "::" + name] = fn;
}

HleDataRegistrar::HleDataRegistrar(const char* lib, const char* name, HleDataInit fn)
{
    dataRegistry()[std::string(lib) + "::" + name] = fn;
}

void hleInit()
{
    auto& funcs = funcRegistry();
    size_t implemented = 0;
    for (int i = 0; i < IMP_COUNT; i++)
    {
        auto it = funcs.find(std::string(kFuncImports[i].lib) + "::" + kFuncImports[i].sym);
        if (it != funcs.end())
        {
            g_impl[i] = it->second;
            implemented++;
            funcs.erase(it);
        }
    }
    // Le reste sert aux bibliothèques chargées par OSDynLoad (erreula, swkbd…).
    RT_INFO("%zu implémentations HLE réservées aux exports dynamiques", funcs.size());

    auto& datas = dataRegistry();
    for (auto& d : kDataImports)
    {
        auto it = datas.find(std::string(d.lib) + "::" + d.sym);
        if (it != datas.end())
            it->second(d.addr);
        else
            RT_WARN("donnée importée sans initialisation : %s::%s", d.lib, d.sym);
    }
    RT_INFO("imports HLE : %zu/%d fonctions implémentées", implemented, int(IMP_COUNT));
}

void hleInvoke(const char* lib, const char* name, PPCContext& ctx)
{
    for (int i = 0; i < IMP_COUNT; i++)
        if (!strcmp(lib, kFuncImports[i].lib) && !strcmp(name, kFuncImports[i].sym))
        {
            if (g_implEx[i].fn)
                g_implEx[i].fn(ctx, g_base, g_implEx[i].user);
            else if (g_impl[i])
                g_impl[i](ctx, g_base);
            else
                fatal("hleInvoke : %s::%s non implémenté", lib, name);
            return;
        }
    if (HleFn fn = findDynamicHle(lib, name))
        return fn(ctx, g_base);
    fatal("hleInvoke : %s::%s inconnu", lib, name);
}

std::vector<std::string> hleListWithoutEx(const std::string& lib)
{
    std::vector<std::string> out;
    for (int i = 0; i < IMP_COUNT; i++)
        if (lib == kFuncImports[i].lib && !g_implEx[i].fn)
            out.push_back(std::string(kFuncImports[i].sym) + (g_impl[i] ? " (stub maison)" : " (rien)"));
    return out;
}

bool hleSetImplementation(const std::string& lib, const std::string& name, HleFnEx fn, void* user)
{
    for (int i = 0; i < IMP_COUNT; i++)
        if (lib == kFuncImports[i].lib && name == kFuncImports[i].sym)
        {
            g_implEx[i] = { fn, user };
            return true;
        }
    return false;
}

HleFn findDynamicHle(const std::string& lib, const std::string& name)
{
    auto& funcs = funcRegistry();
    auto it = funcs.find(lib + "::" + name);
    return it == funcs.end() ? nullptr : it->second;
}

uint32_t findImportAddress(const char* lib, const char* sym)
{
    for (auto& f : kFuncImports)
        if (!strcmp(f.lib, lib) && !strcmp(f.sym, sym))
            return f.addr;
    return 0;
}

void logf(LogLevel level, const char* fmt, ...)
{
    static const char* tags[] = { "trace", "info", "warn", "ERREUR" };
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard lock(s_logMutex);
    if (!s_logFile)
        s_logFile = fopen("pikmin3.log", "w");
    if (level != LogLevel::Trace)
        fprintf(stderr, "[%s] %s\n", tags[int(level)], buf);
    if (s_logFile)
    {
        fprintf(s_logFile, "[%s] %s\n", tags[int(level)], buf);
        fflush(s_logFile);
    }
}

void fatal(const char* fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logf(LogLevel::Error, "%s", buf);
    fflush(stderr);
    std::_Exit(1);
}

// ---------------------------------------------------------------- printf invité
namespace
{

struct ArgSource
{
    virtual uint32_t u32() = 0;
    virtual uint64_t u64() = 0;
    virtual double f64() = 0;
    virtual ~ArgSource() = default;
};

// Appel variadique direct : GPR r[n]..r10, FPR f1..f8, puis zone de débordement 8(r1).
struct RegisterArgs final : ArgSource
{
    PPCContext& ctx;
    int gpr;
    int fpr = 1;
    uint32_t stack;
    RegisterArgs(PPCContext& c, int first) : ctx(c), gpr(first), stack(c.r1.u32 + 8) {}

    uint32_t reg(int n)
    {
        const PPCRegister* r[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
        return r[n - 3]->u32;
    }
    uint32_t u32() override
    {
        if (gpr <= 10)
            return reg(gpr++);
        uint32_t v = load32(stack);
        stack += 4;
        return v;
    }
    uint64_t u64() override
    {
        if (gpr % 2 == 0)
            gpr++; // paires de registres alignées (r3:r4, r5:r6…)
        if (gpr < 10)
        {
            uint64_t v = (uint64_t(reg(gpr)) << 32) | reg(gpr + 1);
            gpr += 2;
            return v;
        }
        gpr = 11;
        stack = (stack + 7) & ~7u;
        uint64_t v = load64(stack);
        stack += 8;
        return v;
    }
    double f64() override
    {
        const PPCRegister* f[] = { &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7, &ctx.f8 };
        if (fpr <= 8)
            return f[fpr++ - 1]->f64;
        stack = (stack + 7) & ~7u;
        uint64_t v = load64(stack);
        stack += 8;
        double d;
        memcpy(&d, &v, 8);
        return d;
    }
};

// va_list SVR4 PowerPC : { u8 gpr; u8 fpr; u16 pad; u32 overflow_arg_area; u32 reg_save_area }
struct VaListArgs final : ArgSource
{
    uint32_t va;
    VaListArgs(uint32_t v) : va(v) {}
    uint32_t u32() override
    {
        uint8_t gpr = load8(va);
        if (gpr < 8)
        {
            store8(va, gpr + 1);
            return load32(load32(va + 8) + gpr * 4);
        }
        uint32_t over = load32(va + 4);
        store32(va + 4, over + 4);
        return load32(over);
    }
    uint64_t u64() override
    {
        uint8_t gpr = load8(va);
        gpr += gpr & 1;
        if (gpr < 7)
        {
            store8(va, gpr + 2);
            uint32_t save = load32(va + 8);
            return (uint64_t(load32(save + gpr * 4)) << 32) | load32(save + gpr * 4 + 4);
        }
        store8(va, 8);
        uint32_t over = (load32(va + 4) + 7) & ~7u;
        store32(va + 4, over + 8);
        return load64(over);
    }
    double f64() override
    {
        uint8_t fpr = load8(va + 1);
        uint64_t v;
        if (fpr < 8)
        {
            store8(va + 1, fpr + 1);
            v = load64(load32(va + 8) + 32 + fpr * 8);
        }
        else
        {
            uint32_t over = (load32(va + 4) + 7) & ~7u;
            store32(va + 4, over + 8);
            v = load64(over);
        }
        double d;
        memcpy(&d, &v, 8);
        return d;
    }
};

std::string format(uint32_t fmtAddr, ArgSource& args)
{
    std::string out;
    const std::string fmt = readString(fmtAddr);
    for (size_t i = 0; i < fmt.size(); i++)
    {
        if (fmt[i] != '%')
        {
            out += fmt[i];
            continue;
        }
        size_t start = i++;
        while (i < fmt.size() && strchr("-+ #0", fmt[i]))
            i++;
        while (i < fmt.size() && (isdigit((unsigned char)fmt[i]) || fmt[i] == '.' || fmt[i] == '*'))
            i++;
        int longs = 0;
        bool half = false;
        while (i < fmt.size() && strchr("hlLqjzt", fmt[i]))
        {
            if (fmt[i] == 'l' || fmt[i] == 'q' || fmt[i] == 'L' || fmt[i] == 'j')
                longs++;
            if (fmt[i] == 'h')
                half = true;
            i++;
        }
        if (i >= fmt.size())
            break;
        char conv = fmt[i];
        std::string spec = fmt.substr(start, i - start);
        // retire les modificateurs de longueur : on réécrit le spécificateur pour l'hôte
        std::string clean;
        for (char c : spec)
            if (!strchr("hlLqjzt", c))
                clean += c;
        char buf[512];
        switch (conv)
        {
        case '%':
            out += '%';
            continue;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (longs >= 2)
                snprintf(buf, sizeof(buf), (clean + "ll" + conv).c_str(), (long long)args.u64());
            else
            {
                uint32_t v = args.u32();
                if (half)
                    v = (conv == 'd' || conv == 'i') ? uint32_t(int32_t(int16_t(v))) : (v & 0xFFFF);
                snprintf(buf, sizeof(buf), (clean + conv).c_str(), v);
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            snprintf(buf, sizeof(buf), (clean + conv).c_str(), args.f64());
            break;
        case 's':
            snprintf(buf, sizeof(buf), (clean + 's').c_str(), readString(args.u32()).c_str());
            break;
        case 'p':
            snprintf(buf, sizeof(buf), "0x%08X", args.u32());
            break;
        case 'n':
            args.u32();
            continue;
        default:
            out += spec + conv;
            continue;
        }
        out += buf;
    }
    return out;
}

} // namespace

std::string formatGuest(PPCContext& ctx, uint32_t fmtAddr, int firstGpr)
{
    RegisterArgs args(ctx, firstGpr);
    return format(fmtAddr, args);
}

std::string formatGuestVaList(uint32_t fmtAddr, uint32_t vaList)
{
    VaListArgs args(vaList);
    return format(fmtAddr, args);
}

} // namespace rt
