// Compatibilité decaf : les fonctions coreinit appelées par le code GX2 de decaf, redirigées
// vers les implémentations HLE de notre runtime (hleInvoke) avec un contexte CPU temporaire.
#include "runtime.h"

#include <cafe/libraries/coreinit/coreinit_alarm.h>
#include <cafe/libraries/coreinit/coreinit_cache.h>
#include <cafe/libraries/coreinit/coreinit_core.h>
#include <cafe/libraries/coreinit/coreinit_memdefaultheap.h>
#include <cafe/libraries/coreinit/coreinit_memory.h>
#include <cafe/libraries/coreinit/coreinit_messagequeue.h>
#include <cafe/libraries/coreinit/coreinit_systeminfo.h>
#include <cafe/libraries/coreinit/coreinit_thread.h>
#include <cafe/libraries/coreinit/coreinit_time.h>

namespace
{

// Contexte de travail : copie du contexte du thread invité courant (pile, SDA…).
struct HleCall
{
   PPCContext ctx;
   int gpr = 3;

   HleCall()
   {
      if (const PPCContext* current = rt::currentContext())
         ctx = *current;
      else
         ctx = PPCContext {};
   }

   HleCall& arg(uint32_t value)
   {
      PPCRegister* r[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
      r[gpr++ - 3]->u64 = value;
      return *this;
   }

   // Les arguments 64 bits occupent une paire de registres alignée (r3:r4, r5:r6…).
   HleCall& arg64(uint64_t value)
   {
      if (gpr % 2 == 0)
         gpr++;
      arg(static_cast<uint32_t>(value >> 32));
      return arg(static_cast<uint32_t>(value));
   }

   template<typename T>
   HleCall& arg(virt_ptr<T> p)
   {
      return arg(virt_cast<virt_addr>(p).getAddress());
   }

   PPCContext& call(const char* name)
   {
      rt::hleInvoke("coreinit", name, ctx);
      return ctx;
   }
};

uint64_t ret64(const PPCContext& ctx)
{
   return (static_cast<uint64_t>(ctx.r3.u32) << 32) | ctx.r4.u32;
}

} // namespace

namespace cafe::coreinit
{

// ---------------------------------------------------------------- temps
OSTime OSGetTime()
{
   return static_cast<OSTime>(ret64(HleCall {}.call("OSGetTime")));
}

OSTime OSGetSystemTime()
{
   return static_cast<OSTime>(ret64(HleCall {}.call("OSGetSystemTime")));
}

namespace internal
{
OSTime msToTicks(OSTimeMilliseconds ms)
{
   return static_cast<OSTime>(ms * static_cast<int64_t>(cpu::timerClockSpeed / 1000));
}
} // namespace internal

virt_ptr<OSSystemInfo> OSGetSystemInfo()
{
   return virt_cast<OSSystemInfo *>(virt_addr { HleCall {}.call("OSGetSystemInfo").r3.u32 });
}

uint32_t OSGetCoreId()
{
   return HleCall {}.call("OSGetCoreId").r3.u32;
}

// ---------------------------------------------------------------- mémoire
// Le runtime donne au GPU des adresses physiques égales aux adresses virtuelles.
phys_addr OSEffectiveToPhysical(virt_addr address)
{
   return phys_addr { address.getAddress() };
}

virt_ptr<void> MEMAllocFromDefaultHeapEx(uint32_t size, int32_t align)
{
   return virt_cast<void *>(virt_addr { rt::defaultHeapAlloc(size, align) });
}

void MEMFreeToDefaultHeap(virt_ptr<void> p)
{
   rt::defaultHeapFree(virt_cast<virt_addr>(p).getAddress());
}

// Mémoire cohérente côté hôte ; libgpu détecte les modifications par hachage.
void DCFlushRange(virt_addr, uint32_t)
{
}

void DCInvalidateRange(virt_addr, uint32_t)
{
}

// ---------------------------------------------------------------- threads, files, alarmes
BOOL OSCreateThreadType(virt_ptr<OSThread> thread, OSThreadEntryPointFn entry, uint32_t argc,
                        virt_ptr<void> argv, virt_ptr<uint32_t> stackTop, uint32_t stackSize,
                        int32_t priority, OSThreadAttributes attributes, OSThreadType)
{
   return HleCall {}
      .arg(thread)
      .arg(entry.getAddress().getAddress())
      .arg(argc)
      .arg(argv)
      .arg(stackTop)
      .arg(stackSize)
      .arg(static_cast<uint32_t>(priority))
      .arg(static_cast<uint32_t>(attributes))
      .call("OSCreateThread").r3.u32;
}

int32_t OSResumeThread(virt_ptr<OSThread> thread)
{
   return HleCall {}.arg(thread).call("OSResumeThread").r3.s32;
}

void OSInitThreadQueue(virt_ptr<OSThreadQueue> queue)
{
   HleCall {}.arg(queue).call("OSInitThreadQueue");
}

void OSSleepThread(virt_ptr<OSThreadQueue> queue)
{
   HleCall {}.arg(queue).call("OSSleepThread");
}

void OSWakeupThread(virt_ptr<OSThreadQueue> queue)
{
   HleCall {}.arg(queue).call("OSWakeupThread");
}

void OSInitMessageQueue(virt_ptr<OSMessageQueue> queue, virt_ptr<OSMessage> messages, uint32_t size)
{
   HleCall {}.arg(queue).arg(messages).arg(size).call("OSInitMessageQueue");
}

BOOL OSSendMessage(virt_ptr<OSMessageQueue> queue, virt_ptr<OSMessage> message, OSMessageFlags flags)
{
   return HleCall {}.arg(queue).arg(message).arg(static_cast<uint32_t>(flags)).call("OSSendMessage").r3.u32;
}

BOOL OSReceiveMessage(virt_ptr<OSMessageQueue> queue, virt_ptr<OSMessage> message, OSMessageFlags flags)
{
   return HleCall {}.arg(queue).arg(message).arg(static_cast<uint32_t>(flags)).call("OSReceiveMessage").r3.u32;
}

void OSCreateAlarm(virt_ptr<OSAlarm> alarm)
{
   HleCall {}.arg(alarm).call("OSCreateAlarm");
}

BOOL OSSetPeriodicAlarm(virt_ptr<OSAlarm> alarm, OSTime start, OSTime interval, AlarmCallbackFn callback)
{
   return HleCall {}
      .arg(alarm)
      .arg64(static_cast<uint64_t>(start))
      .arg64(static_cast<uint64_t>(interval))
      .arg(callback.getAddress().getAddress())
      .call("OSSetPeriodicAlarm").r3.u32;
}

} // namespace cafe::coreinit
