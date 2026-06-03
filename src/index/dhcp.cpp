#include "index/dhcp.hpp"

#include "drivers/misaka_mail.hpp"
#include "index/dns.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/last_order.hpp"

namespace index {

namespace district = index::imaginary_number_district;

namespace {

// DHCP message types (RFC 2131 §3.1).
constexpr uint8_t kDhcpDiscover = 1;
constexpr uint8_t kDhcpOffer = 2;
constexpr uint8_t kDhcpRequest = 3;
constexpr uint8_t kDhcpAck = 5;

// Magic cookie (RFC 2131 §3) + common option codes (RFC 2132).
constexpr uint32_t kDhcpCookie = 0x63825363;
constexpr uint8_t kOptMsgType = 53;
constexpr uint8_t kOptSubnetMask = 1;
constexpr uint8_t kOptRouter = 3;
constexpr uint8_t kOptDns = 6;
constexpr uint8_t kOptRequestedIp = 50;
constexpr uint8_t kOptServerId = 54;
constexpr uint8_t kOptParamList = 55;
constexpr uint8_t kOptEnd = 255;

// Single in-flight DHCP transaction. The callback fills this when a reply
// matching our XID lands; the main loop polls.
struct InFlight {
    bool waiting = false;
    uint32_t xid = 0;
    uint8_t resp[600] = {};
    uint32_t resp_len = 0;
} g_inflight;

void wbe32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint32_t rbe32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

bool dhcp_recv_cb(const uint8_t * /*src_ip*/, uint16_t /*src_port*/,
                  const uint8_t *payload, uint32_t len, void * /*cookie*/) {
    if (!g_inflight.waiting || len < 240 || len > sizeof(g_inflight.resp)) return false;
    // DHCP frame: op(1)/htype(1)/hlen(1)/hops(1)/xid(4)/secs(2)/flags(2)/...
    if (payload[0] != 2 /*BOOTREPLY*/) return false;
    if (rbe32(payload + 4) != g_inflight.xid) return false;
    for (uint32_t i = 0; i < len; ++i) g_inflight.resp[i] = payload[i];
    g_inflight.resp_len = len;
    g_inflight.waiting = false;
    return true;
}

// Build a DHCP message (DISCOVER or REQUEST). For DISCOVER we don't yet have
// an IP; for REQUEST we echo the offered IP via options 50 (requested IP) and
// 54 (server identifier). Returns total length.
uint32_t build_dhcp(uint8_t *out, uint32_t xid, const uint8_t mac[6],
                    uint8_t msg_type, const uint8_t *offered_ip,
                    const uint8_t *server_id) {
    for (uint32_t i = 0; i < 240; ++i) out[i] = 0;
    out[0] = 1; // BOOTREQUEST
    out[1] = 1; // Ethernet
    out[2] = 6; // hlen
    wbe32(out + 4, xid);
    // flags = broadcast (so the server addresses the OFFER to all-ones; we
    // don't have an IP yet so unicast would be moot).
    out[10] = 0x80; out[11] = 0x00;
    for (uint32_t i = 0; i < 6; ++i) out[28 + i] = mac[i];
    wbe32(out + 236, kDhcpCookie);
    uint32_t o = 240;
    out[o++] = kOptMsgType; out[o++] = 1; out[o++] = msg_type;
    out[o++] = kOptParamList; out[o++] = 4;
    out[o++] = kOptSubnetMask;
    out[o++] = kOptRouter;
    out[o++] = kOptDns;
    out[o++] = kOptServerId;
    if (msg_type == kDhcpRequest && offered_ip != nullptr) {
        out[o++] = kOptRequestedIp; out[o++] = 4;
        for (uint32_t i = 0; i < 4; ++i) out[o++] = offered_ip[i];
        if (server_id != nullptr) {
            out[o++] = kOptServerId; out[o++] = 4;
            for (uint32_t i = 0; i < 4; ++i) out[o++] = server_id[i];
        }
    }
    out[o++] = kOptEnd;
    return o;
}

// Walk options from offset 240; return a pointer to the value of `code` and
// write its length to *out_len, or nullptr if not present.
const uint8_t *opt_get(const uint8_t *pkt, uint32_t len, uint8_t code,
                       uint8_t *out_len) {
    uint32_t o = 240;
    while (o + 2 <= len) {
        const uint8_t c = pkt[o];
        if (c == kOptEnd) return nullptr;
        if (c == 0) { ++o; continue; } // PAD
        const uint8_t l = pkt[o + 1];
        if (o + 2 + l > len) return nullptr;
        if (c == code) {
            if (out_len) *out_len = l;
            return pkt + o + 2;
        }
        o += 2 + l;
    }
    return nullptr;
}

void print_ip(const uint8_t *ip) {
    district::dec(ip[0]); district::putc('.');
    district::dec(ip[1]); district::putc('.');
    district::dec(ip[2]); district::putc('.');
    district::dec(ip[3]);
}

// Send one DHCP exchange request and wait for the reply matching our XID.
// Used twice: DISCOVER and REQUEST. Returns true if the in-flight buffer
// holds a reply; the caller decodes the options.
bool send_and_wait(uint8_t *pkt, uint32_t len, uint32_t timeout_ticks) {
    const uint8_t src_ip[4] = {0, 0, 0, 0};
    const uint8_t dst_ip[4] = {0xff, 0xff, 0xff, 0xff};
    g_inflight.waiting = true;
    g_inflight.resp_len = 0;
    if (!drivers::misaka_mail_send_udp_ex(src_ip, dst_ip, 68, 67, pkt, len)) {
        g_inflight.waiting = false;
        return false;
    }
    const uint64_t deadline = last_order_ticks() + timeout_ticks;
    while (g_inflight.waiting && last_order_ticks() < deadline) {
        drivers::misaka_mail_pump(20);
    }
    return g_inflight.resp_len > 0;
}

} // namespace

bool dhcp_acquire(uint32_t timeout_ticks) {
    const auto &mail = drivers::misaka_mail_status();
    if (!mail.present) {
        district::writeln("dhcp: no NIC.");
        return false;
    }

    const auto prev_cb = drivers::misaka_mail_subscribe_udp(68, dhcp_recv_cb, nullptr);

    uint32_t xid = static_cast<uint32_t>(last_order_ticks() ^ 0xa1ea1573u);
    if (xid == 0) xid = 1;
    g_inflight.xid = xid;

    district::writeln("dhcp: DISCOVER -> 255.255.255.255:67");
    uint8_t pkt[400];
    uint32_t pkt_len = build_dhcp(pkt, xid, mail.mac, kDhcpDiscover, nullptr, nullptr);
    if (!send_and_wait(pkt, pkt_len, timeout_ticks)) {
        district::writeln("dhcp: no OFFER (timeout).");
        drivers::misaka_mail_subscribe_udp(68, prev_cb, nullptr);
        return false;
    }
    // Verify it's an OFFER.
    uint8_t opt_len = 0;
    const uint8_t *mt = opt_get(g_inflight.resp, g_inflight.resp_len, kOptMsgType, &opt_len);
    if (mt == nullptr || opt_len != 1 || mt[0] != kDhcpOffer) {
        district::writeln("dhcp: reply not OFFER.");
        drivers::misaka_mail_subscribe_udp(68, prev_cb, nullptr);
        return false;
    }
    uint8_t offered_ip[4]; for (uint32_t i = 0; i < 4; ++i) offered_ip[i] = g_inflight.resp[16 + i];
    const uint8_t *sid = opt_get(g_inflight.resp, g_inflight.resp_len, kOptServerId, &opt_len);
    if (sid == nullptr || opt_len != 4) {
        district::writeln("dhcp: OFFER missing server-id.");
        drivers::misaka_mail_subscribe_udp(68, prev_cb, nullptr);
        return false;
    }
    uint8_t server_id[4]; for (uint32_t i = 0; i < 4; ++i) server_id[i] = sid[i];
    district::write("dhcp: OFFER ");
    print_ip(offered_ip);
    district::write(" from server ");
    print_ip(server_id);
    district::putc('\n');

    district::writeln("dhcp: REQUEST");
    pkt_len = build_dhcp(pkt, xid, mail.mac, kDhcpRequest, offered_ip, server_id);
    if (!send_and_wait(pkt, pkt_len, timeout_ticks)) {
        district::writeln("dhcp: no ACK (timeout).");
        drivers::misaka_mail_subscribe_udp(68, prev_cb, nullptr);
        return false;
    }
    mt = opt_get(g_inflight.resp, g_inflight.resp_len, kOptMsgType, &opt_len);
    if (mt == nullptr || opt_len != 1 || mt[0] != kDhcpAck) {
        district::writeln("dhcp: reply not ACK.");
        drivers::misaka_mail_subscribe_udp(68, prev_cb, nullptr);
        return false;
    }
    const uint8_t *router = opt_get(g_inflight.resp, g_inflight.resp_len, kOptRouter, &opt_len);
    const uint8_t *resolver = opt_get(g_inflight.resp, g_inflight.resp_len, kOptDns, &opt_len);
    drivers::misaka_mail_set_ip(offered_ip[0], offered_ip[1], offered_ip[2], offered_ip[3]);
    if (router != nullptr && opt_len >= 4) {
        // The set_ip helper already inferred a /24 gateway; nothing more to
        // do here unless we want a non-default gateway. The library API
        // doesn't expose a separate setter today.
        (void)router;
    }
    if (resolver != nullptr && opt_len >= 4) {
        dns_set_server(resolver[0], resolver[1], resolver[2], resolver[3]);
    }
    district::write("dhcp: ACK -- IP ");
    print_ip(offered_ip);
    district::writeln(" installed.");

    drivers::misaka_mail_subscribe_udp(68, prev_cb, nullptr);
    return true;
}

} // namespace index
