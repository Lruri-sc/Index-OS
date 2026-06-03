#pragma once

#include <stdint.h>

namespace index::drivers {

// Underground: Academy City's subterranean web of tunnels and conduits that
// quietly links every facility above. Here it is the PCIe host bridge -- the bus
// fabric the CPU reaches every device through. It speaks ECAM (config space is
// memory-mapped: a device is addressed by its bus:device:function coordinates),
// enumerates what is plugged in, and hands each device an MMIO region (a BAR) so
// it can be driven. The virtio-blk disk (Underline) is merely the first rider on
// this network; NICs, GPUs, etc. would hang off the same Underground.
struct PciAddr {
    uint8_t bus = 0;
    uint8_t dev = 0;
    uint8_t fn = 0;
    bool valid = false;
};

// Remember the ECAM base and reset the BAR allocator. Call once, after the MMU
// is on (config space is reached through the high-half device alias).
void underground_init(uint64_t ecam_base);

// Config-space access for one function, at byte offset `off`.
uint32_t underground_cfg_read32(PciAddr a, uint32_t off);
uint16_t underground_cfg_read16(PciAddr a, uint32_t off);
uint8_t underground_cfg_read8(PciAddr a, uint32_t off);
void underground_cfg_write32(PciAddr a, uint32_t off, uint32_t value);
void underground_cfg_write16(PciAddr a, uint32_t off, uint16_t value);

// Find the first function on bus 0 whose vendor matches and whose device id is
// `id_a` or `id_b`. Returns {valid=false} if none.
PciAddr underground_find(uint16_t vendor, uint16_t id_a, uint16_t id_b);

// Ensure BAR `index` (0..5) has an MMIO address and return it. If firmware left
// the BAR unassigned (zero) we size it and assign from the 32-bit MMIO window;
// 64-bit memory BARs consume two slots. Also enables Memory Space + Bus Master
// in the command register so the device can DMA and answer MMIO. Returns 0 on
// error (not a memory BAR, etc.).
uint64_t underground_assign_bar(PciAddr a, uint32_t index);

// Absolute MMIO addresses of a modern virtio device's three config structures,
// resolved from its vendor-specific PCI capabilities (each names a BAR+offset,
// whose BAR this assigns). Shared by every virtio-pci device (blk, net, ...).
struct VirtioPciCfg {
    uint64_t common = 0; // virtio_pci_common_cfg
    uint64_t notify = 0;  // notify structure base
    uint64_t device = 0;  // device-specific config (e.g. blk capacity / net mac)
    uint64_t isr = 0;     // ISR status (read to ack/deassert a legacy INTx line)
    uint32_t notify_mult = 0; // notify_off_multiplier
    bool valid = false;
};

// Walk `a`'s capability list, classify the virtio cfg_types, assign the BARs
// they reference, and return the resolved structure addresses. valid=false if
// the required common+notify structures were not found.
VirtioPciCfg underground_virtio_cfg(PciAddr a);

// Walk every present function on bus 0 and quiesce devices we don't drive:
// halt the device-specific state machine (xHCI USBCMD.R/S=0, HDA GCTL=0) and
// then clear Bus Master + Memory Space in the PCI command register so QEMU's
// emulators stop scheduling work on our behalf. Mainstream OSes do the
// equivalent (PCI power-management D3, or driver unload). Without it the
// host-side emulator keeps polling its event ring / HID descriptors / audio
// DMA at the device's intrinsic rate, even when no guest driver is present --
// which is what burns ~100% of one host core in UTM. virtio devices we DO
// use (vendor 0x1AF4) are skipped.
void underground_quiesce_unused();

} // namespace index::drivers
