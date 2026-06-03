#pragma once

#include <stdint.h>

#include "index/artificial_heaven.hpp"

namespace index {

// Base of the kernel (higher) half of the virtual address space. Must match
// HIGH_BASE in linker.ld. The kernel is linked at kHighHalfBase + load_phys.
constexpr uint64_t kHighHalfBase = 0xFFFFFF8000000000ULL;

// Turn a physical address into its high-half alias (TTBR1 mapping).
inline uint64_t teleport_high_alias(uint64_t phys) {
    return phys | kHighHalfBase;
}

// Teleport (空間移動): Shirai Kuroko's ability to move an object from one set of
// coordinates to another through an internal calculation. Here it is the MMU /
// address translation: a page table that maps virtual coordinates onto physical
// ones. This first version is an identity map (VA == PA) whose real purpose is
// to turn the MMU on so RAM becomes cacheable and MMIO is correctly typed as
// device memory; a higher-half relocation is a future coordinate move.
struct Teleport {
    bool enabled = false;
    uint32_t va_bits = 0;   // virtual address width (from T0SZ)
    uint32_t pa_bits = 0;   // physical address width (from ID_AA64MMFR0)
    uint32_t blocks = 0;    // 1 GiB blocks mapped, including the device block
    uint64_t mapped_top = 0; // highest mapped physical address (exclusive)
    bool wx_protected = false; // kernel mapped W^X (RO text, NX data)
};

// Build the identity map and switch the MMU on. Returns the resulting profile.
Teleport teleport_enable(const ArtificialHeaven &heaven);

// Bring a secondary core's MMU online using the boot core's already-built page
// tables and cached register profile. Must run on the secondary itself, with its
// MMU still off. Returns false if the boot core never ran teleport_enable.
bool teleport_enable_secondary();

// Last result, for `status` and other read-only callers.
const Teleport &teleport_status();

// Physical address of the kernel's top-level page table (the identity/kernel
// TTBR0 value). Used as the address space for trusted code with no private map.
uint64_t teleport_kernel_ttbr0();

// The kernel's L1 table (512 entries). A per-process address space copies this
// so the kernel's own low-VA mappings (e.g. its stack) stay valid at EL1, then
// overrides entry 0 with the process's private low 1 GiB.
const uint64_t *teleport_kernel_l1();

} // namespace index
