#pragma once

#include <stdint.h>

namespace index::mmio {

// Added to every device address before access. It is 0 while the MMU is off
// (addresses are physical) and becomes the high-half base once the MMU is on,
// so device MMIO goes through the kernel's TTBR1 mapping and keeps working even
// when TTBR0 holds a user process's private address space. Set in teleport.cpp.
extern uint64_t g_mmio_offset;

inline uint32_t read32(uint64_t addr) {
    return *reinterpret_cast<volatile uint32_t *>(addr | g_mmio_offset);
}

inline void write32(uint64_t addr, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(addr | g_mmio_offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

// 8/16-bit variants, needed for the virtio-pci modern common-config block whose
// fields are u8 (device_status) and u16 (queue_select/size/enable).
inline uint8_t read8(uint64_t addr) {
    return *reinterpret_cast<volatile uint8_t *>(addr | g_mmio_offset);
}

inline void write8(uint64_t addr, uint8_t value) {
    *reinterpret_cast<volatile uint8_t *>(addr | g_mmio_offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

inline uint16_t read16(uint64_t addr) {
    return *reinterpret_cast<volatile uint16_t *>(addr | g_mmio_offset);
}

inline void write16(uint64_t addr, uint16_t value) {
    *reinterpret_cast<volatile uint16_t *>(addr | g_mmio_offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

} // namespace index::mmio
