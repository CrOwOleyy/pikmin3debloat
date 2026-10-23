// Cale remplaçant libcpu de decaf pour libgpu : notre runtime fournit déjà le CPU (code
// recompilé) et la mémoire invitée. libgpu n'en attend que deux choses :
//  * les bases de traduction d'adresses : virtuelle et « physique » pointent toutes deux sur
//    la mémoire invitée (le runtime donne au GPU des adresses physiques = virtuelles) ;
//  * getMemoryState, pour savoir si le CPU a modifié une texture ou un buffer. La mémoire
//    invitée est réservée avec MEM_WRITE_WATCH (runtime/memory.cpp) : on demande au système les
//    pages de 4 Kio de la plage écrites depuis la dernière demande, chacune reçoit un nouveau
//    numéro de génération, et l'état de la plage est l'empreinte des générations de ses pages
//    (au lieu d'une empreinte de toutes ses données).
#include <common/datahash.h>
#include <libcpu/memtrack.h>
#include <libcpu/mmu.h>

#include <windows.h>

#include <mutex>
#include <vector>

namespace cpu
{

namespace internal
{
uintptr_t BaseVirtualAddress = 0;
uintptr_t BasePhysicalAddress = 0;
} // namespace internal

namespace
{
constexpr uint32_t PageShift = 12;
std::mutex sLock;
std::vector<uint64_t> sGenerations(1ull << (32 - PageShift), 0);
uint64_t sNextGeneration = 1;
std::vector<void *> sDirty;
} // namespace

void
refreshMemoryState()
{
   // Rien à faire : getMemoryState interroge le système pour la plage demandée.
}

MemtrackState
getMemoryState(PhysicalAddress physicalAddress, uint32_t size)
{
   if (!size) {
      return MemtrackState { 0 };
   }

   const uint64_t address = physicalAddress.getAddress();
   const uint64_t firstPage = address >> PageShift;
   const uint64_t lastPage = (address + size - 1) >> PageShift;
   const uint64_t pageCount = lastPage - firstPage + 1;
   auto base = reinterpret_cast<uint8_t *>(internal::BasePhysicalAddress);

   // Petites plages (sommets, uniformes…) : l'empreinte des données coûte moins qu'un appel
   // système.
   if (size < 16 * 1024) {
      return MemtrackState { DataHash {}.write(base + address, size).value() };
   }

   std::lock_guard guard(sLock);
   if (sDirty.size() < pageCount) {
      sDirty.resize(pageCount);
   }
   ULONG_PTR count = pageCount;
   DWORD granularity = 0;
   if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base + (firstPage << PageShift), pageCount << PageShift,
                     sDirty.data(), &count, &granularity) != 0) {
      // Plage non suivie : empreinte des données.
      return MemtrackState { DataHash {}.write(base + address, size).value() };
   }
   for (ULONG_PTR i = 0; i < count; ++i) {
      auto page = (reinterpret_cast<uint8_t *>(sDirty[i]) - base) >> PageShift;
      sGenerations[page] = sNextGeneration++;
   }
   return MemtrackState { DataHash {}.write(&sGenerations[firstPage], pageCount * sizeof(uint64_t)).value() };
}

} // namespace cpu
