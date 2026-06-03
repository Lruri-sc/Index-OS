#include "index/imaginary_number_channel.hpp"

#include "index/esper.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/last_order.hpp"
#include "index/usermode.hpp" // linux_ipc_wake

namespace index {

namespace district = index::imaginary_number_district;

namespace {

INChannel g_channels[kMaxChannels];

bool valid(int idx) {
    return idx >= 0 && static_cast<uint32_t>(idx) < kMaxChannels &&
           g_channels[idx].kind != ChannelKind::Free;
}

bool path_eq(const INChannel &c, const char *path, uint32_t plen) {
    if (c.path_len != plen) return false;
    for (uint32_t i = 0; i < plen; ++i) {
        if (c.path[i] != path[i]) return false;
    }
    return true;
}

int find_bound(const char *path, uint32_t plen, ChannelKind kind, ChannelState st) {
    if (plen == 0) return -1; // anonymous never collides
    for (uint32_t i = 0; i < kMaxChannels; ++i) {
        const auto &c = g_channels[i];
        if (c.kind == kind && c.state == st && path_eq(c, path, plen)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int find_bound_any_state(const char *path, uint32_t plen, ChannelKind kind) {
    if (plen == 0) return -1;
    for (uint32_t i = 0; i < kMaxChannels; ++i) {
        const auto &c = g_channels[i];
        if (c.kind == kind && c.path_len == plen) {
            if (path_eq(c, path, plen)) return static_cast<int>(i);
        }
    }
    return -1;
}

// rx_avail is the one ring counter that BOTH the sender (inc_send, on one core)
// and the receiver (inc_try_recv, on another) mutate. A plain ++/-- is a torn
// cross-core read-modify-write: a lost increment hides a delivered byte, so the
// receiver never sees a privsep monitor<->child RPC reply and blocks on
// ChannelRecv forever -- which deadlocked the OpenSSH preauth->postauth handoff
// on SMP (monitor stuck draining the child's log while the child waited on a
// reply it couldn't observe). Make it a lock-free SPSC counter (matching the
// Antenna rx ring): the producer's add carries RELEASE (publishing rx_buf +
// rx_total), the consumer's load carries ACQUIRE. rx_head/rx_tail stay
// single-owner.
inline uint32_t rxa_load(const INChannel &c) {
    return __atomic_load_n(&c.rx_avail, __ATOMIC_ACQUIRE);
}
inline void rxa_add(INChannel &c, uint32_t n) {
    __atomic_add_fetch(&c.rx_avail, n, __ATOMIC_RELEASE);
}
inline void rxa_sub(INChannel &c, uint32_t n) {
    __atomic_sub_fetch(&c.rx_avail, n, __ATOMIC_ACQ_REL);
}

void rx_put(INChannel &c, uint8_t b) {
    c.rx_buf[c.rx_tail] = b;
    c.rx_tail = (c.rx_tail + 1) % kChannelRxBytes;
    ++c.rx_total; // monotonic write position; published by the release-add below
    rxa_add(c, 1);
}

uint8_t rx_get(INChannel &c) {
    const uint8_t b = c.rx_buf[c.rx_head];
    c.rx_head = (c.rx_head + 1) % kChannelRxBytes;
    rxa_sub(c, 1);
    return b;
}

void free_slot(int idx) {
    INChannel &c = g_channels[idx];
    // Drop any in-flight SCM_RIGHTS fds that were never received -- each still
    // holds a backend reference. Mirrors Linux unix_release freeing scm_fp_list.
    for (uint32_t i = 0; i < c.scm_count; ++i) linux_release_fd_backend(c.scm_pending[i]);
    c.scm_count = 0;
    c = INChannel{};
}

void copy_path(INChannel &c, const char *src, uint32_t plen) {
    uint32_t n = plen > kChannelPathCap ? kChannelPathCap : plen;
    for (uint32_t i = 0; i < n; ++i) c.path[i] = src[i];
    c.path_len = n;
}

} // namespace

int inc_socket(uint8_t type) {
    if (type != 1 /*SOCK_STREAM*/ && type != 2 /*SOCK_DGRAM*/) return -1;
    for (uint32_t i = 0; i < kMaxChannels; ++i) {
        if (g_channels[i].kind == ChannelKind::Free) {
            g_channels[i] = INChannel{};
            g_channels[i].kind = (type == 1) ? ChannelKind::Stream : ChannelKind::Dgram;
            g_channels[i].state = ChannelState::Closed;
            g_channels[i].refs = 1;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool inc_bind(int idx, const char *path, uint32_t plen) {
    if (!valid(idx)) return false;
    INChannel &c = g_channels[idx];
    if (c.state != ChannelState::Closed && c.state != ChannelState::Bound) return false;
    // Path collisions: another channel of the same kind already bound here.
    if (plen > 0 && find_bound_any_state(path, plen, c.kind) >= 0) return false;
    copy_path(c, path, plen);
    c.state = ChannelState::Bound;
    return true;
}

bool inc_listen(int idx, uint32_t backlog) {
    if (!valid(idx)) return false;
    INChannel &c = g_channels[idx];
    if (c.kind != ChannelKind::Stream) return false;
    if (c.state != ChannelState::Bound) return false;
    if (c.path_len == 0) return false; // need a name to listen on
    c.state = ChannelState::Listen;
    c.backlog = (backlog == 0) ? 1 : backlog;
    if (c.backlog > kChannelAcceptCap) c.backlog = kChannelAcceptCap;
    return true;
}

int inc_try_accept(int idx, char *peer_path_out, uint32_t *peer_plen_out) {
    if (!valid(idx)) return -1;
    INChannel &listener = g_channels[idx];
    if (listener.kind != ChannelKind::Stream || listener.state != ChannelState::Listen)
        return -1;
    if (listener.accept_count == 0) return -1;
    const int child = listener.accept_queue[0];
    for (uint32_t k = 1; k < listener.accept_count; ++k) {
        listener.accept_queue[k - 1] = listener.accept_queue[k];
    }
    --listener.accept_count;
    listener.accept_queue[listener.accept_count] = -1;
    if (peer_path_out != nullptr && peer_plen_out != nullptr &&
        child >= 0 && g_channels[child].peer_idx >= 0) {
        const INChannel &client = g_channels[g_channels[child].peer_idx];
        const uint32_t n = client.path_len > kChannelPathCap ? kChannelPathCap : client.path_len;
        for (uint32_t i = 0; i < n; ++i) peer_path_out[i] = client.path[i];
        *peer_plen_out = n;
    } else if (peer_plen_out != nullptr) {
        *peer_plen_out = 0;
    }
    return child;
}

int64_t inc_try_recv(int idx, uint8_t *buf, uint32_t cap, bool *peer_gone) {
    if (!valid(idx)) return -1;
    INChannel &c = g_channels[idx];
    if (c.kind != ChannelKind::Stream) return -1;
    if (peer_gone) *peer_gone = (c.state == ChannelState::PeerGone);
    const uint32_t avail = rxa_load(c); // ACQUIRE: see the sender's published bytes
    if (avail == 0) return 0;
    const uint32_t n = cap > avail ? avail : cap;
    for (uint32_t i = 0; i < n; ++i) buf[i] = rx_get(c);
    return static_cast<int64_t>(n);
}

int inc_accept(int idx, char *peer_path_out, uint32_t *peer_plen_out,
               uint32_t timeout_ticks) {
    if (!valid(idx)) return -1;
    INChannel &listener = g_channels[idx];
    if (listener.kind != ChannelKind::Stream || listener.state != ChannelState::Listen) {
        return -1;
    }
    const uint64_t deadline = last_order_ticks() + timeout_ticks;
    while (listener.accept_count == 0 && last_order_ticks() < deadline) {
        asm volatile("yield");
    }
    if (listener.accept_count == 0) return -1;
    const int child = listener.accept_queue[0];
    for (uint32_t k = 1; k < listener.accept_count; ++k) {
        listener.accept_queue[k - 1] = listener.accept_queue[k];
    }
    --listener.accept_count;
    listener.accept_queue[listener.accept_count] = -1;
    // Peer of the server-side channel is the client; its bound path (if any)
    // gives us the peer address for accept(addr, addrlen).
    if (peer_path_out != nullptr && peer_plen_out != nullptr) {
        if (child >= 0 && g_channels[child].peer_idx >= 0) {
            const INChannel &client = g_channels[g_channels[child].peer_idx];
            const uint32_t n = client.path_len > kChannelPathCap ? kChannelPathCap : client.path_len;
            for (uint32_t i = 0; i < n; ++i) peer_path_out[i] = client.path[i];
            *peer_plen_out = n;
        } else {
            *peer_plen_out = 0;
        }
    }
    return child;
}

bool inc_connect(int idx, const char *path, uint32_t plen, uint32_t timeout_ticks) {
    if (!valid(idx)) return false;
    INChannel &client = g_channels[idx];
    if (client.kind != ChannelKind::Stream) {
        // dgram connect just remembers the default peer; we don't track that
        // explicitly. Callers should use sendto() with an explicit address.
        return false;
    }
    if (client.state == ChannelState::Connected) return false;
    const int srv_idx = find_bound(path, plen, ChannelKind::Stream, ChannelState::Listen);
    if (srv_idx < 0) return false;
    INChannel &server = g_channels[srv_idx];
    if (server.accept_count >= server.backlog) return false;

    // Allocate the server-side channel that will be handed to accept().
    int child_idx = -1;
    for (uint32_t i = 0; i < kMaxChannels; ++i) {
        if (g_channels[i].kind == ChannelKind::Free) { child_idx = static_cast<int>(i); break; }
    }
    if (child_idx < 0) return false;
    INChannel &child = g_channels[child_idx];
    child = INChannel{};
    child.kind = ChannelKind::Stream;
    child.state = ChannelState::Connected;
    child.refs = 1; // accept() consumes this single reference

    // Pair up: client <-> child. Adopt the server's path on the child so
    // recvfrom/getpeername have a meaningful peer.
    client.state = ChannelState::Connected;
    client.peer_idx = child_idx;
    child.peer_idx = idx;
    copy_path(child, server.path, server.path_len);

    server.accept_queue[server.accept_count++] = child_idx;
    (void)timeout_ticks;
    // Wake any Esper parked on accept(server), or polling the listen socket.
    linux_ipc_wake(Esper::IpcWaitKind::ChannelAccept, srv_idx);
    linux_ipc_wake(Esper::IpcWaitKind::PollWait, -1);
    return true;
}

int64_t inc_send(int idx, const uint8_t *buf, uint32_t len) {
    if (!valid(idx)) return -9; // -EBADF
    INChannel &c = g_channels[idx];
    if (c.kind != ChannelKind::Stream) return -107; // -ENOTCONN-ish
    if (c.state != ChannelState::Connected) return -32; // -EPIPE
    if (c.peer_idx < 0 || !valid(c.peer_idx)) return -32;
    INChannel &peer = g_channels[c.peer_idx];
    const uint32_t free = kChannelRxBytes - rxa_load(peer);
    uint32_t n = len > free ? free : len;
    for (uint32_t i = 0; i < n; ++i) rx_put(peer, buf[i]);
    if (n > 0) {
        linux_ipc_wake(Esper::IpcWaitKind::ChannelRecv, c.peer_idx);
        // Also wake pollers: a peer blocked in ppoll/select on this socketpair
        // (e.g. the OpenSSH privsep monitor) parks on PollWait, NOT ChannelRecv.
        // The 100 Hz tick that would otherwise re-poll it is suppressed at idle
        // (tickless), and a socketpair carries no NIC IRQ to wake the box, so
        // without this an idle poller never sees the data -> privsep deadlock.
        linux_ipc_wake(Esper::IpcWaitKind::PollWait, -1);
    }
    return static_cast<int64_t>(n);
}

int64_t inc_recv(int idx, uint8_t *buf, uint32_t cap, uint32_t timeout_ticks) {
    if (!valid(idx)) return -9;
    INChannel &c = g_channels[idx];
    if (c.kind != ChannelKind::Stream) return -107;
    const uint64_t deadline = last_order_ticks() + timeout_ticks;
    while (rxa_load(c) == 0 && c.state == ChannelState::Connected &&
           last_order_ticks() < deadline) {
        asm volatile("yield");
    }
    const uint32_t avail = rxa_load(c);
    if (avail == 0) {
        if (c.state == ChannelState::PeerGone) return 0; // EOF
        return 0; // timeout
    }
    uint32_t n = cap > avail ? avail : cap;
    for (uint32_t i = 0; i < n; ++i) buf[i] = rx_get(c);
    return static_cast<int64_t>(n);
}

int64_t inc_sendto(int idx, const char *dst_path, uint32_t dst_plen,
                   const uint8_t *buf, uint32_t len) {
    if (!valid(idx)) return -9;
    INChannel &c = g_channels[idx];
    if (c.kind != ChannelKind::Dgram) return -101; // -ENETUNREACH-ish
    if (len > kChannelDgramMax) return -90; // -EMSGSIZE
    const int target = find_bound_any_state(dst_path, dst_plen, ChannelKind::Dgram);
    if (target < 0) return -111; // -ECONNREFUSED
    INChannel &peer = g_channels[target];
    const uint32_t need = 4 + c.path_len + len;
    if (need > kChannelRxBytes - rxa_load(peer)) return -90;
    // Header: src_path_len(u16) payload_len(u16) src_path[plen] payload[len]
    rx_put(peer, static_cast<uint8_t>(c.path_len >> 8));
    rx_put(peer, static_cast<uint8_t>(c.path_len & 0xff));
    rx_put(peer, static_cast<uint8_t>(len >> 8));
    rx_put(peer, static_cast<uint8_t>(len & 0xff));
    for (uint32_t i = 0; i < c.path_len; ++i) rx_put(peer, static_cast<uint8_t>(c.path[i]));
    for (uint32_t i = 0; i < len; ++i) rx_put(peer, buf[i]);
    linux_ipc_wake(Esper::IpcWaitKind::ChannelRecv, target);
    linux_ipc_wake(Esper::IpcWaitKind::PollWait, -1); // wake ppoll/select waiters too
    return static_cast<int64_t>(len);
}

int64_t inc_recvfrom(int idx, uint8_t *buf, uint32_t cap,
                     char *src_path_out, uint32_t *src_plen_out,
                     uint32_t timeout_ticks) {
    if (!valid(idx)) return -9;
    INChannel &c = g_channels[idx];
    if (c.kind != ChannelKind::Dgram) return -107;
    const uint64_t deadline = last_order_ticks() + timeout_ticks;
    while (rxa_load(c) == 0 && last_order_ticks() < deadline) {
        asm volatile("yield");
    }
    if (rxa_load(c) == 0) return 0;

    // Read header.
    const uint16_t plen = (rx_get(c) << 8) | rx_get(c);
    const uint16_t dlen = (rx_get(c) << 8) | rx_get(c);
    if (src_path_out != nullptr) {
        const uint32_t take = plen > kChannelPathCap ? kChannelPathCap : plen;
        for (uint32_t i = 0; i < take; ++i) src_path_out[i] = static_cast<char>(rx_get(c));
        // Skip any extra path bytes we couldn't fit.
        for (uint32_t i = take; i < plen; ++i) (void)rx_get(c);
        if (src_plen_out != nullptr) *src_plen_out = take;
    } else {
        for (uint32_t i = 0; i < plen; ++i) (void)rx_get(c);
        if (src_plen_out != nullptr) *src_plen_out = 0;
    }
    uint32_t n = dlen > cap ? cap : dlen;
    for (uint32_t i = 0; i < n; ++i) buf[i] = rx_get(c);
    for (uint32_t i = n; i < dlen; ++i) (void)rx_get(c); // discard tail of truncated dgram
    return static_cast<int64_t>(n);
}

void inc_close(int idx) {
    if (!valid(idx)) return;
    INChannel &c = g_channels[idx];
    if (c.refs > 0) --c.refs;
    if (c.refs > 0) return;
    // Last reference: notify the peer (if stream) and free the slot.
    if (c.kind == ChannelKind::Stream && c.peer_idx >= 0 && valid(c.peer_idx)) {
        INChannel &peer = g_channels[c.peer_idx];
        peer.state = ChannelState::PeerGone;
        peer.peer_idx = -1;
        // Wake the peer: a blocking recv sees EOF, a poller sees POLLHUP. The
        // tickless idle timer won't re-poll it, so wake explicitly.
        linux_ipc_wake(Esper::IpcWaitKind::ChannelRecv, c.peer_idx);
        linux_ipc_wake(Esper::IpcWaitKind::PollWait, -1);
    }
    free_slot(idx);
}

void inc_inc_ref(int idx) {
    if (valid(idx)) ++g_channels[idx].refs;
}

bool inc_socketpair(int out[2]) {
    int a = -1, b = -1;
    for (uint32_t i = 0; i < kMaxChannels; ++i) {
        if (g_channels[i].kind == ChannelKind::Free) {
            if (a < 0) a = static_cast<int>(i);
            else if (b < 0) { b = static_cast<int>(i); break; }
        }
    }
    if (a < 0 || b < 0) return false;
    g_channels[a] = INChannel{};
    g_channels[b] = INChannel{};
    g_channels[a].kind = g_channels[b].kind = ChannelKind::Stream;
    g_channels[a].state = g_channels[b].state = ChannelState::Connected;
    g_channels[a].refs = g_channels[b].refs = 1;
    g_channels[a].peer_idx = b;
    g_channels[b].peer_idx = a;
    out[0] = a;
    out[1] = b;
    return true;
}

bool inc_get_local(int idx, char *out_path, uint32_t *out_plen) {
    if (!valid(idx)) return false;
    const INChannel &c = g_channels[idx];
    if (c.path_len == 0) {
        if (out_plen) *out_plen = 0;
        return true;
    }
    const uint32_t n = c.path_len > kChannelPathCap ? kChannelPathCap : c.path_len;
    if (out_path) for (uint32_t i = 0; i < n; ++i) out_path[i] = c.path[i];
    if (out_plen) *out_plen = n;
    return true;
}

uint32_t inc_attach_fds(int idx, const Fd *fds, uint32_t n) {
    if (!valid(idx)) return 0;
    INChannel &c = g_channels[idx];
    if (c.kind != ChannelKind::Stream || c.state != ChannelState::Connected) return 0;
    if (c.peer_idx < 0 || !valid(c.peer_idx)) return 0;
    // The byte stream flows into the peer's rx ring (see inc_send), so the fds
    // park on the peer too -- the receiver reads both from its own channel.
    INChannel &peer = g_channels[c.peer_idx];
    uint32_t moved = 0;
    while (moved < n && peer.scm_count < kChannelScmMax) {
        // Tie each fd to the current stream position: it rides with the data
        // bytes inc_send pushes next (attach is always called right before the
        // send). recvmsg only collects it once the reader has consumed past
        // here -- so back-to-back mm_send_fd() messages stay separate.
        peer.scm_pos[peer.scm_count] = peer.rx_total;
        peer.scm_pending[peer.scm_count] = fds[moved];
        ++peer.scm_count;
        ++moved;
    }
    return moved;
}

uint32_t inc_take_fds(int idx, Fd *out, uint32_t max) {
    if (!valid(idx)) return 0;
    INChannel &c = g_channels[idx];
    // Only hand over fds whose byte-stream position has already been consumed
    // (the data they ride with was read out). scm_pos is non-decreasing, so the
    // deliverable set is a leading run. This keeps two back-to-back mm_send_fd()
    // messages from being merged into a single recvmsg (the bug that made the
    // pty fd-pass fail: a CMSG_SPACE-sized buffer let one recvmsg greedily grab
    // both fds, starving the next).
    const uint64_t consumed = c.rx_total - rxa_load(c); // ACQUIRE orders rx_total
    uint32_t take = 0;
    while (take < c.scm_count && take < max && c.scm_pos[take] < consumed) ++take;
    for (uint32_t i = 0; i < take; ++i) out[i] = c.scm_pending[i];
    // Shift the remainder down and clear the vacated tail so teardown won't
    // re-drop fds we've just handed off (ownership has moved to the caller).
    const uint32_t remain = c.scm_count - take;
    for (uint32_t i = 0; i < remain; ++i) {
        c.scm_pending[i] = c.scm_pending[i + take];
        c.scm_pos[i] = c.scm_pos[i + take];
    }
    for (uint32_t i = remain; i < c.scm_count; ++i) { c.scm_pending[i] = Fd{}; c.scm_pos[i] = 0; }
    c.scm_count = remain;
    return take;
}

uint32_t inc_poll(int idx) {
    if (!valid(idx)) return 0x10;
    const INChannel &c = g_channels[idx];
    uint32_t re = 0;
    const uint32_t avail = rxa_load(c);
    if (avail > 0) re |= 0x1;
    if (c.state == ChannelState::PeerGone && avail == 0) re |= 0x1; // EOF
    if (c.state == ChannelState::Connected) re |= 0x4;
    if (c.state == ChannelState::PeerGone) re |= 0x10;
    return re;
}

void inc_report() {
    district::writeln("Imaginary Number Channels:");
    bool any = false;
    for (uint32_t i = 0; i < kMaxChannels; ++i) {
        const INChannel &c = g_channels[i];
        if (c.kind == ChannelKind::Free) continue;
        any = true;
        district::write("  [");
        district::dec(i);
        district::write("] ");
        district::write(c.kind == ChannelKind::Stream ? "stream" : "dgram");
        district::write(" ");
        switch (c.state) {
        case ChannelState::Closed: district::write("CLOSED"); break;
        case ChannelState::Bound: district::write("BOUND"); break;
        case ChannelState::Listen: district::write("LISTEN"); break;
        case ChannelState::Connected: district::write("CONNECTED"); break;
        case ChannelState::PeerGone: district::write("PEER_GONE"); break;
        }
        district::write(" refs=");
        district::dec(c.refs);
        if (c.path_len > 0) {
            district::write(" path=");
            for (uint32_t k = 0; k < c.path_len; ++k) district::putc(c.path[k] ? c.path[k] : '@');
        }
        if (c.peer_idx >= 0) {
            district::write(" peer=");
            district::dec(c.peer_idx);
        }
        district::writeln("");
    }
    if (!any) district::writeln("  (none)");
}

} // namespace index
