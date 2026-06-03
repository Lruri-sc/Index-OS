#pragma once

#include <stdint.h>

namespace index::drivers {

// Tsuchimikado: a freelance triple-agent in Academy City who quietly relays
// messages across faction lines. Here he is the virtio-serial driver -- a
// side-channel between the guest (us) and the host (QEMU's QMP / SPICE
// vdagent), used to advertise "the guest is alive and the agent port is
// open." Without this, UTM's QEMU keeps polling for a missing
// `qemu-guest-agent` daemon at ~60 Hz and burns ~100% of a host core on the
// mainloop alone, even when the kernel itself is idle.
//
// Minimal: probe the device, negotiate VIRTIO_CONSOLE_F_MULTIPORT, set up
// the control + per-port virtqueues, transmit VIRTIO_CONSOLE_DEVICE_READY +
// PORT_READY + PORT_OPEN for each port. We don't parse the QMP protocol
// itself -- the host-side server only needs the port to look "connected" to
// stop polling. Anything QGA sends to us we silently drop.
struct Tsuchimikado {
    bool present = false;
    uint32_t num_ports = 0; // includes port 0 console
};

Tsuchimikado tsuchimikado_probe();
const Tsuchimikado &tsuchimikado_status();

// Drain any pending control + per-port traffic, replying as a QGA / SPICE
// vdagent stub would. Called in a loop from a dedicated Sister; cheap on the
// no-work path (just reads two vring used.idx slots). Returns the total
// number of events handled, which the caller can use to gate misaka_sleep
// so we don't burn CPU when idle.
uint32_t tsuchimikado_pump();

// Spawn the agent Sister that keeps the QGA / vdagent ports "connected" by
// answering host-side polls. Must run after misaka_network_init.
void tsuchimikado_start();

} // namespace index::drivers
