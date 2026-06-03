#include "drivers/misaka_mail.hpp"

#include "drivers/aleister.hpp"
#include "drivers/underground.hpp"
#include "index/antenna.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/last_order.hpp"
#include "index/mmio.hpp"
#include "index/teleport.hpp"

namespace index::drivers {

namespace district = index::imaginary_number_district;

namespace {

using index::mmio::read16;
using index::mmio::read32;
using index::mmio::read8;
using index::mmio::write16;
using index::mmio::write32;
using index::mmio::write8;

// --- virtio common-config offsets (same block as virtio-blk) ---
constexpr uint64_t kDriverFeatureSel = 0x08;
constexpr uint64_t kDriverFeature = 0x0c;
constexpr uint64_t kDeviceStatus = 0x14;   // u8
constexpr uint64_t kQueueSelect = 0x16;    // u16
constexpr uint64_t kQueueSize = 0x18;      // u16
constexpr uint64_t kQueueEnable = 0x1c;    // u16
constexpr uint64_t kQueueNotifyOff = 0x1e; // u16
constexpr uint64_t kQueueDesc = 0x20;      // u64
constexpr uint64_t kQueueDriver = 0x28;    // u64
constexpr uint64_t kQueueDevice = 0x30;    // u64

constexpr uint8_t kStatusAck = 1;
constexpr uint8_t kStatusDriver = 2;
constexpr uint8_t kStatusDriverOk = 4;
constexpr uint8_t kStatusFeaturesOk = 8;

constexpr uint16_t kVirtioVendor = 0x1af4;
constexpr uint16_t kVirtioNetLegacyId = 0x1000;
constexpr uint16_t kVirtioNetModernId = 0x1041;
constexpr uint32_t kFeatMac = 1u << 5; // VIRTIO_NET_F_MAC (feature word 0)
constexpr uint32_t kFeatEventIdx = 1u << 29; // VIRTIO_RING_F_EVENT_IDX (word 0)

constexpr uint16_t kDescWrite = 2;

constexpr uint32_t kRing = 16;       // entries per queue (both RX and TX)
constexpr uint32_t kBufSize = 2048;  // per packet buffer (net hdr + frame)
constexpr uint32_t kNetHdr = 12;     // virtio_net_hdr (VERSION_1, always 12 bytes)
constexpr uint32_t kRxVq = 0;
constexpr uint32_t kTxVq = 1;

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

// Each queue's rings: desc + avail share a page (both small, correctly aligned);
// used gets its own page so it meets virtio 1.0's alignment on a separate addr.
struct QueueMem {
    alignas(4096) VDesc desc[kRing];
    VAvail avail;
    alignas(4096) VUsed used;
};

alignas(4096) QueueMem g_rx;
alignas(4096) QueueMem g_tx;
alignas(4096) uint8_t g_rxbuf[kRing][kBufSize];
alignas(4096) uint8_t g_txbuf[kRing][kBufSize];

uint16_t g_rx_last = 0;
uint16_t g_tx_last = 0;
uint32_t g_tx_next = 0;
uint16_t g_icmp_seq = 0;

uint64_t g_rx_notify = 0;
uint64_t g_tx_notify = 0;
uint64_t g_isr = 0; // virtio ISR status reg: read to ack/deassert the INTx line

MisakaMail g_mail;

// QEMU 'virt' routes the 4 PCIe INTx pins (INTA..D) to GIC SPIs 3..6, i.e.
// INTIDs 35..38. We don't know which pin this NIC was swizzled onto, so we
// enable and handle all four; only ours ever fires (the disk is virtio-mmio,
// not PCIe, so nothing else shares these lines here).
constexpr uint32_t kPciIntxBase = 35;
constexpr uint32_t kPciIntxCount = 4;
// Master switch for the legacy-INTx RX interrupt. OFF: hangs probe on Apple
// HVF (real GICv3). See the long note at the enable site in misaka_mail_probe.
constexpr bool kRxIrq = false;

// Legacy-INTx ISR handler. INTx is level-triggered, so we must read the virtio
// ISR status register to deassert the line before EOI or it re-fires forever.
// The actual RX/TX ring draining happens in the after-EOI hook (network_tick),
// which runs on every IRQ; here we only acknowledge the device.
void net_irq(void *) {
    if (g_isr != 0) {
        (void)read8(g_isr); // reading clears ISR + drops INTx
    }
}
uint8_t g_gw_mac[6] = {};
bool g_gw_known = false;

// UDP port subscribers for kernel clients (DHCP) that don't go through the
// Antenna fd table. handle_inbound consults this when no Antenna matched.
struct UdpSub { uint16_t port; MisakaUdpCallback cb; void *cookie; };
UdpSub g_udp_subs[4] = {};

// Device DMA uses guest-physical addresses; our pointers are high-half VAs whose
// low 39 bits equal the physical address (same trick as Underline).
uint64_t phys(const volatile void *p) {
    return reinterpret_cast<uint64_t>(p) & ~index::kHighHalfBase;
}

void zero(void *p, uint32_t n) {
    auto *b = static_cast<uint8_t *>(p);
    for (uint32_t i = 0; i < n; ++i) {
        b[i] = 0;
    }
}

void copy(void *d, const void *s, uint32_t n) {
    auto *dst = static_cast<uint8_t *>(d);
    const auto *src = static_cast<const uint8_t *>(s);
    for (uint32_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
}

bool ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// --- big-endian (network order) writers/readers into a byte buffer ---
void wbe16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
uint16_t rbe16(const uint8_t *p) {
    return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}

// Internet checksum (RFC 1071): ones-complement sum of 16-bit big-endian words.
uint16_t inet_csum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        sum += rbe16(data + i);
    }
    if (len & 1) {
        sum += uint16_t(data[len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return uint16_t(~sum);
}

const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr uint16_t kEtherArp = 0x0806;
constexpr uint16_t kEtherIpv4 = 0x0800;
constexpr uint8_t kProtoIcmp = 1;
constexpr uint8_t kProtoUdp = 17;
constexpr uint8_t kProtoTcp = 6;

void wbe32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
uint32_t rbe32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// --- virtqueue plumbing ---

// Select queue `vq`, point the device at our rings, enable it, and return the
// absolute notify address for it.
uint64_t setup_queue(uint64_t common, uint64_t notify_base, uint32_t notify_mult,
                     uint32_t vq, QueueMem &q) {
    write16(common + kQueueSelect, uint16_t(vq));
    write16(common + kQueueSize, uint16_t(kRing));
    write32(common + kQueueDesc, uint32_t(phys(q.desc)));
    write32(common + kQueueDesc + 4, uint32_t(phys(q.desc) >> 32));
    write32(common + kQueueDriver, uint32_t(phys(&q.avail)));
    write32(common + kQueueDriver + 4, uint32_t(phys(&q.avail) >> 32));
    write32(common + kQueueDevice, uint32_t(phys(&q.used)));
    write32(common + kQueueDevice + 4, uint32_t(phys(&q.used) >> 32));
    const uint16_t off = read16(common + kQueueNotifyOff);
    write16(common + kQueueEnable, 1);
    return notify_base + uint64_t(off) * notify_mult;
}

// Hand one RX buffer back to the device (device-writable) and make it available.
void rx_post(uint32_t slot) {
    g_rx.desc[slot].addr = phys(g_rxbuf[slot]);
    g_rx.desc[slot].len = kBufSize;
    g_rx.desc[slot].flags = kDescWrite;
    g_rx.desc[slot].next = 0;
    g_rx.avail.ring[g_rx.avail.idx % kRing] = uint16_t(slot);
    asm volatile("dsb sy" ::: "memory");
    g_rx.avail.idx = uint16_t(g_rx.avail.idx + 1);
    asm volatile("dsb sy" ::: "memory");
}

// Transmit one ethernet frame (we prepend the zeroed virtio_net_hdr).
void net_tx(const uint8_t *frame, uint32_t len) {
    if (!g_mail.present || len + kNetHdr > kBufSize) {
        return;
    }
    const uint32_t slot = g_tx_next % kRing;
    g_tx_next = g_tx_next + 1;
    uint8_t *buf = g_txbuf[slot];
    zero(buf, kNetHdr);
    copy(buf + kNetHdr, frame, len);

    g_tx.desc[slot].addr = phys(buf);
    g_tx.desc[slot].len = kNetHdr + len;
    g_tx.desc[slot].flags = 0; // device reads it; no NEXT, no WRITE
    g_tx.desc[slot].next = 0;
    g_tx.avail.ring[g_tx.avail.idx % kRing] = uint16_t(slot);
    asm volatile("dsb sy" ::: "memory");
    g_tx.avail.idx = uint16_t(g_tx.avail.idx + 1);
    asm volatile("dsb sy" ::: "memory");
    write16(g_tx_notify, uint16_t(kTxVq));
    ++g_mail.tx_frames;

    // Reclaim (bounded) so slots are reusable for the next packet.
    for (uint64_t spin = 0; spin < 50000000ULL; ++spin) {
        asm volatile("dsb sy" ::: "memory");
        if (g_tx.used.idx != g_tx_last) {
            g_tx_last = g_tx.used.idx;
            break;
        }
    }
}

// Poll the RX ring for a frame, up to `timeout` LastOrder ticks. On success copies
// the ethernet frame (past the net hdr) into `out`, sets *len, recycles the buffer.
// `yield`s every iteration: when called from a syscall handler IRQs are masked
// (so last_order_ticks() is frozen and we'd spin forever), and the yield gives
// QEMU's iothread a chance to deliver virtio packets from SLIRP.
bool net_rx_poll(uint8_t *out, uint32_t cap, uint32_t *len, uint64_t timeout) {
    const uint64_t start = last_order_ticks();
    const uint64_t deadline = start + timeout;
    // Bound the spin in case the ticks counter is frozen (svc with IRQs masked):
    // ~50M iterations is roughly the same wall-clock budget as the tick deadline
    // on TCG.
    uint64_t spins = 0;
    constexpr uint64_t kSpinCap = 50ULL * 1000 * 1000;
    while (last_order_ticks() <= deadline && spins < kSpinCap * (timeout + 1)) {
        asm volatile("yield" ::: "memory"); // hint to QEMU's TCG vCPU scheduler
        asm volatile("dsb sy" ::: "memory");
        ++spins;
        if (g_rx.used.idx == g_rx_last) {
            continue;
        }
        // Acquire after observing the advanced used.idx, before reading the
        // ring entry + buffer (== dma_rmb(); see misaka_mail_drain). The
        // pre-existing dsb sy above sits before the used.idx test, so it does
        // not order that load ahead of these -- this is the barrier that does.
        asm volatile("dmb oshld" ::: "memory");
        const VUsedElem &e = g_rx.used.ring[g_rx_last % kRing];
        const uint32_t slot = e.id % kRing;
        uint32_t total = e.len;
        g_rx_last = uint16_t(g_rx_last + 1);

        uint32_t n = 0;
        if (total > kNetHdr) {
            n = total - kNetHdr;
            if (n > cap) {
                n = cap;
            }
            copy(out, g_rxbuf[slot] + kNetHdr, n);
        }
        *len = n;
        rx_post(slot); // recycle the buffer
        // Re-arm the RX interrupt watermark (kRxIrq) or keep it suppressed.
        g_rx.avail.used_event = uint16_t(kRxIrq ? g_rx.used.idx : g_rx.used.idx - 1);
        write16(g_rx_notify, uint16_t(kRxVq));
        ++g_mail.rx_frames;
        return true;
    }
    return false;
}

// --- protocol builders (write into `f`, return total ethernet frame length) ---

uint32_t build_eth(uint8_t *f, const uint8_t *dst, uint16_t ethertype) {
    copy(f, dst, 6);
    copy(f + 6, g_mail.mac, 6);
    wbe16(f + 12, ethertype);
    return 14;
}

// ARP (opcode 1=request, 2=reply). For a request, target_mac is ignored/zero.
uint32_t build_arp(uint8_t *f, uint16_t op, const uint8_t *target_mac,
                   const uint8_t *target_ip) {
    const uint8_t *eth_dst = (op == 1) ? kBroadcast : target_mac;
    uint32_t o = build_eth(f, eth_dst, kEtherArp);
    wbe16(f + o + 0, 1);         // htype = Ethernet
    wbe16(f + o + 2, kEtherIpv4); // ptype = IPv4
    f[o + 4] = 6;                // hlen
    f[o + 5] = 4;                // plen
    wbe16(f + o + 6, op);
    copy(f + o + 8, g_mail.mac, 6);  // sender mac
    copy(f + o + 14, g_mail.ip, 4);  // sender ip
    copy(f + o + 18, (op == 1) ? (const uint8_t *)"\0\0\0\0\0\0" : target_mac, 6);
    copy(f + o + 24, target_ip, 4);  // target ip
    return o + 28;
}

// IPv4 + ICMP echo. `icmp_type` 8=request, 0=reply. Returns frame length.
uint32_t build_icmp(uint8_t *f, const uint8_t *dst_mac, const uint8_t *dst_ip,
                    uint8_t icmp_type, uint16_t id, uint16_t seq,
                    const uint8_t *payload, uint32_t payload_len) {
    uint32_t o = build_eth(f, dst_mac, kEtherIpv4);
    uint8_t *ip = f + o;
    const uint32_t icmp_len = 8 + payload_len;
    const uint32_t ip_total = 20 + icmp_len;
    zero(ip, 20);
    ip[0] = 0x45;          // version 4, IHL 5
    wbe16(ip + 2, uint16_t(ip_total));
    wbe16(ip + 4, 0x0000); // id
    ip[8] = 64;            // TTL
    ip[9] = kProtoIcmp;
    copy(ip + 12, g_mail.ip, 4);
    copy(ip + 16, dst_ip, 4);
    wbe16(ip + 10, inet_csum(ip, 20));

    uint8_t *icmp = ip + 20;
    zero(icmp, 8);
    icmp[0] = icmp_type; // 8 request / 0 reply
    icmp[1] = 0;         // code
    wbe16(icmp + 4, id);
    wbe16(icmp + 6, seq);
    if (payload_len) {
        copy(icmp + 8, payload, payload_len);
    }
    wbe16(icmp + 2, inet_csum(icmp, icmp_len));
    return o + ip_total;
}

// One-shot IP pseudo-header + protocol-header + payload checksum. RFC 793
// requires this for TCP (RFC 768 makes it optional for UDP, which we do skip).
// `proto` is kProtoTcp here; src_ip/dst_ip from the IP header; seg_len is the
// transport-layer length (header + payload). Walks the pseudo-header
// {src_ip(4), dst_ip(4), 0, proto, len(2)} and then the segment buffer.
uint16_t pseudo_csum(const uint8_t *src_ip, const uint8_t *dst_ip,
                     uint8_t proto, uint16_t seg_len,
                     const uint8_t *seg, uint32_t seg_total) {
    uint32_t sum = 0;
    sum += rbe16(src_ip + 0); sum += rbe16(src_ip + 2);
    sum += rbe16(dst_ip + 0); sum += rbe16(dst_ip + 2);
    sum += proto;
    sum += seg_len;
    for (uint32_t i = 0; i + 1 < seg_total; i += 2) {
        sum += rbe16(seg + i);
    }
    if (seg_total & 1) {
        sum += uint16_t(seg[seg_total - 1] << 8);
    }
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return uint16_t(~sum);
}

// IPv4 + UDP with explicit source IP. Layout: eth(14) + ipv4(20) + udp(8) +
// payload. Checksum is left 0 (legal per RFC 768). DHCP uses this to send
// from 0.0.0.0 before we have a lease.
uint32_t build_udp_ex(uint8_t *f, const uint8_t *dst_mac, const uint8_t *src_ip,
                      const uint8_t *dst_ip, uint16_t src_port, uint16_t dst_port,
                      const uint8_t *payload, uint32_t plen) {
    uint32_t o = build_eth(f, dst_mac, kEtherIpv4);
    uint8_t *ip = f + o;
    const uint32_t udp_len = 8 + plen;
    const uint32_t ip_total = 20 + udp_len;
    zero(ip, 20);
    ip[0] = 0x45;
    wbe16(ip + 2, uint16_t(ip_total));
    wbe16(ip + 4, 0x0000); // id (not fragmenting)
    ip[8] = 64;            // TTL
    ip[9] = kProtoUdp;
    copy(ip + 12, src_ip, 4);
    copy(ip + 16, dst_ip, 4);
    wbe16(ip + 10, inet_csum(ip, 20));

    uint8_t *udp = ip + 20;
    wbe16(udp + 0, src_port);
    wbe16(udp + 2, dst_port);
    wbe16(udp + 4, uint16_t(udp_len));
    wbe16(udp + 6, 0); // checksum = 0 (optional for IPv4)
    if (plen) copy(udp + 8, payload, plen);
    return o + ip_total;
}

// Convenience: send with our current IP as source. The vast majority of
// callers want this; only DHCP overrides.
uint32_t build_udp(uint8_t *f, const uint8_t *dst_mac, const uint8_t *dst_ip,
                   uint16_t src_port, uint16_t dst_port,
                   const uint8_t *payload, uint32_t plen) {
    return build_udp_ex(f, dst_mac, g_mail.ip, dst_ip, src_port, dst_port,
                        payload, plen);
}

// IPv4 + TCP. Layout: eth(14) + ipv4(20) + tcp(20 with no options) + payload.
// The TCP checksum is mandatory and includes the IP pseudo-header; we compute
// it once after the segment is laid down. Returns the frame length.
uint32_t build_tcp(uint8_t *f, const uint8_t *dst_mac, const uint8_t *dst_ip,
                   uint16_t src_port, uint16_t dst_port, uint8_t flags,
                   uint32_t seq, uint32_t ack, uint16_t window,
                   const uint8_t *payload, uint32_t plen) {
    uint32_t o = build_eth(f, dst_mac, kEtherIpv4);
    uint8_t *ip = f + o;
    const uint32_t tcp_hdr = 20;
    const uint32_t tcp_total = tcp_hdr + plen;
    const uint32_t ip_total = 20 + tcp_total;
    zero(ip, 20);
    ip[0] = 0x45;
    wbe16(ip + 2, uint16_t(ip_total));
    wbe16(ip + 4, 0x0000);
    ip[8] = 64;
    ip[9] = kProtoTcp;
    copy(ip + 12, g_mail.ip, 4);
    copy(ip + 16, dst_ip, 4);
    wbe16(ip + 10, inet_csum(ip, 20));

    uint8_t *tcp = ip + 20;
    zero(tcp, 20);
    wbe16(tcp + 0, src_port);
    wbe16(tcp + 2, dst_port);
    wbe32(tcp + 4, seq);
    wbe32(tcp + 8, ack);
    tcp[12] = 0x50; // data offset = 5 (20 bytes), no options
    tcp[13] = flags;
    wbe16(tcp + 14, window); // receive window advertised by the caller
    wbe16(tcp + 16, 0);    // checksum filled below
    wbe16(tcp + 18, 0);    // urgent ptr
    if (plen) copy(tcp + 20, payload, plen);
    const uint16_t csum = pseudo_csum(g_mail.ip, dst_ip, kProtoTcp,
                                       uint16_t(tcp_total), tcp, tcp_total);
    wbe16(tcp + 16, csum);
    return o + ip_total;
}

bool same_subnet(const uint8_t *ip) {
    // Crude /24: same first three octets as us means on-link.
    return ip[0] == g_mail.ip[0] && ip[1] == g_mail.ip[1] && ip[2] == g_mail.ip[2];
}

// Handle an inbound frame for the responder role: answer ARP who-has-us and ICMP
// echo requests to us. Returns a short tag for logging, or nullptr if ignored.
const char *handle_inbound(const uint8_t *f, uint32_t len) {
    if (len < 14) {
        return nullptr;
    }
    const uint16_t eth = rbe16(f + 12);
    if (eth == kEtherArp && len >= 42) {
        const uint8_t *arp = f + 14;
        if (rbe16(arp + 6) == 1) { // request
            const uint8_t *tip = arp + 24;
            if (ip_eq(tip, g_mail.ip)) {
                uint8_t out[64];
                const uint8_t *sender_mac = arp + 8;
                const uint8_t *sender_ip = arp + 14;
                uint32_t n = build_arp(out, 2, sender_mac, sender_ip);
                net_tx(out, n);
                return "ARP request -> replied";
            }
        }
    } else if (eth == kEtherIpv4 && len >= 34) {
        const uint8_t *ip = f + 14;
        const uint32_t ihl = (ip[0] & 0x0f) * 4;
        if (ip[9] == kProtoIcmp && len >= 14 + ihl + 8) {
            const uint8_t *dst_ip = ip + 16;
            if (ip_eq(dst_ip, g_mail.ip)) {
                const uint8_t *icmp = ip + ihl;
                if (icmp[0] == 8) { // echo request
                    const uint8_t *src_ip = ip + 12;
                    const uint16_t id = rbe16(icmp + 4);
                    const uint16_t seq = rbe16(icmp + 6);
                    uint8_t out[1600];
                    uint32_t n = build_icmp(out, f + 6, src_ip, 0, id, seq,
                                            icmp + 8, rbe16(ip + 2) - ihl - 8);
                    net_tx(out, n);
                    return "ICMP echo -> replied";
                }
            }
        } else if (ip[9] == kProtoUdp && len >= 14 + ihl + 8) {
            const uint8_t *dst_ip = ip + 16;
            // Accept packets aimed at us or at the limited broadcast address
            // (255.255.255.255). The latter is required for DHCP OFFER/ACK
            // when the client doesn't yet have a unicast address.
            const bool to_us = ip_eq(dst_ip, g_mail.ip);
            const bool to_bcast = dst_ip[0] == 0xff && dst_ip[1] == 0xff &&
                                  dst_ip[2] == 0xff && dst_ip[3] == 0xff;
            if (to_us || to_bcast) {
                const uint8_t *src_ip = ip + 12;
                const uint8_t *udp = ip + ihl;
                const uint16_t src_port = rbe16(udp + 0);
                const uint16_t dst_port = rbe16(udp + 2);
                const uint16_t udp_total = rbe16(udp + 4);
                if (udp_total >= 8 && udp_total <= len - 14 - ihl) {
                    const uint8_t *payload = udp + 8;
                    const uint32_t payload_len = udp_total - 8;
                    if (antenna_deliver_udp(src_ip, src_port, dst_port,
                                            payload, payload_len)) {
                        return "UDP -> delivered";
                    }
                    // Fallback: a kernel client (DHCP) subscribed to this port.
                    for (auto &s : g_udp_subs) {
                        if (s.cb != nullptr && s.port == dst_port &&
                            s.cb(src_ip, src_port, payload, payload_len, s.cookie)) {
                            return "UDP -> subscriber";
                        }
                    }
                    return "UDP -> no socket";
                }
            }
        } else if (ip[9] == kProtoTcp && len >= 14 + ihl + 20) {
            const uint8_t *dst_ip = ip + 16;
            if (ip_eq(dst_ip, g_mail.ip)) {
                const uint8_t *src_ip = ip + 12;
                const uint8_t *tcp = ip + ihl;
                const uint16_t src_port = rbe16(tcp + 0);
                const uint16_t dst_port = rbe16(tcp + 2);
                const uint32_t seq = rbe32(tcp + 4);
                const uint32_t ack = rbe32(tcp + 8);
                const uint32_t data_off = (tcp[12] >> 4) * 4u; // bytes
                const uint8_t flags = tcp[13];
                if (data_off >= 20 && data_off <= len - 14 - ihl) {
                    const uint8_t *payload = tcp + data_off;
                    const uint32_t ip_total = rbe16(ip + 2);
                    const uint32_t payload_len = ip_total - ihl - data_off;
                    if (antenna_deliver_tcp(src_ip, src_port, dst_port, flags,
                                            seq, ack, payload, payload_len)) {
                        return "TCP -> delivered";
                    }
                    return "TCP -> no socket";
                }
            }
        }
    }
    return nullptr;
}

// Resolve `ip` to a MAC, sending an ARP request and waiting briefly. Caches the
// gateway. Returns false on timeout.
bool arp_resolve(const uint8_t *ip, uint8_t *mac_out) {
    if (g_gw_known && ip_eq(ip, g_mail.gateway)) {
        copy(mac_out, g_gw_mac, 6);
        return true;
    }
    uint8_t req[64];
    uint32_t n = build_arp(req, 1, nullptr, ip);
    net_tx(req, n);

    uint8_t frame[1600];
    uint32_t flen = 0;
    for (uint32_t tries = 0; tries < 100; ++tries) {
        if (!net_rx_poll(frame, sizeof(frame), &flen, 100)) {
            continue;
        }
        if (flen >= 42 && rbe16(frame + 12) == kEtherArp) {
            const uint8_t *arp = frame + 14;
            if (rbe16(arp + 6) == 2) { // reply
                const uint8_t *sip = arp + 14;
                if (ip_eq(sip, ip)) {
                    copy(mac_out, arp + 8, 6);
                    if (ip_eq(ip, g_mail.gateway)) {
                        copy(g_gw_mac, arp + 8, 6);
                        g_gw_known = true;
                    }
                    return true;
                }
            }
        }
        handle_inbound(frame, flen); // be a good neighbour while we wait
    }
    return false;
}

void print_mac(const uint8_t *m) {
    const char *hex = "0123456789abcdef";
    for (uint32_t i = 0; i < 6; ++i) {
        if (i) {
            district::putc(':');
        }
        district::putc(hex[m[i] >> 4]);
        district::putc(hex[m[i] & 0xf]);
    }
}

void print_ip(const uint8_t *ip) {
    for (uint32_t i = 0; i < 4; ++i) {
        if (i) {
            district::putc('.');
        }
        district::dec(ip[i]);
    }
}

} // namespace

MisakaMail misaka_mail_probe() {
    MisakaMail mail;
    zero(&g_rx, sizeof(g_rx));
    zero(&g_tx, sizeof(g_tx));

    const PciAddr dev =
        underground_find(kVirtioVendor, kVirtioNetLegacyId, kVirtioNetModernId);
    if (!dev.valid) {
        g_mail = mail;
        return mail;
    }
    const VirtioPciCfg cfg = underground_virtio_cfg(dev);
    if (!cfg.valid || cfg.device == 0) {
        g_mail = mail;
        return mail;
    }
    g_isr = cfg.isr;
    if (kRxIrq) {
        // Allow the device to assert its legacy INTx line: clear Interrupt
        // Disable (PCI command register bit 10), else the RX interrupt is gagged.
        const uint16_t cmd = underground_cfg_read16(dev, 0x04);
        underground_cfg_write16(dev, 0x04, uint16_t(cmd & ~uint16_t(1u << 10)));
    }

    const uint64_t common = cfg.common;
    write8(common + kDeviceStatus, 0);
    write8(common + kDeviceStatus, kStatusAck);
    write8(common + kDeviceStatus, kStatusAck | kStatusDriver);

    // Accept VIRTIO_F_VERSION_1 (bit 32), VIRTIO_NET_F_MAC (bit 5), and
    // VIRTIO_RING_F_EVENT_IDX (bit 29). EVENT_IDX lets us tell the device
    // exactly which used.idx value should next trigger an IRQ via
    // avail.used_event -- set HIGH ("never") on the RX queue suppresses the
    // mainloop-wake storm that vmnet broadcast traffic otherwise causes,
    // since QEMU drains macOS network packets to the device whether or not
    // we consume them.
    write32(common + kDriverFeatureSel, 1);
    write32(common + kDriverFeature, 1u);
    write32(common + kDriverFeatureSel, 0);
    write32(common + kDriverFeature, kFeatMac | kFeatEventIdx);
    write8(common + kDeviceStatus, kStatusAck | kStatusDriver | kStatusFeaturesOk);
    if ((read8(common + kDeviceStatus) & kStatusFeaturesOk) == 0) {
        g_mail = mail;
        return mail;
    }

    for (uint32_t i = 0; i < 6; ++i) {
        mail.mac[i] = read8(cfg.device + i); // virtio_net_config.mac
    }

    // We keep the default IP/gateway; bring up RX (q0) then TX (q1).
    g_rx_notify = setup_queue(common, cfg.notify, cfg.notify_mult, kRxVq, g_rx);
    g_tx_notify = setup_queue(common, cfg.notify, cfg.notify_mult, kTxVq, g_tx);
    write8(common + kDeviceStatus,
           kStatusAck | kStatusDriver | kStatusFeaturesOk | kStatusDriverOk);

    mail.present = true;
    g_mail = mail;
    g_rx_last = g_rx.used.idx;
    g_tx_last = g_tx.used.idx;

    // EVENT_IDX, RX queue: avail.used_event = "the used.idx at which the device
    // should next interrupt us". By vring_need_event,
    //   need = (new_idx - event - 1) < (new_idx - old_idx)
    // setting event = used.idx makes the device interrupt as soon as it puts
    // the NEXT packet in the used ring -- i.e. RX IRQ enabled. (event =
    // used.idx-1 was the old "never interrupt" suppression.) We need the IRQ so
    // an idle, tickless box wakes to drain RX and unblock parked pollers; the
    // ISR handler drains the ring, which is exactly what stops vmnet's mainloop
    // spin (the ring no longer stays full), so this also subsumes the old fix.
    // kRxIrq: enable the legacy-INTx RX interrupt. OFF for now -- enabling it
    // hangs misaka_mail_probe on Apple HVF (real GICv3): QEMU TCG (GICv2 AND
    // GICv3) tolerates the SPI 35..38 enable, but on HVF the box wedges right
    // here, before the "MisakaMail (net)" banner. The post-auth hang it was
    // meant to fix is actually handled by the non-blocking antenna_tcp_send;
    // RX is drained by the syscall pumps + 100 Hz tick as before. Re-enable +
    // fix the HVF GICv3 SPI path (IROUTER?) if a tickless-idle RX wake is ever
    // needed.
    g_rx.avail.used_event = uint16_t(kRxIrq ? g_rx.used.idx : g_rx.used.idx - 1);
    // TX: we reap descriptors synchronously in net_tx, no IRQ wanted.
    g_tx.avail.used_event = uint16_t(g_tx.used.idx - 1);

    if (kRxIrq) {
        // Route the NIC's legacy INTx (one of GIC SPI 35..38) to net_irq + drain.
        for (uint32_t i = 0; i < kPciIntxCount; ++i) {
            aleister_register(kPciIntxBase + i, net_irq, nullptr);
            aleister_enable(kPciIntxBase + i, 0x40);
        }
    }

    // Fill the RX ring with empty buffers and tell the device they are available.
    for (uint32_t i = 0; i < kRing; ++i) {
        rx_post(i);
    }
    write16(g_rx_notify, uint16_t(kRxVq));
    return mail;
}

const MisakaMail &misaka_mail_status() {
    return g_mail;
}

void misaka_mail_set_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    g_mail.ip[0] = a;
    g_mail.ip[1] = b;
    g_mail.ip[2] = c;
    g_mail.ip[3] = d;
    g_mail.gateway[0] = a;
    g_mail.gateway[1] = b;
    g_mail.gateway[2] = c;
    g_mail.gateway[3] = 1; // assume x.y.z.1 is the gateway
    g_gw_known = false;
}

void misaka_mail_ping(const uint8_t ip[4]) {
    if (!g_mail.present) {
        district::writeln("net: no NIC.");
        return;
    }
    uint8_t target[4];
    if (ip != nullptr) {
        copy(target, ip, 4);
    } else {
        copy(target, g_mail.gateway, 4);
    }
    const uint8_t *nexthop = same_subnet(target) ? target : g_mail.gateway;

    uint8_t mac[6];
    if (!arp_resolve(nexthop, mac)) {
        district::write("net: ARP timeout for ");
        print_ip(nexthop);
        district::writeln("");
        return;
    }

    const uint16_t id = 0x4d4b; // 'MK'
    const uint16_t seq = g_icmp_seq++;
    uint8_t payload[32];
    for (uint32_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = uint8_t('a' + (i % 26));
    }
    uint8_t out[1600];
    uint32_t n = build_icmp(out, mac, target, 8, id, seq, payload, sizeof(payload));
    const uint64_t t0 = last_order_ticks();
    net_tx(out, n);

    uint8_t frame[1600];
    uint32_t flen = 0;
    for (uint32_t tries = 0; tries < 100; ++tries) {
        if (!net_rx_poll(frame, sizeof(frame), &flen, 100)) {
            continue;
        }
        if (flen >= 34 && rbe16(frame + 12) == kEtherIpv4) {
            const uint8_t *ipp = frame + 14;
            const uint32_t ihl = (ipp[0] & 0x0f) * 4;
            const uint8_t *icmp = ipp + ihl;
            if (ipp[9] == kProtoIcmp && icmp[0] == 0 && rbe16(icmp + 4) == id &&
                rbe16(icmp + 6) == seq) {
                district::write("reply from ");
                print_ip(ipp + 12);
                district::write(" seq=");
                district::dec(seq);
                district::write(" (~");
                district::dec(last_order_ticks() - t0);
                district::writeln(" ticks)");
                return;
            }
        }
        handle_inbound(frame, flen);
    }
    district::writeln("net: no reply (timeout).");
}

void misaka_mail_serve() {
    if (!g_mail.present) {
        district::writeln("net: no NIC.");
        return;
    }
    district::write("net: serving on ");
    print_ip(g_mail.ip);
    district::writeln(" -- answering ARP/ICMP; press a key to stop.");

    uint8_t frame[1600];
    uint32_t flen = 0;
    for (;;) {
        if (district::try_read() >= 0) {
            district::writeln("net: stopped serving.");
            return;
        }
        if (net_rx_poll(frame, sizeof(frame), &flen, 50)) {
            const char *tag = handle_inbound(frame, flen);
            if (tag != nullptr) {
                district::write("  ");
                district::writeln(tag);
            }
        }
    }
}

bool misaka_mail_send_udp(const uint8_t dst_ip[4], uint16_t src_port,
                          uint16_t dst_port, const uint8_t *payload, uint32_t plen) {
    if (!g_mail.present || dst_ip == nullptr) return false;
    if (plen > 1500 - 20 - 8) return false; // single-frame MTU
    // Loopback: route packets to 127.x.x.x or our own address back to the
    // matching Antenna without touching the NIC. Lets the guest run a server
    // and client in the same kernel.
    if (dst_ip[0] == 127 ||
        (dst_ip[0] == g_mail.ip[0] && dst_ip[1] == g_mail.ip[1] &&
         dst_ip[2] == g_mail.ip[2] && dst_ip[3] == g_mail.ip[3])) {
        const uint8_t src_ip[4] = {127, 0, 0, 1};
        return antenna_deliver_udp(src_ip, src_port, dst_port, payload, plen);
    }
    // Resolve the link-layer next hop: directly to dst on-link, gateway off.
    uint8_t mac[6];
    const uint8_t *target = same_subnet(dst_ip) ? dst_ip : g_mail.gateway;
    if (!arp_resolve(target, mac)) return false;
    uint8_t out[1600];
    const uint32_t n = build_udp(out, mac, dst_ip, src_port, dst_port, payload, plen);
    net_tx(out, n);
    return true;
}

bool misaka_mail_send_udp_ex(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                             uint16_t src_port, uint16_t dst_port,
                             const uint8_t *payload, uint32_t plen) {
    if (!g_mail.present || src_ip == nullptr || dst_ip == nullptr) return false;
    if (plen > 1500 - 20 - 8) return false;
    uint8_t mac[6];
    const bool bcast = dst_ip[0] == 0xff && dst_ip[1] == 0xff &&
                       dst_ip[2] == 0xff && dst_ip[3] == 0xff;
    if (bcast) {
        for (uint32_t i = 0; i < 6; ++i) mac[i] = 0xff;
    } else {
        const uint8_t *target = same_subnet(dst_ip) ? dst_ip : g_mail.gateway;
        if (!arp_resolve(target, mac)) return false;
    }
    uint8_t out[1600];
    const uint32_t n = build_udp_ex(out, mac, src_ip, dst_ip, src_port, dst_port,
                                    payload, plen);
    net_tx(out, n);
    return true;
}

MisakaUdpCallback misaka_mail_subscribe_udp(uint16_t port, MisakaUdpCallback cb,
                                             void *cookie) {
    for (auto &s : g_udp_subs) {
        if (s.port == port) {
            MisakaUdpCallback prev = s.cb;
            s.cb = cb; s.cookie = cookie;
            return prev;
        }
    }
    for (auto &s : g_udp_subs) {
        if (s.cb == nullptr) {
            s.port = port; s.cb = cb; s.cookie = cookie;
            return nullptr;
        }
    }
    return nullptr;
}

bool misaka_mail_send_tcp(const uint8_t dst_ip[4], uint16_t src_port,
                          uint16_t dst_port, uint8_t flags, uint32_t seq,
                          uint32_t ack, uint16_t window,
                          const uint8_t *payload, uint32_t plen) {
    if (!g_mail.present || dst_ip == nullptr) return false;
    if (plen > 1500 - 20 - 20) return false; // single-frame MSS
    // Loopback fast-path (see misaka_mail_send_udp for the rationale).
    if (dst_ip[0] == 127 ||
        (dst_ip[0] == g_mail.ip[0] && dst_ip[1] == g_mail.ip[1] &&
         dst_ip[2] == g_mail.ip[2] && dst_ip[3] == g_mail.ip[3])) {
        const uint8_t src_ip[4] = {127, 0, 0, 1};
        antenna_deliver_tcp(src_ip, src_port, dst_port, flags, seq, ack,
                            payload, plen);
        return true;
    }
    uint8_t mac[6];
    const uint8_t *target = same_subnet(dst_ip) ? dst_ip : g_mail.gateway;
    if (!arp_resolve(target, mac)) return false;
    uint8_t out[1600];
    const uint32_t n = build_tcp(out, mac, dst_ip, src_port, dst_port, flags,
                                  seq, ack, window, payload, plen);
    net_tx(out, n);
    return true;
}

uint32_t misaka_mail_drain() {
    if (!g_mail.present) return 0;
    uint8_t frame[1600];
    uint32_t flen = 0;
    uint32_t count = 0;
    // Walk only the already-arrived used-ring entries; do NOT spin waiting.
    // net_rx_poll with timeout=0 would still spin once -- we bypass that by
    // checking used.idx directly. Each iteration drains one packet and
    // recycles its buffer, exactly mirroring net_rx_poll's success path.
    while (g_rx.used.idx != g_rx_last) {
        // Acquire barrier. The device exposes the used-ring entry and the
        // packet buffer BEFORE it advances used.idx (smp_wmb on its side);
        // we must not read the entry/buffer until that prior store is
        // visible to this core, or we copy a stale/torn frame. Mirrors
        // Linux virtqueue_get_buf's virtio_rmb() (== dma_rmb() == dmb oshld
        // on arm64). Missing here, this drain -- the *sole* RX path during
        // the SSH key exchange -- intermittently fed sshd a corrupted
        // KEXINIT/KEX_ECDH_INIT, so the server signed a mismatched exchange
        // hash and the client rejected it with "incorrect signature". The
        // "memory" clobber also stops the compiler from reordering the
        // used.idx load (the while test) past the ring/buffer loads.
        asm volatile("dmb oshld" ::: "memory");
        const VUsedElem &e = g_rx.used.ring[g_rx_last % kRing];
        const uint32_t slot = e.id % kRing;
        uint32_t total = e.len;
        g_rx_last = uint16_t(g_rx_last + 1);
        if (total > kNetHdr) {
            uint32_t n = total - kNetHdr;
            if (n > sizeof(frame)) n = sizeof(frame);
            for (uint32_t i = 0; i < n; ++i) frame[i] = g_rxbuf[slot][kNetHdr + i];
            flen = n;
            handle_inbound(frame, flen);
        }
        rx_post(slot); // recycle so vmnet can fill it again
        ++g_mail.rx_frames;
        ++count;
        if (count >= 32) break; // cap so a flooded ring doesn't starve us
    }
    if (count > 0) {
        // Re-arm the RX interrupt watermark at used.idx so the next delivered
        // packet raises a fresh INTx (kRxIrq); otherwise keep it suppressed.
        g_rx.avail.used_event = uint16_t(kRxIrq ? g_rx.used.idx : g_rx.used.idx - 1);
        write16(g_rx_notify, uint16_t(kRxVq));
    }
    return count;
}

uint32_t misaka_mail_pump(uint32_t timeout_ticks) {
    if (!g_mail.present) return 0;
    uint8_t frame[1600];
    uint32_t flen = 0;
    uint32_t count = 0;
    // First pass: timeout for the first packet (so callers can park here while
    // waiting for a reply). Each subsequent packet is processed without
    // additional waiting -- we just drain whatever's already queued.
    for (;;) {
        const uint32_t wait = (count == 0) ? timeout_ticks : 0;
        if (!net_rx_poll(frame, sizeof(frame), &flen, wait)) break;
        handle_inbound(frame, flen);
        ++count;
        if (count >= 16) break; // bounded, don't starve the caller
    }
    return count;
}

void misaka_mail_report() {
    if (!g_mail.present) {
        district::writeln("MisakaMail: no NIC.");
        return;
    }
    district::write("MisakaMail: link up, MAC ");
    print_mac(g_mail.mac);
    district::write(" IP ");
    print_ip(g_mail.ip);
    district::write(" gw ");
    print_ip(g_mail.gateway);
    if (g_gw_known) {
        district::write(" (");
        print_mac(g_gw_mac);
        district::write(")");
    }
    district::write("\n  tx=");
    district::dec(g_mail.tx_frames);
    district::write(" rx=");
    district::dec(g_mail.rx_frames);
    district::writeln(" frames");
}

} // namespace index::drivers
