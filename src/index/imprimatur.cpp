#include "index/imprimatur.hpp"

#include "index/misaka_network.hpp"

namespace index {

namespace {

void enqueue(Imprimatur &sem, uint32_t sister) {
    if (sem.waiting >= kImprimaturMaxWaiters) {
        return;
    }
    sem.waiters[sem.tail] = sister;
    sem.tail = (sem.tail + 1) % kImprimaturMaxWaiters;
    ++sem.waiting;
}

uint32_t dequeue(Imprimatur &sem) {
    const uint32_t sister = sem.waiters[sem.head];
    sem.head = (sem.head + 1) % kImprimaturMaxWaiters;
    --sem.waiting;
    return sister;
}

} // namespace

void imprimatur_init(Imprimatur &sem, int32_t permits) {
    sem.permits = permits;
    sem.head = 0;
    sem.tail = 0;
    sem.waiting = 0;
}

void imprimatur_wait(Imprimatur &sem) {
    // The scheduler lock makes "test permits, then block" atomic against a post
    // on any core, so a permit handed off concurrently can never be lost.
    const uint64_t flags = misaka_network_lock();
    if (sem.permits > 0) {
        --sem.permits;
        misaka_network_unlock(flags);
        return;
    }

    // No permit: queue up and block. When a future post() hands a permit
    // straight to us we resume here, already holding it (no permit re-check).
    enqueue(sem, misaka_network_current_id());
    misaka_network_block_current_locked(); // switches away; lock re-held on resume
    misaka_network_unlock(flags);
}

void imprimatur_post(Imprimatur &sem) {
    const uint64_t flags = misaka_network_lock();
    if (sem.waiting > 0) {
        misaka_network_unblock_locked(dequeue(sem)); // direct hand-off, permits unchanged
    } else {
        ++sem.permits;
    }
    misaka_network_unlock(flags);
}

} // namespace index
