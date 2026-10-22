// coreinit : tas mémoire MEM* (expanded heap, frame heap, allocateurs, tas par défaut).
//
// Les tas Cafe OS gardent leurs métadonnées dans la mémoire qu'ils gèrent ; ici elles
// vivent côté hôte (le jeu ne lit pas ces en-têtes, seule l'API compte). Le handle d'un
// tas est l'adresse invitée de son en-tête (0x40 octets réservés en tête de zone).
#include "kernel.h"

#include <map>
#include <unordered_map>

using namespace rt;

namespace
{

constexpr uint32_t kHeapHeader = 0x40;

struct ExpHeap
{
    uint32_t start, end;
    std::map<uint32_t, uint32_t> freeBlocks; // adresse -> taille
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> used; // ptr -> (bloc, taille bloc)
};

struct FrmHeap
{
    uint32_t start, end, head, tail;
};

std::mutex s_memLock;
std::unordered_map<uint32_t, ExpHeap> s_expHeaps;
std::unordered_map<uint32_t, FrmHeap> s_frmHeaps;
uint32_t s_arenaHeap[3]; // MEM1, MEM2, FG
uint32_t s_defaultHeap = 0;

uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
uint32_t alignDown(uint32_t v, uint32_t a) { return v & ~(a - 1); }

uint32_t createExpHeap(uint32_t addr, uint32_t size)
{
    const uint32_t handle = alignUp(addr, 4);
    ExpHeap h;
    h.start = alignUp(handle + kHeapHeader, 0x40);
    h.end = alignDown(addr + size, 4);
    h.freeBlocks[h.start] = h.end - h.start;
    s_expHeaps[handle] = std::move(h);
    store32(handle, 0x45585048); // 'EXPH'
    return handle;
}

uint32_t expAlloc(uint32_t handle, uint32_t size, int32_t align)
{
    auto it = s_expHeaps.find(handle);
    if (it == s_expHeaps.end())
        return 0;
    ExpHeap& h = it->second;
    size = alignUp(size ? size : 1, 4);
    const uint32_t a = uint32_t(align < 0 ? -align : align) < 4 ? 4 : uint32_t(align < 0 ? -align : align);
    if (align >= 0)
    {
        for (auto& [bs, bsz] : h.freeBlocks)
        {
            const uint32_t p = alignUp(bs, a);
            if (p + size > bs + bsz || p < bs)
                continue;
            const uint32_t blockStart = bs, blockSize = bsz;
            h.freeBlocks.erase(blockStart);
            if (p > blockStart)
                h.freeBlocks[blockStart] = p - blockStart;
            if (p + size < blockStart + blockSize)
                h.freeBlocks[p + size] = blockStart + blockSize - (p + size);
            h.used[p] = { p, size };
            return p;
        }
    }
    else
    {
        for (auto rit = h.freeBlocks.rbegin(); rit != h.freeBlocks.rend(); ++rit)
        {
            const uint32_t bs = rit->first, bsz = rit->second;
            if (bsz < size)
                continue;
            const uint32_t p = alignDown(bs + bsz - size, a);
            if (p < bs)
                continue;
            h.freeBlocks.erase(bs);
            if (p > bs)
                h.freeBlocks[bs] = p - bs;
            if (p + size < bs + bsz)
                h.freeBlocks[p + size] = bs + bsz - (p + size);
            h.used[p] = { p, size };
            return p;
        }
    }
    return 0;
}

void expFree(uint32_t handle, uint32_t p)
{
    auto it = s_expHeaps.find(handle);
    if (it == s_expHeaps.end())
        return;
    ExpHeap& h = it->second;
    auto u = h.used.find(p);
    if (u == h.used.end())
    {
        RT_WARN("MEMFreeToExpHeap : 0x%08X n'appartient pas au tas 0x%08X", p, handle);
        return;
    }
    uint32_t start = u->second.first, size = u->second.second;
    h.used.erase(u);
    auto next = h.freeBlocks.lower_bound(start);
    if (next != h.freeBlocks.end() && next->first == start + size)
    {
        size += next->second;
        h.freeBlocks.erase(next);
    }
    auto prev = h.freeBlocks.lower_bound(start);
    if (prev != h.freeBlocks.begin())
    {
        --prev;
        if (prev->first + prev->second == start)
        {
            start = prev->first;
            size += prev->second;
            h.freeBlocks.erase(prev);
        }
    }
    h.freeBlocks[start] = size;
}

uint32_t expAllocatable(uint32_t handle, int32_t align)
{
    auto it = s_expHeaps.find(handle);
    if (it == s_expHeaps.end())
        return 0;
    const uint32_t a = uint32_t(align < 0 ? -align : align) < 4 ? 4 : uint32_t(align < 0 ? -align : align);
    uint32_t best = 0;
    for (auto& [bs, bsz] : it->second.freeBlocks)
    {
        const uint32_t p = alignUp(bs, a);
        if (p < bs + bsz && bs + bsz - p > best)
            best = bs + bsz - p;
    }
    return alignDown(best, 4);
}

uint32_t createFrmHeap(uint32_t addr, uint32_t size)
{
    const uint32_t handle = alignUp(addr, 4);
    FrmHeap h;
    h.start = h.head = alignUp(handle + kHeapHeader, 0x40);
    h.end = h.tail = alignDown(addr + size, 4);
    s_frmHeaps[handle] = h;
    store32(handle, 0x46524D48); // 'FRMH'
    return handle;
}

uint32_t frmAlloc(uint32_t handle, uint32_t size, int32_t align)
{
    auto it = s_frmHeaps.find(handle);
    if (it == s_frmHeaps.end())
        return 0;
    FrmHeap& h = it->second;
    const uint32_t a = uint32_t(align < 0 ? -align : align) < 4 ? 4 : uint32_t(align < 0 ? -align : align);
    size = alignUp(size, 4);
    if (align >= 0)
    {
        const uint32_t p = alignUp(h.head, a);
        if (p + size > h.tail)
            return 0;
        h.head = p + size;
        return p;
    }
    if (h.tail < size)
        return 0;
    const uint32_t p = alignDown(h.tail - size, a);
    if (p < h.head)
        return 0;
    h.tail = p;
    return p;
}

// Allocation dans n'importe quel tas connu (MEMAllocFromAllocator, tas par défaut…).
uint32_t defaultAlloc(uint32_t size, int32_t align)
{
    std::lock_guard lock(s_memLock);
    return expAlloc(s_defaultHeap, size, align);
}

PPC_FUNC(hostAllocFromDefaultHeap) { ctx.r3.u64 = defaultAlloc(ctx.r3.u32, 0x40); }
PPC_FUNC(hostAllocFromDefaultHeapEx) { ctx.r3.u64 = defaultAlloc(ctx.r3.u32, ctx.r4.s32); }
PPC_FUNC(hostFreeToDefaultHeap)
{
    std::lock_guard lock(s_memLock);
    expFree(s_defaultHeap, ctx.r3.u32);
}

// Fonctions de l'allocateur MEMInitAllocatorForExpHeap : alloc(allocator, size), free(allocator, ptr)
PPC_FUNC(hostAllocatorExpAlloc)
{
    std::lock_guard lock(s_memLock);
    const uint32_t allocator = ctx.r3.u32;
    ctx.r3.u64 = expAlloc(load32(allocator + 4), ctx.r4.u32, int32_t(load32(allocator + 8)));
}
PPC_FUNC(hostAllocatorExpFree)
{
    std::lock_guard lock(s_memLock);
    expFree(load32(ctx.r3.u32 + 4), ctx.r4.u32);
}

uint32_t s_expAllocatorFuncs = 0;

} // namespace

namespace rt
{
uint32_t defaultHeapAlloc(uint32_t size, int32_t align) { return defaultAlloc(size, align); }

void defaultHeapFree(uint32_t p)
{
    std::lock_guard lock(s_memLock);
    expFree(s_defaultHeap, p);
}

// Tas de base créés au démarrage (état d'une application Cafe OS fraîchement lancée).
void memHeapsInit(uint32_t mem2Start)
{
    s_arenaHeap[0] = createFrmHeap(kMem1Base, kMem1Size);
    s_defaultHeap = createExpHeap(mem2Start, 0x4C000000 - mem2Start);
    s_arenaHeap[1] = s_defaultHeap;
    s_arenaHeap[2] = createFrmHeap(kFgBucketBase, kFgBucketSize);
    RT_INFO("tas MEM2 par défaut : 0x%08X..0x4C000000", mem2Start);
}
} // namespace rt

HLE_DATA(coreinit, MEMAllocFromDefaultHeap) { store32(addr, exportHostFunction(hostAllocFromDefaultHeap)); }
HLE_DATA(coreinit, MEMAllocFromDefaultHeapEx) { store32(addr, exportHostFunction(hostAllocFromDefaultHeapEx)); }
HLE_DATA(coreinit, MEMFreeToDefaultHeap) { store32(addr, exportHostFunction(hostFreeToDefaultHeap)); }

// MEMHeapHandle MEMGetBaseHeapHandle(MEMBaseHeapType arena)
// MEMBaseHeapType : MEM1 = 0, MEM2 = 1, FG (foreground bucket) = 8
HLE_FUNC(coreinit, MEMGetBaseHeapHandle)
{
    switch (ARG_U32(3))
    {
    case 0: RET_U32(s_arenaHeap[0]); break;
    case 1: RET_U32(s_arenaHeap[1]); break;
    case 8: RET_U32(s_arenaHeap[2]); break;
    default: RET_U32(0); break;
    }
}

// MEMHeapHandle MEMCreateExpHeapEx(void* addr, uint32_t size, uint16_t flags)
HLE_FUNC(coreinit, MEMCreateExpHeapEx)
{
    std::lock_guard lock(s_memLock);
    RET_U32(createExpHeap(ARG_U32(3), ARG_U32(4)));
}

HLE_FUNC(coreinit, MEMDestroyExpHeap)
{
    std::lock_guard lock(s_memLock);
    const uint32_t h = ARG_U32(3);
    s_expHeaps.erase(h);
    RET_U32(h);
}

// void* MEMAllocFromExpHeapEx(MEMHeapHandle, uint32_t size, int32_t align)
HLE_FUNC(coreinit, MEMAllocFromExpHeapEx)
{
    std::lock_guard lock(s_memLock);
    RET_U32(expAlloc(ARG_U32(3), ARG_U32(4), ARG_S32(5)));
}

HLE_FUNC(coreinit, MEMFreeToExpHeap)
{
    std::lock_guard lock(s_memLock);
    expFree(ARG_U32(3), ARG_U32(4));
}

HLE_FUNC(coreinit, MEMGetAllocatableSizeForExpHeapEx)
{
    std::lock_guard lock(s_memLock);
    RET_U32(expAllocatable(ARG_U32(3), ARG_S32(4)));
}

HLE_FUNC(coreinit, MEMGetTotalFreeSizeForExpHeap)
{
    std::lock_guard lock(s_memLock);
    uint32_t total = 0;
    if (auto it = s_expHeaps.find(ARG_U32(3)); it != s_expHeaps.end())
        for (auto& [bs, bsz] : it->second.freeBlocks)
            total += bsz;
    RET_U32(total);
}

HLE_FUNC(coreinit, MEMCreateFrmHeapEx)
{
    std::lock_guard lock(s_memLock);
    RET_U32(createFrmHeap(ARG_U32(3), ARG_U32(4)));
}

HLE_FUNC(coreinit, MEMDestroyFrmHeap)
{
    std::lock_guard lock(s_memLock);
    s_frmHeaps.erase(ARG_U32(3));
    RET_U32(ARG_U32(3));
}

HLE_FUNC(coreinit, MEMAllocFromFrmHeapEx)
{
    std::lock_guard lock(s_memLock);
    RET_U32(frmAlloc(ARG_U32(3), ARG_U32(4), ARG_S32(5)));
}

HLE_FUNC(coreinit, MEMGetAllocatableSizeForFrmHeapEx)
{
    std::lock_guard lock(s_memLock);
    auto it = s_frmHeaps.find(ARG_U32(3));
    if (it == s_frmHeaps.end())
    {
        RET_U32(0);
        return;
    }
    const int32_t align = ARG_S32(4);
    const uint32_t a = uint32_t(align < 0 ? -align : align) < 4 ? 4 : uint32_t(align < 0 ? -align : align);
    const uint32_t p = alignUp(it->second.head, a);
    RET_U32(p < it->second.tail ? alignDown(it->second.tail - p, 4) : 0);
}

// void MEMInitAllocatorForExpHeap(MEMAllocator*, MEMHeapHandle, int32_t align)
HLE_FUNC(coreinit, MEMInitAllocatorForExpHeap)
{
    if (!s_expAllocatorFuncs)
    {
        s_expAllocatorFuncs = sysAlloc(8);
        store32(s_expAllocatorFuncs, exportHostFunction(hostAllocatorExpAlloc));
        store32(s_expAllocatorFuncs + 4, exportHostFunction(hostAllocatorExpFree));
    }
    const uint32_t allocator = ARG_U32(3);
    store32(allocator + 0, s_expAllocatorFuncs);
    store32(allocator + 4, ARG_U32(4));
    store32(allocator + 8, ARG_U32(5));
    store32(allocator + 12, 0);
}

// Un MEMAllocator peut venir du jeu (fonctions invitées) : on passe par sa table.
HLE_FUNC(coreinit, MEMAllocFromAllocator)
{
    const uint32_t allocator = ARG_U32(3);
    RET_U32(callGuest(ctx, load32(load32(allocator)), { allocator, ARG_U32(4) }));
}

HLE_FUNC(coreinit, MEMFreeToAllocator)
{
    const uint32_t allocator = ARG_U32(3);
    callGuest(ctx, load32(load32(allocator) + 4), { allocator, ARG_U32(4) });
}

// ================================================================= copies mémoire
HLE_FUNC(coreinit, memcpy)
{
    memmove(ptr(ARG_U32(3)), ptr(ARG_U32(4)), ARG_U32(5));
}
HLE_FUNC(coreinit, memmove)
{
    memmove(ptr(ARG_U32(3)), ptr(ARG_U32(4)), ARG_U32(5));
}
HLE_FUNC(coreinit, memset)
{
    memset(ptr(ARG_U32(3)), int(ARG_U32(4) & 0xFF), ARG_U32(5));
}
HLE_FUNC(coreinit, OSBlockMove)
{
    memmove(ptr(ARG_U32(3)), ptr(ARG_U32(4)), ARG_U32(5));
}
HLE_FUNC(coreinit, OSBlockSet)
{
    memset(ptr(ARG_U32(3)), int(ARG_U32(4) & 0xFF), ARG_U32(5));
}

// Caches : sans objet sur l'hôte (mémoire cohérente).
HLE_FUNC(coreinit, DCFlushRange) {}
HLE_FUNC(coreinit, DCFlushRangeNoSync) {}
HLE_FUNC(coreinit, DCInvalidateRange) {}
HLE_FUNC(coreinit, DCStoreRange) {}
HLE_FUNC(coreinit, DCStoreRangeNoSync) {}
HLE_FUNC(coreinit, DCZeroRange) { memset(ptr(ARG_U32(3)), 0, ARG_U32(4)); }
HLE_FUNC(coreinit, OSMemoryBarrier) {}
HLE_FUNC(coreinit, OSCoherencyBarrier) {}
HLE_FUNC(coreinit, OSEnforceInorderIO) {}
HLE_FUNC(coreinit, OSIsAddressRangeDCValid) { RET_U32(1); }

// ================================================================= cache verrouillé (LC)
// 16 Kio de « locked cache » par cœur, utilisés comme mémoire de travail rapide.
namespace
{
uint32_t s_lcBase = 0;
uint32_t s_lcUsed = 0;
constexpr uint32_t kLcSize = 0x4000;
}

HLE_FUNC(coreinit, LCAlloc)
{
    std::lock_guard lock(s_memLock);
    if (!s_lcBase)
        s_lcBase = sysAlloc(kLcSize, 0x200);
    const uint32_t size = alignUp(ARG_U32(3), 0x200);
    if (s_lcUsed + size > kLcSize)
    {
        RET_U32(0);
        return;
    }
    RET_U32(s_lcBase + s_lcUsed);
    s_lcUsed += size;
}
HLE_FUNC(coreinit, LCDealloc)
{
    std::lock_guard lock(s_memLock);
    s_lcUsed = 0;
}
HLE_FUNC(coreinit, LCGetUnallocated) { RET_U32(kLcSize - s_lcUsed); }
HLE_FUNC(coreinit, LCEnableDMA) { RET_U32(1); }
HLE_FUNC(coreinit, LCDisableDMA) {}
HLE_FUNC(coreinit, LCLoadDMABlocks) { memmove(ptr(ARG_U32(3)), ptr(ARG_U32(4)), (ARG_U32(5) ? ARG_U32(5) : 128) * 32); }
HLE_FUNC(coreinit, LCStoreDMABlocks) { memmove(ptr(ARG_U32(3)), ptr(ARG_U32(4)), (ARG_U32(5) ? ARG_U32(5) : 128) * 32); }
HLE_FUNC(coreinit, LCWaitDMAQueue) {}
