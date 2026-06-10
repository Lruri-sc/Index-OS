#include "drivers/underline.hpp"

#include "drivers/aleister.hpp" // route+enable the used-buffer SPI + bind its handler
#include "drivers/underground.hpp"
#include "index/mmio.hpp"
#include "index/teleport.hpp"

namespace index::drivers {

namespace {

using index::mmio::read32;
using index::mmio::write32;

// The `virt` machine exposes 32 virtio-mmio transport slots, each 0x200 bytes.
constexpr uint64_t kMmioBase = 0x0a000000;
constexpr uint64_t kMmioStride = 0x200;
constexpr uint32_t kMmioSlots = 32;

constexpr uint32_t kMagic = 0x74726976; // "virt"
constexpr uint32_t kDeviceIdBlock = 2;

// virtio-mmio register offsets (legacy + modern).
constexpr uint64_t kMagicValue = 0x000;
constexpr uint64_t kVersion = 0x004;
constexpr uint64_t kDeviceId = 0x008;
constexpr uint64_t kDeviceFeaturesSel = 0x014;
constexpr uint64_t kDriverFeatures = 0x020;
constexpr uint64_t kDriverFeaturesSel = 0x024;
constexpr uint64_t kGuestPageSize = 0x028; // legacy
constexpr uint64_t kQueueSel = 0x030;
constexpr uint64_t kQueueNumMax = 0x034;
constexpr uint64_t kQueueNum = 0x038;
constexpr uint64_t kQueueAlign = 0x03c; // legacy
constexpr uint64_t kQueuePfn = 0x040;   // legacy
constexpr uint64_t kQueueReady = 0x044; // modern
constexpr uint64_t kQueueNotify = 0x050;
constexpr uint64_t kStatus = 0x070;
constexpr uint64_t kQueueDescLow = 0x080;   // modern
constexpr uint64_t kQueueDescHigh = 0x084;
constexpr uint64_t kQueueDriverLow = 0x090;
constexpr uint64_t kQueueDriverHigh = 0x094;
constexpr uint64_t kQueueDeviceLow = 0x0a0;
constexpr uint64_t kQueueDeviceHigh = 0x0a4;
constexpr uint64_t kInterruptStatus = 0x060;
constexpr uint64_t kInterruptAck = 0x064;
constexpr uint64_t kConfig = 0x100;
constexpr uint32_t kVirtMmioIntidBase = 48; // virt: virtio-mmio slot i -> INTID 48+i
constexpr uint8_t kBlkIrqPriority = 0x80;
volatile uint32_t g_blk_irq_count = 0;
uint64_t g_blk_irq_base = 0;
uint32_t g_blk_irq_intid = 0;

constexpr uint32_t kStatusAck = 1;
constexpr uint32_t kStatusDriver = 2;
constexpr uint32_t kStatusDriverOk = 4;
constexpr uint32_t kStatusFeaturesOk = 8;

constexpr uint16_t kDescNext = 1;
constexpr uint16_t kDescWrite = 2;

constexpr uint32_t kQueueSize = 8;
constexpr uint32_t kQueueAlignBytes = 4096;
constexpr uint32_t kBlkTypeIn = 0;  // VIRTIO_BLK_T_IN  (read)
constexpr uint32_t kBlkTypeOut = 1; // VIRTIO_BLK_T_OUT (write)

// virtio-pci (modern). virtio devices are PCI vendor 0x1af4; virtio-blk is the
// transitional device 0x1001 (QEMU default, exposes modern caps) or the pure
// modern 0x1042. The modern interface lives behind vendor-specific capabilities.
constexpr uint16_t kVirtioVendor = 0x1af4;
constexpr uint16_t kVirtioBlkLegacyId = 0x1001;
constexpr uint16_t kVirtioBlkModernId = 0x1042;

// virtio_pci_common_cfg field offsets (within the common-cfg structure). We
// don't negotiate optional features, so the device_feature read regs are unused.
constexpr uint64_t kCommonDriverFeatureSel = 0x08;
constexpr uint64_t kCommonDriverFeature = 0x0c;
constexpr uint64_t kCommonDeviceStatus = 0x14; // u8
constexpr uint64_t kCommonQueueSelect = 0x16;  // u16
constexpr uint64_t kCommonQueueSize = 0x18;    // u16
constexpr uint64_t kCommonQueueEnable = 0x1c;  // u16
constexpr uint64_t kCommonQueueNotifyOff = 0x1e; // u16
constexpr uint64_t kCommonQueueDesc = 0x20;    // u64
constexpr uint64_t kCommonQueueDriver = 0x28;  // u64
constexpr uint64_t kCommonQueueDevice = 0x30;  // u64

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[kQueueSize];
    uint16_t used_event;
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem ring[kQueueSize];
    uint16_t avail_event;
};

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

// Single contiguous, page-aligned queue: descriptors, then the avail ring, then
// the used ring at a QueueAlign boundary. This layout satisfies both the legacy
// transport (one QueuePFN) and the modern one (three separate addresses).
alignas(4096) uint8_t g_queue[2 * kQueueAlignBytes];
alignas(16) virtio_blk_req g_req;
alignas(16) uint8_t g_status_byte[16];

virtq_desc *const g_desc = reinterpret_cast<virtq_desc *>(g_queue);
virtq_avail *const g_avail =
    reinterpret_cast<virtq_avail *>(g_queue + sizeof(virtq_desc) * kQueueSize);
virtq_used *const g_used = reinterpret_cast<virtq_used *>(g_queue + kQueueAlignBytes);

uint16_t g_last_used = 0;
Underline g_disk;

// Device DMA uses guest-physical addresses; our pointers are high-half VAs
// whose low 39 bits equal the physical address.
uint64_t phys(const volatile void *p) {
    return reinterpret_cast<uint64_t>(p) & ~index::kHighHalfBase;
}

void zero(void *p, uint32_t n) {
    auto *b = static_cast<uint8_t *>(p);
    for (uint32_t i = 0; i < n; ++i) {
        b[i] = 0;
    }
}

bool setup_legacy(uint64_t base) {
    write32(base + kStatus, 0);
    write32(base + kStatus, kStatusAck);
    write32(base + kStatus, kStatusAck | kStatusDriver);

    write32(base + kDeviceFeaturesSel, 0);
    write32(base + kDriverFeaturesSel, 0);
    write32(base + kDriverFeatures, 0); // negotiate no optional features

    write32(base + kGuestPageSize, kQueueAlignBytes);

    write32(base + kQueueSel, 0);
    if (read32(base + kQueueNumMax) < kQueueSize) {
        return false;
    }
    write32(base + kQueueNum, kQueueSize);
    write32(base + kQueueAlign, kQueueAlignBytes);
    write32(base + kQueuePfn, static_cast<uint32_t>(phys(g_queue) / kQueueAlignBytes));

    write32(base + kStatus, kStatusAck | kStatusDriver | kStatusDriverOk);
    return true;
}

bool setup_modern(uint64_t base) {
    write32(base + kStatus, 0);
    write32(base + kStatus, kStatusAck);
    write32(base + kStatus, kStatusAck | kStatusDriver);

    write32(base + kDriverFeaturesSel, 1);
    write32(base + kDriverFeatures, 1u); // VIRTIO_F_VERSION_1 (bit 32)
    write32(base + kDriverFeaturesSel, 0);
    write32(base + kDriverFeatures, 0u);

    write32(base + kStatus, kStatusAck | kStatusDriver | kStatusFeaturesOk);
    if ((read32(base + kStatus) & kStatusFeaturesOk) == 0) {
        return false;
    }

    write32(base + kQueueSel, 0);
    if (read32(base + kQueueNumMax) < kQueueSize) {
        return false;
    }
    write32(base + kQueueNum, kQueueSize);
    write32(base + kQueueDescLow, static_cast<uint32_t>(phys(g_desc)));
    write32(base + kQueueDescHigh, static_cast<uint32_t>(phys(g_desc) >> 32));
    write32(base + kQueueDriverLow, static_cast<uint32_t>(phys(g_avail)));
    write32(base + kQueueDriverHigh, static_cast<uint32_t>(phys(g_avail) >> 32));
    write32(base + kQueueDeviceLow, static_cast<uint32_t>(phys(g_used)));
    write32(base + kQueueDeviceHigh, static_cast<uint32_t>(phys(g_used) >> 32));
    write32(base + kQueueReady, 1);

    write32(base + kStatus, kStatusAck | kStatusDriver | kStatusFeaturesOk | kStatusDriverOk);
    return true;
}

// Set up a virtio-blk device found on PCIe, driving it through the modern
// (virtio 1.0) interface: walk the vendor-specific capability list to locate the
// common-config, notify, and device-config structures (each names a BAR + offset),
// assign those BARs via Underground, then program the queue through common-config.
// The queue rings (g_desc/g_avail/g_used) and underline_read are shared with mmio.
bool setup_modern_pci(Underline &disk) {
    const PciAddr dev =
        underground_find(kVirtioVendor, kVirtioBlkLegacyId, kVirtioBlkModernId);
    if (!dev.valid) {
        return false;
    }

    // Resolve the virtio config structures (assigns BARs) via the shared locator.
    const VirtioPciCfg cfg = underground_virtio_cfg(dev);
    if (!cfg.valid) {
        return false;
    }
    const uint64_t common = cfg.common;
    const uint64_t notify = cfg.notify;
    const uint64_t device_cfg = cfg.device;
    const uint32_t notify_mult = cfg.notify_mult;

    using index::mmio::read16;
    using index::mmio::read8;
    using index::mmio::write16;
    using index::mmio::write8;

    // Reset, then acknowledge + driver.
    write8(common + kCommonDeviceStatus, 0);
    write8(common + kCommonDeviceStatus, kStatusAck);
    write8(common + kCommonDeviceStatus, kStatusAck | kStatusDriver);

    // Accept only VIRTIO_F_VERSION_1 (feature bit 32: select word 1, bit 0).
    write32(common + kCommonDriverFeatureSel, 1);
    write32(common + kCommonDriverFeature, 1u);
    write32(common + kCommonDriverFeatureSel, 0);
    write32(common + kCommonDriverFeature, 0u);
    write8(common + kCommonDeviceStatus, kStatusAck | kStatusDriver | kStatusFeaturesOk);
    if ((read8(common + kCommonDeviceStatus) & kStatusFeaturesOk) == 0) {
        return false; // device rejected our feature set
    }

    // Configure queue 0.
    write16(common + kCommonQueueSelect, 0);
    uint16_t qmax = read16(common + kCommonQueueSize);
    if (qmax == 0) {
        return false;
    }
    if (qmax > kQueueSize) {
        qmax = kQueueSize;
    }
    write16(common + kCommonQueueSize, qmax);

    mmio::write32(common + kCommonQueueDesc, static_cast<uint32_t>(phys(g_desc)));
    mmio::write32(common + kCommonQueueDesc + 4, static_cast<uint32_t>(phys(g_desc) >> 32));
    mmio::write32(common + kCommonQueueDriver, static_cast<uint32_t>(phys(g_avail)));
    mmio::write32(common + kCommonQueueDriver + 4, static_cast<uint32_t>(phys(g_avail) >> 32));
    mmio::write32(common + kCommonQueueDevice, static_cast<uint32_t>(phys(g_used)));
    mmio::write32(common + kCommonQueueDevice + 4, static_cast<uint32_t>(phys(g_used) >> 32));

    const uint16_t notify_off = read16(common + kCommonQueueNotifyOff);
    write16(common + kCommonQueueEnable, 1);
    write8(common + kCommonDeviceStatus,
           kStatusAck | kStatusDriver | kStatusFeaturesOk | kStatusDriverOk);

    disk.transport = UnderlineTransport::pci;
    disk.notify_addr = notify + static_cast<uint64_t>(notify_off) * notify_mult;
    disk.present = true;
    g_last_used = g_used->idx;
    if (device_cfg != 0) {
        const uint64_t lo = read32(device_cfg + 0);
        const uint64_t hi = read32(device_cfg + 4);
        disk.capacity_sectors = (hi << 32) | lo; // virtio_blk_config.capacity
    }
    return true;
}

void blk_irq_handler(void * /*ctx*/) {
    if (g_blk_irq_base != 0) {
        const uint32_t isr = read32(g_blk_irq_base + kInterruptStatus);
        if (isr != 0) {
            write32(g_blk_irq_base + kInterruptAck, isr);
        }
    }
    g_blk_irq_count = g_blk_irq_count + 1;
}

} // namespace

const Underline &underline_status() {
    return g_disk;
}

// Arm the virtio-MMIO used-buffer completion IRQ. MUST be called AFTER the boot
// teleport to the high half (stable VBAR) and once the scheduler is live -- arming
// it earlier (before teleport) hung: the SPI, asserted while VBAR was switching,
// was taken against a transient vector. Idempotent guard via g_blk_irq_count seed.
void underline_enable_irq() {
    if (!g_disk.present || g_disk.transport != UnderlineTransport::mmio ||
        g_blk_irq_base == 0) {
        return;
    }
    const uint32_t isr = read32(g_blk_irq_base + kInterruptStatus); // clear backlog
    if (isr != 0) {
        write32(g_blk_irq_base + kInterruptAck, isr);
    }
    // DO NOT arm the GIC SPI. Enabling the virtio-blk used-buffer completion SPI
    // (aleister_enable(intid 48)) WEDGES THE BOX -- exhaustively bisected to this
    // single line: enabling the level SPI stalls GIC delivery so even the timer PPI
    // stops, and boot dies right here (this is enter_necessarius's first action,
    // right after "entering Necessarius"). The platform completes virtio
    // synchronously on the doorbell vm-exit, so the driver stays on doorbell-poll
    // (underline_read with the 256-spin re-ring) -- the only working design. This
    // hook is now a safe no-op kept so the boot path needn't change. See
    // jvm-bringup.md "virtio 完成中断".
    (void)&blk_irq_handler; // kept for /proc diagnostics; never armed
}

uint32_t underline_irq_count() {
    return g_blk_irq_count;
}

Underline underline_probe() {
    Underline disk;
    zero(g_queue, sizeof(g_queue));

    for (uint32_t i = 0; i < kMmioSlots; ++i) {
        const uint64_t base = kMmioBase + i * kMmioStride;
        if (read32(base + kMagicValue) != kMagic) {
            continue;
        }
        if (read32(base + kDeviceId) != kDeviceIdBlock) {
            continue;
        }
        const uint32_t version = read32(base + kVersion);
        const bool ok = (version == 1) ? setup_legacy(base) : setup_modern(base);
        if (!ok) {
            continue;
        }
        g_last_used = g_used->idx;
        disk.base = base;
        disk.notify_addr = base + kQueueNotify; // MMIO: notify is a fixed register
        disk.transport = UnderlineTransport::mmio;
        disk.present = true;
        g_blk_irq_base = base;
        g_blk_irq_intid = kVirtMmioIntidBase + i;
        const uint64_t lo = read32(base + kConfig + 0);
        const uint64_t hi = read32(base + kConfig + 4);
        disk.capacity_sectors = (hi << 32) | lo;
        g_disk = disk;
        return disk;
    }

    // No virtio-mmio device: try virtio-blk on PCIe (Underground), the transport
    // the UTM GUI's "VirtIO" disk uses.
    if (setup_modern_pci(disk)) {
        g_disk = disk;
        return disk;
    }

    g_disk = disk;
    return disk;
}

bool underline_read(const Underline &disk, uint64_t sector, void *buffer,
                    uint32_t bytes) {
    if (!disk.present) {
        return false;
    }

    g_req.type = kBlkTypeIn;
    g_req.reserved = 0;
    g_req.sector = sector;
    g_status_byte[0] = 0xff;

    g_desc[0].addr = phys(&g_req);
    g_desc[0].len = sizeof(virtio_blk_req);
    g_desc[0].flags = kDescNext;
    g_desc[0].next = 1;

    g_desc[1].addr = phys(buffer);
    g_desc[1].len = bytes; // multi-sector: device reads bytes/512 sectors at once
    g_desc[1].flags = kDescNext | kDescWrite;
    g_desc[1].next = 2;

    g_desc[2].addr = phys(g_status_byte);
    g_desc[2].len = 1;
    g_desc[2].flags = kDescWrite;
    g_desc[2].next = 0;

    g_avail->ring[g_avail->idx % kQueueSize] = 0; // chain head
    asm volatile("dsb sy" ::: "memory");
    g_avail->idx = g_avail->idx + 1;
    asm volatile("dsb sy" ::: "memory");

    write32(disk.notify_addr, 0); // notify queue 0 (mmio: fixed reg; pci: computed addr)

    for (uint64_t spin = 0; spin < 200000000ULL; ++spin) {
        // Observe the device-DMA'd used index with a VOLATILE READ, not a per-spin
        // `dsb sy`. The old loop did a full-system barrier (~100ns on HVF) EVERY
        // iteration; a multi-thousand-spin wait then burned milliseconds (measured:
        // `ls -laR` = 14002 reads / 47M spins, ~4.7s of which was the dsb itself --
        // the actual cost, NOT the disk). The virtio used ring is cache-coherent,
        // so a volatile reload sees the device's write; we only dsb when a change
        // appears (confirm the payload/status are visible) or periodically (bound
        // observe latency). This cut per-spin cost ~100x.
        const uint16_t cur = *static_cast<volatile uint16_t *>(&g_used->idx);
        if (cur != g_last_used) {
            asm volatile("dsb sy" ::: "memory"); // confirm: data + status visible
            g_last_used = g_used->idx;
            return g_status_byte[0] == 0;
        }
        if ((spin & 0xFF) == 0xFF) {
            asm volatile("dsb sy" ::: "memory"); // periodic: bound observe latency
        }
        // HVF: the used ring is filled by qemu's main loop, which makes progress on
        // this device only when the guest rings the doorbell (an MMIO store that
        // vm-exits). Each in-flight read needs ~35 such pumps before qemu's async
        // host I/O finishes, so the spin count between doorbells is pure busy-wait.
        // The interval is therefore the lever: re-ringing every 256 spins instead
        // of the old 4096 cut the boot read profile 8.3x (measured on Apple HVF,
        // GICv3, virtio-blk-mmio: avg 66821 -> 8087 spins/read, max 225280 -> 23552;
        // doorbells/read stay ~32, so vm-exit overhead barely moves). 64 (25x) and
        // 16 (113x) go further but march toward the per-spin BQL-thrash-and-hang
        // floor the IRQ-less design hit; 256 keeps a 16x safety margin. (Async
        // completion IRQ is impossible here: WFI-wait/probe-enable/deferred-enable/
        // IRQ-masked-ring were all re-confirmed to hang -- nothing to wait on.)
        if ((spin & 0xFF) == 0xFF) {
            write32(disk.notify_addr, 0);
        }
    }
    return false; // timed out
}

bool underline_write(const Underline &disk, uint64_t sector, const void *buffer) {
    if (!disk.present) {
        return false;
    }

    g_req.type = kBlkTypeOut; // device READS the data buffer and writes the disk
    g_req.reserved = 0;
    g_req.sector = sector;
    g_status_byte[0] = 0xff;

    g_desc[0].addr = phys(&g_req);
    g_desc[0].len = sizeof(virtio_blk_req);
    g_desc[0].flags = kDescNext;
    g_desc[0].next = 1;

    // Data descriptor: device-readable (no kDescWrite), unlike the read path.
    g_desc[1].addr = phys(const_cast<void *>(buffer));
    g_desc[1].len = kUnderlineSectorSize;
    g_desc[1].flags = kDescNext;
    g_desc[1].next = 2;

    g_desc[2].addr = phys(g_status_byte);
    g_desc[2].len = 1;
    g_desc[2].flags = kDescWrite;
    g_desc[2].next = 0;

    g_avail->ring[g_avail->idx % kQueueSize] = 0; // chain head
    asm volatile("dsb sy" ::: "memory");
    g_avail->idx = g_avail->idx + 1;
    asm volatile("dsb sy" ::: "memory");

    write32(disk.notify_addr, 0);

    for (uint64_t spin = 0; spin < 200000000ULL; ++spin) {
        asm volatile("dsb sy" ::: "memory");
        if (g_used->idx != g_last_used) {
            g_last_used = g_used->idx;
            asm volatile("dsb sy" ::: "memory");
            return g_status_byte[0] == 0;
        }
        // HVF: the used ring is filled by qemu's MAIN LOOP, which only gets to run
        // when the vCPU vm-exits occasionally. A pure busy-poll keeps the host on
        // the vCPU and the aio completion never runs (the SMP=1 java read of
        // sector ~62k hung at CPU 0% right here; console tracing's occasional MMIO
        // write vm-exited and unstuck it = the Heisenbug). Re-ring the doorbell
        // every 256 spins -- an MMIO store vm-exits, letting qemu run its main
        // loop to complete the request -- but only OCCASIONALLY: a per-spin
        // doorbell thrashes the BQL and still hung. No IRQ unmask, so it's safe in
        // syscall context (WFI/enable-IRQ crashed init via EL1-IRQ nesting).
        // 256 (was 4096): matches underline_read; the larger interval left writes
        // spinning ~8x longer before qemu's main loop ran the completion, which is
        // most of the heavy guest-write (apt/dpkg, busybox cp) slowness on HVF.
        if ((spin & 0xFF) == 0xFF) {
            write32(disk.notify_addr, 0);
        }
    }
    return false; // timed out
}

} // namespace index::drivers
