#include "drivers/aleister.hpp"

#include "arch/aarch64/cpu.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/mmio.hpp"

namespace index::drivers {

namespace {

// --- GICv2 distributor (GICD) register offsets. -----------------------------
constexpr uint64_t kGicdCtlr = 0x000;
constexpr uint64_t kGicdIsenabler = 0x100; // +4*(intid/32)
constexpr uint64_t kGicdIpriorityr = 0x400; // +intid (one byte each)
constexpr uint64_t kGicdItargetsr = 0x800;  // +intid (one byte each), v2 only

// --- GICv2 CPU interface (GICC) register offsets. ---------------------------
constexpr uint64_t kGiccCtlr = 0x000;
constexpr uint64_t kGiccPmr = 0x004;
constexpr uint64_t kGiccIar = 0x00C;
constexpr uint64_t kGiccEoir = 0x010;

// --- GICv3 distributor extras (the GICD above is shared). --------------------
// GICD_CTLR for GICv3 in a single security state: enable affinity routing
// (ARE_NS, bit 4) plus Group1NS (bit 1) and Group0 (bit 0).
// GICD_CTLR bits: 0=EnableGrp0, 1=EnableGrp1NS, 4=ARE_S, 5=ARE_NS (when DS=0;
// when DS=1, bit 4 is the single ARE). We're in NS world via HVF -- if DS=0
// (the QEMU/HVF default) we need bit 5 set or GICD_IROUTER writes are ignored
// and SPIs (PL011 RX = 33) never route to any PE. Set both 4 and 5 to be
// robust across DS modes. Without bit 5: login console reads sleep forever on
// any Apple-Silicon HVF run (manifested as "can't type at login prompt under
// SMP" -- single-core GICv2 path used ITARGETSR which doesn't need ARE).
constexpr uint32_t kGicdCtlrAreG1G0 = (1u << 5) | (1u << 4) | (1u << 1) | (1u << 0);

// --- GICv3 redistributor (GICR) layout. -------------------------------------
// Each core owns a 128 KiB redistributor: a 64 KiB RD_base frame followed by a
// 64 KiB SGI_base frame. QEMU virt lays the per-core frames out contiguously
// from the region base in MPIDR-affinity order (which equals our boot order).
constexpr uint64_t kGicrStride = 0x20000;    // 128 KiB per core (RD + SGI)
constexpr uint64_t kGicrSgiOffset = 0x10000; // SGI_base = RD_base + 64 KiB
constexpr uint64_t kGicrWaker = 0x0014;      // in RD_base
constexpr uint64_t kGicrIgroupr0 = 0x0080;   // in SGI_base
constexpr uint64_t kGicrIsenabler0 = 0x0100; // in SGI_base
constexpr uint64_t kGicrIpriorityr = 0x0400; // in SGI_base, +intid
constexpr uint32_t kGicrWakerProcessorSleep = 1u << 1;
constexpr uint32_t kGicrWakerChildrenAsleep = 1u << 2;

constexpr uint32_t kSpiBase = 32;    // SGIs 0..15, PPIs 16..31, SPIs 32+
constexpr uint32_t kSpuriousFloor = 1020;
constexpr uint32_t kMaxHandlers = 64;

// --- GICv3 CPU interface system registers (ICC_*_EL1). ----------------------
// Accessed via MSR/MRS, not MMIO. Encodings spelled out so the freestanding
// toolchain assembles them without needing -march=...+gic.
inline uint64_t read_icc_sre() {
    uint64_t v;
    asm volatile("mrs %0, S3_0_C12_C12_5" : "=r"(v)); // ICC_SRE_EL1
    return v;
}
inline void write_icc_sre(uint64_t v) {
    asm volatile("msr S3_0_C12_C12_5, %0" ::"r"(v)); // ICC_SRE_EL1
}
inline void write_icc_pmr(uint64_t v) {
    asm volatile("msr S3_0_C4_C6_0, %0" ::"r"(v)); // ICC_PMR_EL1
}
inline void write_icc_igrpen1(uint64_t v) {
    asm volatile("msr S3_0_C12_C12_7, %0" ::"r"(v)); // ICC_IGRPEN1_EL1
}
inline uint64_t read_icc_iar1() {
    uint64_t v;
    asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v)); // ICC_IAR1_EL1
    return v;
}
inline void write_icc_eoir1(uint64_t v) {
    asm volatile("msr S3_0_C12_C12_1, %0" ::"r"(v)); // ICC_EOIR1_EL1
}
inline void write_icc_sgi1r(uint64_t v) {
    asm volatile("msr S3_0_C12_C11_5, %0" ::"r"(v)); // ICC_SGI1R_EL1
    asm volatile("isb");
}

Aleister g_gic;
IrqHandler g_handlers[kMaxHandlers] = {};
void *g_contexts[kMaxHandlers] = {};
void (*g_after_eoi)(uint64_t *frame) = nullptr;

// This core's GICR RD_base. We bring cores up one at a time in boot order, so a
// simple monotonically-increasing index tracks which redistributor frame is
// "the calling core's". (No IPIs/affinity routing means we never need to map an
// arbitrary MPIDR to a frame.)
uint32_t g_gicr_next_index = 0;

uint64_t this_core_gicr_rd() {
    return g_gic.gicr + static_cast<uint64_t>(g_gicr_next_index) * kGicrStride;
}

// --- GICv3 per-core bring-up. -----------------------------------------------
void gicv3_wake_redistributor(uint64_t rd_base) {
    // Clear ProcessorSleep, then wait for the redistributor to report it is no
    // longer asleep (ChildrenAsleep == 0) before touching SGI/PPI registers.
    uint32_t waker = mmio::read32(rd_base + kGicrWaker);
    waker &= ~kGicrWakerProcessorSleep;
    mmio::write32(rd_base + kGicrWaker, waker);
    for (uint32_t spin = 0; spin < 1000000; ++spin) {
        if ((mmio::read32(rd_base + kGicrWaker) & kGicrWakerChildrenAsleep) == 0) {
            break;
        }
    }
}

void gicv3_init_cpu_interface() {
    const uint64_t rd_base = this_core_gicr_rd();
    gicv3_wake_redistributor(rd_base);

    // Default all SGIs/PPIs on this core to Group 1 (NS) so Group1 delivery
    // through ICC_IAR1 sees them.
    const uint64_t sgi_base = rd_base + kGicrSgiOffset;
    mmio::write32(sgi_base + kGicrIgroupr0, 0xFFFFFFFFu);
    // Enable all 16 SGIs on this core's redistributor so reschedule IPIs are
    // delivered (ISENABLER0 low 16 bits = SGI 0..15). Priority left at reset
    // (0 = highest); PMR=0xF0 admits it.
    mmio::write32(sgi_base + kGicrIsenabler0, 0x0000FFFFu);

    // Enable the system-register CPU interface (ICC_SRE_EL1.SRE = bit 0), then
    // unmask all priorities and enable Group 1 interrupt delivery.
    write_icc_sre(read_icc_sre() | 1u);
    arch::isb();
    write_icc_pmr(0xF0);
    write_icc_igrpen1(1);
    arch::isb();
}

} // namespace

void aleister_init(const Aleister &gic) {
    g_gic = gic;
    if (!gic.available) {
        return;
    }

    if (gic.version == GicVersion::v3) {
        mmio::write32(gic.gicd + kGicdCtlr, kGicdCtlrAreG1G0); // ARE + groups
        gicv3_init_cpu_interface();                            // boot core
        return;
    }

    mmio::write32(gic.gicd + kGicdCtlr, 1); // enable the distributor (global)
    aleister_init_cpu_interface();          // this core's CPU interface
}

void aleister_init_cpu_interface() {
    if (!g_gic.available) {
        return;
    }
    if (g_gic.version == GicVersion::v3) {
        // A secondary core adopts the next redistributor frame in boot order.
        ++g_gicr_next_index;
        gicv3_init_cpu_interface();
        return;
    }
    // GICv2 GICC is banked per-core: each core writes the same MMIO base to
    // configure its own CPU interface. The distributor stays as boot left it.
    mmio::write32(g_gic.gicc + kGiccPmr, 0xF0); // unmask every priority level
    mmio::write32(g_gic.gicc + kGiccCtlr, 1);   // enable the CPU interface
}

void aleister_enable(uint32_t intid, uint8_t priority) {
    if (!g_gic.available) {
        return;
    }

    if (g_gic.version == GicVersion::v3 && intid < kSpiBase) {
        // SGIs/PPIs (0..31) are private: program them in THIS core's
        // redistributor SGI frame, not the distributor.
        const uint64_t sgi_base = this_core_gicr_rd() + kGicrSgiOffset;
        auto *prio = reinterpret_cast<volatile uint8_t *>(sgi_base + kGicrIpriorityr + intid);
        *prio = priority;
        mmio::write32(sgi_base + kGicrIsenabler0, 1u << (intid % 32));
        return;
    }

    auto *prio = reinterpret_cast<volatile uint8_t *>(g_gic.gicd + kGicdIpriorityr + intid);
    *prio = priority;

    if (intid >= kSpiBase) {
        if (g_gic.version == GicVersion::v2) {
            // GICv2: steer SPIs at core 0 via ITARGETSR (8-bit CPU mask).
            auto *target = reinterpret_cast<volatile uint8_t *>(g_gic.gicd + kGicdItargetsr + intid);
            *target = 0x01;
        } else /* v3 */ {
            // GICv3 SPI setup (three things, ALL required):
            //  1. GICD_IGROUPR<n>: mark Group 1NS so it acks via ICC_IAR1
            //     (ICC_IAR0 acks Group 0; our irq_dispatch reads IAR1).
            //     SPIs default to Group 0 at reset.
            //  2. GICD_IROUTER<n> @ GICD + 0x6000 + 8*n: route to the boot
            //     core's affinity (Aff0..3 from MPIDR). bit[31]=IRM is 0 =
            //     deliver to specific PE; ARE_NS in GICD_CTLR must be set.
            //  3. GICD_ISENABLER (below) to enable.
            // Without (1)+(2) the PL011 RX SPI 33 went nowhere under HVF +
            // GICv3 -- "can't type at login" symptom. GICv2 single-core
            // worked because ITARGETSR + groups defaulting to 1 (legacy).
            constexpr uint64_t kGicdIgroupr = 0x080;
            constexpr uint64_t kGicdIrouterBase = 0x6000;
            auto *grp = reinterpret_cast<volatile uint32_t *>(
                g_gic.gicd + kGicdIgroupr + (intid / 32) * 4);
            *grp = *grp | (1u << (intid % 32));
            const uint64_t mpidr = arch::read_mpidr();
            const uint64_t aff = (mpidr & 0xFFFFFFULL) |
                                 ((mpidr & (0xFFULL << 32)) /* Aff3 */);
            auto *router = reinterpret_cast<volatile uint64_t *>(
                g_gic.gicd + kGicdIrouterBase + 8ULL * intid);
            *router = aff;
        }
    }

    mmio::write32(g_gic.gicd + kGicdIsenabler + (intid / 32) * 4, 1u << (intid % 32));
}

void aleister_register(uint32_t intid, IrqHandler handler, void *ctx) {
    if (intid >= kMaxHandlers) {
        return;
    }
    g_handlers[intid] = handler;
    g_contexts[intid] = ctx;
}

void aleister_set_after_eoi(void (*hook)(uint64_t *frame)) {
    g_after_eoi = hook;
}

extern "C" void irq_dispatch(uint64_t *frame) {
    if (!g_gic.available) {
        return;
    }

    const uint32_t iar = (g_gic.version == GicVersion::v3)
                             ? static_cast<uint32_t>(read_icc_iar1())
                             : mmio::read32(g_gic.gicc + kGiccIar);
    const uint32_t intid = iar & 0xFFFFFF; // v3 INTID is 24-bit; v2 uses low 10

    if (intid >= kSpuriousFloor) {
        return; // spurious interrupt: do not acknowledge
    }

    if (intid < kMaxHandlers && g_handlers[intid] != nullptr) {
        g_handlers[intid](g_contexts[intid]);
    }

    if (g_gic.version == GicVersion::v3) {
        write_icc_eoir1(iar);
    } else {
        mmio::write32(g_gic.gicc + kGiccEoir, iar);
    }

    if (g_after_eoi != nullptr) {
        g_after_eoi(frame);
    }
}

// Send a Group-1 SGI (software-generated interrupt, INTID 0..15) to the PE
// identified by `target_mpidr`. This is the GICv3 IPI primitive -- the
// reschedule IPI that wakes a specific idle CPU when work lands on its
// runqueue (Linux's smp_send_reschedule). Encodes ICC_SGI1R_EL1:
//   [55:48] Aff3  [40:32] Aff2  [27:24] INTID  [23:16] Aff1  [15:0] TargetList
// TargetList is a bitmask of Aff0 within the (Aff3.Aff2.Aff1) cluster, so the
// target's Aff0 must be < 16 (true for QEMU virt / Apple HVF small configs).
// GICv2 has no system-register SGI path here; Index only raises IPIs under
// GICv3 (multi-core EL0), so v2 is a no-op.
void aleister_send_sgi(uint64_t target_mpidr, uint32_t sgi) {
    if (!g_gic.available || g_gic.version != GicVersion::v3 || sgi > 15) {
        return;
    }
    const uint64_t aff0 = target_mpidr & 0xff;
    const uint64_t aff1 = (target_mpidr >> 8) & 0xff;
    const uint64_t aff2 = (target_mpidr >> 16) & 0xff;
    const uint64_t aff3 = (target_mpidr >> 32) & 0xff;
    const uint64_t val = (static_cast<uint64_t>(sgi) << 24) |
                         (aff1 << 16) | (aff2 << 32) | (aff3 << 48) |
                         (1ULL << (aff0 & 0xf));
    asm volatile("dsb ish" ::: "memory"); // publish runqueue writes before the IPI
    write_icc_sgi1r(val);
}

} // namespace index::drivers
