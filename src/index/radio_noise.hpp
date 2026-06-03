#pragma once

#include <stdint.h>

#include "index/imprimatur.hpp"
#include "index/judgement.hpp"

namespace index {

// RadioNoise: the codename of the Sister (Radio Noise) project. The Sisters
// exchange signals across the Misaka Network; here that is a bounded blocking
// message queue (mailbox) carrying 64-bit messages between Sisters. It composes
// the lower primitives: two Imprimatur semaphores count free and filled slots
// (so senders block when full and receivers block when empty), and a Judgement
// mutex guards the ring buffer indices.
constexpr uint32_t kRadioNoiseCapacity = 4;

struct RadioNoise {
    uint64_t buffer[kRadioNoiseCapacity] = {};
    uint32_t head = 0; // next slot to read
    uint32_t tail = 0; // next slot to write
    Imprimatur free_slots;  // counts empty slots (init capacity)
    Imprimatur filled_slots; // counts ready messages (init 0)
    Judgement mutex;         // protects head/tail/buffer
};

void radio_noise_init(RadioNoise &q);

// Send blocks while the queue is full; receive blocks while it is empty.
void radio_noise_send(RadioNoise &q, uint64_t message);
uint64_t radio_noise_recv(RadioNoise &q);

} // namespace index
