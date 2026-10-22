// coreinit : threads, mutex, conditions, événements, files de messages, rendez-vous,
// alarmes, temps, interruptions, verrous de la libc GHS.
//
// Chaque thread Cafe OS tourne sur un thread hôte avec son propre PPCContext. L'état
// des primitives vit dans leurs structures invitées (offsets decaf-emu) et n'est
// modifié que sous g_lock.
#include "kernel.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <ctime>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rt::kernel
{

std::mutex g_lock;
std::condition_variable g_cv;

static thread_local Thread* t_current = nullptr;
static std::unordered_map<uint32_t, Thread*> s_threads; // OSThread -> état hôte
static uint32_t s_sdaBase = 0;
static uint32_t s_sda2Base = 0;
static uint16_t s_nextThreadId = 1;
static const auto s_bootClock = std::chrono::steady_clock::now();
static uint64_t s_baseTicks = 0; // OSGetTime au démarrage (ticks depuis le 01/01/2000)

Thread* current() { return t_current; }
uint32_t sdaBase() { return s_sdaBase; }
uint32_t sda2Base() { return s_sda2Base; }
void setSdaBases(uint32_t sda, uint32_t sda2)
{
    s_sdaBase = sda;
    s_sda2Base = sda2;
}

uint64_t systemTicks()
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s_bootClock).count();
    return uint64_t(ns) * kTimerSpeed / 1000000000ull;
}

uint64_t timeTicks()
{
    if (!s_baseTicks)
    {
        const int64_t unixSecs = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        s_baseTicks = uint64_t(unixSecs - 946684800) * kTimerSpeed - systemTicks();
    }
    return s_baseTicks + systemTicks();
}

static std::chrono::nanoseconds ticksToDuration(uint64_t ticks)
{
    return std::chrono::nanoseconds(ticks * 1000000000ull / kTimerSpeed);
}

static PPCContext* newContext()
{
    auto* ctx = new PPCContext{};
    ctx->fpscr.loadFromHost();
    ctx->r2.u64 = s_sda2Base;
    ctx->r13.u64 = s_sdaBase;
    return ctx;
}

static void initThreadStruct(uint32_t t, uint32_t entry, uint32_t stackTop, uint32_t stackSize, int32_t prio, uint8_t attr)
{
    memset(ptr(t), 0, kThreadSize);
    store32(t + kThreadTag, 0x74487244); // 'tHrD'
    store8(t + kThreadState, 1);
    store8(t + kThreadAttr, attr);
    store16(t + kThreadId, s_nextThreadId++);
    store32(t + kThreadSuspend, 1);
    store32(t + kThreadPriority, uint32_t(prio));
    store32(t + kThreadBasePriority, uint32_t(prio));
    store32(t + kThreadStackStart, stackTop);
    store32(t + kThreadStackEnd, stackTop - stackSize);
    store32(t + kThreadEntry, entry);
}

static int coreFromAffinity(uint8_t attr)
{
    if (attr & 2) return 1;
    if (attr & 1) return 0;
    if (attr & 4) return 2;
    return 1;
}

// Pile hôte large : la récursion invitée devient récursion native.
static HANDLE startHostThread(LPTHREAD_START_ROUTINE fn, void* arg)
{
    HANDLE h = CreateThread(nullptr, 256u << 20, fn, arg, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h)
        fatal("CreateThread a échoué (%lu)", GetLastError());
    return h;
}

struct ThreadStart
{
    Thread* thread;
    uint32_t entry;
    uint32_t argc;
    uint32_t argv;
};

static DWORD WINAPI threadMain(void* p)
{
    auto* start = static_cast<ThreadStart*>(p);
    Thread* t = start->thread;
    t_current = t;
    setCurrentContext(t->ctx);
    PPCContext& ctx = *t->ctx;
    ctx.r3.u64 = start->argc;
    ctx.r4.u64 = start->argv;
    const uint32_t entry = start->entry;
    delete start;

    RT_INFO("thread %s démarré (entrée 0x%08X, cœur %d)", readString(load32(t->guest + kThreadName)).c_str(), entry, t->core);
    uint32_t value;
    try
    {
        PPCCallIndirect(ctx, g_base, entry);
        value = ctx.r3.u32;
    }
    catch (const ThreadExit& e)
    {
        value = e.value;
    }
    {
        std::lock_guard lock(g_lock);
        t->exited = true;
        t->exitValue = value;
        store32(t->guest + kThreadExitValue, value);
        store8(t->guest + kThreadState, 8); // Moribund
    }
    g_cv.notify_all();
    return 0;
}

struct ServiceStart
{
    std::string name;
    int core;
    std::function<void()> body;
};

static DWORD WINAPI serviceThreadMain(void* p)
{
    std::unique_ptr<ServiceStart> start(static_cast<ServiceStart*>(p));
    constexpr uint32_t kStackSize = 0x40000;
    auto* t = new Thread;
    {
        std::lock_guard lock(g_lock);
        t->guest = sysAlloc(kThreadSize, 8);
        const uint32_t stack = sysAlloc(kStackSize, 16) + kStackSize;
        initThreadStruct(t->guest, 0, stack, kStackSize, 0, uint8_t(1u << start->core));
        const uint32_t name = sysAlloc(uint32_t(start->name.size() + 1));
        writeString(name, start->name);
        store32(t->guest + kThreadName, name);
        store32(t->guest + kThreadSuspend, 0);
        store8(t->guest + kThreadState, 2);
        t->ctx = newContext();
        t->ctx->r1.u64 = stack - 0x10;
        t->core = start->core;
        t->suspend = 0;
        t->running = true;
        s_threads[t->guest] = t;
    }
    t_current = t;
    setCurrentContext(t->ctx);
    RT_INFO("thread système %s démarré (cœur %d)", start->name.c_str(), start->core);
    start->body();
    return 0;
}

void startServiceThread(const char* name, int core, std::function<void()> body)
{
    CloseHandle(startHostThread(serviceThreadMain, new ServiceStart{ name, core, std::move(body) }));
}

Thread* createMainThread(uint32_t stackSize)
{
    auto* t = new Thread;
    t->guest = sysAlloc(kThreadSize, 8);
    const uint32_t stack = sysAlloc(stackSize, 16) + stackSize;
    initThreadStruct(t->guest, 0, stack, stackSize, 16, 0x2 | 0x8);
    const uint32_t name = sysAlloc(32);
    writeString(name, "{SYS Thread Default Core 1}");
    store32(t->guest + kThreadName, name);
    store32(t->guest + kThreadSuspend, 0);
    store8(t->guest + kThreadState, 2);
    t->ctx = newContext();
    t->ctx->r1.u64 = stack - 0x10;
    t->core = 1;
    t->suspend = 0;
    t->running = true;
    s_threads[t->guest] = t;
    t_current = t;
    setCurrentContext(t->ctx);
    return t;
}

void dumpThreads()
{
    std::lock_guard lock(g_lock);
    RT_INFO("---- état des threads ----");
    for (auto& [guest, t] : s_threads)
    {
        const std::string name = readString(load32(guest + kThreadName));
        const char* state = t->exited ? "terminé" : (!t->running ? "suspendu" : (t->waitWhat ? "bloqué" : "actif"));
        if (t->waitWhat)
            RT_INFO("  %-32s cœur %d %-8s %s(0x%08X) appelé depuis 0x%08X", name.c_str(), t->core, state,
                t->waitWhat, t->waitObj, t->waitLr);
        else
        {
            RT_INFO("  %-32s cœur %d %-8s dernier lr=0x%08X r1=0x%08X", name.c_str(), t->core, state,
                t->ctx ? uint32_t(t->ctx->lr) : 0, t->ctx ? t->ctx->r1.u32 : 0);
            if (t->ctx && t->running && !t->exited)
            {
                const PPCContext& c = *t->ctx;
                RT_INFO("      r3=%08X r4=%08X r5=%08X r25=%08X r26=%08X r27=%08X r28=%08X r29=%08X r30=%08X r31=%08X",
                    c.r3.u32, c.r4.u32, c.r5.u32, c.r25.u32, c.r26.u32, c.r27.u32, c.r28.u32, c.r29.u32,
                    c.r30.u32, c.r31.u32);
                for (uint32_t reg : { c.r25.u32, c.r30.u32, c.r31.u32 })
                {
                    if (reg < kMem2Base || reg >= kMem2End - 0xC0)
                        continue;
                    std::string words;
                    char buf[16];
                    for (uint32_t off = 0; off < 0xC0; off += 4)
                    {
                        snprintf(buf, sizeof(buf), "%s%08X", off % 0x20 ? " " : (off ? "\n        " : ""), load32(reg + off));
                        words += buf;
                    }
                    RT_INFO("      [%08X]\n        %s", reg, words.c_str());
                }
            }
        }
    }
}

// Marque le thread courant comme bloqué le temps d'une attente (à construire sous g_lock).
struct WaitMark
{
    Thread* t;
    WaitMark(const char* what, uint32_t obj, uint32_t lr) : t(current())
    {
        if (t)
        {
            t->waitWhat = what;
            t->waitObj = obj;
            t->waitLr = lr;
        }
    }
    ~WaitMark()
    {
        if (t)
            t->waitWhat = nullptr;
    }
};

static Thread* threadOf(uint32_t guest)
{
    auto it = s_threads.find(guest);
    return it == s_threads.end() ? nullptr : it->second;
}

} // namespace rt::kernel

using namespace rt;
using namespace rt::kernel;

// ================================================================= threads
// BOOL OSCreateThread(OSThread*, entry, argc, argv, stackTop, stackSize, priority, attr)
HLE_FUNC(coreinit, OSCreateThread)
{
    const uint32_t thread = ARG_U32(3), entry = ARG_U32(4), argc = ARG_U32(5), argv = ARG_U32(6);
    const uint32_t stack = ARG_U32(7), stackSize = ARG_U32(8);
    const int32_t prio = ARG_S32(9);
    const uint8_t attr = uint8_t(ARG_U32(10));
    std::lock_guard lock(g_lock);
    initThreadStruct(thread, entry, stack, stackSize, prio, attr);
    auto* t = new Thread;
    t->guest = thread;
    t->ctx = newContext();
    t->ctx->r1.u64 = (stack & ~0xFu) - 0x10;
    store32(t->ctx->r1.u32, 0);
    t->core = coreFromAffinity(attr);
    // argc/argv conservés jusqu'au démarrage (OSResumeThread)
    t->ctx->r3.u64 = argc;
    t->ctx->r4.u64 = argv;
    s_threads[thread] = t;
    RET_U32(1);
}

HLE_FUNC(coreinit, OSResumeThread)
{
    const uint32_t thread = ARG_U32(3);
    std::lock_guard lock(g_lock);
    Thread* t = threadOf(thread);
    if (!t)
    {
        RT_WARN("OSResumeThread sur un thread inconnu 0x%08X", thread);
        RET_S32(0);
        return;
    }
    const int previous = t->suspend;
    if (t->suspend > 0)
        t->suspend--;
    store32(thread + kThreadSuspend, uint32_t(t->suspend));
    if (t->suspend == 0 && !t->running)
    {
        t->running = true;
        store8(thread + kThreadState, 2);
        auto* start = new ThreadStart{ t, load32(thread + kThreadEntry), t->ctx->r3.u32, t->ctx->r4.u32 };
        HANDLE h = startHostThread(threadMain, start);
        CloseHandle(h);
    }
    g_cv.notify_all();
    RET_S32(previous);
}

HLE_FUNC(coreinit, OSExitThread)
{
    throw ThreadExit{ ARG_U32(3) };
}

HLE_FUNC(coreinit, OSJoinThread)
{
    const uint32_t thread = ARG_U32(3), outValue = ARG_U32(4);
    std::unique_lock lock(g_lock);
    Thread* t = threadOf(thread);
    if (!t)
    {
        RET_U32(0);
        return;
    }
    WaitMark wm("OSJoinThread", thread, uint32_t(ctx.lr));
    g_cv.wait(lock, [&] { return t->exited; });
    if (outValue)
        store32(outValue, t->exitValue);
    RET_U32(1);
}

HLE_FUNC(coreinit, OSGetCurrentThread) { RET_U32(current() ? current()->guest : 0); }

HLE_FUNC(coreinit, OSSetThreadName)
{
    store32(ARG_U32(3) + kThreadName, ARG_U32(4));
}

HLE_FUNC(coreinit, OSGetThreadPriority) { RET_U32(load32(ARG_U32(3) + kThreadPriority)); }

HLE_FUNC(coreinit, OSSetThreadPriority)
{
    store32(ARG_U32(3) + kThreadPriority, ARG_U32(4));
    store32(ARG_U32(3) + kThreadBasePriority, ARG_U32(4));
    RET_U32(1);
}

HLE_FUNC(coreinit, OSSetThreadAffinity)
{
    const uint32_t thread = ARG_U32(3);
    const uint8_t aff = uint8_t(ARG_U32(4) & 7);
    store8(thread + kThreadAttr, (load8(thread + kThreadAttr) & ~7) | aff);
    std::lock_guard lock(g_lock);
    if (Thread* t = threadOf(thread); t && !t->running)
        t->core = coreFromAffinity(aff);
    RET_U32(1);
}

HLE_FUNC(coreinit, OSSetThreadSpecific)
{
    if (current() && ARG_U32(3) < 16)
        store32(current()->guest + kThreadSpecific + ARG_U32(3) * 4, ARG_U32(4));
}

HLE_FUNC(coreinit, OSGetThreadSpecific)
{
    RET_U32(current() && ARG_U32(3) < 16 ? load32(current()->guest + kThreadSpecific + ARG_U32(3) * 4) : 0);
}

HLE_FUNC(coreinit, OSYieldThread) { std::this_thread::yield(); }

HLE_FUNC(coreinit, OSSleepTicks)
{
    const uint64_t ticks = (uint64_t(ARG_U32(3)) << 32) | ARG_U32(4);
    std::this_thread::sleep_for(ticksToDuration(ticks));
}

HLE_FUNC(coreinit, OSGetCoreId) { RET_U32(current() ? current()->core : 1); }

HLE_FUNC(coreinit, OSInitThreadQueue) { memset(ptr(ARG_U32(3)), 0, 0x10); }

// Files de threads (OSSleepThread / OSWakeupThread) : utilisées par GX2 pour attendre la
// vsync ou un flip. Le réveil vient d'un autre thread hôte (alarme, interruption GPU) et
// peut tomber entre le test de l'appelant et sa mise en sommeil : l'attente est bornée
// (20 ms, plus d'une trame) pour ne jamais rester bloquée sur un réveil manqué.
static std::unordered_map<uint32_t, uint64_t> s_threadQueueGeneration;

HLE_FUNC(coreinit, OSSleepThread)
{
    const uint32_t queue = ARG_U32(3);
    std::unique_lock lock(g_lock);
    const uint64_t gen = s_threadQueueGeneration[queue];
    WaitMark wm("OSSleepThread", queue, uint32_t(ctx.lr));
    g_cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return s_threadQueueGeneration[queue] != gen; });
}

HLE_FUNC(coreinit, OSWakeupThread)
{
    std::lock_guard lock(g_lock);
    s_threadQueueGeneration[ARG_U32(3)]++;
    g_cv.notify_all();
}

// ================================================================= interruptions
static thread_local uint32_t t_interrupts = 1;
HLE_FUNC(coreinit, OSDisableInterrupts)
{
    RET_U32(t_interrupts);
    t_interrupts = 0;
}
HLE_FUNC(coreinit, OSRestoreInterrupts)
{
    RET_U32(t_interrupts);
    t_interrupts = ARG_U32(3);
}

// ================================================================= mutex
// OSMutex : tag 'mUtX' +0, owner +0x1C, count +0x20 (taille 0x2C)
static void mutexLock(std::unique_lock<std::mutex>& lock, uint32_t m, uint32_t lr)
{
    const uint32_t self = current()->guest;
    WaitMark wm("OSLockMutex", m, lr);
    g_cv.wait(lock, [&] {
        uint32_t owner = load32(m + 0x1C);
        return owner == 0 || owner == self;
    });
    store32(m + 0x1C, self);
    store32(m + 0x20, load32(m + 0x20) + 1);
}

static void mutexUnlock(uint32_t m)
{
    uint32_t count = load32(m + 0x20);
    if (count == 0)
        return;
    store32(m + 0x20, --count);
    if (count == 0)
    {
        store32(m + 0x1C, 0);
        g_cv.notify_all();
    }
}

static void initMutex(uint32_t m, uint32_t name)
{
    memset(ptr(m), 0, 0x2C);
    store32(m, 0x6D557458); // 'mUtX'
    store32(m + 4, name);
}

HLE_FUNC(coreinit, OSInitMutex) { initMutex(ARG_U32(3), 0); }

HLE_FUNC(coreinit, OSLockMutex)
{
    std::unique_lock lock(g_lock);
    mutexLock(lock, ARG_U32(3), uint32_t(ctx.lr));
}

HLE_FUNC(coreinit, OSTryLockMutex)
{
    std::lock_guard lock(g_lock);
    const uint32_t m = ARG_U32(3), self = current()->guest;
    const uint32_t owner = load32(m + 0x1C);
    if (owner != 0 && owner != self)
    {
        RET_U32(0);
        return;
    }
    store32(m + 0x1C, self);
    store32(m + 0x20, load32(m + 0x20) + 1);
    RET_U32(1);
}

HLE_FUNC(coreinit, OSUnlockMutex)
{
    std::lock_guard lock(g_lock);
    mutexUnlock(ARG_U32(3));
}

// ================================================================= conditions
static std::unordered_map<uint32_t, uint64_t> s_condGeneration;

HLE_FUNC(coreinit, OSInitCond)
{
    memset(ptr(ARG_U32(3)), 0, 0x1C);
    store32(ARG_U32(3), 0x634F6E44); // 'cOnD'
}

HLE_FUNC(coreinit, OSWaitCond)
{
    const uint32_t cond = ARG_U32(3), m = ARG_U32(4);
    std::unique_lock lock(g_lock);
    const uint32_t count = load32(m + 0x20);
    store32(m + 0x20, 0);
    store32(m + 0x1C, 0);
    g_cv.notify_all();
    const uint64_t gen = s_condGeneration[cond];
    {
        WaitMark wm("OSWaitCond", cond, uint32_t(ctx.lr));
        g_cv.wait(lock, [&] { return s_condGeneration[cond] != gen; });
    }
    mutexLock(lock, m, uint32_t(ctx.lr));
    store32(m + 0x20, count);
}

HLE_FUNC(coreinit, OSSignalCond)
{
    std::lock_guard lock(g_lock);
    s_condGeneration[ARG_U32(3)]++;
    g_cv.notify_all();
}

// ================================================================= événements
// OSEvent : tag 'eVnT' +0, name +4, value +0xC, mode +0x20 (0 = manuel, 1 = auto-reset)
HLE_FUNC(coreinit, OSInitEvent)
{
    const uint32_t e = ARG_U32(3);
    memset(ptr(e), 0, 0x24);
    store32(e, 0x65566E54); // 'eVnT'
    store32(e + 0xC, ARG_U32(4) ? 1 : 0);
    store32(e + 0x20, ARG_U32(5));
}

HLE_FUNC(coreinit, OSSignalEvent)
{
    std::lock_guard lock(g_lock);
    store32(ARG_U32(3) + 0xC, 1);
    g_cv.notify_all();
}

HLE_FUNC(coreinit, OSResetEvent)
{
    std::lock_guard lock(g_lock);
    store32(ARG_U32(3) + 0xC, 0);
}

HLE_FUNC(coreinit, OSWaitEvent)
{
    const uint32_t e = ARG_U32(3);
    std::unique_lock lock(g_lock);
    WaitMark wm("OSWaitEvent", e, uint32_t(ctx.lr));
    g_cv.wait(lock, [&] { return load32(e + 0xC) != 0; });
    if (load32(e + 0x20) == 1)
        store32(e + 0xC, 0);
}

HLE_FUNC(coreinit, OSWaitEventWithTimeout)
{
    const uint32_t e = ARG_U32(3);
    const uint64_t ns = (uint64_t(ARG_U32(5)) << 32) | ARG_U32(6);
    std::unique_lock lock(g_lock);
    WaitMark wm("OSWaitEventWithTimeout", e, uint32_t(ctx.lr));
    const bool ok = g_cv.wait_for(lock, std::chrono::nanoseconds(ns), [&] { return load32(e + 0xC) != 0; });
    if (ok && load32(e + 0x20) == 1)
        store32(e + 0xC, 0);
    RET_U32(ok ? 1 : 0);
}

// ================================================================= files de messages
// OSMessageQueue : tag 'mSgQ' +0, messages +0x2C, size +0x30, first +0x34, used +0x38
HLE_FUNC(coreinit, OSInitMessageQueue)
{
    const uint32_t q = ARG_U32(3);
    memset(ptr(q), 0, 0x3C);
    store32(q, 0x6D536751);
    store32(q + 0x2C, ARG_U32(4));
    store32(q + 0x30, ARG_U32(5));
}

HLE_FUNC(coreinit, OSSendMessage)
{
    const uint32_t q = ARG_U32(3), msg = ARG_U32(4), flags = ARG_U32(5);
    std::unique_lock lock(g_lock);
    const uint32_t size = load32(q + 0x30);
    if (load32(q + 0x38) >= size)
    {
        if (!(flags & 1))
        {
            RET_U32(0);
            return;
        }
        WaitMark wm("OSSendMessage", q, uint32_t(ctx.lr));
        g_cv.wait(lock, [&] { return load32(q + 0x38) < size; });
    }
    const uint32_t used = load32(q + 0x38);
    const uint32_t slot = (load32(q + 0x34) + used) % size;
    memcpy(ptr(load32(q + 0x2C) + slot * 16), ptr(msg), 16);
    store32(q + 0x38, used + 1);
    g_cv.notify_all();
    RET_U32(1);
}

HLE_FUNC(coreinit, OSReceiveMessage)
{
    const uint32_t q = ARG_U32(3), msg = ARG_U32(4), flags = ARG_U32(5);
    std::unique_lock lock(g_lock);
    if (load32(q + 0x38) == 0)
    {
        if (!(flags & 1))
        {
            RET_U32(0);
            return;
        }
        WaitMark wm("OSReceiveMessage", q, uint32_t(ctx.lr));
        g_cv.wait(lock, [&] { return load32(q + 0x38) != 0; });
    }
    const uint32_t first = load32(q + 0x34);
    memcpy(ptr(msg), ptr(load32(q + 0x2C) + first * 16), 16);
    store32(q + 0x34, (first + 1) % load32(q + 0x30));
    store32(q + 0x38, load32(q + 0x38) - 1);
    g_cv.notify_all();
    RET_U32(1);
}

// ================================================================= rendez-vous
HLE_FUNC(coreinit, OSInitRendezvous) { memset(ptr(ARG_U32(3)), 0, 0x10); }

HLE_FUNC(coreinit, OSWaitRendezvous)
{
    const uint32_t rv = ARG_U32(3), mask = ARG_U32(4);
    std::unique_lock lock(g_lock);
    store32(rv + current()->core * 4, 1);
    g_cv.notify_all();
    WaitMark wm("OSWaitRendezvous", rv, uint32_t(ctx.lr));
    g_cv.wait(lock, [&] {
        for (int c = 0; c < 3; c++)
            if ((mask & (1u << c)) && !load32(rv + c * 4))
                return false;
        return true;
    });
    RET_U32(1);
}

// ================================================================= temps
HLE_FUNC(coreinit, OSGetTime) { RET_U64(timeTicks()); }
HLE_FUNC(coreinit, OSGetSystemTime) { RET_U64(systemTicks()); }
HLE_FUNC(coreinit, OSGetTick) { RET_U32(uint32_t(systemTicks())); }

HLE_FUNC(coreinit, OSGetSystemInfo)
{
    static uint32_t info = 0;
    if (!info)
    {
        info = sysAlloc(0x20);
        store32(info + 0x0, uint32_t(kBusSpeed));
        store32(info + 0x4, uint32_t(kCoreSpeed));
        store64(info + 0x8, timeTicks() - systemTicks());
    }
    RET_U32(info);
}

// void OSTicksToCalendarTime(OSTime time, OSCalendarTime* out)
HLE_FUNC(coreinit, OSTicksToCalendarTime)
{
    const int64_t ticks = int64_t((uint64_t(ARG_U32(3)) << 32) | ARG_U32(4));
    const uint32_t out = ARG_U32(5);
    const int64_t totalUs = ticks / int64_t(kTimerSpeed / 1000000);
    const time_t secs = time_t(totalUs / 1000000) + 946684800;
    tm t{};
    gmtime_s(&t, &secs);
    store32(out + 0x00, t.tm_sec);
    store32(out + 0x04, t.tm_min);
    store32(out + 0x08, t.tm_hour);
    store32(out + 0x0C, t.tm_mday);
    store32(out + 0x10, t.tm_mon);
    store32(out + 0x14, t.tm_year + 1900);
    store32(out + 0x18, t.tm_wday);
    store32(out + 0x1C, t.tm_yday);
    store32(out + 0x20, uint32_t((totalUs / 1000) % 1000));
    store32(out + 0x24, uint32_t(totalUs % 1000));
}

// ================================================================= alarmes
// OSAlarm : tag 'aLrM' +0, name +4, callback +0xC, nextFire +0x18, period +0x28,
// start +0x30, userData +0x38, state +0x3C (taille 0x58)
namespace
{
struct AlarmState
{
    uint64_t fire;
    uint64_t period;
    uint32_t handler;
};
std::map<uint32_t, AlarmState> s_alarms;
bool s_alarmThreadStarted = false;

void alarmThreadMain()
{
    PPCContext& ctx = *currentContext();
    const uint32_t osContext = sysAlloc(0x320, 8);
    std::unique_lock lock(g_lock);
    for (;;)
    {
        if (s_alarms.empty())
        {
            g_cv.wait(lock);
            continue;
        }
        uint32_t alarm = 0;
        uint64_t soonest = UINT64_MAX;
        for (auto& [a, st] : s_alarms)
            if (st.fire < soonest)
            {
                soonest = st.fire;
                alarm = a;
            }
        const uint64_t now = timeTicks();
        if (soonest > now)
        {
            g_cv.wait_for(lock, ticksToDuration(soonest - now));
            continue;
        }
        AlarmState st = s_alarms[alarm];
        if (st.period)
            s_alarms[alarm].fire += st.period;
        else
            s_alarms.erase(alarm);
        lock.unlock();
        callGuest(ctx, st.handler, { alarm, osContext });
        lock.lock();
    }
}

void setAlarm(uint32_t alarm, uint64_t fire, uint64_t period, uint32_t handler)
{
    std::lock_guard lock(g_lock);
    s_alarms[alarm] = { fire, period, handler };
    store32(alarm + 0xC, handler);
    store64(alarm + 0x18, fire);
    store64(alarm + 0x28, period);
    if (!s_alarmThreadStarted)
    {
        s_alarmThreadStarted = true;
        startServiceThread("Alarm Thread", 1, alarmThreadMain);
    }
    g_cv.notify_all();
}
} // namespace

static void initAlarm(uint32_t alarm, uint32_t name)
{
    memset(ptr(alarm), 0, 0x58);
    store32(alarm, 0x614C724D); // 'aLrM'
    store32(alarm + 4, name);
}

HLE_FUNC(coreinit, OSCreateAlarm) { initAlarm(ARG_U32(3), 0); }
HLE_FUNC(coreinit, OSCreateAlarmEx) { initAlarm(ARG_U32(3), ARG_U32(4)); }

// BOOL OSSetAlarm(OSAlarm*, OSTime delay, handler)
HLE_FUNC(coreinit, OSSetAlarm)
{
    const uint64_t delay = (uint64_t(ARG_U32(5)) << 32) | ARG_U32(6);
    setAlarm(ARG_U32(3), timeTicks() + delay, 0, ARG_U32(7));
    RET_U32(1);
}

// BOOL OSSetPeriodicAlarm(OSAlarm*, OSTime start, OSTime interval, handler)
HLE_FUNC(coreinit, OSSetPeriodicAlarm)
{
    const uint64_t start = (uint64_t(ARG_U32(5)) << 32) | ARG_U32(6);
    const uint64_t interval = (uint64_t(ARG_U32(7)) << 32) | ARG_U32(8);
    const uint64_t now = timeTicks();
    uint64_t fire = start;
    if (interval && fire < now)
        fire += ((now - fire) / interval + 1) * interval;
    setAlarm(ARG_U32(3), fire, interval, ARG_U32(9));
    RET_U32(1);
}

HLE_FUNC(coreinit, OSCancelAlarm)
{
    std::lock_guard lock(g_lock);
    RET_U32(s_alarms.erase(ARG_U32(3)) ? 1 : 0);
}

HLE_FUNC(coreinit, OSCancelAlarms)
{
    // Annule les alarmes d'un groupe (champ group +0x10)
    std::lock_guard lock(g_lock);
    const uint32_t group = ARG_U32(3);
    for (auto it = s_alarms.begin(); it != s_alarms.end();)
        it = load32(it->first + 0x10) == group ? s_alarms.erase(it) : std::next(it);
}

HLE_FUNC(coreinit, OSSetAlarmUserData) { store32(ARG_U32(3) + 0x38, ARG_U32(4)); }
HLE_FUNC(coreinit, OSGetAlarmUserData) { RET_U32(load32(ARG_U32(3) + 0x38)); }

// ================================================================= libc GHS
namespace
{
std::recursive_mutex s_ghsLock;
std::recursive_mutex s_flocks[21];
uint32_t s_flockSlots = 0; // 21 index de verrou (uint32) en mémoire invitée
uint32_t s_iob = 0;
} // namespace

HLE_FUNC(coreinit, __ghsLock) { s_ghsLock.lock(); }
HLE_FUNC(coreinit, __ghsUnlock) { s_ghsLock.unlock(); }

HLE_FUNC(coreinit, __ghs_flock_ptr)
{
    if (!s_flockSlots)
    {
        s_flockSlots = sysAlloc(21 * 4);
        for (uint32_t i = 0; i < 21; i++)
            store32(s_flockSlots + i * 4, i);
    }
    uint32_t index = s_iob ? (ARG_U32(3) - s_iob) / 0x10 : 20;
    if (index > 20)
        index = 20;
    RET_U32(s_flockSlots + index * 4);
}

HLE_FUNC(coreinit, __ghs_flock_file) { s_flocks[ARG_U32(3) % 21].lock(); }
HLE_FUNC(coreinit, __ghs_funlock_file) { s_flocks[ARG_U32(3) % 21].unlock(); }

HLE_FUNC(coreinit, __ghs_mtx_init)
{
    const uint32_t m = sysAlloc(0x2C, 8);
    initMutex(m, 0);
    store32(ARG_U32(3), m);
}

HLE_FUNC(coreinit, __ghs_mtx_dst) {}

HLE_FUNC(coreinit, __ghs_mtx_lock)
{
    std::unique_lock lock(g_lock);
    mutexLock(lock, load32(ARG_U32(3)), uint32_t(ctx.lr));
}

HLE_FUNC(coreinit, __ghs_mtx_unlock)
{
    std::lock_guard lock(g_lock);
    mutexUnlock(load32(ARG_U32(3)));
}

HLE_DATA(coreinit, _iob)
{
    s_iob = addr;
    // stdin / stdout / stderr : canaux 0/1/2, lecture / écriture
    store32(addr + 0x0C, (0u << 18) | 4);
    store32(addr + 0x1C, (1u << 18) | 2);
    store32(addr + 0x2C, (2u << 18) | 2);
}

HLE_DATA(coreinit, __gh_FOPEN_MAX) { store16(addr, 20); }
HLE_DATA(coreinit, __atexit_cleanup) {}
HLE_DATA(coreinit, __cpp_exception_init_ptr) {}
HLE_DATA(coreinit, __cpp_exception_cleanup_ptr) {}
