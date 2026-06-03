#include "drivers/tsuchimikado.hpp"

#include "drivers/underground.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/misaka_network.hpp"
#include "index/mmio.hpp"
#include "index/teleport.hpp"

namespace index::drivers {

namespace district = index::imaginary_number_district;

namespace {

using index::mmio::read8;
using index::mmio::read16;
using index::mmio::read32;
using index::mmio::write8;
using index::mmio::write16;
using index::mmio::write32;

// virtio-pci common-config offsets (mirrors random_vector / misaka_mail).
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
constexpr uint16_t kVirtioConsoleLegacyId = 0x1003;
constexpr uint16_t kVirtioConsoleModernId = 0x1043;

// virtio-console device-config layout (per spec 1.1 section 5.3.4):
//   le16 cols, le16 rows, le32 max_nr_ports, le32 emerg_wr.
constexpr uint64_t kDevMaxNrPorts = 4;

// VIRTIO_CONSOLE_F_MULTIPORT (bit 1, feature word 0) lets the device offer
// more than one port and exposes the control queues. UTM's virtserialport
// configuration always negotiates it.
constexpr uint32_t kFeatMultiport = 1u << 1;
constexpr uint32_t kFeatEventIdx = 1u << 29;

// Control message (struct virtio_console_control), little-endian.
struct __attribute__((packed)) VConsoleCtrl {
    uint32_t id;
    uint16_t event;
    uint16_t value;
};

constexpr uint16_t kEvtDeviceReady = 3; // guest -> device, value=1 means "I'm here"
constexpr uint16_t kEvtPortAdd = 1;     // device -> guest
constexpr uint16_t kEvtPortRemove = 2;  // device -> guest
constexpr uint16_t kEvtPortReady = 0;   // guest -> device, value=1 = ready to use
constexpr uint16_t kEvtPortOpen = 4;    // bidirectional, value=1 = open
constexpr uint16_t kEvtPortName = 6;    // device -> guest, name string follows the struct

// Each ring is small -- this driver is one-shot for the QGA handshake, no
// throughput target.
constexpr uint16_t kDescWrite = 2;
constexpr uint32_t kRing = 8;
constexpr uint32_t kCtrlMsgSize = 64; // ctrl struct + optional name payload
constexpr uint32_t kMaxQueues = 16;   // 2 per port + 2 control; UTM has 1 port

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

struct Queue {
    QueueMem ring;
    uint8_t buf[kRing][kCtrlMsgSize];
    uint64_t notify_addr = 0;
    uint16_t last_used = 0;
    uint16_t next_avail = 0;
};

alignas(4096) Queue g_q[kMaxQueues];

Tsuchimikado g_state;

uint64_t phys(const volatile void *p) {
    return reinterpret_cast<uint64_t>(p) & ~index::kHighHalfBase;
}

void zero(void *p, uint32_t n) {
    auto *b = static_cast<uint8_t *>(p);
    for (uint32_t i = 0; i < n; ++i) b[i] = 0;
}

uint64_t setup_queue(uint64_t common, uint64_t notify_base, uint32_t notify_mult,
                     uint16_t idx, Queue &q) {
    write16(common + kQueueSelect, idx);
    write16(common + kQueueSize, uint16_t(kRing));
    write32(common + kQueueDesc, uint32_t(phys(q.ring.desc)));
    write32(common + kQueueDesc + 4, uint32_t(phys(q.ring.desc) >> 32));
    write32(common + kQueueDriver, uint32_t(phys(&q.ring.avail)));
    write32(common + kQueueDriver + 4, uint32_t(phys(&q.ring.avail) >> 32));
    write32(common + kQueueDevice, uint32_t(phys(&q.ring.used)));
    write32(common + kQueueDevice + 4, uint32_t(phys(&q.ring.used) >> 32));
    const uint16_t off = read16(common + kQueueNotifyOff);
    write16(common + kQueueEnable, 1);
    // EVENT_IDX-style permanent IRQ suppression: we poll the rings
    // synchronously during probe, never want IRQs. See misaka_mail comments.
    q.ring.avail.used_event = uint16_t(q.ring.used.idx - 1);
    q.last_used = q.ring.used.idx;
    return notify_base + uint64_t(off) * notify_mult;
}

// Post an RX buffer in queue `idx` at descriptor `slot`. The device will write
// up to kCtrlMsgSize bytes into it.
void post_rx(uint16_t qidx, uint32_t slot) {
    Queue &q = g_q[qidx];
    q.ring.desc[slot].addr = phys(q.buf[slot]);
    q.ring.desc[slot].len = kCtrlMsgSize;
    q.ring.desc[slot].flags = kDescWrite;
    q.ring.desc[slot].next = 0;
    q.ring.avail.ring[q.ring.avail.idx % kRing] = uint16_t(slot);
    asm volatile("dsb sy" ::: "memory");
    q.ring.avail.idx = uint16_t(q.ring.avail.idx + 1);
}

// Send a control message (no payload) on the control TX queue.
void send_ctrl(uint32_t port_id, uint16_t event, uint16_t value) {
    constexpr uint16_t kCtrlTx = 3;
    Queue &q = g_q[kCtrlTx];
    const uint32_t slot = q.next_avail % kRing;
    q.next_avail = uint16_t(q.next_avail + 1);
    auto *msg = reinterpret_cast<VConsoleCtrl *>(q.buf[slot]);
    msg->id = port_id;
    msg->event = event;
    msg->value = value;
    q.ring.desc[slot].addr = phys(q.buf[slot]);
    q.ring.desc[slot].len = sizeof(VConsoleCtrl);
    q.ring.desc[slot].flags = 0; // read by device
    q.ring.desc[slot].next = 0;
    q.ring.avail.ring[q.ring.avail.idx % kRing] = uint16_t(slot);
    asm volatile("dsb sy" ::: "memory");
    q.ring.avail.idx = uint16_t(q.ring.avail.idx + 1);
    asm volatile("dsb sy" ::: "memory");
    write16(q.notify_addr, kCtrlTx);
}

// Drain the control RX queue and reply with PORT_READY / PORT_OPEN per the
// virtio-console handshake. Returns the number of events processed.
uint32_t pump_ctrl_rx() {
    constexpr uint16_t kCtrlRx = 2;
    Queue &q = g_q[kCtrlRx];
    uint32_t handled = 0;
    while (q.ring.used.idx != q.last_used) {
        const VUsedElem &e = q.ring.used.ring[q.last_used % kRing];
        q.last_used = uint16_t(q.last_used + 1);
        const uint32_t slot = e.id % kRing;
        if (e.len >= sizeof(VConsoleCtrl)) {
            const auto *msg = reinterpret_cast<const VConsoleCtrl *>(q.buf[slot]);
            const uint32_t port_id = msg->id;
            switch (msg->event) {
            case kEvtPortAdd:
                send_ctrl(port_id, kEvtPortReady, 1);
                break;
            case kEvtPortName:
                // ignored -- we don't care which name, we ack all ports.
                break;
            case kEvtPortOpen:
                // Host opened the port; mirror the state so QGA / vdagent on
                // the host see a fully connected channel and stop polling.
                if (msg->value != 0) send_ctrl(port_id, kEvtPortOpen, 1);
                break;
            case kEvtPortRemove:
            default:
                break;
            }
            ++handled;
        }
        // Recycle the buffer so the device can write the next event.
        post_rx(kCtrlRx, slot);
        // Re-arm the used_event watermark to "current - 1" so the device
        // continues to skip IRQs on RX.
        q.ring.avail.used_event = uint16_t(q.ring.used.idx - 1);
    }
    if (handled > 0) {
        write16(q.notify_addr, kCtrlRx);
    }
    return handled;
}

// Per-port outgoing buffer for QGA replies. Reuses the queue's buf[] array
// for TX too (we never have both RX and TX traffic on the same slot
// simultaneously: RX slots are recycled via post_rx).
constexpr uint32_t kReplyCap = kCtrlMsgSize - 4;

// Find an unsigned-decimal integer in a JSON request, written after `"id":`.
// Returns false if not found.
bool find_id(const uint8_t *buf, uint32_t len, uint64_t *out) {
    const char *needle = "\"id\":";
    const uint32_t needle_len = 5;
    for (uint32_t i = 0; i + needle_len < len; ++i) {
        bool match = true;
        for (uint32_t k = 0; k < needle_len; ++k) {
            if (buf[i + k] != static_cast<uint8_t>(needle[k])) { match = false; break; }
        }
        if (!match) continue;
        uint32_t j = i + needle_len;
        while (j < len && (buf[j] == ' ' || buf[j] == '\t')) ++j;
        if (j >= len || buf[j] < '0' || buf[j] > '9') return false;
        uint64_t v = 0;
        while (j < len && buf[j] >= '0' && buf[j] <= '9') {
            v = v * 10 + (buf[j] - '0');
            ++j;
        }
        *out = v;
        return true;
    }
    return false;
}

uint32_t emit_dec(uint8_t *out, uint64_t v) {
    char tmp[24];
    uint32_t n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v > 0) { tmp[n++] = char('0' + v % 10); v /= 10; } }
    for (uint32_t i = 0; i < n; ++i) out[i] = uint8_t(tmp[n - 1 - i]);
    return n;
}

// Send a JSON reply on port `port_id` (the per-port TX queue is at index
// 4 + 2*(port_id-1) + 1 for ports > 0). For port 0 (console), TX is queue 1.
void send_port_reply(uint32_t port_id, const uint8_t *payload, uint32_t plen) {
    const uint16_t qidx = (port_id == 0) ? 1 : uint16_t(4 + 2 * (port_id - 1) + 1);
    if (qidx >= kMaxQueues) return;
    Queue &q = g_q[qidx];
    if (q.notify_addr == 0) return;
    const uint32_t slot = q.next_avail % kRing;
    q.next_avail = uint16_t(q.next_avail + 1);
    if (plen > kCtrlMsgSize) plen = kCtrlMsgSize;
    for (uint32_t i = 0; i < plen; ++i) q.buf[slot][i] = payload[i];
    q.ring.desc[slot].addr = phys(q.buf[slot]);
    q.ring.desc[slot].len = plen;
    q.ring.desc[slot].flags = 0; // device reads from us
    q.ring.desc[slot].next = 0;
    q.ring.avail.ring[q.ring.avail.idx % kRing] = uint16_t(slot);
    asm volatile("dsb sy" ::: "memory");
    q.ring.avail.idx = uint16_t(q.ring.avail.idx + 1);
    asm volatile("dsb sy" ::: "memory");
    write16(q.notify_addr, qidx);
}

// React to QGA JSON on a per-port RX. Three commands are commonly issued by
// QEMU monitor / libvirt as keepalives:
//   {"execute":"guest-ping"}                       -> {"return":{}}
//   {"execute":"guest-sync","arguments":{"id":N}}  -> {"return":N}
//   {"execute":"guest-sync-delimited",...}         -> \xff{"return":N}
// We answer all three (only the actually-sent one matters for any given line).
void handle_qga_line(uint32_t port_id, const uint8_t *line, uint32_t len) {
    uint8_t reply[kReplyCap];
    uint32_t r = 0;
    uint64_t id = 0;
    const bool has_id = find_id(line, len, &id);
    // Look for "guest-sync-delimited" -- if present, lead the reply with \xff.
    const char *delim = "guest-sync-delimited";
    bool is_delimited = false;
    for (uint32_t i = 0; i + 20 < len; ++i) {
        bool match = true;
        for (uint32_t k = 0; k < 20; ++k) {
            if (line[i + k] != static_cast<uint8_t>(delim[k])) { match = false; break; }
        }
        if (match) { is_delimited = true; break; }
    }
    if (is_delimited) reply[r++] = 0xFF;
    const char *p = has_id ? "{\"return\":" : "{\"return\":{}}";
    while (*p && r < kReplyCap) reply[r++] = uint8_t(*p++);
    if (has_id) {
        r += emit_dec(reply + r, id);
        if (r < kReplyCap) reply[r++] = '}';
    }
    if (r < kReplyCap) reply[r++] = '\n';
    send_port_reply(port_id, reply, r);
}

// Drain RX from a per-port queue. Returns events handled. Splits buffer into
// lines on '\n' and dispatches each line to handle_qga_line.
uint32_t pump_port_rx(uint16_t qidx, uint32_t port_id) {
    if (qidx >= kMaxQueues) return 0;
    Queue &q = g_q[qidx];
    if (q.notify_addr == 0) return 0;
    uint32_t handled = 0;
    while (q.ring.used.idx != q.last_used) {
        const VUsedElem &e = q.ring.used.ring[q.last_used % kRing];
        q.last_used = uint16_t(q.last_used + 1);
        const uint32_t slot = e.id % kRing;
        if (e.len > 0) {
            const uint8_t *buf = q.buf[slot];
            uint32_t len = e.len;
            if (len > kCtrlMsgSize) len = kCtrlMsgSize;
            // Split on newline so multi-line input gets one answer per command.
            uint32_t start = 0;
            for (uint32_t i = 0; i < len; ++i) {
                if (buf[i] == '\n') {
                    if (i > start) handle_qga_line(port_id, buf + start, i - start);
                    start = i + 1;
                }
            }
            if (start < len) handle_qga_line(port_id, buf + start, len - start);
        }
        post_rx(qidx, slot);
        q.ring.avail.used_event = uint16_t(q.ring.used.idx - 1);
        ++handled;
    }
    if (handled > 0) write16(q.notify_addr, qidx);
    return handled;
}

void agent_entry(void *) {
    // Continuous service loop: drain control + per-port traffic, then sleep
    // until something happens. With no work, this Sister blocks instead of
    // spinning -- the misaka scheduler runs idle_entry meanwhile.
    for (;;) {
        uint32_t did = tsuchimikado_pump();
        if (did == 0) {
            misaka_network_sleep(2); // ~20 ms
        }
    }
}

} // namespace

Tsuchimikado tsuchimikado_probe() {
    g_state = Tsuchimikado{};
    zero(g_q, sizeof(g_q));
    PciAddr a = underground_find(kVirtioVendor, kVirtioConsoleLegacyId, kVirtioConsoleModernId);
    if (!a.valid) return g_state;
    VirtioPciCfg cfg = underground_virtio_cfg(a);
    if (!cfg.valid || cfg.device == 0) return g_state;

    write8(cfg.common + kDeviceStatus, 0);
    write8(cfg.common + kDeviceStatus, kStatusAck);
    write8(cfg.common + kDeviceStatus, kStatusAck | kStatusDriver);

    // Accept VIRTIO_F_VERSION_1 (bit 32), MULTIPORT, EVENT_IDX.
    write32(cfg.common + kDriverFeatureSel, 1);
    write32(cfg.common + kDriverFeature, 1u);
    write32(cfg.common + kDriverFeatureSel, 0);
    write32(cfg.common + kDriverFeature, kFeatMultiport | kFeatEventIdx);
    write8(cfg.common + kDeviceStatus, kStatusAck | kStatusDriver | kStatusFeaturesOk);
    if ((read8(cfg.common + kDeviceStatus) & kStatusFeaturesOk) == 0) {
        return g_state;
    }

    const uint32_t max_ports = read32(cfg.device + kDevMaxNrPorts);
    g_state.num_ports = max_ports;
    // Queue layout with MULTIPORT (spec 5.3.2):
    //   vq 0 = port 0 RX, vq 1 = port 0 TX,
    //   vq 2 = ctrl RX,   vq 3 = ctrl TX,
    //   vq 4+2k = port (k+1) RX, vq 5+2k = port (k+1) TX.
    const uint32_t total_queues = 4 + 2 * (max_ports > 1 ? max_ports - 1 : 0);
    const uint32_t setup_count = total_queues > kMaxQueues ? kMaxQueues : total_queues;
    for (uint32_t i = 0; i < setup_count; ++i) {
        g_q[i].notify_addr = setup_queue(cfg.common, cfg.notify, cfg.notify_mult,
                                         uint16_t(i), g_q[i]);
    }
    write8(cfg.common + kDeviceStatus,
           kStatusAck | kStatusDriver | kStatusFeaturesOk | kStatusDriverOk);

    // Pre-post buffers in the control RX queue and every per-port RX queue
    // so the device can deliver PORT_ADD / PORT_NAME etc. without backing up.
    constexpr uint16_t kCtrlRx = 2;
    for (uint32_t s = 0; s < kRing; ++s) post_rx(kCtrlRx, s);
    write16(g_q[kCtrlRx].notify_addr, kCtrlRx);
    // Per-port RX queues 4, 6, 8, ... -- each port (k+1) has RX at 4+2k.
    for (uint32_t k = 1; k < max_ports && (4 + 2 * (k - 1) + 1) < setup_count; ++k) {
        const uint32_t qidx = 4 + 2 * (k - 1);
        for (uint32_t s = 0; s < kRing; ++s) post_rx(uint16_t(qidx), s);
        write16(g_q[qidx].notify_addr, qidx);
    }

    // Tell the device we're here. This is the trigger for PORT_ADD events on
    // the control queue.
    send_ctrl(0xFFFFFFFFu, kEvtDeviceReady, 1);

    // Pump the initial PORT_ADD / PORT_NAME / PORT_OPEN events. QEMU under
    // HVF answers in microseconds, so even a quiet stretch of 5M cycles
    // strongly implies the device has finished its initial announcement.
    // The agent Sister (spawned later) handles anything that lands after.
    g_state.present = true;
    uint64_t quiet = 0;
    for (uint64_t spin = 0; spin < 20000000ULL; ++spin) {
        asm volatile("dsb sy" ::: "memory");
        if (pump_ctrl_rx() != 0) {
            quiet = 0;
        } else if (++quiet > 200000ULL) {
            break;
        }
    }
    return g_state;
}

const Tsuchimikado &tsuchimikado_status() { return g_state; }

uint32_t tsuchimikado_pump() {
    if (!g_state.present) return 0;
    uint32_t handled = pump_ctrl_rx();
    // Each port (id >= 1) has its own RX queue at 4 + 2*(id-1).
    for (uint32_t pid = 1; pid < g_state.num_ports; ++pid) {
        const uint32_t qidx = 4 + 2 * (pid - 1);
        if (qidx >= kMaxQueues) break;
        handled += pump_port_rx(uint16_t(qidx), pid);
    }
    return handled;
}

void tsuchimikado_start() {
    if (!g_state.present) return;
    misaka_network_spawn_named(agent_entry, nullptr, "tsuchimikado");
}

} // namespace index::drivers
