#pragma once

#include <stdint.h>

namespace index::drivers {

// RandomVector: each Misaka Sister is generated with a unique random vector
// of individual differences that distinguishes her from the others -- in this
// kernel the same metaphor names the hardware entropy source. Backed by
// virtio-rng (PCI device 0x1005), it gives /dev/random + getrandom real bits
// instead of a cycle-counter PRNG.
struct RandomVector {
    bool present = false;
    uint8_t mac_placeholder[2] = {}; // unused; keeps the struct cache-friendly
    uint64_t bytes_drawn = 0;
};

RandomVector random_vector_probe();
const RandomVector &random_vector_status();

// Fill `buf` with `len` bytes of entropy. Returns the bytes actually obtained
// (always `len` if present; 0 if the device is missing).
uint32_t random_vector_read(uint8_t *buf, uint32_t len);

void random_vector_report();

} // namespace index::drivers
