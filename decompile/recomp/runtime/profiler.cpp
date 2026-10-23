// --profile <s> : profileur par échantillonnage. Pendant <s> secondes (après 60 s de jeu), un
// thread suspend chaque thread du processus toutes les millisecondes, relève son pointeur
// d'instruction, puis affiche les fonctions les plus fréquentes (symboles du .pdb via dbghelp).
#include "runtime.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace rt
{
uint32_t g_profileSeconds = 0;

static std::string symbolName(HANDLE process, uint64_t address)
{
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    return SymFromAddr(process, address, &displacement, symbol) ? symbol->Name : "?";
}

static void profileLoop()
{
    std::this_thread::sleep_for(std::chrono::seconds(60));
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

    // Threads du processus (ouverts une fois)
    const DWORD self = GetCurrentThreadId();
    std::vector<std::pair<DWORD, HANDLE>> threads;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry { sizeof(entry) };
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != self)
            if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID))
                threads.push_back({ entry.th32ThreadID, h });
    CloseHandle(snapshot);

    // Par thread : pile d'appels résumée en « fonction feuille <- 4 appelants », hors attente
    // comptée à part avec son premier appelant hors système.
    struct ThreadStats { uint32_t samples = 0; std::map<std::string, uint32_t> stacks; };
    std::map<DWORD, ThreadStats> stats;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(g_profileSeconds);
    uint32_t passes = 0;
    while (std::chrono::steady_clock::now() < end)
    {
        for (auto& [id, thread] : threads)
        {
            if (SuspendThread(thread) == DWORD(-1))
                continue;
            CONTEXT context {};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context))
            {
                STACKFRAME64 frame {};
                frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
                std::vector<uint64_t> pcs;
                for (int depth = 0; depth < 16 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                         &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); depth++)
                    pcs.push_back(frame.AddrPC.Offset);
                ResumeThread(thread);
                std::string key;
                for (size_t i = 0; i < pcs.size() && i < 12; i++)
                    key += (i ? " <- " : "") + symbolName(process, pcs[i]);
                auto& st = stats[id];
                st.samples++;
                st.stacks[key]++;
            }
            else
                ResumeThread(thread);
        }
        ++passes;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    RT_INFO("profil : %u passes", passes);
    for (auto& [id, st] : stats)
    {
        std::vector<std::pair<uint32_t, std::string>> sorted;
        for (auto& [key, count] : st.stacks)
            sorted.push_back({ count, key });
        std::sort(sorted.rbegin(), sorted.rend());
        RT_INFO("profil thread %lu (%u échantillons)", id, st.samples);
        const bool gpu = !sorted.empty() && sorted[0].second.find("ringbuffer") != std::string::npos;
        for (size_t i = 0; i < sorted.size() && i < (gpu ? 30 : 4); i++)
            RT_INFO("profil   %5.1f%%  %s", 100.0 * sorted[i].first / st.samples, sorted[i].second.c_str());
    }
}

void profileStart()
{
    if (g_profileSeconds)
        std::thread(profileLoop).detach();
}
} // namespace rt
