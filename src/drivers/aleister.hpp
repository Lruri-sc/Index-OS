#pragma once

#include <stdint.h>

namespace index::drivers {

// Aleister Crowley sits in the Windowless Building and routes every signal in
// Academy City to wherever it must go. Here he is the GIC interrupt
// controller: the distributor (GICD) decides which interrupt reaches the core,
// and the CPU interface acknowledges and retires it. IRQ sources register a
// handler keyed by their interrupt ID and Aleister dispatches them.
//
// Two GIC architectures are supported, chosen at boot from the device tree:
//   - GICv2: the CPU interface is MMIO (GICC). Used by TCG `-M virt,gic-version=2`.
//   - GICv3: the CPU interface is system registers (ICC_*_EL1) and each core has
//     its own redistributor (GICR) frame. REQUIRED by Apple HVF, which has no
//     GICv2. SPIs/IPIs aren't needed here -- the only interrupt is the per-core
//     virtual-timer PPI (27) -- so only the SGI/PPI redistributor path is wired.
using IrqHandler = void (*)(void *ctx);

enum class GicVersion : uint8_t { v2, v3 };

struct Aleister {
    uint64_t gicd = 0; // distributor MMIO base (both versions)
    uint64_t gicc = 0; // GICv2 CPU interface MMIO base (v2 only)
    uint64_t gicr = 0; // GICv3 redistributor region base (v3 only); per-core frames follow
    GicVersion version = GicVersion::v2;
    bool available = false;
};

// Bring up the distributor (global) + this (boot) core's CPU interface and
// remember this controller as the active one used by the IRQ dispatcher.
void aleister_init(const Aleister &gic);

// Bring up only the calling core's GICC (CPU interface). The distributor is
// global and already initialised by aleister_init on the boot core; each
// secondary calls this to unmask interrupts on its own banked CPU interface.
void aleister_init_cpu_interface();

// Route + unmask one interrupt ID at the given priority (lower value = higher).
void aleister_enable(uint32_t intid, uint8_t priority);

// Bind a C handler to an interrupt ID. Called from IRQ context on each arrival.
void aleister_register(uint32_t intid, IrqHandler handler, void *ctx);

// Hook run from IRQ context *after* the interrupt is EOI'd at the GIC, passed
// the saved register frame (so it can preempt an interrupted EL0 process). This
// is where rescheduling happens: doing it post-EOI keeps the GIC's active state
// correct even when the hook switches away and never returns here.
void aleister_set_after_eoi(void (*hook)(uint64_t *frame));

// Send a software-generated interrupt (SGI / IPI, INTID 0..15) to the PE with
// the given MPIDR. GICv3 only (the multi-core EL0 reschedule IPI). No-op on
// GICv2. dsb before the trigger publishes prior runqueue writes to the target.
void aleister_send_sgi(uint64_t target_mpidr, uint32_t sgi);

} // namespace index::drivers
