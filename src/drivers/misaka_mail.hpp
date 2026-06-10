#pragma once

#include <stdint.h>

namespace index::drivers {

// MisakaMail: the Sisters pass signals to one another across the Misaka Network;
// this is the kernel's letterbox to the outside world. It is a virtio-net NIC
// (discovered on the Underground PCIe bus) plus a hand-written minimal network
// stack -- Ethernet, ARP, IPv4, ICMP -- enough to ping out and to answer pings.
// Kin to MisakaNetwork (the scheduler) and RadioNoise (the in-kernel mailbox).
struct MisakaMail {
    bool present = false;
    uint8_t mac[6] = {};
    uint8_t ip[4] = {10, 0, 2, 15};   // static; SLIRP's default guest lease
    uint8_t gateway[4] = {10, 0, 2, 2};
    uint64_t tx_frames = 0;
    uint64_t rx_frames = 0;
};

// Find and initialise the virtio-net device. Underground must be initialised
// first. Result is cached; also available via misaka_mail_status().
MisakaMail misaka_mail_probe();
const MisakaMail &misaka_mail_status();

// Change our static IPv4 address at runtime (e.g. for bridged networking, where
// the VM joins your LAN and you want a reachable address). Gateway = x.y.z.1.
void misaka_mail_set_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

// Re-ring the virtio-net RX doorbell (vm-exit) so qemu's main loop runs SLIRP and
// delivers pending RX. network_tick calls this each tick while a TCP connection
// is live, to keep large downloads (apt) from stalling on HVF BQL starvation.
void misaka_mail_kick_rx();

// Send one ICMP echo request to `ip` and wait for the reply (resolving the
// target's MAC via ARP first). Prints the outcome. `ip` nullptr -> the gateway.
void misaka_mail_ping(const uint8_t ip[4]);

// Loop answering inbound ARP requests and ICMP echo requests addressed to us,
// so the VM is pingable from outside. Returns when a console key is pressed.
void misaka_mail_serve();

// Print link status, MAC, IP, gateway, and frame counters.
void misaka_mail_report();

// Send a UDP datagram. Resolves the next hop (gateway for off-link
// addresses), builds the Ethernet/IPv4/UDP frame, and queues it on the TX
// ring. Returns false if the NIC isn't present, ARP times out, or the
// payload is larger than one Ethernet frame.
bool misaka_mail_send_udp(const uint8_t dst_ip[4], uint16_t src_port,
                          uint16_t dst_port, const uint8_t *payload, uint32_t plen);

// Extended variant for clients that need to override the source IP (DHCP
// DISCOVER sends from 0.0.0.0 because we don't have a lease yet) or send to
// the limited broadcast address 255.255.255.255 (uses the broadcast MAC and
// skips ARP). Returns false on the same conditions as misaka_mail_send_udp.
bool misaka_mail_send_udp_ex(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                             uint16_t src_port, uint16_t dst_port,
                             const uint8_t *payload, uint32_t plen);

// Subscribe to inbound UDP frames bound for `port` -- bypasses the Antenna
// lookup so DHCP/DNS code that needs to drive its own loop can receive
// directly. `cb` is invoked from handle_inbound; payload is the UDP body.
// Returns the previous subscriber (or nullptr).
using MisakaUdpCallback = bool (*)(const uint8_t src_ip[4], uint16_t src_port,
                                   const uint8_t *payload, uint32_t len, void *cookie);
MisakaUdpCallback misaka_mail_subscribe_udp(uint16_t port, MisakaUdpCallback cb,
                                             void *cookie);

// Send one TCP segment. `flags` is the TCP flags byte (SYN/ACK/FIN/RST/PSH);
// seq/ack are host-order; payload may be null/0 for control packets. `window`
// is the advertised receive window in bytes (capped to 65535). Resolves ARP
// for the next hop the same way UDP does.
bool misaka_mail_send_tcp(const uint8_t dst_ip[4], uint16_t src_port,
                          uint16_t dst_port, uint8_t flags, uint32_t seq,
                          uint32_t ack, uint16_t window,
                          const uint8_t *payload, uint32_t plen);

// Drain inbound frames for up to `timeout_ticks` LastOrder ticks (~10 ms each
// at 100 Hz), running each through handle_inbound: ARP/ICMP responder, UDP
// demux into Antennas. Returns the number of frames processed. Used by
// Antenna's recvfrom to drive RX while a socket is blocked.
uint32_t misaka_mail_pump(uint32_t timeout_ticks);

// Non-blocking RX drain: walk every packet already sitting in the virtio-net
// used ring, run handle_inbound on each, recycle the buffers so vmnet can
// keep injecting. Pure background hygiene: without this, when no socket is
// reading, the ring fills with macOS broadcast traffic (mDNS / IPv6 ND) and
// vmnet retries delivery in a tight loop -- which is the single largest
// contributor to QEMU host CPU under UTM (removing the Network device drops
// it from ~200% to 0%). Safe to call from any context, returns immediately
// once the ring is empty. Returns the number of frames drained.
uint32_t misaka_mail_drain();

} // namespace index::drivers
