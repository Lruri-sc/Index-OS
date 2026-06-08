#pragma once

#include <stdint.h>

namespace index::drivers {

// Underline: Academy City's deep data substrate. Here it is the persistent
// block device -- a virtio-blk disk reached over EITHER the virtio-mmio transport
// (`-device virtio-blk-device`) OR virtio-over-PCIe (`-device virtio-blk-pci`,
// what the UTM app's GUI "VirtIO" disk produces), discovered through Underground.
// Sectors are 512 bytes. The block-transfer core is identical for both transports;
// they differ only in discovery, queue setup, and where the queue-notify is written.
constexpr uint32_t kUnderlineSectorSize = 512;

enum class UnderlineTransport { none, mmio, pci };

struct Underline {
    uint64_t base = 0;             // virtio-mmio slot base (mmio transport only)
    uint64_t notify_addr = 0;      // absolute MMIO address to write the queue index to
    UnderlineTransport transport = UnderlineTransport::none;
    bool present = false;          // a virtio-blk device was found and set up
    uint64_t capacity_sectors = 0; // device capacity in 512-byte sectors
};

// Find a virtio-blk device -- first on the virtio-mmio window, then (via
// Underground) on PCIe -- and initialise it. The result is cached and also
// available via underline_status(). underground_init() must run first for the
// PCIe fallback to work.
Underline underline_probe();
const Underline &underline_status();

// Read `bytes` (a multiple of 512; default one sector) starting at `sector`
// into `buffer`, in ONE virtio request. Reading a 4 KB ext2 block is a single
// request, not 8 per-sector requests -> 8x fewer requests + busy-polls (this
// per-sector loop dominated java startup). `buffer` must be physically
// contiguous over the whole length (the kernel's static block buffers are).
bool underline_read(const Underline &disk, uint64_t sector, void *buffer,
                    uint32_t bytes = kUnderlineSectorSize);

// Write one 512-byte sector from `buffer` to the disk. Returns false on
// error/timeout. The buffer must be a full sector; partial updates are
// read-modify-write at the caller.
bool underline_write(const Underline &disk, uint64_t sector, const void *buffer);

// Arm the virtio-MMIO completion IRQ. Call only AFTER the high-half teleport +
// scheduler bring-up (enabling it during early boot / across the teleport hung).
void underline_enable_irq();
uint32_t underline_irq_count(); // completion IRQs handled (for /proc/interrupts)

} // namespace index::drivers
