// Cale remplaçant libcpu de decaf pour libgpu : notre runtime fournit déjà le CPU (code
// recompilé) et la mémoire invitée. libgpu n'en attend que deux choses :
//  * les bases de traduction d'adresses : virtuelle et « physique » pointent toutes deux sur
//    la mémoire invitée (le runtime donne au GPU des adresses physiques = virtuelles) ;
//  * getMemoryState, pour savoir si le CPU a modifié une texture ou un buffer : comme decaf
//    quand le suivi des écritures est désactivé, on hache la plage.
#include <common/datahash.h>
#include <libcpu/memtrack.h>
#include <libcpu/mmu.h>

namespace cpu
{

namespace internal
{
uintptr_t BaseVirtualAddress = 0;
uintptr_t BasePhysicalAddress = 0;
} // namespace internal

MemtrackState
getMemoryState(PhysicalAddress physicalAddress, uint32_t size)
{
   auto ptr = reinterpret_cast<void *>(internal::BasePhysicalAddress + physicalAddress.getAddress());
   return MemtrackState { DataHash {}.write(ptr, size).value() };
}

} // namespace cpu
