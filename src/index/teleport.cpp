#include "index/teleport.hpp"

#include "index/mmio.hpp"
#include "index/types.hpp"

namespace index::mmio {
// While the MMU is off this is 0 (device addresses are physical); enabling the
// MMU switches it to the high-half base so MMIO uses the TTBR1 device mapping.
uint64_t g_mmio_offset = 0;
} // namespace index::mmio

// Section boundaries from linker.ld (all 4 KiB aligned).
extern "C" char __text_start[];
extern "C" char __text_end[];
extern "C" char __rodata_start[];
extern "C" char __rodata_end[];
extern "C" char __data_start[];
extern "C" char __image_end[];
extern "C" char __user_text_start[];
extern "C" char __user_text_end[];
extern "C" char __user_stack_start[];
extern "C" char __user_stack_end[];

namespace index {

namespace {

constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint32_t kEntries = 512;

// Descriptor type bits[1:0].
constexpr uint64_t kTable = 0b11ULL; // table descriptor (points to next level)
constexpr uint64_t kBlock = 0b01ULL; // block at level 1/2
constexpr uint64_t kPage = 0b11ULL;  // page at level 3

// Lower attributes.
constexpr uint64_t kAttrDevice = 0ULL << 2; // MAIR index 0
constexpr uint64_t kAttrNormal = 1ULL << 2; // MAIR index 1
constexpr uint64_t kApRw = 0ULL << 6;        // EL1 read/write, EL0 none
constexpr uint64_t kApRwBoth = 1ULL << 6;    // EL1 + EL0 read/write
constexpr uint64_t kApRo = 2ULL << 6;        // EL1 read-only, EL0 none
constexpr uint64_t kApRoBoth = 3ULL << 6;    // EL1 + EL0 read-only
constexpr uint64_t kShInner = 3ULL << 8;     // inner shareable (normal)
constexpr uint64_t kShOuter = 2ULL << 8;     // device (shareability ignored)
constexpr uint64_t kAf = 1ULL << 10;         // access flag

// Upper attributes.
constexpr uint64_t kPxn = 1ULL << 53; // privileged execute-never
constexpr uint64_t kUxn = 1ULL << 54; // unprivileged execute-never

// Composite attribute sets for the kernel image regions.
constexpr uint64_t kNormalRwNx = kAttrNormal | kApRw | kShInner | kAf | kPxn | kUxn;
constexpr uint64_t kNormalRoX = kAttrNormal | kApRo | kShInner | kAf | kUxn; // exec at EL1
constexpr uint64_t kNormalRoNx = kAttrNormal | kApRo | kShInner | kAf | kPxn | kUxn;
constexpr uint64_t kDeviceRwNx = kAttrDevice | kApRw | kShOuter | kAf | kPxn | kUxn;
// EL0 user pages: code is readable+executable at EL0 (but not executable at
// EL1, PXN), data/stack is read/write at EL0 and never executable.
constexpr uint64_t kUserRoX = kAttrNormal | kApRoBoth | kShInner | kAf | kPxn;
constexpr uint64_t kUserRwNx = kAttrNormal | kApRwBoth | kShInner | kAf | kPxn | kUxn;

// One L1 (1 GiB/entry), one L2 for the kernel's GiB (2 MiB/entry), one L3 for
// the kernel's first 2 MiB (4 KiB/entry). Everything else stays in big blocks.
alignas(4096) uint64_t g_l1[kEntries];
alignas(4096) uint64_t g_l2[kEntries]; // covers 0x40000000..0x80000000
alignas(4096) uint64_t g_l3[kEntries]; // covers 0x40000000..0x40200000

Teleport g_status;

uint32_t parange_to_bits(uint64_t field) {
    switch (field & 0xf) {
    case 0: return 32;
    case 1: return 36;
    case 2: return 40;
    case 3: return 42;
    case 4: return 44;
    case 5: return 48;
    default: return 48;
    }
}

uint64_t addr(const void *p) {
    return reinterpret_cast<uint64_t>(p);
}

// Per-page attributes for a physical page inside the kernel's first 2 MiB.
uint64_t kernel_page_attrs(uint64_t pa) {
    if (pa >= addr(__user_text_start) && pa < addr(__user_text_end)) {
        return kUserRoX;     // EL0 code: user read/exec, kernel cannot execute
    }
    if (pa >= addr(__user_stack_start) && pa < addr(__user_stack_end)) {
        return kUserRwNx;    // EL0 stack: user read/write, no-execute
    }
    if (pa >= addr(__text_start) && pa < addr(__text_end)) {
        return kNormalRoX;   // code: read-only, executable
    }
    if (pa >= addr(__rodata_start) && pa < addr(__rodata_end)) {
        return kNormalRoNx;  // constants: read-only, no-execute
    }
    if (pa >= addr(__data_start) && pa < addr(__image_end)) {
        return kNormalRwNx;  // data/bss/stack: read-write, no-execute
    }
    return kNormalRwNx;      // slack below/around the image
}

// The MMU register profile the boot core computes once. Secondaries replay the
// exact same values so every core shares one set of page tables. Aligned to a
// cache line and cleaned to PoC (see teleport_enable) so a secondary -- which
// reads this with its MMU still off, i.e. non-cacheable -- sees RAM, not a stale
// line sitting in the boot core's write-back cache.
struct SavedMmuRegs {
    uint64_t mair = 0;
    uint64_t tcr = 0;
    uint64_t ttbr = 0; // physical address of g_l1
    bool valid = false;
};
alignas(64) SavedMmuRegs g_saved;

// Program MAIR/TCR/TTBR0/TTBR1 and switch the MMU on (M + C + I). Identical on
// the boot core and every secondary; the page tables themselves are built once.
void apply_mmu_regs(uint64_t mair, uint64_t tcr, uint64_t ttbr) {
    asm volatile("dsb ish" ::: "memory");
    asm volatile("ic iallu" ::: "memory");
    asm volatile("dsb ish" ::: "memory");
    asm volatile("msr mair_el1, %0" ::"r"(mair));
    asm volatile("msr tcr_el1, %0" ::"r"(tcr));
    asm volatile("msr ttbr0_el1, %0" ::"r"(ttbr));
    asm volatile("msr ttbr1_el1, %0" ::"r"(ttbr));
    asm volatile("isb");
    asm volatile("tlbi vmalle1");
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");

    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0);   // M
    sctlr |= (1ULL << 2);   // C
    sctlr |= (1ULL << 12);  // I
    // EL0-enable bits Linux sets so userspace can read cache geometry + do its
    // own cache maintenance without trapping to EL1 (EC 0x18). The OpenJDK JVM
    // reads CTR_EL0 (cache line sizes) during CPU feature detection and uses
    // dc cvau / ic ivau to flush its JIT code cache; without these it took a
    // trapped-MSR EL0 fault and died.
    sctlr |= (1ULL << 14);  // DZE  -- EL0 `dc zva`
    sctlr |= (1ULL << 15);  // UCT  -- EL0 read of CTR_EL0
    sctlr |= (1ULL << 26);  // UCI  -- EL0 dc cvau/civac/cvac + ic ivau
    asm volatile("msr sctlr_el1, %0" ::"r"(sctlr));
    asm volatile("isb");
}

} // namespace

Teleport teleport_enable(const ArtificialHeaven &heaven) {
    Teleport t;

    uint64_t ram_top = 0;
    for (uint32_t i = 0; i < heaven.memory_count; ++i) {
        const uint64_t end = heaven.memory[i].base + heaven.memory[i].size;
        if (end > ram_top) {
            ram_top = end;
        }
    }
    if (ram_top == 0) {
        ram_top = 2 * kGiB;
    }

    uint32_t blocks = static_cast<uint32_t>(align_up(ram_top, kGiB) / kGiB);
    if (blocks < 2) {
        blocks = 2;
    }
    if (blocks > kEntries) {
        blocks = kEntries;
    }

    for (uint32_t i = 0; i < kEntries; ++i) {
        g_l1[i] = 0;
        g_l2[i] = 0;
        g_l3[i] = 0;
    }

    // L3: the kernel's first 2 MiB, 4 KiB pages with W^X permissions.
    for (uint32_t i = 0; i < kEntries; ++i) {
        const uint64_t pa = 0x40000000ULL + static_cast<uint64_t>(i) * 4096ULL;
        g_l3[i] = pa | kPage | kernel_page_attrs(pa);
    }

    // L2 (covers 0x40000000..0x80000000): entry 0 -> the L3 table above, the
    // rest are 2 MiB normal RW/NX blocks for the remainder of RAM in this GiB.
    g_l2[0] = addr(g_l3) | kTable;
    for (uint32_t i = 1; i < kEntries; ++i) {
        const uint64_t pa = 0x40000000ULL + static_cast<uint64_t>(i) * 2ULL * kMiB;
        g_l2[i] = pa | kBlock | kNormalRwNx;
    }

    // L1: entry 0 = low MMIO (device), entry 1 = the kernel's GiB via L2,
    // entries 2.. = remaining RAM as 1 GiB normal RW/NX blocks.
    g_l1[0] = 0ULL | kBlock | kDeviceRwNx;
    g_l1[1] = addr(g_l2) | kTable;
    for (uint32_t i = 2; i < blocks; ++i) {
        const uint64_t pa = static_cast<uint64_t>(i) * kGiB;
        g_l1[i] = pa | kBlock | kNormalRwNx;
    }

    // Map the GiB containing the PCIe ECAM config space as device memory, so the
    // Underground bus driver can reach it. On `virt` the ECAM is at 0x4010000000
    // (L1 entry 256), far above RAM, so it gets its own device block. (The PCIe
    // MMIO/BAR window at 0x10000000 already falls inside entry 0's device block.)
    if (heaven.pcie_ecam != 0) {
        const uint32_t idx = static_cast<uint32_t>(heaven.pcie_ecam / kGiB);
        if (idx < kEntries && g_l1[idx] == 0) {
            g_l1[idx] = (static_cast<uint64_t>(idx) * kGiB) | kBlock | kDeviceRwNx;
        }
    }

    const uint64_t mair = (0xFFULL << 8) | 0x00ULL; // Attr1 Normal-WB, Attr0 Device

    uint64_t mmfr0 = 0;
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    const uint32_t pa_bits = parange_to_bits(mmfr0);
    uint64_t ips = mmfr0 & 0xf;
    if (ips > 5) {
        ips = 5;
    }

    // TTBR0 covers the low (identity) half, TTBR1 the high (kernel) half. Both
    // use 39-bit regions, so VA[38:0] indexes the same table -> one g_l1 serves
    // both: physical P is reachable at P (TTBR0) and at kHighHalfBase|P (TTBR1).
    const uint64_t t0sz = 25; // 39-bit VA for TTBR0 (identity alias / device)
    const uint64_t t1sz = 25; // 39-bit VA for TTBR1 (kernel high half)
    const uint64_t tcr =
        t0sz |
        (1ULL << 8)  |  // IRGN0 = write-back, write-allocate
        (1ULL << 10) |  // ORGN0 = write-back, write-allocate
        (3ULL << 12) |  // SH0   = inner shareable
        (0ULL << 14) |  // TG0   = 4 KiB granule (TTBR0)
        (t1sz << 16) |  // T1SZ
        (1ULL << 24) |  // IRGN1 = write-back, write-allocate
        (1ULL << 26) |  // ORGN1 = write-back, write-allocate
        (3ULL << 28) |  // SH1   = inner shareable
        (2ULL << 30) |  // TG1   = 4 KiB granule (TTBR1 encoding: 0b10)
        (ips << 32);    // EPD1 left 0 so TTBR1 walks are enabled

    const uint64_t ttbr = addr(g_l1);

    g_saved.mair = mair;
    g_saved.tcr = tcr;
    g_saved.ttbr = ttbr;
    g_saved.valid = true;

    apply_mmu_regs(mair, tcr, ttbr);

    // Push g_saved out to RAM (PoC) so a secondary core can read its MMU profile
    // while its own MMU is still off (and therefore reads non-cacheably).
    asm volatile("dc cvac, %0" ::"r"(&g_saved) : "memory");
    asm volatile("dsb ish" ::: "memory");

    // MMU is on: device MMIO now goes through the TTBR1 high-half alias, so it
    // keeps working once TTBR0 holds a per-process user address space.
    index::mmio::g_mmio_offset = kHighHalfBase;

    t.enabled = true;
    t.va_bits = 64 - static_cast<uint32_t>(t0sz);
    t.pa_bits = pa_bits;
    t.blocks = blocks;
    t.mapped_top = static_cast<uint64_t>(blocks) * kGiB;
    t.wx_protected = true;

    g_status = t;
    return t;
}

bool teleport_enable_secondary() {
    if (!g_saved.valid) {
        return false; // boot core never built the tables
    }
    // Reuse the boot core's page tables (already inner-shareable) and register
    // profile; this core just points its MMU at them and switches on.
    apply_mmu_regs(g_saved.mair, g_saved.tcr, g_saved.ttbr);
    return true;
}

const Teleport &teleport_status() {
    return g_status;
}

uint64_t teleport_kernel_ttbr0() {
    return reinterpret_cast<uint64_t>(g_l1) & ~kHighHalfBase;
}

// Unmangled accessor so leave_user's asm can reinstate the kernel page table
// before returning into the scheduler/idle loop (see user_switch.S).
extern "C" uint64_t kernel_ttbr0_phys() {
    return reinterpret_cast<uint64_t>(g_l1) & ~kHighHalfBase;
}

const uint64_t *teleport_kernel_l1() {
    return g_l1;
}

} // namespace index
