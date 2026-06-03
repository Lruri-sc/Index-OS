#pragma once

#include <stdint.h>

namespace index {

// Antenna: a network socket. Named for the Misaka clones' brainwave receivers
// (the antennae) -- the user-facing receiver of network traffic, sibling to
// MisakaMail (the link-layer letterbox). Each Antenna owns a small RX ring of
// datagrams that MisakaMail's handle_inbound demultiplexes into.
//
// Phase Net-1 only supports IPv4 UDP. TCP needs its own state machine and is
// deferred to a later wave.

constexpr uint32_t kMaxAntennas = 16;
// Per-socket RX ring. UDP encodes each datagram as [src_ip(4)][src_port(2)]
// [len(2)][payload]; TCP just appends payload bytes (byte stream).
constexpr uint32_t kAntennaRxBytes = 8192;
// TCP retransmit buffer: holds at most one MSS-sized chunk of unacked data so
// we can resend on a timer. Larger sends are flushed in pieces by the caller.
constexpr uint32_t kAntennaTxBytes = 2048;

enum class AntennaProto : uint8_t {
    Free = 0,
    Udp = 1,
    Tcp = 2,
};

enum class TcpState : uint8_t {
    Closed = 0,
    Listen,       // passive open; routing incoming SYNs to fresh Antennas
    SynSent,      // sent SYN, waiting for SYN-ACK
    SynRcvd,      // got SYN, sent SYN-ACK; waiting for ACK
    Established,  // handshake done, data flowing
    FinWait1,     // we sent FIN, waiting for ACK (or FIN)
    FinWait2,     // peer ACK'd our FIN, waiting for their FIN
    CloseWait,    // peer sent FIN; we should send FIN to close
    LastAck,      // sent FIN after CloseWait, waiting for ACK
    Closing,      // simultaneous close
    TimeWait,     // both FINs ACK'd; brief quiet period (we drop straight to Closed)
    Reset,        // RST received; connection broken
};

// How many accepted-but-not-yet-accepted() connections a listen socket can
// hold. Beyond this we drop the SYN (the client will retransmit).
constexpr uint32_t kAntennaAcceptCap = 4;

// Out-of-order segment cache: when bytes arrive ahead of rcv_nxt we park them
// here and drain into rx_buf as the gap fills. Two slots covers the common
// SLIRP path where packets occasionally reorder; more would just waste RAM.
constexpr uint32_t kAntennaOooSlots = 2;
constexpr uint32_t kAntennaOooSegBytes = 1460; // one Ethernet MSS

struct OooSeg {
    uint32_t seq = 0;
    uint16_t len = 0; // 0 = empty slot
    uint8_t data[kAntennaOooSegBytes] = {};
};

struct Antenna {
    AntennaProto proto = AntennaProto::Free;
    uint16_t refs = 0; // fork/dup3 inc, close dec; freed when this hits 0
    uint16_t local_port = 0;
    uint8_t remote_ip[4] = {};
    uint16_t remote_port = 0;
    bool has_remote = false;

    // RX ring. For UDP: datagram-aligned (8-byte header + payload per record).
    // For TCP: a raw byte stream (no headers; recv reads in-order bytes).
    uint8_t rx_buf[kAntennaRxBytes] = {};
    uint32_t rx_head = 0; // next byte to read out
    uint32_t rx_tail = 0; // next byte to write in
    uint32_t rx_avail = 0; // bytes currently used

    // --- TCP-only state (unused for UDP) ---
    TcpState tcp_state = TcpState::Closed;
    uint32_t snd_iss = 0;   // our initial send sequence number
    uint32_t snd_nxt = 0;   // next sequence number we'll send
    uint32_t snd_una = 0;   // oldest unacked sequence number
    uint32_t rcv_irs = 0;   // peer's initial receive sequence number
    uint32_t rcv_nxt = 0;   // next sequence number we expect to receive
    bool peer_fin = false;  // peer has sent FIN; reads see EOF after rx ring drains
    // Retransmit buffer (held while waiting for ack). tx_seq is the seq of the
    // first byte in tx_buf; tx_len bytes are pending; tx_deadline is when we
    // resend if still unacked.
    uint8_t tx_buf[kAntennaTxBytes] = {};
    uint32_t tx_len = 0;
    uint32_t tx_seq = 0;
    uint64_t tx_deadline_ticks = 0;
    uint8_t tx_retries = 0;

    // Listen-socket bookkeeping. Only the Listen Antenna uses these: incoming
    // SYNs cause a fresh Antenna to be allocated and its index queued here so
    // accept() can pop it off.
    int accept_queue[kAntennaAcceptCap] = {-1, -1, -1, -1};
    uint32_t accept_count = 0;
    uint32_t backlog = 0;

    // Out-of-order receive buffer (TCP). Up to kAntennaOooSlots segments that
    // arrived ahead of rcv_nxt; drained into rx_buf as soon as the gap fills.
    OooSeg ooo[kAntennaOooSlots] = {};
};

// Helper: current advertised receive window for `a` (free bytes in rx_buf,
// capped at the wire 16-bit limit). misaka_mail_send_tcp uses this to fill
// the WIN field in outbound segments so the peer doesn't overflow our ring.
uint16_t antenna_rcv_window(const Antenna &a);

// --- public Antenna API ---

// Create a UDP socket; returns the Antenna index (>= 0) or -1 on table full.
int antenna_socket_udp();

// bind(port=0) auto-assigns an ephemeral port; otherwise installs the requested
// port (must be unique among Udp Antennas). Returns false on conflict.
bool antenna_bind(int idx, uint16_t port);

// connect(): remember a default remote for sendto(NULL, 0). Doesn't talk to
// the network for UDP (no handshake). Returns false on bad idx.
bool antenna_connect(int idx, const uint8_t ip[4], uint16_t port);

// sendto: encode a UDP datagram and hand it to MisakaMail. dst_ip/dst_port
// nullptr means "use the connect()ed remote". Returns bytes sent or -errno.
int64_t antenna_sendto(int idx, const uint8_t *dst_ip, uint16_t dst_port,
                       const uint8_t *buf, uint32_t len);

// recvfrom: pop one datagram from the RX ring. If the ring is empty, drives
// MisakaMail's RX pump for up to `timeout_ticks` LastOrder ticks. Writes the
// sender address to src_ip/*src_port if non-null. Returns bytes received,
// 0 if would-block on a non-blocking socket, or -errno.
int64_t antenna_recvfrom(int idx, uint8_t *buf, uint32_t cap, uint8_t *src_ip,
                         uint16_t *src_port, uint32_t timeout_ticks);

// Local address for getsockname(). Always returns our NIC IP + the bound port.
bool antenna_get_local(int idx, uint8_t local_ip[4], uint16_t *local_port);
// Peer address for getpeername() (the connected/accepted remote endpoint).
bool antenna_get_remote(int idx, uint8_t remote_ip[4], uint16_t *remote_port);

// poll/epoll readiness mask (POLLIN/OUT/HUP bits), non-consuming.
uint32_t antenna_poll(int idx);

// Tear down an Antenna reference (close()). Drops the refcount and frees the
// slot when it hits zero. fork/dup3 share the same Antenna and inc the count
// via antenna_inc_ref.
void antenna_close(int idx);

// Take an extra reference to an Antenna. Used when fork/clone copies an
// Esper's fd table (the child shares the socket with the parent) and when
// dup/dup3 copies an fd entry.
void antenna_inc_ref(int idx);

// MisakaMail calls this from handle_inbound when a UDP datagram arrives.
// Looks up the Antenna bound to dst_port (and matching the connect()ed peer
// if any) and appends the payload to its RX ring. Returns true if delivered.
bool antenna_deliver_udp(const uint8_t src_ip[4], uint16_t src_port,
                         uint16_t dst_port, const uint8_t *payload, uint32_t len);

// --- TCP API (client-side: active connect; passive listen/accept TBD) ---

int antenna_socket_tcp();

// Active connect: send SYN, wait for SYN-ACK, send ACK, transition to
// Established. Blocks (driving MisakaMail's RX) up to ~10 s. Returns true
// on success, false on timeout / RST / table full.
bool antenna_tcp_connect(int idx, const uint8_t ip[4], uint16_t port);

// Send up to `len` bytes (caller may need to call repeatedly for large data).
// Stages the chunk in the retransmit buffer, encodes a PSH+ACK segment, and
// transmits. Returns bytes accepted, 0 on broken peer, or -errno.
int64_t antenna_tcp_send(int idx, const uint8_t *buf, uint32_t len);

// Pop bytes from the RX byte stream, pumping MisakaMail until something
// arrives or the deadline passes. Returns 0 once the peer has sent FIN and
// all in-flight bytes have been delivered.
int64_t antenna_tcp_recv(int idx, uint8_t *buf, uint32_t cap, uint32_t timeout_ticks);

// Non-blocking byte-stream recv. Writes bytes when available; sets *peer_gone
// to true if the peer has FIN'd or the connection was reset. Returns the
// byte count (0 when no data AND peer not gone -> caller should park on
// AntennaRecv).
int64_t antenna_tcp_try_recv(int idx, uint8_t *buf, uint32_t cap, bool *peer_gone);

// Initiate active close (send FIN). The slot stays around until the peer ACKs
// and FINs back -- antenna_close drops the user's reference, and the deferred
// teardown happens during the next pump.
void antenna_tcp_close(int idx);

// Mark a TCP socket as passively listening on `local_port`. Must be bound
// first. Returns true on success.
bool antenna_tcp_listen(int idx, uint32_t backlog);

// Block (driving MisakaMail's RX) until a fully-handshaked connection lands in
// this listen socket's accept queue, then return its Antenna index (already
// in ESTABLISHED). Fills *peer_ip/*peer_port if non-null. Returns -1 on
// timeout or a closed/invalid listen socket.
int antenna_tcp_accept(int idx, uint8_t peer_ip[4], uint16_t *peer_port,
                       uint32_t timeout_ticks);

// Non-blocking accept: return the index of the first Established child in the
// accept_queue (dequeueing it), or -1 if there isn't one yet. Used by the
// Linux syscall layer to support park-and-retry blocking semantics.
int antenna_tcp_try_accept(int idx, uint8_t peer_ip[4], uint16_t *peer_port);

// MisakaMail dispatches an inbound TCP segment here. The segment payload is
// after the TCP header; `flags`, `seq`, `ack`, `window` come from the header.
bool antenna_deliver_tcp(const uint8_t src_ip[4], uint16_t src_port,
                         uint16_t dst_port, uint8_t flags, uint32_t seq,
                         uint32_t ack, const uint8_t *payload, uint32_t plen);

// TCP flag bits we encode/decode.
constexpr uint8_t kTcpFin = 0x01;
constexpr uint8_t kTcpSyn = 0x02;
constexpr uint8_t kTcpRst = 0x04;
constexpr uint8_t kTcpPsh = 0x08;
constexpr uint8_t kTcpAck = 0x10;

// Debug helper: print the Antenna table.
void antenna_report();

// True if any TCP socket is mid-connection (handshake/established/closing), so
// the boot-core idle loop keeps polling the NIC at 100 Hz (the RX IRQ is gated
// off). See the definition for why.
bool antenna_has_active_socket();

} // namespace index
