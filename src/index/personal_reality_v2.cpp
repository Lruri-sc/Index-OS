#include "index/personal_reality_v2.hpp"

#include "arch/aarch64/cpu.hpp"  // arch::this_cpu_id for the re-entrant Pr2Guard
#include "index/anti_skill.hpp"  // serialize page-table mutation across cores (Pr2Guard)
#include "index/artificial_heaven.hpp"
#include "index/esper.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/lateran.hpp"
#include "index/teleport.hpp"
#include "index/tree_diagram.hpp"

namespace index {

namespace {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kPageMask = kPageSize - 1;
constexpr uint32_t kEntriesPerTable = 512;

// Descriptor bits, matching teleport.cpp's table layout (MAIR index 1 =
// Normal, index 0 = Device). Block at L1/L2 vs page at L3 share the bottom
// two bits; tables use 0b11.
constexpr uint64_t kTable = 0b11ULL;
constexpr uint64_t kBlock = 0b01ULL; // L1/L2 block descriptor (kernel maps 1GiB blocks)
constexpr uint64_t kPage = 0b11ULL;
constexpr uint64_t kAttrNormal = 1ULL << 2;
constexpr uint64_t kApRwBoth = 1ULL << 6;   // EL1 + EL0 RW
constexpr uint64_t kApRoBoth = 3ULL << 6;   // EL1 + EL0 RO
constexpr uint64_t kShInner = 3ULL << 8;
constexpr uint64_t kAf = 1ULL << 10;
constexpr uint64_t kPxn = 1ULL << 53;       // privileged execute-never (always set for EL0 pages)
constexpr uint64_t kUxn = 1ULL << 54;       // unprivileged execute-never

// L1 entries cover 1 GiB each (bits [38:30]); L2 covers 2 MiB (bits [29:21]);
// L3 covers 4 KiB (bits [20:12]). Standard 4 KiB granule, 39-bit VA.
constexpr uint64_t l1_index(uint64_t va) { return (va >> 30) & 0x1ff; }
constexpr uint64_t l2_index(uint64_t va) { return (va >> 21) & 0x1ff; }
constexpr uint64_t l3_index(uint64_t va) { return (va >> 12) & 0x1ff; }

// Read/write a physical page via its high-half alias. TTBR1 still points at
// the kernel g_l1 even while a user's private TTBR0 is loaded, so a kernel
// pointer of the form (kHighHalfBase | pa) is always valid for RAM PAs.
uint64_t *table_at(uint64_t phys) {
    return reinterpret_cast<uint64_t *>(teleport_high_alias(phys));
}

void zero_page(uint64_t phys) {
    auto *p = reinterpret_cast<uint64_t *>(teleport_high_alias(phys));
    for (uint32_t i = 0; i < kPageSize / sizeof(uint64_t); ++i) {
        p[i] = 0;
    }
}

// Allocate a 4 KiB page from TreeDiagram, zeroed. Returns its PA or 0 on OOM.
uint64_t alloc_page_zeroed() {
    void *p = tree_diagram_alloc_page();
    if (p == nullptr) {
        return 0;
    }
    const uint64_t pa = reinterpret_cast<uint64_t>(p) & ~kHighHalfBase;
    zero_page(pa);
    return pa;
}

// SMP coherency for a freshly written executable page. The code bytes were
// written through the kernel's high-half alias of page_pa, so clean those lines
// from the D-cache to the PoU, then invalidate the I-cache for the page on EVERY
// PE: `ic ivau` broadcasts in the inner-shareable domain (ARMv8 instruction-
// cache maintenance is by-PA to the PoU, so cleaning/invalidating via the alias
// VA is architecturally correct). The previous `ic iallu` only flushed the
// faulting core's I-cache -- a sibling core that executed the page kept a stale
// line and took the intermittent SMP instruction-permission fault (single core
// has one I-cache, flushed locally, so it never reproduced there).
void make_exec_coherent(uint64_t page_pa) {
    const uint64_t kva = teleport_high_alias(page_pa);
    for (uint64_t off = 0; off < kPageSize; off += 64) {
        asm volatile("dc cvau, %0" ::"r"(kva + off) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
    for (uint64_t off = 0; off < kPageSize; off += 64) {
        asm volatile("ic ivau, %0" ::"r"(kva + off) : "memory");
    }
    asm volatile("dsb ish; isb" ::: "memory");
}

// --- physical-page reference counts ---------------------------------------
// A leaf (data) page can be mapped into more than one address space at once
// (CoW fork shares it read-only; this is the only sharing for now). We track a
// reference count per physical page so the page is returned to TreeDiagram only
// when the last mapping drops. Page-table pages (L1/L2/L3) are per-process and
// freed directly on teardown, so they are not refcounted here.
//
// virt RAM starts at 0x40000000; we cover 2 GiB above that. Index =
// (pa - base) >> 12. Out-of-range PAs (shouldn't happen for user pages) are
// ignored, so refcounting silently no-ops rather than corrupting memory.
constexpr uint64_t kRamBase = 0x40000000ULL;
constexpr uint64_t kRefSpanPages = 0x80000000ULL / kPageSize; // 2 GiB / 4 KiB
uint16_t g_page_refs[kRefSpanPages];

uint64_t ref_index(uint64_t pa) {
    if (pa < kRamBase) {
        return kRefSpanPages; // sentinel "out of range"
    }
    const uint64_t idx = (pa - kRamBase) >> 12;
    return idx < kRefSpanPages ? idx : kRefSpanPages;
}

// Lock-free atomics on the refcount: lets linux_deliver_signal hold
// g_sched_lock across pr2_write_user (which calls page_ref / page_unref
// on the COW path) without re-entering the scheduler lock. ACQ_REL on the
// inc/dec gives release-ordered publication of the page's content to
// future readers and acquire on the "reached zero" observation so the
// freer sees all prior writes.
void page_ref(uint64_t pa) {
    const uint64_t i = ref_index(pa);
    if (i < kRefSpanPages) {
        __atomic_fetch_add(&g_page_refs[i], 1, __ATOMIC_ACQ_REL);
    }
}

uint16_t page_refcount(uint64_t pa) {
    const uint64_t i = ref_index(pa);
    return i < kRefSpanPages
        ? __atomic_load_n(&g_page_refs[i], __ATOMIC_ACQUIRE)
        : 1; // untracked -> treat as unique
}

// Drop one reference; free the page back to TreeDiagram when it hits zero.
// CAS loop preserves the "> 0" guard the legacy single-core version had
// (silently no-op on underflow instead of corrupting the counter).
void page_unref(uint64_t pa) {
    const uint64_t i = ref_index(pa);
    if (i >= kRefSpanPages) {
        return; // untracked page: leave it alone
    }
    uint16_t expected = __atomic_load_n(&g_page_refs[i], __ATOMIC_ACQUIRE);
    while (expected > 0) {
        if (__atomic_compare_exchange_n(&g_page_refs[i], &expected,
                static_cast<uint16_t>(expected - 1), false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (expected == 1) {
                tree_diagram_free_page(pa);
            }
            return;
        }
    }
}

// Convert (prot, kind) to descriptor attribute bits for a page at L3.
uint64_t page_attrs(uint8_t prot) {
    uint64_t a = kAttrNormal | kShInner | kAf | kPxn;
    a |= (prot & kVmaProtW) ? kApRwBoth : kApRoBoth;
    if (!(prot & kVmaProtX)) {
        a |= kUxn;
    }
    return a;
}

// Walk (or lazily build) L1 -> L2 -> L3 for VA `va` in `l1_pa`. Returns a
// pointer to the L3 entry, or nullptr if a page-table page couldn't be
// allocated.
uint64_t *ensure_l3_entry(uint64_t l1_pa, uint64_t va) {
    uint64_t *l1 = table_at(l1_pa);
    uint64_t &l1e = l1[l1_index(va)];
    // SECURITY: refuse to descend into a table still SHARED with the kernel's
    // master L1 (the inherited kernel mappings -- e.g. the kernel's own GiB at
    // L1[1], a kTable -> kernel g_l2/g_l3). Otherwise a user fault in that VA
    // range walks into the kernel's shared leaf tables and pr2_handle_fault
    // rewrites a kernel PTE to an EL0-accessible page -> the faulting process
    // gains RW to kernel memory across every address space (a mmap MAP_FIXED at
    // 0x40000000 then a touch is enough). RAM-block entries (L1[2..], kBlock) are
    // privatized by the break-before-make below and never alias the kernel's
    // tables, so only the shared kTable is dangerous. Mainstream kernels keep
    // user mappings strictly in the user half; here we reject any VA whose
    // top-level entry is still the kernel's shared table -> the access SIGSEGVs.
    if ((l1e & 0b11ULL) == kTable && l1e == teleport_kernel_l1()[l1_index(va)]) {
        return nullptr;
    }
    uint64_t l2_pa;
    if ((l1e & 0b11ULL) == kTable) {
        l2_pa = l1e & 0x0000FFFFFFFFF000ULL;
    } else {
        const uint64_t old_l1e = l1e;
        l2_pa = alloc_page_zeroed();
        if (l2_pa == 0) {
            return nullptr;
        }
        // BREAK-BEFORE-MAKE. l1e here is NOT a table -- it is either invalid
        // (0b00) or a valid 1 GiB BLOCK (0b01). The block case is the norm:
        // pr2_create_addr_space copies the kernel L1 (all 1 GiB device/RAM
        // blocks) into every address space, so a process's first fault in a
        // given 1 GiB window converts that inherited block into a per-process
        // table. ARM (Arm ARM D8 "Using break-before-make") FORBIDS replacing a
        // live block with a table for the same VA in one store: the TLB may then
        // cache the old block AND the new table simultaneously -> a TLB conflict
        // abort or an UNPREDICTABLE/amalgamated translation. That is the broad,
        // intermittent, SMP-only corruption seen here (user L1 permission fault
        // on the process's own code, wild kernel branches). Do it correctly:
        // invalidate, DSB, broadcast TLB flush, then write the table. (Every
        // mainstream arm64 kernel -- Linux, seL4, Xen -- follows BBM.)
        if ((old_l1e & 0b11ULL) == kBlock) {
            l1e = 0;
            asm volatile("dsb ish" ::: "memory");
            asm volatile("tlbi vmalle1is" ::: "memory");
            asm volatile("dsb ish; isb" ::: "memory");
        }
        l1e = l2_pa | kTable;
        asm volatile("dsb ish" ::: "memory"); // make the new L1 entry visible to the walker
    }
    uint64_t *l2 = table_at(l2_pa);
    uint64_t &l2e = l2[l2_index(va)];
    uint64_t l3_pa;
    if ((l2e & 0b11ULL) == kTable) {
        l3_pa = l2e & 0x0000FFFFFFFFF000ULL;
    } else {
        const uint64_t old_l2e = l2e;
        l3_pa = alloc_page_zeroed();
        if (l3_pa == 0) {
            return nullptr;
        }
        // Break-before-make for an inherited 2 MiB BLOCK -> table, same ARM rule
        // as the L1 level above. (Per-process L2s are usually freshly-zeroed so
        // this rarely fires, but a shared kernel L2 with block entries would.)
        if ((old_l2e & 0b11ULL) == kBlock) {
            l2e = 0;
            asm volatile("dsb ish" ::: "memory");
            asm volatile("tlbi vmalle1is" ::: "memory");
            asm volatile("dsb ish; isb" ::: "memory");
        }
        l2e = l3_pa | kTable;
        asm volatile("dsb ish" ::: "memory");
    }
    uint64_t *l3 = table_at(l3_pa);
    return &l3[l3_index(va)];
}

// Find the VMA covering `va` in e's (shared) address space. The list lives in
// e->mm (the PersonalReality jointly owned by all threads), so every thread
// sees the same map. nullptr mm = Index legacy-pool Esper -> no VMA list.
const Vma *find_vma(const Esper *e, uint64_t va) {
    if (e == nullptr || e->mm == nullptr) {
        return nullptr;
    }
    const PersonalReality *mm = e->mm;
    for (uint32_t i = 0; i < mm->vma_count; ++i) {
        const Vma &v = mm->vmas[i];
        if (v.kind != VmaKind::Free && va >= v.start && va < v.end) {
            return &v;
        }
    }
    return nullptr;
}

// Page-table serialization. pr2_handle_fault / ensure_l3_entry / pr2_write_user
// / pr2_fork / pr2_destroy / pr2_mprotect read-modify-write an Esper's page
// tables (L1/L2/L3 descriptors, CoW page copies) and VMA list with no lock.
// Two cores touching the SAME address space then race: e.g. concurrent
// ensure_l3_entry on one empty L1 entry both alloc an L2 and one store wins, so
// the loser's L2/L3 subtree (and every mapping under it) leaks and that user VA
// resolves to a stale / wrong page -- the residual SMP "broad wild-write"
// (a user VA mapped to a kernel page or junk; the process then rets/derefs into
// a kernel address -> EL0 fault). Mainstream kernels serialize this (Linux's
// mmap_lock + the split page-table locks); Index uses one lock around every
// page-table-mutating entry. Same-address-space concurrency arises from threads
// (CLONE_VM share one ttbr0) and from a fault on one core while another core
// writes that process's memory (deliver_pending_status / wait4 reap ->
// pr2_write_user, which walks the *target's* page table).
//
// Same idiom + safety argument as Lateran's FsGuard: re-entrant on one CPU
// (g_pt_owner) so pr2_write_user -> pr2_handle_fault and pr2_fork ->
// pr2_create_addr_space don't self-deadlock; deliberately NOT irqsave because
// esper_preempt never preempts EL1 (a fault/syscall holding this lock cannot be
// switched out mid-section) and no IRQ handler touches page tables, so there is
// no same-core re-entrant or self-deadlock path. The lock only ever nests
// TreeDiagram's allocator lock and never takes g_esper_lock, so the global
// order g_esper_lock -> g_pt_lock -> tree_diagram lock has no cycle.
AntiSkill g_pt_lock;
volatile int32_t g_pt_owner = -1; // CPU currently mutating page tables, -1 = none

struct Pr2Guard {
    bool owns;
    Pr2Guard() {
        const int32_t cpu = static_cast<int32_t>(arch::this_cpu_id());
        if (g_pt_owner == cpu) { // already inside on this CPU -> re-entrant
            owns = false;
            return;
        }
        anti_skill_lock(g_pt_lock);
        g_pt_owner = cpu;
        owns = true;
    }
    ~Pr2Guard() {
        if (!owns) return;
        g_pt_owner = -1;
        anti_skill_unlock(g_pt_lock);
    }
    Pr2Guard(const Pr2Guard &) = delete;
    Pr2Guard &operator=(const Pr2Guard &) = delete;
};

} // namespace

// ---- PersonalReality (address space / mm) pool + refcount -----------------
// The Index analogue of Linux mm_struct allocation. 2*kMaxEspers slots cover
// the transient fork/exec window where a process briefly references two address
// spaces. Each slot is ~28 KiB (Vma[512]); the pool is a line-scanned table
// guarded by g_esper_lock, exactly like usermode.cpp's g_as_refs. fork/exec are
// rare so the linear scan + lock latency is negligible.
namespace {
constexpr uint32_t kMaxRealities = 2 * kMaxEspers;
PersonalReality g_realities[kMaxRealities];
} // namespace

PersonalReality *reality_alloc() {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    for (auto &r : g_realities) {
        if (!r.in_use) {
            // Reset only the metadata -- NOT `r = PersonalReality{}`, which would
            // materialize a ~78 KiB stack temporary (Vma vmas[512] incl. file_path)
            // and overflow the 64 KiB kernel stack (boot hung in init's first
            // reality_alloc once file_path grew the struct). vmas[] need not be
            // cleared: vma_count gates every read (find_vma loops i<vma_count) and
            // pr2_add_vma fully rewrites each slot (incl. file_path) before use.
            r.ttbr0 = 0;
            r.vma_count = 0;
            r.brk_start = 0;
            r.brk_cur = 0;
            r.mmap_next = 0;
            r.in_use = true;
            r.refs = 1;
            anti_skill_unlock_irqrestore(g_esper_lock, flags);
            return &r;
        }
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return nullptr; // pool exhausted
}

void reality_ref(PersonalReality *mm) {
    if (mm == nullptr) return;
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    ++mm->refs;
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
}

void reality_unref(Esper *e) {
    if (e == nullptr || e->mm == nullptr) return;
    PersonalReality *mm = e->mm;
    e->mm = nullptr; // drop e's reference regardless of who frees the space
    bool destroy = false;
    {
        const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
        if (mm->refs > 0 && --mm->refs == 0) destroy = true;
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
    }
    if (!destroy) return;
    // Last sharer: this CPU's TTBR0 may still point at mm->ttbr0. Switch to the
    // kernel table BEFORE freeing the page tables (mirror Linux switching to
    // init_mm before mmdrop) so a kernel fetch through TTBR0 can't hit a page
    // we just returned to the allocator -- the SMP use-after-free guard that
    // as_unref used to provide.
    asm volatile("msr ttbr0_el1, %0" ::"r"(teleport_kernel_ttbr0()) : "memory");
    asm volatile("dsb ish; tlbi vmalle1is; dsb ish; isb" ::: "memory");
    pr2_destroy(mm); // frees page tables + leaf pages; sets mm->ttbr0 = 0
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    mm->in_use = false;
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
}

uint32_t reality_active_count() {
    uint32_t n = 0;
    for (auto &r : g_realities) if (r.in_use) ++n;
    return n;
}

bool pr2_create_addr_space(Esper *e) {
    if (e == nullptr || e->mm == nullptr) {
        return false;
    }
    Pr2Guard _g;
    const uint64_t l1_pa = alloc_page_zeroed();
    if (l1_pa == 0) {
        return false;
    }
    // Inherit the kernel's mappings -- entry 1 keeps the kernel RAM block
    // (AP=EL1-only so EL0 can't reach it), other kernel device blocks stay
    // for any future kernel access. Then leave entry 0 empty (the user's
    // low-VA space) plus any other entries the user might want; faults on
    // those VAs will trigger pr2_handle_fault, which will lazily install
    // L2/L3 tables and pages.
    const uint64_t *kl1 = teleport_kernel_l1();
    uint64_t *l1 = table_at(l1_pa);
    for (uint32_t i = 0; i < kEntriesPerTable; ++i) {
        l1[i] = kl1[i];
    }
    // entry 0 was a device block in the kernel; clear it so EL0 faults land
    // in pr2_handle_fault for the typical low-VA layout (entry 0 = 0..1 GiB,
    // covers 0x10000 / 0x400000 / etc).
    l1[0] = 0;
    e->mm->ttbr0 = l1_pa; // authoritative page-table base
    e->ttbr0 = l1_pa;     // fast-path cache for the context switch (mirror)
    e->mm->vma_count = 0;
    return true;
}

bool pr2_add_vma(Esper *e, uint64_t start, uint64_t end, uint8_t prot,
                 uint8_t kind, const uint8_t *file_src, uint64_t file_off,
                 uint64_t file_size, uint64_t seg_vaddr,
                 const char *file_path) {
    if (e == nullptr || e->mm == nullptr || end <= start) {
        return false;
    }
    PersonalReality *mm = e->mm; // VMAs live in the shared address space
    Pr2Guard _g; // the VMA list is read by find_vma during concurrent faults
    // Reuse a Free slot left behind by pr2_remove_vma_range. Without this the
    // table fills monotonically: busybox shell's malloc rounds large allocs
    // to mmap+munmap, and every command leaks ~1 slot. kMaxVmas=32 was hit
    // around 20-25 commands -> mmap returned -ENOMEM -> "sh: out of memory".
    uint32_t slot = kMaxVmas;
    for (uint32_t i = 0; i < mm->vma_count; ++i) {
        if (mm->vmas[i].kind == VmaKind::Free) { slot = i; break; }
    }
    if (slot == kMaxVmas) {
        if (mm->vma_count >= kMaxVmas) {
            return false;
        }
        slot = mm->vma_count++;
    }
    Vma &v = mm->vmas[slot];
    v.start = start & ~kPageMask;                                  // page-align down
    v.end = (end + kPageMask) & ~kPageMask;                        // and up
    v.prot = prot;
    v.kind = static_cast<VmaKind>(kind);
    v.file_src = file_src;
    v.file_off = file_off;
    v.file_size = file_size;
    v.seg_pad = (seg_vaddr != 0 && seg_vaddr >= v.start) ? (seg_vaddr - v.start) : 0;
    // Demand-paging source path (for file_src==nullptr file mmaps). Copied so it
    // outlives the fd, which a program may close right after mmap. Slot reuse
    // requires always (re)setting it -- clear it when no path is given.
    if (file_path != nullptr) {
        uint32_t i = 0;
        for (; i + 1 < sizeof(v.file_path) && file_path[i] != '\0'; ++i) v.file_path[i] = file_path[i];
        v.file_path[i] = '\0';
    } else {
        v.file_path[0] = '\0';
    }
    return true;
}

// Resolve a user VA to its physical page (faulting it in), returning a
// kernel-usable pointer to the exact byte via the high-half alias, or nullptr
// if the VA isn't backed by a VMA / out of memory.
static uint8_t *user_byte_ptr(Esper *e, uint64_t va) {
    if (!pr2_handle_fault(e, va)) {
        return nullptr;
    }
    uint64_t *l3e = ensure_l3_entry(e->ttbr0, va & ~kPageMask);
    if (l3e == nullptr || (*l3e & 0b11ULL) != kPage) {
        return nullptr;
    }
    const uint64_t pa = (*l3e & 0x0000FFFFFFFFF000ULL) | (va & kPageMask);
    return reinterpret_cast<uint8_t *>(teleport_high_alias(pa));
}

bool pr2_write_user(Esper *e, uint64_t va, const void *src, uint64_t n) {
    if (e == nullptr) {
        return false;
    }
    Pr2Guard _g; // hold across the whole multi-byte/multi-page write
    const auto *s = static_cast<const uint8_t *>(src);
    for (uint64_t i = 0; i < n; ++i) {
        uint8_t *dst = user_byte_ptr(e, va + i);
        if (dst == nullptr) {
            return false;
        }
        *dst = s[i];
    }
    return true;
}

// Read n bytes from `e`'s user address space (va) into kernel `dst`. The mirror
// of pr2_write_user, used by ptrace PEEK to read a tracee's memory from the
// tracer's syscall context (user_byte_ptr faults the page in via e's own page
// table). Returns false if any byte's VA is unmapped/unfaultable.
bool pr2_read_user(Esper *e, uint64_t va, void *dst, uint64_t n) {
    if (e == nullptr) {
        return false;
    }
    Pr2Guard _g;
    auto *d = static_cast<uint8_t *>(dst);
    for (uint64_t i = 0; i < n; ++i) {
        const uint8_t *src = user_byte_ptr(e, va + i);
        if (src == nullptr) {
            return false;
        }
        d[i] = *src;
    }
    return true;
}

// Read-only page-table walk: return the L3 entry value for `va` in `l1_pa`, or
// 0 if any level is absent (does NOT allocate tables, unlike ensure_l3_entry).
static uint64_t lookup_l3(uint64_t l1_pa, uint64_t va) {
    uint64_t *l1 = table_at(l1_pa);
    const uint64_t l1e = l1[l1_index(va)];
    if ((l1e & 0b11ULL) != kTable) return 0;
    uint64_t *l2 = table_at(l1e & 0x0000FFFFFFFFF000ULL);
    const uint64_t l2e = l2[l2_index(va)];
    if ((l2e & 0b11ULL) != kTable) return 0;
    uint64_t *l3 = table_at(l2e & 0x0000FFFFFFFFF000ULL);
    return l3[l3_index(va)];
}

bool pr2_fork(Esper *parent, Esper *child) {
    if (parent == nullptr || child == nullptr || parent->mm == nullptr) {
        return false;
    }
    // A fork gets a brand-new, independent address space (its own
    // PersonalReality). reality_alloc takes g_esper_lock, so call it OUTSIDE the
    // page-table Pr2Guard to keep the global lock order (g_esper_lock first,
    // then g_pt_lock).
    child->mm = reality_alloc();
    if (child->mm == nullptr) {
        return false; // address-space pool exhausted
    }
    PersonalReality *pm = parent->mm;
    PersonalReality *cm = child->mm;
    bool ok = false;
    {
        Pr2Guard _g; // pr2_create_addr_space below re-enters the guard (no-op)
        if (pr2_create_addr_space(child)) {
            // Copy the VMA list verbatim (file_src pointers are shared read-only
            // ELF image bytes; the caller refcounts those images). brk/mmap
            // bookkeeping is part of the address space, so copy it too.
            cm->vma_count = pm->vma_count;
            for (uint32_t i = 0; i < pm->vma_count; ++i) {
                cm->vmas[i] = pm->vmas[i];
            }
            cm->brk_start = pm->brk_start;
            cm->brk_cur = pm->brk_cur;
            cm->mmap_next = pm->mmap_next;
            // Copy-on-write: share every currently-mapped page between parent and
            // child read-only, bumping its refcount. The first write in either
            // address space takes a permission fault that pr2_handle_fault
            // resolves by copying the page privately. Far cheaper than an eager
            // whole-copy; unmapped pages still fault in independently later.
            ok = true;
            for (uint32_t i = 0; i < pm->vma_count && ok; ++i) {
                Vma &pv = pm->vmas[i];
                if (pv.kind == VmaKind::Free) continue;
                for (uint64_t va = pv.start; va < pv.end; va += kPageSize) {
                    uint64_t *pe = ensure_l3_entry(parent->ttbr0, va);
                    if (pe == nullptr || (*pe & 0b11ULL) != kPage) {
                        continue; // not mapped in parent -> child faults it in later
                    }
                    const uint64_t ppa = *pe & 0x0000FFFFFFFFF000ULL;
                    // Downgrade the parent's mapping to read-only so its next
                    // write also copy-on-writes (preserve exec/UXN bits + PA).
                    const uint64_t attrs_ro = page_attrs(static_cast<uint8_t>(pv.prot & ~kVmaProtW));
                    *pe = ppa | kPage | attrs_ro;
                    // Map the same physical page read-only in the child + share.
                    uint64_t *ce = ensure_l3_entry(child->ttbr0, va);
                    if (ce == nullptr) { ok = false; break; }
                    *ce = ppa | kPage | attrs_ro;
                    page_ref(ppa); // now mapped in two address spaces
                    asm volatile("tlbi vaae1is, %0" ::"r"(va >> 12)); // flush parent's stale RW TLB
                }
            }
            asm volatile("dsb ish; isb" ::: "memory");
        }
    }
    if (!ok) {
        reality_unref(child); // release the half-built space (outside Pr2Guard)
        return false;
    }
    return true;
}

void pr2_destroy(PersonalReality *mm) {
    if (mm == nullptr || mm->ttbr0 == 0) {
        return;
    }
    Pr2Guard _g;
    const uint64_t l1_pa = mm->ttbr0;
    uint64_t *l1 = table_at(l1_pa);
    const uint64_t *kl1 = teleport_kernel_l1();
    constexpr uint64_t kAddrMask = 0x0000FFFFFFFFF000ULL;
    for (uint32_t i = 0; i < kEntriesPerTable; ++i) {
        // Inherited kernel entries are identical to the kernel L1 -- skip them
        // (they map kernel RAM / device space we must not free). Only entries
        // pr2 created for this process differ and point to per-process tables.
        if (l1[i] == kl1[i] || (l1[i] & 0b11ULL) != kTable) {
            continue;
        }
        const uint64_t l2_pa = l1[i] & kAddrMask;
        uint64_t *l2 = table_at(l2_pa);
        for (uint32_t j = 0; j < kEntriesPerTable; ++j) {
            if ((l2[j] & 0b11ULL) != kTable) {
                continue;
            }
            const uint64_t l3_pa = l2[j] & kAddrMask;
            uint64_t *l3 = table_at(l3_pa);
            for (uint32_t k = 0; k < kEntriesPerTable; ++k) {
                if ((l3[k] & 0b11ULL) == kPage) {
                    page_unref(l3[k] & kAddrMask); // free leaf when last sharer drops
                }
            }
            tree_diagram_free_page(l3_pa); // per-process L3 table page
        }
        tree_diagram_free_page(l2_pa); // per-process L2 table page
    }
    tree_diagram_free_page(l1_pa); // the L1 table page itself
    mm->ttbr0 = 0;
}

// Punch a hole [lo, hi) out of the address space: truncate or invalidate any
// VMA that overlaps the range and clear the corresponding L3 page table
// entries. Used by MAP_FIXED to displace an existing mapping (ld-musl's
// PT_LOAD layout: a wide R|X cover from offset 0, then RW MAP_FIXED over the
// tail for .data/.bss). A VMA completely contained in [lo, hi) is freed; one
// that pokes out on exactly one side is truncated. A VMA straddling both
// sides (hole in the middle) loses the tail past `lo` -- a real split would
// need a free VMA slot we may not have, and ld-musl doesn't generate that
// pattern.
void pr2_remove_vma_range(Esper *e, uint64_t lo, uint64_t hi) {
    if (e == nullptr || e->mm == nullptr || hi <= lo) {
        return;
    }
    PersonalReality *mm = e->mm; // munmap operates on the shared address space
    Pr2Guard _g;
    lo &= ~kPageMask;
    hi = (hi + kPageMask) & ~kPageMask;
    for (uint32_t i = 0; i < mm->vma_count; ++i) {
        Vma &v = mm->vmas[i];
        if (v.kind == VmaKind::Free || v.end <= lo || v.start >= hi) {
            continue;
        }
        if (lo <= v.start && v.end <= hi) {
            // Completely covered: free the slot.
            v.kind = VmaKind::Free;
            v.start = v.end = 0;
            v.file_src = nullptr;
            v.file_size = 0;
            v.file_off = 0;
            v.seg_pad = 0;
            v.prot = 0;
        } else if (v.start < lo && v.end <= hi) {
            v.end = lo;                      // truncate tail
        } else if (v.start >= lo && v.end > hi) {
            // truncate head; adjust file_off so file_src still points at the
            // right byte for whatever's left of the segment.
            const uint64_t shift = hi - v.start;
            if (v.kind == VmaKind::File) {
                v.file_off += shift;
                v.file_size = (v.file_size > shift) ? (v.file_size - shift) : 0;
                if (v.seg_pad > shift) v.seg_pad -= shift;
                else                   v.seg_pad = 0;
            }
            v.start = hi;
        } else {
            // Middle hole -- losing the tail past `lo` is the only thing we
            // can do without a free slot to split into.
            v.end = lo;
        }
    }
    // Tear down already-mapped pages so the new mapping's permissions actually
    // apply on next access.
    for (uint64_t p = lo; p < hi; p += kPageSize) {
        const uint64_t pe = lookup_l3(e->ttbr0, p);
        if ((pe & 0b11ULL) != kPage) continue;
        uint64_t *l3e = ensure_l3_entry(e->ttbr0, p);
        if (l3e == nullptr) continue;
        const uint64_t pa = *l3e & 0x0000FFFFFFFFF000ULL;
        *l3e = 0;
        page_unref(pa);
        asm volatile("dsb ish" ::: "memory");
        asm volatile("tlbi vaae1is, %0" ::"r"(p >> 12));
    }
    asm volatile("dsb ish; isb" ::: "memory");
}

void pr2_mprotect(Esper *e, uint64_t va, uint64_t len, uint8_t prot) {
    if (e == nullptr || e->mm == nullptr || len == 0) {
        return;
    }
    PersonalReality *mm = e->mm; // mprotect the shared address space (all threads see it)
    Pr2Guard _g;
    const uint64_t lo = va & ~kPageMask;
    const uint64_t hi = (va + len + kPageMask) & ~kPageMask;
    // Apply the new prot to EXACTLY [lo, hi). A VMA that straddles a boundary is
    // SPLIT so the parts outside the range keep their old prot. The old code did
    // a wholesale `v.prot = prot` on any overlapping VMA -- but GNU_RELRO
    // mprotects just the FRONT of the data segment to RO, which then turned the
    // segment's .bss read-only too. A later write to .bss faulted, pr2_handle_
    // fault re-applied the RO VMA prot and returned "handled", the eret re-ran
    // the store, it faulted again... a silent 100%-CPU permission-fault spin
    // (this is exactly what hung the OpenJDK JVM at thread startup). Splitting
    // matches Linux mprotect, which operates on page ranges, not whole regions.
    const uint32_t n = mm->vma_count;
    for (uint32_t i = 0; i < n; ++i) {
        const Vma orig = mm->vmas[i];
        if (orig.kind == VmaKind::Free || orig.start >= hi || orig.end <= lo) {
            continue;
        }
        if (orig.start >= lo && orig.end <= hi) {
            mm->vmas[i].prot = prot; // fully inside the range -- just change prot
            continue;
        }
        // Straddles a boundary: drop it, re-add up to 3 sub-ranges. For a
        // file-backed VMA each sub-range keeps the same file_src; a sub-range
        // that starts past the file-data origin rebases file_off so demand
        // faults still copy the right bytes (the fault handler reads
        // file_src[file_off + (va - seg_va)]).
        const uint64_t seg_va = orig.start + orig.seg_pad;
        const uint64_t seg_end = seg_va + orig.file_size;
        mm->vmas[i].kind = VmaKind::Free; // remove original (frees the slot for re-add)
        auto readd = [&](uint64_t s, uint64_t en, uint8_t pr) {
            if (en <= s) return;
            if (orig.kind == VmaKind::File && (orig.file_src != nullptr || orig.file_path[0] != '\0')) {
                uint64_t nsv, noff, nsz;
                const uint64_t fe = (seg_end < en) ? seg_end : en; // file-data end in [s,en)
                if (s <= seg_va) { nsv = seg_va; noff = orig.file_off;
                                   nsz = (fe > nsv) ? (fe - nsv) : 0; }
                else { nsv = s; noff = orig.file_off + (s - seg_va);
                       nsz = (fe > s) ? (fe - s) : 0; }
                // Preserve demand-paging: a split file VMA keeps file_src (resident
                // buffer = ELF) OR file_path (demand mmap) -- pass whichever it uses.
                pr2_add_vma(e, s, en, pr, static_cast<uint8_t>(VmaKind::File),
                            orig.file_src, noff, nsz, nsv,
                            orig.file_src != nullptr ? nullptr : orig.file_path);
            } else {
                pr2_add_vma(e, s, en, pr, static_cast<uint8_t>(orig.kind),
                            nullptr, 0, 0, 0);
            }
        };
        if (orig.start < lo) readd(orig.start, lo, orig.prot);                 // before: old prot
        readd(orig.start > lo ? orig.start : lo,
              orig.end < hi ? orig.end : hi, prot);                            // in-range: new prot
        if (orig.end > hi) readd(hi, orig.end, orig.prot);                     // after: old prot
    }
    // Rewrite already-mapped pages in range with the new permission bits.
    for (uint64_t p = lo; p < hi; p += kPageSize) {
        const uint64_t pe = lookup_l3(e->ttbr0, p);
        if ((pe & 0b11ULL) != kPage) {
            continue;
        }
        const uint64_t pa = pe & 0x0000FFFFFFFFF000ULL;
        uint64_t *l3e = ensure_l3_entry(e->ttbr0, p);
        if (l3e == nullptr) continue;
        *l3e = pa | kPage | page_attrs(prot);
        asm volatile("dsb ish" ::: "memory");
        asm volatile("tlbi vaae1is, %0" ::"r"(p >> 12));
        if (prot & kVmaProtX) {
            make_exec_coherent(pa); // broadcast I-cache invalidate (SMP)
        }
    }
    asm volatile("dsb ish; isb" ::: "memory");
}

uint64_t pr2_user_phys(Esper *e, uint64_t va) {
    if (e == nullptr) {
        return 0;
    }
    Pr2Guard _g; // span fault-in + walk so the L3 entry can't change between them
    if (!pr2_handle_fault(e, va)) {
        return 0;
    }
    const uint64_t pe = lookup_l3(e->ttbr0, va & ~kPageMask);
    if ((pe & 0b11ULL) != kPage) {
        return 0;
    }
    return (pe & 0x0000FFFFFFFFF000ULL) | (va & kPageMask);
}

bool pr2_range_free(const Esper *e, uint64_t lo, uint64_t hi) {
    if (e == nullptr || e->mm == nullptr || hi <= lo) {
        return false;
    }
    const PersonalReality *mm = e->mm;
    for (uint32_t i = 0; i < mm->vma_count; ++i) {
        const Vma &v = mm->vmas[i];
        if (v.kind == VmaKind::Free) continue;
        if (lo < v.end && v.start < hi) return false; // overlaps a live VMA
    }
    return true;
}

bool pr2_prefault_range(Esper *e, uint64_t va, uint64_t len) {
    if (e == nullptr || len == 0) {
        return true;
    }
    Pr2Guard _g;
    const uint64_t first = va & ~kPageMask;
    const uint64_t last = (va + len - 1) & ~kPageMask;
    for (uint64_t page = first; page <= last; page += kPageSize) {
        // If the page is already mapped, ensure_l3_entry would write the same
        // value -- harmless. If not, pr2_handle_fault installs it.
        if (!pr2_handle_fault(e, page)) {
            return false;
        }
    }
    return true;
}

bool pr2_handle_fault(Esper *e, uint64_t far) {
    if (e == nullptr) {
        return false;
    }
    Pr2Guard _g;
    const uint64_t page_va = far & ~kPageMask;
    const Vma *v = find_vma(e, page_va);
    if (v == nullptr) {
        return false;
    }
    // Make sure the L1/L2/L3 chain exists, then check the L3 entry: if it's
    // already a valid page we have nothing to do (this happens when
    // pr2_prefault_range walks a buffer whose first byte already faulted in
    // a page that also covers the second byte).
    uint64_t *l3e = ensure_l3_entry(e->ttbr0, page_va);
    if (l3e == nullptr) {
        return false;
    }
    if ((*l3e & 0b11ULL) == kPage) {
        // Page already present -- this is a permission fault. Two cases:
        //  - Copy-on-write: the VMA is writable but the page is a shared (RO)
        //    CoW page (refcount > 1, left by fork). Copy it into a private page
        //    so the write doesn't disturb the other address space.
        //  - Otherwise just re-derive the entry's permission bits from the
        //    VMA's current prot (e.g. mprotect raised it), so the access retries.
        const uint64_t pa = *l3e & 0x0000FFFFFFFFF000ULL;
        const bool want_write = (v->prot & kVmaProtW) != 0;
        if (want_write && page_refcount(pa) > 1) {
            const uint64_t npa = alloc_page_zeroed();
            if (npa == 0) {
                return false;
            }
            const uint8_t *src = reinterpret_cast<const uint8_t *>(teleport_high_alias(pa));
            uint8_t *dst = reinterpret_cast<uint8_t *>(teleport_high_alias(npa));
            for (uint64_t b = 0; b < kPageSize; ++b) dst[b] = src[b];
            page_unref(pa);  // drop our share of the old page
            page_ref(npa);   // own the fresh private copy
            *l3e = npa | kPage | page_attrs(v->prot);
            if (v->prot & kVmaProtX) {
                make_exec_coherent(npa); // broadcast I-cache invalidate (SMP)
            }
        } else {
            *l3e = pa | kPage | page_attrs(v->prot);
        }
        asm volatile("dsb ish" ::: "memory");
        asm volatile("tlbi vaae1is, %0" ::"r"(page_va >> 12));
        asm volatile("dsb ish; isb" ::: "memory");
        return true;
    }
    const uint64_t page_pa = alloc_page_zeroed();
    if (page_pa == 0) {
        return false;
    }
    // Fill the page. Anon/Stack/Brk are already zero from alloc_page_zeroed.
    // File-backed: the segment's content lives in [seg_va, seg_va + file_size)
    // where seg_va = v->start + v->seg_pad. Compute the overlap of that range
    // with this 4 KiB page and memcpy just that slice; everything else in the
    // page stays zero (the .bss tail and any pre-segment padding from
    // page-aligning v->start down).
    if (v->kind == VmaKind::File && (v->file_src != nullptr || v->file_path[0] != '\0')) {
        const uint64_t seg_va = v->start + v->seg_pad;
        const uint64_t seg_end = seg_va + v->file_size;
        const uint64_t pg_lo = page_va;
        const uint64_t pg_hi = page_va + kPageSize;
        const uint64_t lo = pg_lo > seg_va ? pg_lo : seg_va;
        const uint64_t hi = pg_hi < seg_end ? pg_hi : seg_end;
        if (lo < hi) {
            const uint64_t dst_off = lo - pg_lo;
            const uint64_t src_off = v->file_off + (lo - seg_va);
            const uint64_t copy = hi - lo;
            auto *dst = reinterpret_cast<uint8_t *>(teleport_high_alias(page_pa));
            if (v->file_src != nullptr) {
                const uint8_t *src = v->file_src + src_off; // resident buffer (ELF segments)
                for (uint64_t i = 0; i < copy; ++i) dst[dst_off + i] = src[i];
            } else {
                // Demand-paged file mmap: read this page's slice straight from the
                // filesystem (no whole-file kernel buffer). Bytes not read stay
                // zero from alloc_page_zeroed (.bss tail / short read at EOF).
                lateran_pread(v->file_path, src_off,
                              reinterpret_cast<char *>(dst + dst_off), static_cast<uint32_t>(copy));
            }
        }
        // Executable page just written via the data side: make it coherent for
        // the instruction fetcher on every PE (broadcast -- see the helper).
        if (v->prot & kVmaProtX) {
            make_exec_coherent(page_pa);
        }
    }
    *l3e = page_pa | kPage | page_attrs(v->prot);
    page_ref(page_pa); // first mapping of this fresh page
    // Make sure the table write is visible and the stale TLB entry (if any)
    // is gone before we return to EL0.
    asm volatile("dsb ish" ::: "memory");
    asm volatile("tlbi vaae1is, %0" ::"r"(page_va >> 12));
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");
    return true;
}

// [WD] Walk `l1_pa`'s tables for `va` and print each level's descriptor. PA-
// bounds-checked at every step (RAM is [0x40000000, 0x80000000) on this board)
// so a corrupted descriptor can't make the dumper itself fault. Called from the
// fatal EL0 fault path to expose page-table corruption (e.g. a level-1 perm
// fault whose L1 entry is garbage / a reused page = use-after-free).
void pr2_dump_walk(uint64_t l1_pa, uint64_t va) {
    namespace d = imaginary_number_district;
    auto in_ram = [](uint64_t pa) {
        return pa >= 0x40000000ULL && pa < 0x80000000ULL;
    };
    d::write("[pr2 walk] ttbr0="); d::hex(l1_pa);
    d::write(" va="); d::hex(va);
    if (!in_ram(l1_pa)) { d::writeln(" (L1 PA out of RAM!)"); return; }
    const uint64_t l1e = table_at(l1_pa)[l1_index(va)];
    d::write(" | L1["); d::dec(l1_index(va)); d::write("]="); d::hex(l1e);
    if ((l1e & 0b11ULL) != kTable) { d::writeln(" (L1 not a table desc)"); return; }
    const uint64_t l2_pa = l1e & 0x0000FFFFFFFFF000ULL;
    if (!in_ram(l2_pa)) { d::writeln(" (L2 PA out of RAM!)"); return; }
    const uint64_t l2e = table_at(l2_pa)[l2_index(va)];
    d::write(" | L2["); d::dec(l2_index(va)); d::write("]="); d::hex(l2e);
    if ((l2e & 0b11ULL) != kTable) { d::writeln(" (L2 not a table desc)"); return; }
    const uint64_t l3_pa = l2e & 0x0000FFFFFFFFF000ULL;
    if (!in_ram(l3_pa)) { d::writeln(" (L3 PA out of RAM!)"); return; }
    const uint64_t l3e = table_at(l3_pa)[l3_index(va)];
    d::write(" | L3["); d::dec(l3_index(va)); d::write("]="); d::hex(l3e);
    d::writeln("");
}

// [JVMDIAG] dump ALL of e's VMAs (capped) -- to see if a thread's VMA list is
// missing a region another thread mapped (CLONE_VM copies the list, shares the
// page tables -> divergence).
void pr2_dump_all_vmas(Esper *e) {
    namespace d = imaginary_number_district;
    if (e == nullptr || e->mm == nullptr) { d::writeln("[pr2 vmas] (no mm)"); return; }
    const PersonalReality *mm = e->mm; // the shared address-space map
    d::write("[pr2 vmas] count="); d::dec(mm->vma_count);
    d::write(" is_thread="); d::dec(static_cast<uint64_t>(e->is_thread ? 1 : 0));
    d::writeln("");
    uint32_t shown = 0;
    for (uint32_t i = 0; i < mm->vma_count && shown < 40; ++i) {
        const Vma &v = mm->vmas[i];
        if (v.kind == VmaKind::Free) continue;
        ++shown;
        d::write("  ["); d::hex(v.start); d::write(","); d::hex(v.end);
        d::write(") p="); d::dec(static_cast<uint64_t>(v.prot));
        d::write(" k="); d::dec(static_cast<uint64_t>(v.kind)); d::writeln("");
    }
}

// [JVMDIAG] dump the VMA covering `va` (prot/kind/range) -- to see whether a
// faulting write hit a VMA that is read-only by design or wrongly mapped.
void pr2_dump_vma(Esper *e, uint64_t va) {
    namespace d = imaginary_number_district;
    const Vma *v = find_vma(e, va & ~kPageMask);
    if (v == nullptr) { d::write("[pr2 vma] none for "); d::hex(va); d::writeln(""); return; }
    d::write("[pr2 vma] va="); d::hex(va);
    d::write(" start="); d::hex(v->start); d::write(" end="); d::hex(v->end);
    d::write(" prot="); d::dec(static_cast<uint64_t>(v->prot));
    d::write("(R"); d::dec((v->prot & kVmaProtR) ? 1 : 0);
    d::write("W"); d::dec((v->prot & kVmaProtW) ? 1 : 0);
    d::write("X"); d::dec((v->prot & kVmaProtX) ? 1 : 0); d::write(")");
    d::write(" kind="); d::dec(static_cast<uint64_t>(v->kind));
    d::write(" filesz="); d::hex(v->file_size); d::writeln("");
}

} // namespace index
