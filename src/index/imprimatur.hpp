#pragma once

#include <stdint.h>

namespace index {

// An imprimatur is the Church's licence to print a book ("let it be printed"),
// the real-world counterpart to the Index Librorum Prohibitorum. Here it is a
// counting semaphore: a Sister must obtain a permit before proceeding. If none
// is available it blocks in a FIFO queue until another Sister grants one.
constexpr uint32_t kImprimaturMaxWaiters = 8;

struct Imprimatur {
    int32_t permits = 0;
    uint32_t waiters[kImprimaturMaxWaiters] = {};
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t waiting = 0;
};

void imprimatur_init(Imprimatur &sem, int32_t permits);

// Acquire a permit, blocking the current Sister until one is available.
void imprimatur_wait(Imprimatur &sem);

// Release a permit: hand it directly to the longest-waiting Sister, or bank it.
void imprimatur_post(Imprimatur &sem);

} // namespace index
