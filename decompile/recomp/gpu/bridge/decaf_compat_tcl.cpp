// Compatibilité decaf : TCL, la couche entre GX2 et le GPU (soumission des tampons de
// commandes PM4, timestamps de fin, interruptions GPU), sur le modèle de
// decaf-emu/src/libdecaf/src/cafe/libraries/tcl.
//
// Les interruptions de libgpu (fin de commande, flip…) sont distribuées par un thread
// système dédié, qui appelle les callbacks enregistrés (fonctions internes de GX2).
#include "kernel.h"

#include <cafe/libraries/tcl/tcl_aperture.h>
#include <cafe/libraries/tcl/tcl_driver.h>
#include <cafe/libraries/tcl/tcl_interrupthandler.h>
#include <cafe/libraries/tcl/tcl_ring.h>
#include <libcpu/state.h>
#include <libgpu/gpu_ih.h>
#include <libgpu/gpu_ringbuffer.h>
#include <libgpu/latte/latte_pm4.h>
#include <libgpu/latte/latte_pm4_commands.h>
#include <libgpu/latte/latte_pm4_sizer.h>
#include <libgpu/latte/latte_pm4_writer.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace
{

constexpr uint32_t kMaxInterruptTypes = 256;
constexpr uint32_t kMaxHandlersPerInterrupt = 4;

struct Handler
{
   uint32_t callback = 0;
   uint32_t userData = 0;
};

std::mutex s_tclMutex;
std::array<std::array<Handler, kMaxHandlersPerInterrupt>, kMaxInterruptTypes> s_handlers;
std::atomic<uint64_t> s_submitTimestamp { 1 };
uint32_t s_retireTimestampAddr = 0; // écrit par le GPU (EVENT_WRITE_EOP), big-endian

std::mutex s_irqMutex;
std::condition_variable s_irqCv;
bool s_irqPending = false;
std::condition_variable s_retireCv;

uint64_t retiredTimestamp()
{
   return rt::load64(s_retireTimestampAddr);
}

template<typename Type>
void writePM4(uint32_t *buffer, uint32_t &pos, const Type &command)
{
   auto &cmd = const_cast<Type &>(command);
   latte::pm4::PacketSizer sizer;
   cmd.serialise(sizer);
   auto writer = latte::pm4::PacketWriter { buffer, pos, Type::Opcode, sizer.getSize() + 1 };
   cmd.serialise(writer);
}

// Paquet EVENT_WRITE_EOP : à la fin des commandes déjà soumises, le GPU écrit le timestamp
// à s_retireTimestampAddr et lève une interruption CP_EOP_EVENT.
uint64_t insertRetiredTimestamp(bool cacheFlush, bool cacheFlushInvalidate, bool writeConfirm)
{
   using namespace latte;
   const uint64_t ts = s_submitTimestamp.fetch_add(1);
   auto eventType = VGT_EVENT_TYPE::BOTTOM_OF_PIPE_TS;
   if (cacheFlush)
      eventType = cacheFlushInvalidate ? VGT_EVENT_TYPE::CACHE_FLUSH_AND_INV_TS_EVENT : VGT_EVENT_TYPE::CACHE_FLUSH_TS;

   std::array<uint32_t, 6> buffer;
   uint32_t pos = 0;
   writePM4(buffer.data(), pos,
      pm4::EventWriteEOP {
         VGT_EVENT_INITIATOR::get(0).EVENT_TYPE(eventType).EVENT_INDEX(VGT_EVENT_INDEX::TS),
         pm4::EW_ADDR_LO::get(0).ADDR_LO(s_retireTimestampAddr >> 2).ENDIAN_SWAP(CB_ENDIAN::SWAP_8IN64),
         pm4::EWP_ADDR_HI::get(0)
            .DATA_SEL(pm4::EWP_DATA_64)
            .INT_SEL(writeConfirm ? pm4::EWP_INT_WRITE_CONFIRM : pm4::EWP_INT_NONE),
         static_cast<uint32_t>(ts & 0xFFFFFFFF),
         static_cast<uint32_t>(ts >> 32)
      });
   gpu::ringbuffer::write({ buffer.data(), pos });
   return ts;
}

// Thread système « GPU7 Interrupts » : vide la file d'interruptions de libgpu et appelle
// les callbacks enregistrés (comme le gestionnaire d'interruption GPU7 de tcl.rpl).
void interruptThread()
{
   PPCContext &ctx = *rt::currentContext();
   const uint32_t entryAddr = rt::sysAlloc(16);
   for (;;)
   {
      {
         std::unique_lock lock(s_irqMutex);
         s_irqCv.wait_for(lock, std::chrono::milliseconds(5), [] { return s_irqPending; });
         s_irqPending = false;
      }
      auto entries = gpu::ih::read();
      for (auto &entry : entries)
      {
         rt::store32(entryAddr + 0x0, entry.word0);
         rt::store32(entryAddr + 0x4, entry.word1);
         rt::store32(entryAddr + 0x8, entry.word2);
         rt::store32(entryAddr + 0xC, entry.word3);
         std::array<Handler, kMaxHandlersPerInterrupt> handlers;
         {
            std::lock_guard lock(s_tclMutex);
            handlers = s_handlers[entry.word0 % kMaxInterruptTypes];
         }
         for (auto &h : handlers)
            if (h.callback)
               rt::callGuest(ctx, h.callback, { entryAddr, h.userData });
      }
      s_retireCv.notify_all();
   }
}

} // namespace

namespace rt
{
void tclInit()
{
   s_retireTimestampAddr = sysAlloc(8, 8);
   gpu::ih::setInterruptCallback([] {
      {
         std::lock_guard lock(s_irqMutex);
         s_irqPending = true;
      }
      s_irqCv.notify_one();
   });
   kernel::startServiceThread("GPU7 Interrupts", 1, interruptThread);
}
} // namespace rt

namespace cafe::tcl
{

TCLStatus TCLReadTimestamp(TCLTimestampID id, virt_ptr<TCLTimestamp> outValue)
{
   switch (id)
   {
   case TCLTimestampID::CPSubmitted:
      *outValue = s_submitTimestamp.load() - 1;
      return TCLStatus::OK;
   case TCLTimestampID::CPRetired:
      *outValue = retiredTimestamp();
      return TCLStatus::OK;
   case TCLTimestampID::DMAERetired:
      *outValue = s_submitTimestamp.load() - 1;
      return TCLStatus::OK;
   default:
      return TCLStatus::InvalidArg;
   }
}

TCLStatus TCLWaitTimestamp(TCLTimestampID id, TCLTimestamp timestamp, coreinit::OSTime timeout)
{
   if (id != TCLTimestampID::CPRetired)
      return TCLStatus::InvalidArg;
   const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::nanoseconds(static_cast<int64_t>(timeout) * 1000000000ll / cpu::timerClockSpeed);
   std::unique_lock lock(s_irqMutex);
   while (retiredTimestamp() < timestamp)
   {
      if (std::chrono::steady_clock::now() >= deadline)
         return TCLStatus::Timeout;
      s_retireCv.wait_for(lock, std::chrono::milliseconds(1));
   }
   return TCLStatus::OK;
}

TCLStatus TCLSubmitToRing(virt_ptr<uint32_t> buffer, uint32_t numWords, virt_ptr<TCLSubmitFlags> submitFlags,
                          virt_ptr<TCLTimestamp> outLastSubmittedTimestamp)
{
   const auto flags = submitFlags ? static_cast<TCLSubmitFlags>(*submitFlags) : TCLSubmitFlags::None;
   gpu::ringbuffer::write({ buffer.getRawPointer(), numWords });

   TCLTimestamp ts;
   if (flags & TCLSubmitFlags::UpdateTimestamp)
      ts = insertRetiredTimestamp(!(flags & TCLSubmitFlags::NoCacheFlush),
                                  !!(flags & TCLSubmitFlags::CacheFlushInvalidate),
                                  !(flags & TCLSubmitFlags::NoWriteConfirmTimestamp));
   else
      ts = s_submitTimestamp.load();
   if (outLastSubmittedTimestamp)
      *outLastSubmittedTimestamp = ts;
   return TCLStatus::OK;
}

TCLStatus TCLIHEnableInterrupt(TCLInterruptType type, BOOL enable)
{
   auto cntl = latte::CP_INT_CNTL::get(0);
   switch (type)
   {
   case TCLInterruptType::UNKNOWN_192: cntl = cntl.UNK17_INT_ENABLE(true); break;
   case TCLInterruptType::CP_RB: cntl = cntl.RB_INT_ENABLE(true); break;
   case TCLInterruptType::CP_IB1: cntl = cntl.IB1_INT_ENABLE(true); break;
   case TCLInterruptType::CP_IB2: cntl = cntl.IB2_INT_ENABLE(true); break;
   case TCLInterruptType::CP_RESERVED_BITS: cntl = cntl.RESERVED_BITS_EXCEPTION(true); break;
   case TCLInterruptType::CP_EOP_EVENT: cntl = cntl.TIME_STAMP_INT_ENABLE(true); break;
   case TCLInterruptType::SCRATCH: cntl = cntl.SCRATCH_INT_ENABLE(true); break;
   case TCLInterruptType::CP_BAD_OPCODE: cntl = cntl.BAD_OPCODE_EXCEPTION(true); break;
   case TCLInterruptType::CP_CTX_EMPTY: cntl = cntl.CNTX_EMPTY_INT_ENABLE(true); break;
   case TCLInterruptType::CP_CTX_BUSY: cntl = cntl.CNTX_BUSY_INT_ENABLE(true); break;
   default: return TCLStatus::InvalidArg;
   }
   if (enable)
      gpu::ih::enable(cntl);
   else
      gpu::ih::disable(cntl);
   return TCLStatus::OK;
}

TCLStatus TCLIHRegister(TCLInterruptType type, TCLInterruptHandlerFn callback, virt_ptr<void> userData)
{
   if (type >= kMaxInterruptTypes)
      return TCLStatus::InvalidArg;
   std::lock_guard lock(s_tclMutex);
   for (auto &h : s_handlers[type])
      if (!h.callback)
      {
         h.callback = callback.getAddress().getAddress();
         h.userData = virt_cast<virt_addr>(userData).getAddress();
         return TCLStatus::OK;
      }
   return TCLStatus::OutOfMemory;
}

TCLStatus TCLIHUnregister(TCLInterruptType type, TCLInterruptHandlerFn callback, virt_ptr<void> userData)
{
   if (type >= kMaxInterruptTypes)
      return TCLStatus::InvalidArg;
   std::lock_guard lock(s_tclMutex);
   for (auto &h : s_handlers[type])
      if (h.callback == callback.getAddress().getAddress() && h.userData == virt_cast<virt_addr>(userData).getAddress())
      {
         h = {};
         return TCLStatus::OK;
      }
   return TCLStatus::InvalidArg;
}

// Apertures de tuilage (accès CPU détuilé à une surface) : non gérées.
TCLStatus TCLAllocTilingAperture(phys_addr, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                 virt_ptr<TCLApertureHandle>, virt_ptr<virt_addr>)
{
   RT_WARN("TCLAllocTilingAperture non géré");
   return TCLStatus::OutOfMemory;
}

TCLStatus TCLFreeTilingAperture(TCLApertureHandle)
{
   return TCLStatus::OK;
}

void TCLSetHangWait(BOOL)
{
}

} // namespace cafe::tcl
