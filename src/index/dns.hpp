#pragma once

#include <stdint.h>

namespace index {

// Resolve a hostname to an IPv4 address by sending a UDP DNS A query to the
// configured DNS server (default 10.0.2.3, SLIRP's). Returns true if an A
// record came back; writes the address into `ip_out`. Timeout is in
// LastOrder ticks (~10 ms each).
bool dns_resolve(const char *name, uint8_t ip_out[4], uint32_t timeout_ticks = 500);

// Change the resolver. Setting to 0.0.0.0 restores the SLIRP default.
void dns_set_server(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

// Print the current resolver address.
void dns_report();

} // namespace index
