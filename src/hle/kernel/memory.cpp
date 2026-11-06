// Memory: NtAllocate/FreeVirtualMemory (user range), contiguous "physical"
// memory (MmAllocateContiguousMemory*), kernel pool (ExAllocatePool*) and the
// Mm* queries. The kernel lays out the guest address space itself (see
// kernel_internal.h) and asks the runtime only to back pages (g_host.commit).

#include <vector>

#include "hle/kernel/kernel_internal.h"

namespace hp2k {

RangeAlloc::RangeAlloc(uint32_t lo, uint32_t hi, uint32_t gran) : lo_(lo), hi_(hi), gran_(gran) {}

uint32_t RangeAlloc::Alloc(uint32_t size, uint32_t align, uint32_t lo_limit, uint32_t hi_limit,
                           bool top_down) {
  if (!size) return 0;
  uint64_t sz = (uint64_t(size) + gran_ - 1) / gran_ * gran_;
  if (align < gran_) align = gran_;
  uint64_t lo = std::max<uint64_t>(lo_, lo_limit);
  uint64_t hi = std::min<uint64_t>(hi_, uint64_t(hi_limit) + 1);
  // Walk gaps between used blocks.
  std::vector<std::pair<uint64_t, uint64_t>> gaps;
  uint64_t prev = lo_;
  for (auto& [s, n] : used_) {
    if (s > prev) gaps.push_back({prev, s});
    prev = std::max<uint64_t>(prev, uint64_t(s) + n);
  }
  if (hi_ > prev) gaps.push_back({prev, hi_});
  auto try_gap = [&](uint64_t a, uint64_t b) -> uint64_t {
    a = std::max(a, lo);
    b = std::min(b, hi);
    if (b <= a) return 0;
    if (!top_down) {
      uint64_t s = (a + align - 1) / align * align;
      return s + sz <= b ? s : 0;
    }
    if (b < sz) return 0;
    uint64_t s = (b - sz) / align * align;
    return s >= a ? s : 0;
  };
  if (!top_down) {
    for (auto& g : gaps)
      if (uint64_t s = try_gap(g.first, g.second)) {
        used_[uint32_t(s)] = uint32_t(sz);
        return uint32_t(s);
      }
  } else {
    for (auto it = gaps.rbegin(); it != gaps.rend(); ++it)
      if (uint64_t s = try_gap(it->first, it->second)) {
        used_[uint32_t(s)] = uint32_t(sz);
        return uint32_t(s);
      }
  }
  return 0;
}

bool RangeAlloc::Reserve(uint32_t va, uint32_t size) {
  uint64_t end = uint64_t(va) + size;
  if (va < lo_ || end > hi_ || !size) return false;
  auto it = used_.upper_bound(va);
  if (it != used_.end() && it->first < end) return false;
  if (it != used_.begin()) {
    auto p = std::prev(it);
    if (uint64_t(p->first) + p->second > va) return false;
  }
  used_[va] = size;
  return true;
}

uint32_t RangeAlloc::Free(uint32_t va) {
  auto it = used_.find(va);
  if (it == used_.end()) return 0;
  uint32_t n = it->second;
  used_.erase(it);
  return n;
}

uint32_t RangeAlloc::SizeOf(uint32_t va) const {
  auto it = used_.find(va);
  return it == used_.end() ? 0 : it->second;
}

bool RangeAlloc::Find(uint32_t va, uint32_t* start, uint32_t* size) const {
  auto it = used_.upper_bound(va);
  if (it == used_.begin()) return false;
  --it;
  if (va >= uint64_t(it->first) + it->second) return false;
  *start = it->first;
  *size = it->second;
  return true;
}

namespace {

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000,
                   kMemRelease = 0x8000, kMemFree = 0x10000, kMemPrivate = 0x20000,
                   kMemImage = 0x1000000, kMemTopDown = 0x100000;
constexpr uint32_t kPageReadWrite = 0x04, kPageNoAccess = 0x01;

std::recursive_mutex g_mem_mu;
RangeAlloc g_user(kUserLo, kUserHi, kPage);
RangeAlloc g_phys(kPhysVa, kPhysVa + kPhysSize, kPage);
RangeAlloc g_pool(kPoolLo, kPoolHi, 16);

// Reserved user regions (NtAllocateVirtualMemory): per-page commit state.
struct Region {
  uint32_t size = 0;
  uint32_t protect = kPageReadWrite;
  std::vector<uint8_t> committed;  // per page
};
std::map<uint32_t, Region> g_regions;
// Contiguous blocks: protection as set by MmSetAddressProtect.
std::map<uint32_t, uint32_t> g_phys_protect;
// Pool pages already committed (one bit per page of the pool range).
std::vector<bool> g_pool_pages((kPoolHi - kPoolLo) / kPage);

bool CommitRange(uint32_t va, uint32_t size) {
  uint32_t a = va & ~(kPage - 1);
  uint64_t b = (uint64_t(va) + size + kPage - 1) & ~uint64_t(kPage - 1);
  return g_host.commit(a, uint32_t(b - a), true);
}

Region* RegionOf(uint32_t va, uint32_t* base) {
  auto it = g_regions.upper_bound(va);
  if (it == g_regions.begin()) return nullptr;
  --it;
  if (va >= uint64_t(it->first) + it->second.size) return nullptr;
  *base = it->first;
  return &it->second;
}

}  // namespace

void MemoryInit() {}

uint32_t PoolAlloc(uint32_t size) {
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  uint32_t va = g_pool.Alloc(size ? size : 1, 16);
  if (!va) return 0;
  uint32_t n = g_pool.SizeOf(va);
  for (uint32_t p = (va - kPoolLo) / kPage; p <= (va + n - 1 - kPoolLo) / kPage; p++)
    if (!g_pool_pages[p]) {
      if (!g_host.commit(kPoolLo + p * kPage, kPage, true)) return 0;
      g_pool_pages[p] = true;
    }
  std::memset(GP(va), 0, n);
  return va;
}

void PoolFree(uint32_t va) {
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  if (!g_pool.Free(va)) Warn("ExFreePool(%08X): not a pool block", va);
}

uint32_t PoolSize(uint32_t va) {
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  return g_pool.SizeOf(va);
}

uint32_t ContiguousAlloc(uint32_t size, uint32_t lo_phys, uint32_t hi_phys, uint32_t align) {
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  if (hi_phys >= kPhysSize) hi_phys = kPhysSize - 1;
  if (align < kPage) align = kPage;
  uint32_t va = g_phys.Alloc(size, align, kPhysVa + lo_phys, kPhysVa + hi_phys);
  if (!va) return 0;
  if (!CommitRange(va, g_phys.SizeOf(va))) return 0;
  std::memset(GP(va), 0, g_phys.SizeOf(va));
  g_phys_protect[va] = kPageReadWrite;
  return va;
}

uint32_t UserAlloc(uint32_t size, uint32_t align) {
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  uint32_t va = g_user.Alloc(size, std::max<uint32_t>(align, 0x10000));
  if (!va) return 0;
  uint32_t n = g_user.SizeOf(va);
  if (!g_host.commit(va, n, true)) return 0;
  Region r;
  r.size = n;
  r.committed.assign(n / kPage, 1);
  g_regions[va] = std::move(r);
  return va;
}

// ---- NtAllocateVirtualMemory(PVOID* Base, ULONG ZeroBits, PSIZE_T Size,
//      ULONG AllocationType, ULONG Protect) ---------------------------------------
KEXPORT(184, NtAllocateVirtualMemory, 5) {
  uint32_t pbase = Arg(c, 0), psize = Arg(c, 2), type = Arg(c, 3), prot = Arg(c, 4);
  uint32_t base = Rd32(pbase), size = Rd32(psize);
  if (!size) return kInvalidParameter;
  if (!(type & (kMemCommit | kMemReserve))) return kInvalidParameter;
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  if (type & kMemReserve) {
    uint32_t start;
    if (base) {
      start = base & ~0xFFFFu;
      uint32_t end = (base + size + kPage - 1) & ~(kPage - 1);
      if (!g_user.Reserve(start, end - start)) return kConflictingAddresses;
    } else {
      start = g_user.Alloc(size, 0x10000, 0, 0xFFFFFFFF, (type & kMemTopDown) != 0);
      if (!start) return kNoMemory;
    }
    uint32_t n = g_user.SizeOf(start);
    Region r;
    r.size = n;
    r.protect = prot;
    r.committed.assign(n / kPage, 0);
    g_regions[start] = std::move(r);
    if (!base) base = start;
    if (!(type & kMemCommit)) {
      Wr32(pbase, start);
      Wr32(psize, n);
      return kSuccess;
    }
  }
  // Commit [base, base+size) inside an existing reservation.
  uint32_t rbase;
  Region* r = RegionOf(base, &rbase);
  if (!r) return kConflictingAddresses;
  uint32_t a = base & ~(kPage - 1);
  uint64_t b = (uint64_t(base) + size + kPage - 1) & ~uint64_t(kPage - 1);
  if (b > uint64_t(rbase) + r->size) return kConflictingAddresses;
  for (uint32_t p = a; p < b; p += kPage) {
    uint8_t& cm = r->committed[(p - rbase) / kPage];
    if (!cm) {
      if (!g_host.commit(p, kPage, true)) return kNoMemory;
      cm = 1;
    }
  }
  Wr32(pbase, a);
  Wr32(psize, uint32_t(b - a));
  return kSuccess;
}

// NtFreeVirtualMemory(PVOID* Base, PSIZE_T Size, ULONG FreeType)
KEXPORT(199, NtFreeVirtualMemory, 3) {
  uint32_t pbase = Arg(c, 0), psize = Arg(c, 1), type = Arg(c, 2);
  uint32_t base = Rd32(pbase), size = Rd32(psize);
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  uint32_t rbase;
  Region* r = RegionOf(base, &rbase);
  if (!r) return kMemoryNotAllocated;
  if (type & kMemRelease) {
    if (base != rbase || (size && size != r->size)) return kFreeVmNotAtBase;
    uint32_t n = r->size;
    if (g_host.decommit) g_host.decommit(rbase, n);
    g_regions.erase(rbase);
    g_user.Free(rbase);
    Wr32(psize, n);
    return kSuccess;
  }
  if (type & kMemDecommit) {
    uint32_t a = base & ~(kPage - 1);
    uint64_t b = size ? (uint64_t(base) + size + kPage - 1) & ~uint64_t(kPage - 1)
                      : uint64_t(rbase) + r->size;
    if (b > uint64_t(rbase) + r->size) return kInvalidParameter;
    for (uint32_t p = a; p < b; p += kPage) r->committed[(p - rbase) / kPage] = 0;
    if (g_host.decommit) g_host.decommit(a, uint32_t(b - a));
    Wr32(pbase, a);
    Wr32(psize, uint32_t(b - a));
    return kSuccess;
  }
  return kInvalidParameter;
}

// NtQueryVirtualMemory(PVOID Base, PMEMORY_BASIC_INFORMATION Info)
KEXPORT(217, NtQueryVirtualMemory, 2) {
  uint32_t va = Arg(c, 0), mbi = Arg(c, 1);
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  uint32_t page = va & ~(kPage - 1);
  uint32_t rbase;
  Region* r = RegionOf(va, &rbase);
  uint32_t alloc_base = 0, alloc_prot = 0, state = kMemFree, prot = kPageNoAccess,
           type = 0, len = kPage;
  if (r) {
    alloc_base = rbase;
    alloc_prot = r->protect;
    type = kMemPrivate;
    uint32_t i = (page - rbase) / kPage;
    uint8_t cm = r->committed[i];
    uint32_t j = i;
    while (j < r->committed.size() && r->committed[j] == cm) j++;
    len = (j - i) * kPage;
    state = cm ? kMemCommit : kMemReserve;
    prot = cm ? r->protect : 0;
  } else if (va >= 0x10000 && va < kUserLo) {
    // The XBE image (and what the loader put below the user range).
    alloc_base = 0x10000;
    alloc_prot = prot = kPageReadWrite;
    state = kMemCommit;
    type = kMemImage;
    len = kUserLo - page;
  } else {
    uint32_t s, n;
    if (g_user.Find(va, &s, &n)) len = s + n - page;
  }
  Wr32(mbi + 0x00, page);
  Wr32(mbi + 0x04, alloc_base);
  Wr32(mbi + 0x08, alloc_prot);
  Wr32(mbi + 0x0C, len);
  Wr32(mbi + 0x10, state);
  Wr32(mbi + 0x14, prot);
  Wr32(mbi + 0x18, type);
  return kSuccess;
}

// ---- contiguous memory ------------------------------------------------------------
KEXPORT(165, MmAllocateContiguousMemory, 1) {
  return ContiguousAlloc(Arg(c, 0), 0, 0xFFFFFFFF, kPage);
}

// MmAllocateContiguousMemoryEx(Size, LowestAcceptableAddress,
//   HighestAcceptableAddress, Alignment, ProtectionType)
KEXPORT(166, MmAllocateContiguousMemoryEx, 5) {
  return ContiguousAlloc(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

KEXPORT(171, MmFreeContiguousMemory, 1) {
  uint32_t va = Arg(c, 0);
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  if (!g_phys.Free(va)) Warn("MmFreeContiguousMemory(%08X): not a block", va);
  g_phys_protect.erase(va);
  return 0;
}

KEXPORT(173, MmGetPhysicalAddress, 1) {
  uint32_t va = Arg(c, 0);
  if (va >= kPhysVa && va < kPhysVa + kPhysSize) return va - kPhysVa;
  return va;  // identity for the image / virtual memory (no real DMA here)
}

// MmClaimGpuInstanceMemory(NumberOfBytes, *NumberOfPaddingBytes): D3D init
// only (replaced by Render); report the top of physical memory.
KEXPORT(168, MmClaimGpuInstanceMemory, 2) {
  if (uint32_t pad = Arg(c, 1)) Wr32(pad, 0);
  return kPhysVa + kPhysSize;
}

KEXPORT(175, MmLockUnlockBufferPages, 3) { return 0; }
KEXPORT(176, MmLockUnlockPhysicalPage, 2) { return 0; }
KEXPORT(178, MmPersistContiguousMemory, 3) { return 0; }

KEXPORT(179, MmQueryAddressProtect, 1) {
  uint32_t va = Arg(c, 0);
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  uint32_t s, n;
  if (g_phys.Find(va, &s, &n)) return g_phys_protect.count(s) ? g_phys_protect[s] : kPageReadWrite;
  uint32_t rbase;
  if (Region* r = RegionOf(va, &rbase))
    return r->committed[(va - rbase) / kPage] ? r->protect : 0;
  if (g_pool.Find(va, &s, &n)) return kPageReadWrite;
  if (va >= 0x10000 && va < kUserLo) return kPageReadWrite;
  return 0;
}

// MmSetAddressProtect(Base, Size, NewProtect): recorded only; guest memory
// stays readable/writable on the host.
KEXPORT(182, MmSetAddressProtect, 3) {
  uint32_t va = Arg(c, 0), prot = Arg(c, 2);
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  uint32_t s, n;
  if (g_phys.Find(va, &s, &n)) g_phys_protect[s] = prot;
  uint32_t rbase;
  if (Region* r = RegionOf(va, &rbase)) r->protect = prot;
  return 0;
}

KEXPORT(180, MmQueryAllocationSize, 1) {
  uint32_t va = Arg(c, 0);
  std::lock_guard<std::recursive_mutex> l(g_mem_mu);
  uint32_t s, n;
  if (g_phys.Find(va, &s, &n)) return n;
  uint32_t rbase;
  if (Region* r = RegionOf(va, &rbase)) return r->size;
  if (g_pool.Find(va, &s, &n)) return n;
  return 0;
}

// ---- pool -----------------------------------------------------------------------------
KEXPORT(14, ExAllocatePool, 1) { return PoolAlloc(Arg(c, 0)); }
KEXPORT(15, ExAllocatePoolWithTag, 2) { return PoolAlloc(Arg(c, 0)); }
KEXPORT(17, ExFreePool, 1) {
  PoolFree(Arg(c, 0));
  return 0;
}
KEXPORT(23, ExQueryPoolBlockSize, 1) { return PoolSize(Arg(c, 0)); }

}  // namespace hp2k
