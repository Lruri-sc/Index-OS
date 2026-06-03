#include "index/dns.hpp"

#include "index/antenna.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/last_order.hpp"

namespace index {

namespace district = index::imaginary_number_district;

namespace {

// DNS server. SLIRP exposes a builtin resolver at 10.0.2.3 (the third address
// in the default 10.0.2.0/24 subnet); programs can override via dns_set_server.
uint8_t g_server[4] = {10, 0, 2, 3};

// Encode "www.example.com\0" into the DNS label format:
// [3]www[7]example[3]com[0]. Returns the number of bytes written, or 0 on
// any name too long for buf.
uint32_t encode_qname(uint8_t *buf, uint32_t cap, const char *name) {
    uint32_t out = 0;
    const char *seg = name;
    while (true) {
        const char *end = seg;
        while (*end != 0 && *end != '.') ++end;
        const uint32_t len = static_cast<uint32_t>(end - seg);
        if (len > 63 || out + 1 + len > cap) return 0;
        buf[out++] = static_cast<uint8_t>(len);
        for (uint32_t i = 0; i < len; ++i) buf[out++] = static_cast<uint8_t>(seg[i]);
        if (*end == 0) break;
        seg = end + 1;
    }
    if (out + 1 > cap) return 0;
    buf[out++] = 0; // terminator
    return out;
}

// Skip a DNS-compressed name in the response. Returns the offset past the
// name, or 0 on malformed input.
uint32_t skip_name(const uint8_t *resp, uint32_t cap, uint32_t off) {
    while (off < cap) {
        const uint8_t len = resp[off];
        if ((len & 0xc0) == 0xc0) {
            return off + 2; // pointer (always 2 bytes; we don't follow)
        }
        if (len == 0) return off + 1; // root label terminator
        off += 1 + len;
    }
    return 0;
}

} // namespace

void dns_set_server(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    g_server[0] = a; g_server[1] = b; g_server[2] = c; g_server[3] = d;
}

bool dns_resolve(const char *name, uint8_t ip_out[4], uint32_t timeout_ticks) {
    if (name == nullptr || ip_out == nullptr) return false;

    // Build the query: 12-byte header + qname + qtype(2) + qclass(2).
    uint8_t pkt[256];
    const uint16_t id = static_cast<uint16_t>(last_order_ticks() & 0xffff);
    pkt[0] = static_cast<uint8_t>(id >> 8);
    pkt[1] = static_cast<uint8_t>(id & 0xff);
    pkt[2] = 0x01; pkt[3] = 0x00; // flags: standard query, recursion desired
    pkt[4] = 0x00; pkt[5] = 0x01; // QDCOUNT = 1
    pkt[6] = 0x00; pkt[7] = 0x00; // ANCOUNT
    pkt[8] = 0x00; pkt[9] = 0x00; // NSCOUNT
    pkt[10] = 0x00; pkt[11] = 0x00; // ARCOUNT
    uint32_t off = 12;
    const uint32_t qname_len = encode_qname(pkt + off, sizeof(pkt) - off - 4, name);
    if (qname_len == 0) return false;
    off += qname_len;
    pkt[off++] = 0x00; pkt[off++] = 0x01; // QTYPE = A
    pkt[off++] = 0x00; pkt[off++] = 0x01; // QCLASS = IN

    const int sock = antenna_socket_udp();
    if (sock < 0) return false;
    antenna_bind(sock, 0); // ephemeral source port

    if (antenna_sendto(sock, g_server, 53, pkt, off) < 0) {
        antenna_close(sock);
        return false;
    }

    uint8_t resp[1500];
    uint8_t src_ip[4];
    uint16_t src_port = 0;
    const int64_t n = antenna_recvfrom(sock, resp, sizeof(resp), src_ip, &src_port,
                                       timeout_ticks);
    antenna_close(sock);
    if (n < 12) return false;

    // Validate header: response, our id, at least one answer.
    if ((static_cast<uint16_t>(resp[0]) << 8 | resp[1]) != id) return false;
    if ((resp[2] & 0x80) == 0) return false; // not a response
    if ((resp[3] & 0x0f) != 0) return false; // RCODE != NOERROR
    const uint16_t qd = static_cast<uint16_t>((resp[4] << 8) | resp[5]);
    const uint16_t an = static_cast<uint16_t>((resp[6] << 8) | resp[7]);
    if (an == 0) return false;

    // Skip questions (qd of them, each is qname + 4 bytes).
    uint32_t cur = 12;
    for (uint16_t q = 0; q < qd; ++q) {
        cur = skip_name(resp, n, cur);
        if (cur == 0 || cur + 4 > static_cast<uint32_t>(n)) return false;
        cur += 4;
    }

    // Walk answers; take the first A (type 1, class 1, rdlen 4).
    for (uint16_t i = 0; i < an && cur + 10 <= static_cast<uint32_t>(n); ++i) {
        cur = skip_name(resp, n, cur);
        if (cur == 0 || cur + 10 > static_cast<uint32_t>(n)) return false;
        const uint16_t atype = static_cast<uint16_t>((resp[cur] << 8) | resp[cur + 1]);
        const uint16_t aclass = static_cast<uint16_t>((resp[cur + 2] << 8) | resp[cur + 3]);
        const uint16_t rdlen = static_cast<uint16_t>((resp[cur + 8] << 8) | resp[cur + 9]);
        cur += 10;
        if (cur + rdlen > static_cast<uint32_t>(n)) return false;
        if (atype == 1 && aclass == 1 && rdlen == 4) {
            for (uint32_t k = 0; k < 4; ++k) ip_out[k] = resp[cur + k];
            return true;
        }
        cur += rdlen;
    }
    return false;
}

void dns_report() {
    district::write("DNS server: ");
    district::dec(g_server[0]); district::putc('.');
    district::dec(g_server[1]); district::putc('.');
    district::dec(g_server[2]); district::putc('.');
    district::dec(g_server[3]); district::putc('\n');
}

} // namespace index
