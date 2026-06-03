#include "index/personal_reality.hpp"

#include "index/esper.hpp"
#include "index/teleport.hpp"

namespace index {

namespace {

constexpr uint64_t kPageSize = 4096;
constexpr uint32_t kEntriesPerTable = 512;
constexpr uint32_t kPagesPerProc = 14; // L1 + L2 + L3 + up to 8 code + 4 stack
constexpr uint32_t kTableBase = 3;     // pool pages 0,1,2 are L1,L2,L3
constexpr uint32_t kMaxCodePages = 7;
constexpr uint32_t kStackPages = 4;

// Descriptor bits (4 KiB granule), matching Teleport's MAIR (index 1 = Normal).
constexpr uint64_t kTable = 0b11ULL;
constexpr uint64_t kPage = 0b11ULL;
constexpr uint64_t kAttrNormal = 1ULL << 2;
constexpr uint64_t kApRwBoth = 1ULL << 6; // EL1 + EL0 read/write
constexpr uint64_t kShInner = 3ULL << 8;
constexpr uint64_t kAf = 1ULL << 10;
constexpr uint64_t kPxn = 1ULL << 53;
constexpr uint64_t kUxn = 1ULL << 54;

// EL0 code: read/write/execute at EL0 (kernel loads it, EL0 runs it); EL1 cannot
// execute it. EL0 stack: read/write, never execute.
constexpr uint64_t kUserCodeAttr = kAttrNormal | kApRwBoth | kShInner | kAf | kPxn;
constexpr uint64_t kUserStackAttr = kUserCodeAttr | kUxn;

// One 4 KiB-aligned page pool, partitioned per Esper slot. Pool pages live in
// kernel .bss: the kernel writes them via their (high-half) VA, while the
// process sees them at low VAs through its private TTBR0.
alignas(4096) uint8_t g_pool[kMaxEspers * kPagesPerProc][kPageSize];

uint8_t *pool_va(uint32_t slot, uint32_t page) {
    return g_pool[slot * kPagesPerProc + page];
}

uint64_t to_phys(const void *va) {
    return reinterpret_cast<uint64_t>(va) & ~kHighHalfBase;
}

uint64_t *as_table(uint8_t *page) {
    return reinterpret_cast<uint64_t *>(page);
}

void zero_page(uint8_t *p) {
    for (uint32_t i = 0; i < kPageSize; ++i) {
        p[i] = 0;
    }
}

} // namespace

PersonalReality personal_reality_build(uint32_t slot, const uint8_t *image,
                                       uint64_t image_size) {
    PersonalReality pr;
    if (slot >= kMaxEspers) {
        return pr;
    }
    const uint32_t code_pages =
        static_cast<uint32_t>((image_size + kPageSize - 1) / kPageSize);
    if (code_pages > kMaxCodePages) {
        return pr; // image too large for a slot
    }

    uint8_t *l1 = pool_va(slot, 0);
    uint8_t *l2 = pool_va(slot, 1);
    uint8_t *l3 = pool_va(slot, 2);
    zero_page(l2);
    zero_page(l3);
    // Inherit the kernel's mappings (so the kernel's own low-VA stack/data stay
    // valid at EL1 while this TTBR0 is active), then override entry 0 with this
    // process's private low 1 GiB. Kernel entries are EL1-only, so EL0 cannot
    // reach them -- the process is still isolated.
    const uint64_t *kl1 = teleport_kernel_l1();
    for (uint32_t i = 0; i < kEntriesPerTable; ++i) {
        as_table(l1)[i] = kl1[i];
    }
    as_table(l1)[0] = to_phys(l2) | kTable; // VA 0..1GiB -> private L2
    as_table(l2)[0] = to_phys(l3) | kTable; // VA 0..2MiB -> L3

    // Map the program image at kUserCodeBase, copying it into private pages.
    for (uint32_t i = 0; i < code_pages; ++i) {
        uint8_t *page = pool_va(slot, kTableBase + i);
        zero_page(page);
        const uint64_t off = static_cast<uint64_t>(i) * kPageSize;
        for (uint64_t b = 0; b < kPageSize && off + b < image_size; ++b) {
            page[b] = image[off + b];
        }
        const uint64_t va = kUserCodeBase + off;
        as_table(l3)[(va >> 12) & 0x1ff] = to_phys(page) | kPage | kUserCodeAttr;
        if (i == 0) {
            pr.code_phys = to_phys(page);
        }
    }

    // Map the stack just below kUserStackTop.
    for (uint32_t i = 0; i < kStackPages; ++i) {
        uint8_t *page = pool_va(slot, kTableBase + code_pages + i);
        zero_page(page);
        const uint64_t va = kUserStackTop - static_cast<uint64_t>(kStackPages - i) * kPageSize;
        as_table(l3)[(va >> 12) & 0x1ff] = to_phys(page) | kPage | kUserStackAttr;
    }

    // The code pages were written through the data side; make them executable.
    asm volatile("dsb ish" ::: "memory");
    asm volatile("ic iallu" ::: "memory");
    asm volatile("dsb ish; isb" ::: "memory");

    pr.ttbr0 = to_phys(l1);
    pr.code_pages = code_pages;
    pr.valid = true;
    return pr;
}

PersonalReality personal_reality_fork(uint32_t parent_slot, uint32_t child_slot,
                                      uint32_t code_pages) {
    PersonalReality pr;
    if (parent_slot >= kMaxEspers || child_slot >= kMaxEspers ||
        code_pages > kMaxCodePages) {
        return pr;
    }

    uint8_t *l1 = pool_va(child_slot, 0);
    uint8_t *l2 = pool_va(child_slot, 1);
    uint8_t *l3 = pool_va(child_slot, 2);
    zero_page(l2);
    zero_page(l3);
    const uint64_t *kl1 = teleport_kernel_l1();
    for (uint32_t i = 0; i < kEntriesPerTable; ++i) {
        as_table(l1)[i] = kl1[i];
    }
    as_table(l1)[0] = to_phys(l2) | kTable;
    as_table(l2)[0] = to_phys(l3) | kTable;

    // Copy the parent's code pages into the child's, mapped at the same VAs.
    for (uint32_t i = 0; i < code_pages; ++i) {
        uint8_t *src = pool_va(parent_slot, kTableBase + i);
        uint8_t *dst = pool_va(child_slot, kTableBase + i);
        for (uint32_t b = 0; b < kPageSize; ++b) {
            dst[b] = src[b];
        }
        const uint64_t va = kUserCodeBase + static_cast<uint64_t>(i) * kPageSize;
        as_table(l3)[(va >> 12) & 0x1ff] = to_phys(dst) | kPage | kUserCodeAttr;
        if (i == 0) {
            pr.code_phys = to_phys(dst);
        }
    }

    // Copy the parent's stack pages the same way.
    for (uint32_t i = 0; i < kStackPages; ++i) {
        uint8_t *src = pool_va(parent_slot, kTableBase + code_pages + i);
        uint8_t *dst = pool_va(child_slot, kTableBase + code_pages + i);
        for (uint32_t b = 0; b < kPageSize; ++b) {
            dst[b] = src[b];
        }
        const uint64_t va = kUserStackTop - static_cast<uint64_t>(kStackPages - i) * kPageSize;
        as_table(l3)[(va >> 12) & 0x1ff] = to_phys(dst) | kPage | kUserStackAttr;
    }

    asm volatile("dsb ish" ::: "memory");
    asm volatile("ic iallu" ::: "memory");
    asm volatile("dsb ish; isb" ::: "memory");

    pr.ttbr0 = to_phys(l1);
    pr.code_pages = code_pages;
    pr.valid = true;
    return pr;
}

} // namespace index
