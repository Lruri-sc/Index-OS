#include "index/antenna.hpp"

#include "drivers/misaka_mail.hpp"
#include "index/esper.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/last_order.hpp"
#include "index/usermode.hpp" // linux_ipc_wake

namespace index {

namespace district = index::imaginary_number_district;

namespace {

Antenna g_ants[kMaxAntennas];
uint16_t g_ephemeral = 49152;

bool valid(int idx) {
    return idx >= 0 && static_cast<uint32_t>(idx) < kMaxAntennas &&
           g_ants[idx].proto != AntennaProto::Free;
}

bool port_taken(uint16_t port) {
    for (auto &a : g_ants) {
        if (a.proto == AntennaProto::Udp && a.local_port == port) {
            return true;
        }
    }
    return false;
}

uint16_t next_ephemeral() {
    for (uint32_t tries = 0; tries < 65536; ++tries) {
        uint16_t p = g_ephemeral++;
        if (g_ephemeral == 0) g_ephemeral = 49152;
        if (p == 0) continue;
        if (!port_taken(p)) return p;
    }
    return 0;
}

// rx_avail is the one ring counter that BOTH the IRQ-side producer
// (antenna_deliver_* run from network_tick on CPU0, and tcp_pump_until's drain)
// and the EL0 consumer (recv/try_recv on any core) mutate. A plain ++/-- is a
// cross-core -- and same-core IRQ-vs-EL0 -- non-atomic read-modify-write: a lost
// increment strands already-written bytes (rx_buf holds them but rx_avail does
// not) so the consumer never reads them -> a permanent stall. That is the SMP
// SSH "after-kexreply" hang: the client's NEWKEYS lands in rx_buf but a raced
// rx_avail += hides it forever (network_tick re-wakes, but rx_avail stays wrong).
// Make it a lock-free SPSC counter instead of taking an IRQ-unsafe lock: the
// producer's add carries RELEASE (publishing its rx_buf writes), the consumer's
// load carries ACQUIRE (observing them). rx_head (consumer-owned) and rx_tail
// (producer-owned) need no atomics -- only this shared counter does.
inline uint32_t rx_avail_load(const Antenna &a) {
    return __atomic_load_n(&a.rx_avail, __ATOMIC_ACQUIRE);
}
inline void rx_avail_add(Antenna &a, uint32_t n) {
    __atomic_add_fetch(&a.rx_avail, n, __ATOMIC_RELEASE);
}
inline void rx_avail_sub(Antenna &a, uint32_t n) {
    __atomic_sub_fetch(&a.rx_avail, n, __ATOMIC_ACQ_REL);
}

void rx_put(Antenna &a, uint8_t b) {
    a.rx_buf[a.rx_tail] = b;
    a.rx_tail = (a.rx_tail + 1) % kAntennaRxBytes;
    rx_avail_add(a, 1);
}

uint8_t rx_get(Antenna &a) {
    const uint8_t b = a.rx_buf[a.rx_head];
    a.rx_head = (a.rx_head + 1) % kAntennaRxBytes;
    rx_avail_sub(a, 1);
    return b;
}

void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dst[i] = src[i];
}

bool ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

} // namespace

int antenna_socket_udp() {
    for (uint32_t i = 0; i < kMaxAntennas; ++i) {
        if (g_ants[i].proto == AntennaProto::Free) {
            g_ants[i] = Antenna{};
            g_ants[i].proto = AntennaProto::Udp;
            g_ants[i].refs = 1;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void antenna_inc_ref(int idx) {
    if (valid(idx)) __atomic_add_fetch(&g_ants[idx].refs, 1, __ATOMIC_ACQ_REL);
}

bool antenna_bind(int idx, uint16_t port) {
    if (!valid(idx)) return false;
    if (port != 0 && port_taken(port)) return false;
    if (port == 0) port = next_ephemeral();
    if (port == 0) return false;
    g_ants[idx].local_port = port;
    return true;
}

bool antenna_connect(int idx, const uint8_t ip[4], uint16_t port) {
    if (!valid(idx)) return false;
    // TCP: do the three-way handshake. The TCP code is defined further down
    // (it touches LastOrder + retransmit timers), so just forward.
    if (g_ants[idx].proto == AntennaProto::Tcp) {
        return antenna_tcp_connect(idx, ip, port);
    }
    // UDP: just record the default peer + ensure a source port is bound.
    copy_bytes(g_ants[idx].remote_ip, ip, 4);
    g_ants[idx].remote_port = port;
    g_ants[idx].has_remote = true;
    if (g_ants[idx].local_port == 0) {
        g_ants[idx].local_port = next_ephemeral();
    }
    return true;
}

int64_t antenna_sendto(int idx, const uint8_t *dst_ip, uint16_t dst_port,
                       const uint8_t *buf, uint32_t len) {
    if (!valid(idx)) return -9; // -EBADF
    Antenna &a = g_ants[idx];
    const uint8_t *ip = dst_ip != nullptr ? dst_ip : a.remote_ip;
    const uint16_t port = dst_port != 0 ? dst_port : a.remote_port;
    if (!a.has_remote && dst_ip == nullptr) return -89; // -EDESTADDRREQ
    if (port == 0) return -22; // -EINVAL
    if (a.local_port == 0) {
        a.local_port = next_ephemeral(); // implicit bind
        if (a.local_port == 0) return -98; // -EADDRINUSE
    }
    if (!drivers::misaka_mail_send_udp(ip, a.local_port, port, buf, len)) {
        return -101; // -ENETUNREACH
    }
    return len;
}

int64_t antenna_recvfrom(int idx, uint8_t *buf, uint32_t cap, uint8_t *src_ip,
                         uint16_t *src_port, uint32_t timeout_ticks) {
    if (!valid(idx)) return -9;
    Antenna &a = g_ants[idx];

    // Drain MisakaMail's RX into Antenna rings until our socket has data or
    // the budget runs out. handle_inbound() calls antenna_deliver_udp which
    // pushes into a.rx_buf.
    if (rx_avail_load(a) == 0) {
        drivers::misaka_mail_pump(timeout_ticks);
    }
    if (rx_avail_load(a) == 0) return 0; // timed out / would block

    // Decode a datagram header: src_ip(4) src_port(2) len(2).
    uint8_t hdr[8];
    for (uint32_t i = 0; i < 8; ++i) hdr[i] = rx_get(a);
    if (src_ip != nullptr) copy_bytes(src_ip, hdr, 4);
    const uint16_t sport = static_cast<uint16_t>((hdr[4] << 8) | hdr[5]);
    const uint16_t plen = static_cast<uint16_t>((hdr[6] << 8) | hdr[7]);
    if (src_port != nullptr) *src_port = sport;

    uint32_t n = plen;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; ++i) buf[i] = rx_get(a);
    // Discard any leftover bytes of this datagram (truncated read).
    for (uint32_t i = n; i < plen; ++i) (void)rx_get(a);
    return static_cast<int64_t>(n);
}

bool antenna_get_local(int idx, uint8_t local_ip[4], uint16_t *local_port) {
    if (!valid(idx)) return false;
    const auto &mail = drivers::misaka_mail_status();
    for (uint32_t i = 0; i < 4; ++i) local_ip[i] = mail.ip[i];
    if (local_port != nullptr) *local_port = g_ants[idx].local_port;
    return true;
}

// Peer (remote) address of a connected / accepted socket. Mirrors
// antenna_get_local; used by getpeername (sshd logs the connecting client).
bool antenna_get_remote(int idx, uint8_t remote_ip[4], uint16_t *remote_port) {
    if (!valid(idx)) return false;
    for (uint32_t i = 0; i < 4; ++i) remote_ip[i] = g_ants[idx].remote_ip[i];
    if (remote_port != nullptr) *remote_port = g_ants[idx].remote_port;
    return true;
}

// Readiness mask for poll/epoll (POLLIN=0x1, POLLOUT=0x4, POLLHUP=0x10) WITHOUT
// consuming data. Real poll() needs this instead of the old "always ready" lie,
// which busy-looped sshd during KEX.
uint32_t antenna_poll(int idx) {
    if (!valid(idx)) return 0x10; // POLLHUP
    const Antenna &a = g_ants[idx];
    uint32_t re = 0;
    if (rx_avail_load(a) > 0) re |= 0x1; // POLLIN: bytes waiting
    if (a.proto == AntennaProto::Tcp) {
        // Listening socket: POLLIN means a SYN landed and a connection is queued
        // for accept(). sshd's accept loop polls the listener for exactly this.
        if (a.tcp_state == TcpState::Listen) {
            if (a.accept_count > 0) re |= 0x1;
        }
        if (a.peer_fin && rx_avail_load(a) == 0) re |= 0x1; // EOF is readable
        if (a.tcp_state == TcpState::Established ||
            a.tcp_state == TcpState::CloseWait) re |= 0x4; // POLLOUT
        if (a.tcp_state == TcpState::Closed ||
            a.tcp_state == TcpState::Reset) re |= 0x10; // POLLHUP
    } else {
        re |= 0x4; // UDP always writable
    }
    return re;
}

// Atomic CAS dec-if-positive (refcount_t style); returns the new value. refs is
// RMW'd cross-core: fork/dup3 bump it on one core while close drops it on
// another, so a plain --/++ is a torn RMW (lost decrement => socket slot leak;
// lost increment => freed early under a peer = UAF). Mirrors Linux f_count.
static uint16_t ant_ref_dec(uint16_t &v) {
    uint16_t cur = __atomic_load_n(&v, __ATOMIC_RELAXED);
    while (cur > 0) {
        if (__atomic_compare_exchange_n(&v, &cur, static_cast<uint16_t>(cur - 1),
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return static_cast<uint16_t>(cur - 1);
    }
    return 0;
}

void antenna_close(int idx) {
    if (!valid(idx)) return;
    if (ant_ref_dec(g_ants[idx].refs) == 0) {
        // For TCP, run the active-close handshake (best effort) before freeing
        // the slot so the peer sees a proper FIN instead of just going away.
        if (g_ants[idx].proto == AntennaProto::Tcp &&
            (g_ants[idx].tcp_state == TcpState::Established ||
             g_ants[idx].tcp_state == TcpState::CloseWait)) {
            g_ants[idx].refs = 1; // tcp_close walks the state machine; needs valid()
            antenna_tcp_close(idx);
            g_ants[idx].refs = 0;
        }
        g_ants[idx] = Antenna{};
    }
}

bool antenna_deliver_udp(const uint8_t src_ip[4], uint16_t src_port,
                         uint16_t dst_port, const uint8_t *payload, uint32_t len) {
    // Find the Antenna bound to dst_port. If multiple sockets match the port,
    // prefer one with a matching connect() peer; otherwise pick the first.
    int chosen = -1;
    for (uint32_t i = 0; i < kMaxAntennas; ++i) {
        Antenna &a = g_ants[i];
        if (a.proto != AntennaProto::Udp || a.local_port != dst_port) continue;
        if (a.has_remote) {
            if (ip_eq(a.remote_ip, src_ip) && a.remote_port == src_port) {
                chosen = static_cast<int>(i);
                break; // exact match wins
            }
        } else if (chosen < 0) {
            chosen = static_cast<int>(i);
        }
    }
    if (chosen < 0) return false;
    Antenna &a = g_ants[chosen];
    const uint32_t need = 8 + len;
    if (need > kAntennaRxBytes - rx_avail_load(a)) {
        return false; // drop -- ring full
    }
    rx_put(a, src_ip[0]); rx_put(a, src_ip[1]);
    rx_put(a, src_ip[2]); rx_put(a, src_ip[3]);
    rx_put(a, static_cast<uint8_t>(src_port >> 8));
    rx_put(a, static_cast<uint8_t>(src_port & 0xff));
    rx_put(a, static_cast<uint8_t>(len >> 8));
    rx_put(a, static_cast<uint8_t>(len & 0xff));
    for (uint32_t i = 0; i < len; ++i) rx_put(a, payload[i]);
    return true;
}

// ---- TCP -----------------------------------------------------------------

namespace {

// Read a CNTPCT-derived random 32-bit (used for ISS). Not cryptographic.
uint32_t rand32() {
    uint64_t c = 0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(c));
    c ^= c << 13; c ^= c >> 7; c ^= c << 17;
    return static_cast<uint32_t>(c & 0xffffffffu);
}

// Hardware counter reads. CNTPCT advances continuously, even while IRQs are
// masked -- unlike last_order_ticks()/g_ticks, which only moves on a *taken*
// timer IRQ. Spin loops that run inside a syscall (IRQs masked) must bound
// themselves by CNTPCT, never g_ticks, or their deadline never arrives.
uint64_t read_cntpct() {
    uint64_t c = 0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(c));
    return c;
}
uint64_t read_cntfrq() {
    uint64_t f = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

void rx_bytes_put(Antenna &a, const uint8_t *src, uint32_t n) {
    const uint32_t free = kAntennaRxBytes - rx_avail_load(a);
    if (n > free) n = free; // drop overflow (TCP would shrink window)
    for (uint32_t i = 0; i < n; ++i) {
        a.rx_buf[a.rx_tail] = src[i];
        a.rx_tail = (a.rx_tail + 1) % kAntennaRxBytes;
    }
    rx_avail_add(a, n); // RELEASE: publishes the rx_buf writes above to consumers
}

// Walk the OOO slots looking for one whose seq == rcv_nxt, drain it into the
// rx ring, advance rcv_nxt, and repeat (a single in-order arrival can fill
// multiple gaps if several segments are buffered).
void tcp_drain_ooo(Antenna &a) {
    bool progress;
    do {
        progress = false;
        for (uint32_t k = 0; k < kAntennaOooSlots; ++k) {
            OooSeg &s = a.ooo[k];
            if (s.len > 0 && s.seq == a.rcv_nxt) {
                rx_bytes_put(a, s.data, s.len);
                a.rcv_nxt += s.len;
                s.len = 0;
                progress = true;
            }
        }
    } while (progress);
}

// Cache an out-of-order segment if there is room. Drops duplicates and any
// segment that wouldn't fit in our advertised window.
void tcp_park_ooo(Antenna &a, uint32_t seq, const uint8_t *payload, uint32_t plen) {
    if (plen == 0 || plen > kAntennaOooSegBytes) return;
    if (seq - a.rcv_nxt >= kAntennaRxBytes) return; // out of window
    // Skip duplicates.
    for (uint32_t k = 0; k < kAntennaOooSlots; ++k) {
        if (a.ooo[k].len > 0 && a.ooo[k].seq == seq) return;
    }
    for (uint32_t k = 0; k < kAntennaOooSlots; ++k) {
        if (a.ooo[k].len == 0) {
            a.ooo[k].seq = seq;
            a.ooo[k].len = static_cast<uint16_t>(plen);
            for (uint32_t i = 0; i < plen; ++i) a.ooo[k].data[i] = payload[i];
            return;
        }
    }
    // All slots full -- drop. Peer will retransmit; their window won't shrink
    // further than what we'd advertise next anyway.
}

void tcp_send_seg(Antenna &a, uint8_t flags, const uint8_t *payload, uint32_t plen) {
    drivers::misaka_mail_send_tcp(a.remote_ip, a.local_port, a.remote_port,
                                  flags, a.snd_nxt, a.rcv_nxt,
                                  antenna_rcv_window(a), payload, plen);
}

void tcp_send_ack(Antenna &a) {
    tcp_send_seg(a, kTcpAck, nullptr, 0);
}

void tcp_stage_retransmit(Antenna &a, uint32_t seq, const uint8_t *payload, uint32_t plen) {
    a.tx_seq = seq;
    a.tx_len = plen;
    for (uint32_t i = 0; i < plen && i < kAntennaTxBytes; ++i) a.tx_buf[i] = payload[i];
    a.tx_deadline_ticks = last_order_ticks() + 50; // 0.5 s @ 100 Hz
    a.tx_retries = 0;
}

void tcp_check_retransmit(Antenna &a) {
    if (a.tx_len == 0) return;
    if (last_order_ticks() < a.tx_deadline_ticks) return;
    if (a.tx_retries >= 5) {
        a.tcp_state = TcpState::Reset;
        a.tx_len = 0;
        return;
    }
    ++a.tx_retries;
    // Resend with the original seq; ack reflects whatever we've received since.
    drivers::misaka_mail_send_tcp(a.remote_ip, a.local_port, a.remote_port,
                                  kTcpAck | kTcpPsh, a.tx_seq, a.rcv_nxt,
                                  antenna_rcv_window(a), a.tx_buf, a.tx_len);
    a.tx_deadline_ticks = last_order_ticks() + 100; // back off a bit
}

// Pump MisakaMail's RX until either the predicate(state) returns true or the
// deadline passes. Also retransmits unacked data when its timer fires.
template <typename Pred>
bool tcp_pump_until(Antenna &a, uint32_t timeout_ticks, Pred pred) {
    // Bound the wait by CNTPCT, NOT last_order_ticks(): this runs in a syscall
    // with IRQs masked, so g_ticks is frozen and a g_ticks deadline would never
    // expire -- the loop would spin forever, freezing the 100 Hz tick and
    // starving every other Esper (the SSH post-auth hangs). CNTPCT counts on
    // regardless of interrupt masking, so the timeout is honoured for real.
    const uint64_t freq = read_cntfrq();
    const uint64_t deadline = read_cntpct() + (freq / 100) * timeout_ticks;
    while (true) {
        if (pred(a)) return true;
        tcp_check_retransmit(a);
        // Bound each pump so retransmit timers get to fire.
        const uint32_t slice = (timeout_ticks > 20) ? 20 : timeout_ticks;
        drivers::misaka_mail_pump(slice);
        if (pred(a)) return true;
        if (read_cntpct() >= deadline) return pred(a);
    }
}

const char *tcp_state_name(TcpState s) {
    switch (s) {
    case TcpState::Closed: return "CLOSED";
    case TcpState::Listen: return "LISTEN";
    case TcpState::SynSent: return "SYN_SENT";
    case TcpState::SynRcvd: return "SYN_RCVD";
    case TcpState::Established: return "ESTABLISHED";
    case TcpState::FinWait1: return "FIN_WAIT1";
    case TcpState::FinWait2: return "FIN_WAIT2";
    case TcpState::CloseWait: return "CLOSE_WAIT";
    case TcpState::LastAck: return "LAST_ACK";
    case TcpState::Closing: return "CLOSING";
    case TcpState::TimeWait: return "TIME_WAIT";
    case TcpState::Reset: return "RESET";
    }
    return "?";
}

} // namespace

uint16_t antenna_rcv_window(const Antenna &a) {
    const uint32_t avail = rx_avail_load(a);
    const uint32_t free = (avail >= kAntennaRxBytes) ? 0 : (kAntennaRxBytes - avail);
    return free > 0xffff ? 0xffff : static_cast<uint16_t>(free);
}

int antenna_socket_tcp() {
    for (uint32_t i = 0; i < kMaxAntennas; ++i) {
        if (g_ants[i].proto == AntennaProto::Free) {
            g_ants[i] = Antenna{};
            g_ants[i].proto = AntennaProto::Tcp;
            g_ants[i].refs = 1;
            g_ants[i].tcp_state = TcpState::Closed;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool antenna_tcp_connect(int idx, const uint8_t ip[4], uint16_t port) {
    if (!valid(idx) || g_ants[idx].proto != AntennaProto::Tcp) return false;
    Antenna &a = g_ants[idx];
    if (a.tcp_state != TcpState::Closed) return false;
    if (a.local_port == 0) {
        a.local_port = next_ephemeral();
        if (a.local_port == 0) return false;
    }
    copy_bytes(a.remote_ip, ip, 4);
    a.remote_port = port;
    a.has_remote = true;
    a.snd_iss = rand32();
    a.snd_nxt = a.snd_iss + 1; // SYN consumes one seq number
    a.snd_una = a.snd_iss;
    a.rcv_nxt = 0; // unknown until SYN-ACK arrives
    a.peer_fin = false;
    a.tcp_state = TcpState::SynSent;

    // Send SYN. Stage it for retransmit (seq = ISS, no payload, FIN/SYN bits
    // consume a seq slot but tx_len stays 0 — the retransmit code resends the
    // raw SYN by checking state + len==0).
    drivers::misaka_mail_send_tcp(a.remote_ip, a.local_port, a.remote_port,
                                  kTcpSyn, a.snd_iss, 0,
                                  antenna_rcv_window(a), nullptr, 0);
    a.tx_deadline_ticks = last_order_ticks() + 100;
    a.tx_retries = 0;

    // Spin (~10 s) waiting for SYN-ACK.
    return tcp_pump_until(a, 1000, [](Antenna &x) {
        return x.tcp_state == TcpState::Established ||
               x.tcp_state == TcpState::Reset;
    }) && a.tcp_state == TcpState::Established;
}

int64_t antenna_tcp_send(int idx, const uint8_t *buf, uint32_t len) {
    if (!valid(idx) || g_ants[idx].proto != AntennaProto::Tcp) return -9;
    Antenna &a = g_ants[idx];
    if (a.tcp_state == TcpState::Reset) return -104; // -ECONNRESET
    if (a.tcp_state != TcpState::Established &&
        a.tcp_state != TcpState::CloseWait) {
        return -107; // -ENOTCONN
    }
    if (len == 0) return 0;
    // Cap to MSS-ish chunk; caller can call again for the rest.
    if (len > 1400) len = 1400;
    // Stage retransmit BEFORE the send so that if the peer ACKs synchronously
    // (loopback case) the inner deliver_tcp can immediately clear tx_len and
    // the pump_until below sees a satisfied predicate without spinning.
    const uint32_t this_seq = a.snd_nxt;
    tcp_stage_retransmit(a, this_seq, buf, len);
    a.snd_nxt += len;
    drivers::misaka_mail_send_tcp(a.remote_ip, a.local_port, a.remote_port,
                                  kTcpAck | kTcpPsh, this_seq, a.rcv_nxt,
                                  antenna_rcv_window(a), buf, len);
    // Do NOT pump-and-spin here waiting for the ACK. That ran a multi-second
    // RX-drain spin inside the write() syscall (EL1) -- which esper_preempt
    // deliberately won't preempt -- so on the uniprocessor it monopolized the
    // core and starved the OpenSSH privsep monitor/session peers, hanging the
    // post-auth session. The segment is already on the wire; its ACK clears
    // tx_len asynchronously when the RX interrupt drains it (or the retransmit
    // timer resends). Return immediately, like a non-blocking TCP send -- which
    // is exactly what sshd's O_NONBLOCK socket expects. (The single retransmit
    // slot now only guards the most recent segment; fine over reliable SLIRP.)
    return static_cast<int64_t>(len);
}

int64_t antenna_tcp_try_recv(int idx, uint8_t *buf, uint32_t cap, bool *peer_gone) {
    if (!valid(idx) || g_ants[idx].proto != AntennaProto::Tcp) return -9;
    Antenna &a = g_ants[idx];
    if (peer_gone) *peer_gone = a.peer_fin || a.tcp_state == TcpState::Reset;
    const uint32_t avail = rx_avail_load(a); // ACQUIRE: see published rx_buf
    if (avail == 0) return 0;
    uint32_t n = cap; if (n > avail) n = avail;
    for (uint32_t i = 0; i < n; ++i) {
        buf[i] = a.rx_buf[a.rx_head];
        a.rx_head = (a.rx_head + 1) % kAntennaRxBytes;
    }
    rx_avail_sub(a, n);
    return static_cast<int64_t>(n);
}

int64_t antenna_tcp_recv(int idx, uint8_t *buf, uint32_t cap, uint32_t timeout_ticks) {
    if (!valid(idx) || g_ants[idx].proto != AntennaProto::Tcp) return -9;
    Antenna &a = g_ants[idx];
    tcp_pump_until(a, timeout_ticks, [](Antenna &x) {
        return rx_avail_load(x) > 0 || x.peer_fin || x.tcp_state == TcpState::Reset;
    });
    const uint32_t avail = rx_avail_load(a); // ACQUIRE: see published rx_buf
    if (avail == 0) {
        if (a.peer_fin || a.tcp_state == TcpState::Reset) return 0; // EOF
        return 0; // timeout: treat as would-block (no errno mapping yet)
    }
    uint32_t n = cap;
    if (n > avail) n = avail;
    for (uint32_t i = 0; i < n; ++i) {
        buf[i] = a.rx_buf[a.rx_head];
        a.rx_head = (a.rx_head + 1) % kAntennaRxBytes;
    }
    rx_avail_sub(a, n);
    return static_cast<int64_t>(n);
}

bool antenna_tcp_listen(int idx, uint32_t backlog) {
    if (!valid(idx) || g_ants[idx].proto != AntennaProto::Tcp) return false;
    Antenna &a = g_ants[idx];
    if (a.tcp_state != TcpState::Closed) return false;
    if (a.local_port == 0) return false; // must be bound first
    a.tcp_state = TcpState::Listen;
    a.backlog = (backlog == 0) ? 1 : backlog;
    if (a.backlog > kAntennaAcceptCap) a.backlog = kAntennaAcceptCap;
    return true;
}

int antenna_tcp_try_accept(int idx, uint8_t peer_ip[4], uint16_t *peer_port) {
    if (!valid(idx) || g_ants[idx].proto != AntennaProto::Tcp) return -1;
    Antenna &a = g_ants[idx];
    if (a.tcp_state != TcpState::Listen) return -1;
    int chosen = -1;
    uint32_t pos = 0;
    for (uint32_t k = 0; k < a.accept_count; ++k) {
        const int j = a.accept_queue[k];
        if (j >= 0 && static_cast<uint32_t>(j) < kMaxAntennas &&
            g_ants[j].tcp_state == TcpState::Established) {
            chosen = j; pos = k; break;
        }
    }
    if (chosen < 0) return -1;
    for (uint32_t k = pos + 1; k < a.accept_count; ++k) {
        a.accept_queue[k - 1] = a.accept_queue[k];
    }
    --a.accept_count;
    a.accept_queue[a.accept_count] = -1;
    Antenna &child = g_ants[chosen];
    if (peer_ip != nullptr) copy_bytes(peer_ip, child.remote_ip, 4);
    if (peer_port != nullptr) *peer_port = child.remote_port;
    return chosen;
}

int antenna_tcp_accept(int idx, uint8_t peer_ip[4], uint16_t *peer_port,
                       uint32_t timeout_ticks) {
    if (!valid(idx) || g_ants[idx].proto != AntennaProto::Tcp) return -1;
    Antenna &a = g_ants[idx];
    if (a.tcp_state != TcpState::Listen) return -1;
    // Wait for a fully-handshaked entry. accept_queue may contain SynRcvd
    // entries that haven't completed yet -- only return ones in Established.
    tcp_pump_until(a, timeout_ticks, [](Antenna &x) {
        for (uint32_t k = 0; k < x.accept_count; ++k) {
            const int j = x.accept_queue[k];
            if (j >= 0 && static_cast<uint32_t>(j) < kMaxAntennas &&
                g_ants[j].tcp_state == TcpState::Established) {
                return true;
            }
        }
        return false;
    });
    // Find the first Established accepted Antenna and remove it from the queue.
    int chosen = -1;
    uint32_t pos = 0;
    for (uint32_t k = 0; k < a.accept_count; ++k) {
        const int j = a.accept_queue[k];
        if (j >= 0 && static_cast<uint32_t>(j) < kMaxAntennas &&
            g_ants[j].tcp_state == TcpState::Established) {
            chosen = j;
            pos = k;
            break;
        }
    }
    if (chosen < 0) return -1;
    // Shift the rest of the queue down.
    for (uint32_t k = pos + 1; k < a.accept_count; ++k) {
        a.accept_queue[k - 1] = a.accept_queue[k];
    }
    --a.accept_count;
    a.accept_queue[a.accept_count] = -1;

    Antenna &child = g_ants[chosen];
    if (peer_ip != nullptr) copy_bytes(peer_ip, child.remote_ip, 4);
    if (peer_port != nullptr) *peer_port = child.remote_port;
    return chosen;
}

void antenna_tcp_close(int idx) {
    if (!valid(idx) || g_ants[idx].proto != AntennaProto::Tcp) return;
    Antenna &a = g_ants[idx];
    if (a.tcp_state == TcpState::Established) {
        // Send FIN+ACK, transition to FinWait1.
        drivers::misaka_mail_send_tcp(a.remote_ip, a.local_port, a.remote_port,
                                      kTcpFin | kTcpAck, a.snd_nxt, a.rcv_nxt,
                                      antenna_rcv_window(a), nullptr, 0);
        a.snd_nxt += 1; // FIN consumes a seq
        a.tcp_state = TcpState::FinWait1;
        // Wait briefly for the peer's ACK + FIN. We tolerate timing out --
        // antenna_close drops the slot regardless.
        tcp_pump_until(a, 200, [](Antenna &x) {
            return x.tcp_state == TcpState::TimeWait ||
                   x.tcp_state == TcpState::Closed ||
                   x.tcp_state == TcpState::Reset;
        });
    } else if (a.tcp_state == TcpState::CloseWait) {
        drivers::misaka_mail_send_tcp(a.remote_ip, a.local_port, a.remote_port,
                                      kTcpFin | kTcpAck, a.snd_nxt, a.rcv_nxt,
                                      antenna_rcv_window(a), nullptr, 0);
        a.snd_nxt += 1;
        a.tcp_state = TcpState::LastAck;
        tcp_pump_until(a, 100, [](Antenna &x) {
            return x.tcp_state == TcpState::Closed ||
                   x.tcp_state == TcpState::Reset;
        });
    }
    // antenna_close below decrements refcount and frees the slot.
}

bool antenna_deliver_tcp(const uint8_t src_ip[4], uint16_t src_port,
                         uint16_t dst_port, uint8_t flags, uint32_t seq,
                         uint32_t ack, const uint8_t *payload, uint32_t plen) {
    // Step 1: try to match an existing connection by 4-tuple. This covers
    // SynSent/SynRcvd/Established/closing sockets.
    int chosen = -1;
    for (uint32_t i = 0; i < kMaxAntennas; ++i) {
        Antenna &x = g_ants[i];
        if (x.proto != AntennaProto::Tcp || x.local_port != dst_port) continue;
        if (x.has_remote && ip_eq(x.remote_ip, src_ip) && x.remote_port == src_port) {
            chosen = static_cast<int>(i);
            break;
        }
    }

    // Step 2: if no connection matched and this is a pure SYN, see whether a
    // Listen socket on dst_port wants to spawn a fresh accepted Antenna.
    if (chosen < 0 && (flags & kTcpSyn) && !(flags & kTcpAck)) {
        int listen_idx = -1;
        for (uint32_t i = 0; i < kMaxAntennas; ++i) {
            Antenna &x = g_ants[i];
            if (x.proto == AntennaProto::Tcp && x.local_port == dst_port &&
                x.tcp_state == TcpState::Listen) {
                listen_idx = static_cast<int>(i);
                break;
            }
        }
        if (listen_idx < 0) return false;
        Antenna &listener = g_ants[listen_idx];
        if (listener.accept_count >= listener.backlog) return false; // drop SYN
        // Allocate a child Antenna for the new connection.
        int child_idx = -1;
        for (uint32_t i = 0; i < kMaxAntennas; ++i) {
            if (g_ants[i].proto == AntennaProto::Free) {
                child_idx = static_cast<int>(i);
                break;
            }
        }
        if (child_idx < 0) return false; // table full; client retransmits SYN
        Antenna &child = g_ants[child_idx];
        child = Antenna{};
        child.proto = AntennaProto::Tcp;
        child.refs = 1; // future fd from accept() owns this
        child.local_port = dst_port;
        copy_bytes(child.remote_ip, src_ip, 4);
        child.remote_port = src_port;
        child.has_remote = true;
        child.snd_iss = rand32();
        child.snd_nxt = child.snd_iss + 1; // SYN-ACK consumes one
        child.snd_una = child.snd_iss;
        child.rcv_irs = seq;
        child.rcv_nxt = seq + 1; // peer SYN consumes one
        child.tcp_state = TcpState::SynRcvd;
        // Enqueue on the listener BEFORE sending SYN-ACK. The loopback path
        // synchronously delivers SYN-ACK -> ACK -> SynRcvd state change, and
        // the wake we issue then needs to see this entry in the queue to
        // find the listen socket.
        listener.accept_queue[listener.accept_count++] = child_idx;
        // SYN-ACK back to the client.
        drivers::misaka_mail_send_tcp(child.remote_ip, child.local_port,
                                      child.remote_port,
                                      kTcpSyn | kTcpAck, child.snd_iss,
                                      child.rcv_nxt,
                                      antenna_rcv_window(child), nullptr, 0);
        return true;
    }

    if (chosen < 0) return false;
    Antenna &a = g_ants[chosen];

    if (flags & kTcpRst) {
        a.tcp_state = TcpState::Reset;
        return true;
    }

    switch (a.tcp_state) {
    case TcpState::SynSent: {
        if ((flags & (kTcpSyn | kTcpAck)) == (kTcpSyn | kTcpAck) && ack == a.snd_nxt) {
            a.rcv_irs = seq;
            a.rcv_nxt = seq + 1; // SYN consumes one seq
            a.snd_una = ack;
            a.tx_len = 0; // SYN was implicitly acked
            a.tcp_state = TcpState::Established;
            tcp_send_ack(a);
        }
        return true;
    }
    case TcpState::SynRcvd: {
        // Waiting for the third ACK of the passive handshake.
        if ((flags & kTcpAck) && ack == a.snd_nxt) {
            a.snd_una = ack;
            a.tx_len = 0;
            a.tcp_state = TcpState::Established;
            // If the client coalesced data with the ACK, deliver it now.
            if (plen > 0 && seq == a.rcv_nxt) {
                rx_bytes_put(a, payload, plen);
                a.rcv_nxt += plen;
                tcp_send_ack(a);
            }
            // Wake any Esper parked on the listen socket waiting for accept.
            for (uint32_t li = 0; li < kMaxAntennas; ++li) {
                Antenna &lst = g_ants[li];
                if (lst.proto != AntennaProto::Tcp ||
                    lst.tcp_state != TcpState::Listen) continue;
                for (uint32_t qk = 0; qk < lst.accept_count; ++qk) {
                    if (lst.accept_queue[qk] == chosen) {
                        linux_ipc_wake(Esper::IpcWaitKind::AntennaAccept,
                                       static_cast<int>(li));
                        break;
                    }
                }
            }
        }
        return true;
    }
    case TcpState::Established:
    case TcpState::FinWait1:
    case TcpState::FinWait2: {
        // ACK clears unacked bytes.
        if ((flags & kTcpAck) && ack > a.snd_una) {
            const uint32_t acked = ack - a.snd_una;
            a.snd_una = ack;
            if (acked >= a.tx_len) {
                a.tx_len = 0;
            } else {
                // Partial ack: shift the buffer.
                for (uint32_t i = 0; i < a.tx_len - acked; ++i) {
                    a.tx_buf[i] = a.tx_buf[i + acked];
                }
                a.tx_len -= acked;
                a.tx_seq += acked;
            }
            // In FIN_WAIT_1, an ACK of our FIN moves us to FIN_WAIT_2.
            if (a.tcp_state == TcpState::FinWait1 && ack == a.snd_nxt) {
                a.tcp_state = TcpState::FinWait2;
            }
        }
        // Payload: in-order arrivals stream into rx_buf and may unblock
        // previously-buffered OOO segments; out-of-order ones get parked.
        bool delivered_data = false;
        if (plen > 0) {
            if (seq == a.rcv_nxt) {
                rx_bytes_put(a, payload, plen);
                a.rcv_nxt += plen;
                tcp_drain_ooo(a);
                tcp_send_ack(a);
                delivered_data = true;
            } else if (seq > a.rcv_nxt) {
                tcp_park_ooo(a, seq, payload, plen);
                tcp_send_ack(a); // duplicate ACK signals the hole
            } else {
                // Old data we already saw; ACK to nudge the peer.
                tcp_send_ack(a);
            }
        }
        if (delivered_data) {
            linux_ipc_wake(Esper::IpcWaitKind::AntennaRecv, chosen);
        }
        // FIN moves us along.
        if (flags & kTcpFin) {
            if (seq == a.rcv_nxt) {
                a.rcv_nxt += 1;
                a.peer_fin = true;
                tcp_send_ack(a);
                // Reads that were parked waiting for more data now see EOF.
                linux_ipc_wake(Esper::IpcWaitKind::AntennaRecv, chosen);
                if (a.tcp_state == TcpState::Established) {
                    a.tcp_state = TcpState::CloseWait;
                } else if (a.tcp_state == TcpState::FinWait2) {
                    a.tcp_state = TcpState::TimeWait;
                } else if (a.tcp_state == TcpState::FinWait1) {
                    a.tcp_state = TcpState::Closing;
                }
            }
        }
        return true;
    }
    case TcpState::LastAck:
    case TcpState::Closing: {
        if ((flags & kTcpAck) && ack == a.snd_nxt) {
            a.tcp_state = TcpState::Closed;
        }
        return true;
    }
    case TcpState::TimeWait:
    case TcpState::Closed:
    case TcpState::Reset:
    case TcpState::CloseWait:
    case TcpState::Listen:
        return true;
    }
    return true;
}

// [SMP idle] True if any TCP socket may still send/receive (handshake,
// established, or closing). The boot core must keep polling the NIC at the full
// 100 Hz while such a socket exists: the RX IRQ is gated off, so network_tick is
// the ONLY thing that drains incoming segments and wakes the waiter. Deep (1 s)
// tickless idle while a connection is live stalled SSH up to a second per
// round-trip (post-auth session setup looked hung). A Listen-only / Closed
// socket does not need the fast poll -- a new SYN is still serviced within the
// 1 s deep-idle cap.
bool antenna_has_active_socket() {
    for (uint32_t i = 0; i < kMaxAntennas; ++i) {
        const Antenna &a = g_ants[i];
        if (a.proto != AntennaProto::Tcp) continue;
        const TcpState s = a.tcp_state;
        if (s != TcpState::Closed && s != TcpState::Listen &&
            s != TcpState::TimeWait && s != TcpState::Reset) {
            return true;
        }
    }
    return false;
}

void antenna_report() {
    district::writeln("Antennas (sockets):");
    bool any = false;
    for (uint32_t i = 0; i < kMaxAntennas; ++i) {
        const Antenna &a = g_ants[i];
        if (a.proto == AntennaProto::Free) continue;
        any = true;
        district::write("  [");
        district::dec(i);
        district::write("] ");
        district::write(a.proto == AntennaProto::Udp ? "udp" : "tcp");
        district::write(" local :");
        district::dec(a.local_port);
        if (a.has_remote) {
            district::write(" -> ");
            district::dec(a.remote_ip[0]); district::putc('.');
            district::dec(a.remote_ip[1]); district::putc('.');
            district::dec(a.remote_ip[2]); district::putc('.');
            district::dec(a.remote_ip[3]); district::putc(':');
            district::dec(a.remote_port);
        }
        district::write(" rx ");
        district::dec(rx_avail_load(a));
        district::write(" bytes");
        if (a.proto == AntennaProto::Tcp) {
            district::write(" state ");
            district::write(tcp_state_name(a.tcp_state));
            district::write(" tx_unacked="); district::dec(a.tx_len);     // [WD] held for retransmit
            district::write(" inflight="); district::dec(static_cast<uint64_t>(a.snd_nxt - a.snd_una));
        }
        district::writeln("");
    }
    if (!any) district::writeln("  (none)");
}

} // namespace index
