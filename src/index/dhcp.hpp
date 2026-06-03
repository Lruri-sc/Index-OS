#pragma once

#include <stdint.h>

namespace index {

// DHCP client: send DISCOVER, accept the first OFFER, send REQUEST, accept
// the matching ACK, install the lease into MisakaMail (sets our IP, gateway,
// and optionally the DNS resolver). Returns true on a complete lease, false
// on timeout or malformed reply. Logs progress on the console.
bool dhcp_acquire(uint32_t timeout_ticks = 1500);

} // namespace index
