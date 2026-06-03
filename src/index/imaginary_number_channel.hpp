#pragma once

#include <stdint.h>

#include "index/esper.hpp" // Fd -- in-flight SCM_RIGHTS fd storage

namespace index {

// ImaginaryNumberChannel: AF_UNIX sockets. The Imaginary Number District is
// the kernel's console boundary; the Channels living inside it are how
// processes in that district talk to one another without going through the
// network stack -- pure in-kernel memcpy, addressed by filesystem paths or
// by the implicit peer pairing socketpair(2) hands you.
//
// Two flavours:
//   Stream  -- SOCK_STREAM, like a TCP loopback but with byte ordering,
//              listen/accept/connect, no ports, no IP packets.
//   Dgram   -- SOCK_DGRAM, like UDP loopback: bound to a path, sendto/recvfrom
//              by path, preserves message boundaries.
// Compared to AF_INET on 127.0.0.1 this is faster (no headers), can be guarded
// by filesystem permissions, and is the universal local-IPC mechanism in
// every Linux distro (Docker, systemd, X11, dbus, postgres, etc.).

constexpr uint32_t kMaxChannels = 16;
constexpr uint32_t kChannelRxBytes = 8192;        // stream/dgram rx ring size
constexpr uint32_t kChannelPathCap = 108;         // matches sun_path
constexpr uint32_t kChannelAcceptCap = 4;
constexpr uint32_t kChannelDgramMax = 1024;       // largest single datagram
constexpr uint32_t kChannelScmMax = 4;            // in-flight SCM_RIGHTS fds/chan

enum class ChannelKind : uint8_t {
    Free = 0,
    Stream = 1,
    Dgram = 2,
};

enum class ChannelState : uint8_t {
    Closed = 0,
    Bound,         // pathname/anon bound, no listen yet (dgram lives here too)
    Listen,        // stream listen
    Connected,     // stream peer pair established
    PeerGone,      // peer closed; reads drain rx ring then return 0
};

struct INChannel {
    ChannelKind kind = ChannelKind::Free;
    ChannelState state = ChannelState::Closed;
    uint16_t refs = 0;
    char path[kChannelPathCap] = {};
    uint32_t path_len = 0;                  // 0 = anonymous
    int peer_idx = -1;                      // stream-Connected: the other end
    int accept_queue[kChannelAcceptCap] = {-1, -1, -1, -1};
    uint32_t accept_count = 0;
    uint32_t backlog = 0;

    // RX ring. Stream: raw byte stream pushed by peer's send. Dgram: framed
    // as [u16 src_path_len][u16 payload_len][path bytes][payload bytes].
    uint8_t rx_buf[kChannelRxBytes] = {};
    uint32_t rx_head = 0;
    uint32_t rx_tail = 0;
    uint32_t rx_avail = 0;
    uint64_t rx_total = 0; // monotonic count of bytes ever written into rx_buf

    // In-flight SCM_RIGHTS file descriptors (sendmsg with an SCM_RIGHTS cmsg).
    // Each entry is a fully-formed Fd that already holds one backend reference
    // (taken at send time). recvmsg moves entries into the receiver's fd table;
    // channel teardown drops any never received. Sized for OpenSSH's monitor
    // protocol, which passes one or two fds per message.
    // scm_pos[i] = rx_total at attach time = the byte-stream position the fd
    // rides with (Linux ties SCM_RIGHTS fds to the sendmsg's data bytes/skb).
    // A recvmsg only collects fds whose position has already been read out, so
    // two separate mm_send_fd() messages don't get merged into one recvmsg.
    Fd scm_pending[kChannelScmMax] = {};
    uint64_t scm_pos[kChannelScmMax] = {};
    uint32_t scm_count = 0;
};

int inc_socket(uint8_t type);             // type: SOCK_STREAM=1 or SOCK_DGRAM=2
bool inc_bind(int idx, const char *path, uint32_t plen);
bool inc_listen(int idx, uint32_t backlog);
int inc_accept(int idx, char *peer_path_out, uint32_t *peer_plen_out,
               uint32_t timeout_ticks);

// Non-blocking accept: pop one queued connected child, or -1 if none.
int inc_try_accept(int idx, char *peer_path_out, uint32_t *peer_plen_out);

// Non-blocking recv (stream): returns 0 if peer has closed and the buffer is
// empty; returns -EAGAIN-style 0 if just empty; -1 on bad fd; positive byte
// count when bytes were available. Caller is expected to park on
// ChannelRecv if 0 was returned without peer_gone.
int64_t inc_try_recv(int idx, uint8_t *buf, uint32_t cap, bool *peer_gone);
bool inc_connect(int idx, const char *path, uint32_t plen, uint32_t timeout_ticks);

// Stream send/recv. Dgram callers should use sendto/recvfrom variants below.
int64_t inc_send(int idx, const uint8_t *buf, uint32_t len);
int64_t inc_recv(int idx, uint8_t *buf, uint32_t cap, uint32_t timeout_ticks);

int64_t inc_sendto(int idx, const char *dst_path, uint32_t dst_plen,
                   const uint8_t *buf, uint32_t len);
int64_t inc_recvfrom(int idx, uint8_t *buf, uint32_t cap,
                     char *src_path_out, uint32_t *src_plen_out,
                     uint32_t timeout_ticks);

void inc_close(int idx);
void inc_inc_ref(int idx);

// socketpair: allocate two stream channels with peer_idx pointing at each
// other and refs=1 each. Returns true and fills out[2].
bool inc_socketpair(int out[2]);

// Local bound path for getsockname; returns false if not bound.
bool inc_get_local(int idx, char *out_path, uint32_t *out_plen);

// SCM_RIGHTS fd passing (AF_UNIX stream). inc_attach_fds queues n already-ref'd
// Fd values onto the *peer's* in-flight list -- call it right before inc_send
// pushes the accompanying byte payload, so a receiver that sees the bytes is
// guaranteed the fds are already parked (cooperative scheduling keeps the
// attach+send atomic). Returns how many were queued (fewer than n if the
// peer's queue is full; the caller must release the overflow). inc_take_fds
// moves up to `max` queued fds out of *this* channel into out[] (ownership
// transfers -- the caller installs or releases them). Returns the count moved.
uint32_t inc_attach_fds(int idx, const Fd *fds, uint32_t n);
uint32_t inc_take_fds(int idx, Fd *out, uint32_t max);

// poll/epoll readiness mask, non-consuming.
uint32_t inc_poll(int idx);

void inc_report();

} // namespace index
