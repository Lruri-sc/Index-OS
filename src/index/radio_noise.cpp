#include "index/radio_noise.hpp"

namespace index {

void radio_noise_init(RadioNoise &q) {
    q.head = 0;
    q.tail = 0;
    imprimatur_init(q.free_slots, kRadioNoiseCapacity);
    imprimatur_init(q.filled_slots, 0);
    judgement_init(q.mutex);
}

void radio_noise_send(RadioNoise &q, uint64_t message) {
    imprimatur_wait(q.free_slots);   // block while the queue is full
    judgement_lock(q.mutex);
    q.buffer[q.tail] = message;
    q.tail = (q.tail + 1) % kRadioNoiseCapacity;
    judgement_unlock(q.mutex);
    imprimatur_post(q.filled_slots); // signal one more ready message
}

uint64_t radio_noise_recv(RadioNoise &q) {
    imprimatur_wait(q.filled_slots); // block while the queue is empty
    judgement_lock(q.mutex);
    const uint64_t message = q.buffer[q.head];
    q.head = (q.head + 1) % kRadioNoiseCapacity;
    judgement_unlock(q.mutex);
    imprimatur_post(q.free_slots);   // free one slot for senders
    return message;
}

} // namespace index
