// Noyau HLE : threads Cafe OS (un thread hôte chacun) et primitives de synchronisation.
#pragma once

#include "runtime.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace rt::kernel
{

// Horloges Wii U : bus 248,625 MHz, timer = bus / 4.
constexpr uint64_t kBusSpeed = 248625000;
constexpr uint64_t kCoreSpeed = 1243125000;
constexpr uint64_t kTimerSpeed = kBusSpeed / 4;

// Champs de OSThread utilisés (offsets decaf-emu, coreinit_thread.h).
constexpr uint32_t kThreadSize = 0x6A0;
constexpr uint32_t kThreadTag = 0x320;      // 'tHrD'
constexpr uint32_t kThreadState = 0x324;
constexpr uint32_t kThreadAttr = 0x325;
constexpr uint32_t kThreadId = 0x326;
constexpr uint32_t kThreadSuspend = 0x328;
constexpr uint32_t kThreadPriority = 0x32C;
constexpr uint32_t kThreadBasePriority = 0x330;
constexpr uint32_t kThreadExitValue = 0x334;
constexpr uint32_t kThreadStackStart = 0x394;
constexpr uint32_t kThreadStackEnd = 0x398;
constexpr uint32_t kThreadEntry = 0x39C;
constexpr uint32_t kThreadSpecific = 0x57C;
constexpr uint32_t kThreadName = 0x5C0;

struct Thread
{
    uint32_t guest = 0;          // adresse de l'OSThread
    PPCContext* ctx = nullptr;
    std::thread host;
    int core = 1;
    int suspend = 1;
    bool running = false;
    bool exited = false;
    uint32_t exitValue = 0;
    // Diagnostic (--dump) : primitive sur laquelle le thread est bloqué, sous g_lock.
    const char* waitWhat = nullptr;
    uint32_t waitObj = 0;
    uint32_t waitLr = 0;
};

// Verrou global du noyau : toutes les primitives HLE l'utilisent ; on réveille tous
// les threads en attente à chaque changement d'état (simple et correct).
extern std::mutex g_lock;
extern std::condition_variable g_cv;

Thread* current();
uint32_t sdaBase();
uint32_t sda2Base();
void setSdaBases(uint32_t sda, uint32_t sda2);

// Crée le thread principal (OSThread + pile) pour le thread hôte appelant.
Thread* createMainThread(uint32_t stackSize);
uint64_t systemTicks();
// Journalise l'état de chaque thread (bloqué sur quoi, depuis où) : option --dump.
void dumpThreads();
uint64_t timeTicks();

// Lance un thread de service du système (alarmes, callbacks AX…) : thread hôte doté
// d'un OSThread invité nommé et d'un contexte CPU, pour appeler du code du jeu.
void startServiceThread(const char* name, int core, std::function<void()> body);

// Termine le thread invité courant (OSExitThread) en déroulant la pile hôte.
struct ThreadExit
{
    uint32_t value;
};

} // namespace rt::kernel
