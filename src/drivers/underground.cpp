#include "drivers/underground.hpp"

#include "index/mmio.hpp"

namespace index::drivers {

namespace {

// Standard PCI config-space register offsets.
constexpr uint32_t kCfgVendor = 0x00;   // u16
constexpr uint32_t kCfgDevice = 0x02;   // u16
constexpr uint32_t kCfgCommand = 0x04;  // u16
constexpr uint32_t kCfgStatus = 0x06;   // u16
constexpr uint32_t kCfgCapPtr = 0x34;   // u8: offset of first capability
constexpr uint32_t kCfgBar0 = 0x10;     // BARs at 0x10,0x14,...,0x24
constexpr uint16_t kCmdMemSpace = 1u << 1;
constexpr uint16_t kCmdBusMaster = 1u << 2;
constexpr uint16_t kStatusCapList = 1u << 4; // capabilities list present

// virtio vendor-specific capability (id 0x09) layout: cap_vndr@0, cap_next@1,
// cap_len@2, cfg_type@3, bar@4, padding@5..7, offset@8 (le32), length@12 (le32);
// the notify cap adds notify_off_multiplier@16.
constexpr uint8_t kCapVendor = 0x09;
constexpr uint32_t kVCapType = 3;
constexpr uint32_t kVCapBar = 4;
constexpr uint32_t kVCapOffset = 8;
constexpr uint32_t kVCapNotifyMult = 16;
constexpr uint8_t kVCfgCommon = 1; // VIRTIO_PCI_CAP_COMMON_CFG
constexpr uint8_t kVCfgNotify = 2; // VIRTIO_PCI_CAP_NOTIFY_CFG
constexpr uint8_t kVCfgIsr = 3;    // VIRTIO_PCI_CAP_ISR_CFG (read to ack INTx)
constexpr uint8_t kVCfgDevice = 4; // VIRTIO_PCI_CAP_DEVICE_CFG

// BAR low-bit decode.
constexpr uint32_t kBarIo = 1u << 0;        // 1 = I/O space (we only assign memory)
constexpr uint32_t kBarTypeMask = 0b11u << 1;
constexpr uint32_t kBarType64 = 0b10u << 1; // 64-bit memory BAR (consumes two slots)

uint64_t g_ecam = 0;

// QEMU `virt` 32-bit MMIO window (DTB `ranges` 0x02000000 entry: CPU 0x10000000,
// size 0x2eff0000). BARs are assigned by bumping up from its base.
constexpr uint64_t kMmioWindowBase = 0x10000000ULL;
constexpr uint64_t kMmioWindowEnd = 0x10000000ULL + 0x2eff0000ULL;
uint64_t g_mmio_cursor = kMmioWindowBase;

uint64_t cfg_base(PciAddr a, uint32_t off) {
    return g_ecam + (static_cast<uint64_t>(a.bus) << 20) +
           (static_cast<uint64_t>(a.dev) << 15) + (static_cast<uint64_t>(a.fn) << 12) +
           (off & 0xfff);
}

uint64_t align_up(uint64_t value, uint64_t align) {
    if (align == 0) {
        return value;
    }
    return (value + align - 1) & ~(align - 1);
}

} // namespace

void underground_init(uint64_t ecam_base) {
    g_ecam = ecam_base;
    g_mmio_cursor = kMmioWindowBase;
}

uint32_t underground_cfg_read32(PciAddr a, uint32_t off) {
    return mmio::read32(cfg_base(a, off));
}

uint16_t underground_cfg_read16(PciAddr a, uint32_t off) {
    return mmio::read16(cfg_base(a, off));
}

uint8_t underground_cfg_read8(PciAddr a, uint32_t off) {
    return mmio::read8(cfg_base(a, off));
}

void underground_cfg_write32(PciAddr a, uint32_t off, uint32_t value) {
    mmio::write32(cfg_base(a, off), value);
}

void underground_cfg_write16(PciAddr a, uint32_t off, uint16_t value) {
    mmio::write16(cfg_base(a, off), value);
}

PciAddr underground_find(uint16_t vendor, uint16_t id_a, uint16_t id_b) {
    if (g_ecam == 0) {
        return PciAddr{};
    }
    // The `virt` machine puts everything on bus 0; scan its 32 device slots.
    for (uint8_t dev = 0; dev < 32; ++dev) {
        PciAddr a{0, dev, 0, true};
        const uint16_t v = underground_cfg_read16(a, kCfgVendor);
        if (v == 0xffff || v != vendor) {
            continue;
        }
        const uint16_t d = underground_cfg_read16(a, kCfgDevice);
        if (d == id_a || d == id_b) {
            return a;
        }
    }
    return PciAddr{};
}

uint64_t underground_assign_bar(PciAddr a, uint32_t index) {
    if (index > 5) {
        return 0;
    }
    const uint32_t off = kCfgBar0 + index * 4;
    const uint32_t bar = underground_cfg_read32(a, off);
    if (bar & kBarIo) {
        return 0; // an I/O-space BAR; we drive virtio in memory-mapped (modern) mode
    }
    const bool is64 = (bar & kBarTypeMask) == kBarType64;

    // If firmware already gave the BAR an address, use it as-is.
    uint64_t existing = bar & ~0xfULL;
    if (is64) {
        existing |= static_cast<uint64_t>(underground_cfg_read32(a, off + 4)) << 32;
    }
    if (existing != 0) {
        underground_cfg_write16(a, kCfgCommand,
                                static_cast<uint16_t>(underground_cfg_read16(a, kCfgCommand) |
                                                      kCmdMemSpace | kCmdBusMaster));
        return existing;
    }

    // Size the BAR: write all-ones, read back the writable bits, decode size.
    underground_cfg_write32(a, off, 0xffffffffu);
    uint32_t lo_mask = underground_cfg_read32(a, off);
    uint32_t hi_mask = 0xffffffffu;
    if (is64) {
        underground_cfg_write32(a, off + 4, 0xffffffffu);
        hi_mask = underground_cfg_read32(a, off + 4);
    }
    uint64_t mask = (static_cast<uint64_t>(hi_mask) << 32) | (lo_mask & ~0xfu);
    if (mask == 0) {
        return 0;
    }
    const uint64_t size = (~mask) + 1; // size = 1 + the inverted mask

    const uint64_t addr = align_up(g_mmio_cursor, size);
    if (addr + size > kMmioWindowEnd) {
        return 0; // window exhausted
    }
    g_mmio_cursor = addr + size;

    underground_cfg_write32(a, off, static_cast<uint32_t>(addr) | (bar & 0xfu));
    if (is64) {
        underground_cfg_write32(a, off + 4, static_cast<uint32_t>(addr >> 32));
    }
    underground_cfg_write16(a, kCfgCommand,
                            static_cast<uint16_t>(underground_cfg_read16(a, kCfgCommand) |
                                                  kCmdMemSpace | kCmdBusMaster));
    return addr;
}

VirtioPciCfg underground_virtio_cfg(PciAddr a) {
    VirtioPciCfg cfg;
    if (!a.valid || (underground_cfg_read16(a, kCfgStatus) & kStatusCapList) == 0) {
        return cfg;
    }
    uint8_t cap = underground_cfg_read8(a, kCfgCapPtr);
    for (uint32_t guard = 0; cap != 0 && guard < 48; ++guard) {
        const uint8_t id = underground_cfg_read8(a, cap + 0);
        const uint8_t next = underground_cfg_read8(a, cap + 1);
        if (id == kCapVendor) {
            const uint8_t cfg_type = underground_cfg_read8(a, cap + kVCapType);
            const uint8_t bar = underground_cfg_read8(a, cap + kVCapBar);
            const uint32_t off = underground_cfg_read32(a, cap + kVCapOffset);
            const uint64_t bar_base = underground_assign_bar(a, bar);
            if (bar_base != 0) {
                if (cfg_type == kVCfgCommon) {
                    cfg.common = bar_base + off;
                } else if (cfg_type == kVCfgNotify) {
                    cfg.notify = bar_base + off;
                    cfg.notify_mult = underground_cfg_read32(a, cap + kVCapNotifyMult);
                } else if (cfg_type == kVCfgIsr) {
                    cfg.isr = bar_base + off;
                } else if (cfg_type == kVCfgDevice) {
                    cfg.device = bar_base + off;
                }
            }
        }
        cap = next;
    }
    cfg.valid = cfg.common != 0 && cfg.notify != 0;
    return cfg;
}

namespace {

constexpr uint16_t kVirtioVendor = 0x1AF4;
constexpr uint32_t kCfgClassRev = 0x08;  // u32: rev(7:0) progIF(15:8) sub(23:16) base(31:24)

// xHCI controller: USBCMD R/S bit lives in operational regs which start
// CAPLENGTH bytes past the BAR. Writing 0 stops the schedule -- QEMU's xhci
// model stops polling its event ring and HID descriptors.
void quiesce_xhci(uint64_t bar0) {
    if (bar0 == 0) return;
    const uint8_t caplen = mmio::read32(bar0) & 0xff; // CAPLENGTH is bits 7:0 of HCIVERSION:CAPLENGTH
    const uint64_t op = bar0 + caplen;
    // USBCMD @ +0x00, USBSTS @ +0x04. Clear R/S (bit 0) + INTE (bit 2) +
    // HSEE (bit 3) -- everything that could schedule work.
    mmio::write32(op + 0x00, 0);
    // Best-effort: wait until HCH bit (USBSTS bit 0) shows the schedule halted.
    for (uint32_t spin = 0; spin < 100000; ++spin) {
        if ((mmio::read32(op + 0x04) & 1u) != 0) break;
    }
}

// Intel HDA controller: GCTL bit 0 (CRST) low forces a controller reset, after
// which the device stops driving CORB/RIRB/DMA loops.
void quiesce_hda(uint64_t bar0) {
    if (bar0 == 0) return;
    mmio::write32(bar0 + 0x08, 0); // GCTL = 0 -> CRST asserted (reset)
}

void quiesce_one(PciAddr a) {
    const uint32_t classrev = underground_cfg_read32(a, kCfgClassRev);
    const uint8_t base_class = (classrev >> 24) & 0xff;
    const uint8_t sub_class  = (classrev >> 16) & 0xff;
    const uint8_t prog_if    = (classrev >>  8) & 0xff;

    if (base_class == 0x0C && sub_class == 0x03 && prog_if == 0x30) {
        // Serial Bus Controller / USB / xHCI
        quiesce_xhci(underground_assign_bar(a, 0));
    } else if (base_class == 0x04 && sub_class == 0x03) {
        // Multimedia Controller / HD Audio
        quiesce_hda(underground_assign_bar(a, 0));
    } else {
        // Unknown class we don't drive: leave registers alone but still drop
        // bus mastering below so the device cannot DMA.
    }

    // Clear Bus Master + Memory Space + I/O Space. The device can't reach
    // memory or have its MMIO solicited, which on QEMU typically transitions
    // its emulator to an idle state -- closest analog to PCI D3hot for us.
    const uint16_t cmd = underground_cfg_read16(a, kCfgCommand);
    underground_cfg_write16(a, kCfgCommand,
        static_cast<uint16_t>(cmd & ~(kCmdMemSpace | kCmdBusMaster | 1u /*I/O*/)));
}

} // namespace

void underground_quiesce_unused() {
    if (g_ecam == 0) return;
    for (uint8_t dev = 0; dev < 32; ++dev) {
        PciAddr a{0, dev, 0, true};
        const uint16_t v = underground_cfg_read16(a, kCfgVendor);
        if (v == 0xffff) continue;       // empty slot
        if (v == kVirtioVendor) {
            const uint16_t d = underground_cfg_read16(a, kCfgDevice);
            const bool driven =
                d == 0x1041 || d == 0x1000 ||  // virtio-net (modern/legacy)
                d == 0x1042 || d == 0x1001 ||  // virtio-blk
                d == 0x1044 || d == 0x1005 ||  // virtio-rng
                d == 0x1043 || d == 0x1003;    // virtio-serial (qga stub)
            if (driven) continue;
        }
        quiesce_one(a);
    }
}

} // namespace index::drivers
