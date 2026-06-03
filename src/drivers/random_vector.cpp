#include "drivers/random_vector.hpp"

#include "drivers/underground.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/last_order.hpp"
#include "index/mmio.hpp"
#include "index/teleport.hpp"

namespace index::drivers {

namespace district = index::imaginary_number_district;

namespace {

using index::mmio::read16;
using index::mmio::read32;
using index::mmio::write16;
using index::mmio::write32;
using index::mmio::write8;

// virtio-pci common-config offsets (same shape as misaka_mail's setup).
constexpr uint64_t kDriverFeatureSel = 0x08;
constexpr uint64_t kDriverFeature = 0x0c;
constexpr uint64_t kDeviceStatus = 0x14;
constexpr uint64_t kQueueSelect = 0x16;
constexpr uint64_t kQueueSize = 0x18;
constexpr uint64_t kQueueEnable = 0x1c;
constexpr uint64_t kQueueNotifyOff = 0x1e;
constexpr uint64_t kQueueDesc = 0x20;
constexpr uint64_t kQueueDriver = 0x28;
constexpr uint64_t kQueueDevice = 0x30;

constexpr uint8_t kStatusAck = 1;
constexpr uint8_t kStatusDriver = 2;
constexpr uint8_t kStatusDriverOk = 4;
constexpr uint8_t kStatusFeaturesOk = 8;

constexpr uint16_t kVirtioVendor = 0x1af4;
constexpr uint16_t kVirtioRngLegacyId = 0x1005;
constexpr uint16_t kVirtioRngModernId = 0x1044;

constexpr uint16_t kDescWrite = 2;
constexpr uint32_t kRing = 8;        // small request ring
constexpr uint32_t kChunkBytes = 64; // bytes per entropy request

struct VDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
struct VAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[kRing];
    uint16_t used_event;
};
struct VUsedElem {
    uint32_t id;
    uint32_t len;
};
struct VUsed {
    uint16_t flags;
    uint16_t idx;
    VUsedElem ring[kRing];
    uint16_t avail_event;
};

struct QueueMem {
    alignas(4096) VDesc desc[kRing];
    VAvail avail;
    alignas(4096) VUsed used;
};

alignas(4096) QueueMem g_q;
alignas(4096) uint8_t g_bufs[kRing][kChunkBytes];

RandomVector g_rv;
uint64_t g_notify = 0;
uint32_t g_next_slot = 0;
uint16_t g_used_last = 0;

uint64_t phys(const volatile void *p) {
    return reinterpret_cast<uint64_t>(p) & ~index::kHighHalfBase;
}

void zero(void *p, uint32_t n) {
    auto *b = static_cast<uint8_t *>(p);
    for (uint32_t i = 0; i < n; ++i) b[i] = 0;
}

uint64_t setup_queue(uint64_t common, uint64_t notify_base, uint32_t notify_mult) {
    write16(common + kQueueSelect, 0);
    write16(common + kQueueSize, uint16_t(kRing));
    write32(common + kQueueDesc, uint32_t(phys(g_q.desc)));
    write32(common + kQueueDesc + 4, uint32_t(phys(g_q.desc) >> 32));
    write32(common + kQueueDriver, uint32_t(phys(&g_q.avail)));
    write32(common + kQueueDriver + 4, uint32_t(phys(&g_q.avail) >> 32));
    write32(common + kQueueDevice, uint32_t(phys(&g_q.used)));
    write32(common + kQueueDevice + 4, uint32_t(phys(&g_q.used) >> 32));
    const uint16_t off = read16(common + kQueueNotifyOff);
    write16(common + kQueueEnable, 1);
    return notify_base + uint64_t(off) * notify_mult;
}

// Request `len` bytes (<= kChunkBytes) and busy-wait for the device to fill.
uint32_t request_chunk(uint8_t *out, uint32_t len) {
    if (!g_rv.present || len == 0) return 0;
    if (len > kChunkBytes) len = kChunkBytes;
    const uint32_t slot = g_next_slot % kRing;
    g_next_slot = g_next_slot + 1;
    g_q.desc[slot].addr = phys(g_bufs[slot]);
    g_q.desc[slot].len = len;
    g_q.desc[slot].flags = kDescWrite;
    g_q.desc[slot].next = 0;
    g_q.avail.ring[g_q.avail.idx % kRing] = uint16_t(slot);
    asm volatile("dsb sy" ::: "memory");
    g_q.avail.idx = uint16_t(g_q.avail.idx + 1);
    asm volatile("dsb sy" ::: "memory");
    write16(g_notify, 0);
    // Spin (bounded) for the device's used update.
    for (uint64_t spin = 0; spin < 50000000ULL; ++spin) {
        asm volatile("dsb sy" ::: "memory");
        if (g_q.used.idx != g_used_last) {
            const VUsedElem &e = g_q.used.ring[g_used_last % kRing];
            g_used_last = uint16_t(g_used_last + 1);
            uint32_t got = e.len;
            if (got > len) got = len;
            for (uint32_t i = 0; i < got; ++i) out[i] = g_bufs[slot][i];
            return got;
        }
    }
    return 0; // device didn't answer
}

} // namespace

RandomVector random_vector_probe() {
    g_rv = RandomVector{};
    zero(&g_q, sizeof(g_q));
    PciAddr a = underground_find(kVirtioVendor, kVirtioRngLegacyId, kVirtioRngModernId);
    if (!a.valid) return g_rv;
    VirtioPciCfg cfg = underground_virtio_cfg(a);
    if (!cfg.valid) return g_rv;

    write8(cfg.common + kDeviceStatus, 0);
    write8(cfg.common + kDeviceStatus, kStatusAck);
    write8(cfg.common + kDeviceStatus, kStatusAck | kStatusDriver);
    // No features to negotiate; just confirm zero feature word.
    write32(cfg.common + kDriverFeatureSel, 0);
    write32(cfg.common + kDriverFeature, 0);
    write8(cfg.common + kDeviceStatus, kStatusAck | kStatusDriver | kStatusFeaturesOk);

    g_notify = setup_queue(cfg.common, cfg.notify, cfg.notify_mult);
    write8(cfg.common + kDeviceStatus,
           kStatusAck | kStatusDriver | kStatusFeaturesOk | kStatusDriverOk);

    g_rv.present = true;
    return g_rv;
}

const RandomVector &random_vector_status() { return g_rv; }

uint32_t random_vector_read(uint8_t *buf, uint32_t len) {
    if (buf == nullptr || len == 0 || !g_rv.present) return 0;
    uint32_t done = 0;
    while (done < len) {
        const uint32_t req = (len - done > kChunkBytes) ? kChunkBytes : (len - done);
        const uint32_t got = request_chunk(buf + done, req);
        if (got == 0) break;
        done += got;
    }
    g_rv.bytes_drawn += done;
    return done;
}

void random_vector_report() {
    if (!g_rv.present) {
        district::writeln("RandomVector: no virtio-rng.");
        return;
    }
    district::write("RandomVector: virtio-rng ready, ");
    district::dec(g_rv.bytes_drawn);
    district::writeln(" bytes drawn");
}

} // namespace index::drivers
