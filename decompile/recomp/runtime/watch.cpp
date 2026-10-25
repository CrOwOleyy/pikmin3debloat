// Surveillance d'écriture en mémoire invitée (diagnostic) : watchRange() pose une page de garde
// sur une plage ; chaque écriture dans la plage journalise la pile d'appels hôte, c'est-à-dire
// les fonctions recompilées (sub_XXXXXXXX) qui l'ont faite. Après un accès hors plage ou une
// lecture, la garde est reposée au pas suivant (drapeau de pas à pas).
#include "runtime.h"

#include <windows.h>
#include <dbghelp.h>

#include <mutex>
#include <string>

namespace rt
{

namespace
{
std::mutex s_lock;
uint32_t s_begin = 0, s_end = 0, s_hits = 0, s_maxHits = 0;
std::string s_label;
thread_local uint8_t* t_rearm = nullptr;

void guard(uint8_t* page)
{
    DWORD old;
    VirtualProtect(page, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
}

std::string stackOf(CONTEXT* source)
{
    const HANDLE process = GetCurrentProcess();
    static bool init = false;
    if (!init)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        init = true;
    }
    CONTEXT context = *source;
    STACKFRAME64 frame {};
    frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    std::string text;
    for (int depth = 0; depth < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame,
             &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); depth++)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        text += depth ? " <- " : "";
        text += SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) ? symbol->Name : "?";
    }
    return text;
}

LONG CALLBACK watchHandler(EXCEPTION_POINTERS* info)
{
    auto* rec = info->ExceptionRecord;
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm)
    {
        guard(t_rearm);
        t_rearm = nullptr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rec->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || rec->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    auto* host = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);
    if (host < g_base || host >= g_base + 0x100000000ull)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t guest = uint32_t(host - g_base);
    const bool write = rec->ExceptionInformation[0] == 1;
    {
        std::lock_guard lock(s_lock);
        if (write && guest >= s_begin && guest < s_end && s_hits < s_maxHits)
        {
            ++s_hits;
            PPCContext* ctx = currentContext();
            RT_INFO("GUET %s : écriture 0x%08X (+0x%X) lr=0x%08X : %s", s_label.c_str(), guest, guest - s_begin,
                ctx ? uint32_t(ctx->lr) : 0, stackOf(info->ContextRecord).c_str());
        }
        if (s_hits >= s_maxHits)
            return EXCEPTION_CONTINUE_EXECUTION; // garde consommée, non reposée
    }
    // Reposer la garde après l'instruction fautive
    t_rearm = g_base + (guest & ~0xFFFu);
    info->ContextRecord->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

std::string hostStack()
{
    CONTEXT context {};
    RtlCaptureContext(&context);
    return stackOf(&context);
}

void watchRange(uint32_t addr, uint32_t size, uint32_t maxHits, const char* label)
{
    static bool installed = false;
    {
        std::lock_guard lock(s_lock);
        if (!installed)
        {
            AddVectoredExceptionHandler(1, watchHandler);
            installed = true;
        }
        s_begin = addr;
        s_end = addr + size;
        s_hits = 0;
        s_maxHits = maxHits;
        s_label = label;
    }
    for (uint32_t page = addr & ~0xFFFu; page < addr + size; page += 0x1000)
        guard(g_base + page);
    RT_INFO("GUET %s : 0x%08X..0x%08X", label, addr, addr + size);
}

} // namespace rt

namespace rt
{

// Recherche d'une chaîne (avec son zéro final) dans la mémoire invitée engagée, hors image ELF :
// journalise chaque adresse avec les 16 octets qui la précèdent (diagnostic).
void findGuestString(const char* text, uint32_t skipBegin, uint32_t skipEnd)
{
    const size_t len = strlen(text) + 1;
    uint8_t* const base = ptr(0);
    for (uint64_t addr = 0x10000; addr < 0x100000000ull;)
    {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(base + addr, &info, sizeof(info)))
            break;
        const uint64_t end = uint64_t(static_cast<uint8_t*>(info.BaseAddress) - base) + info.RegionSize;
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            for (uint64_t a = addr; a + len <= end; a++)
                if (base[a] == uint8_t(text[0]) && !memcmp(base + a, text, len) && (a < skipBegin || a >= skipEnd))
                {
                    char context[80] = {};
                    for (int i = 0; i < 16; i++)
                        snprintf(context + i * 3, 4, "%02X ", base[a - 16 + i]);
                    RT_INFO("chaîne « %s » en %08X (avant : %s)", text, uint32_t(a), context);
                }
        addr = end;
    }
}

} // namespace rt
