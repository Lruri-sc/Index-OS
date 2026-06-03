#pragma once

#include <stdint.h>

namespace index {

// Judgement (風紀委員) keeps order in Academy City. Here it is a mutex: at most
// one Sister may hold it at a time, so a critical section runs without
// interference. Built on the scheduler's block/unblock queue; waiters line up
// FIFO and the lock is handed straight to the next in line on release.
constexpr uint32_t kJudgementMaxWaiters = 8;

constexpr uint32_t kJudgementNoOwner = 0xffffffffu;

struct Judgement {
    bool held = false;
    uint32_t owner = kJudgementNoOwner; // Sister id holding the lock
    uint32_t waiters[kJudgementMaxWaiters] = {};
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t waiting = 0;
};

void judgement_init(Judgement &lock);

// Acquire the lock, blocking the current Sister until it is free.
void judgement_lock(Judgement &lock);

// Release the lock; only the owner may unlock. Hands off to the next waiter.
void judgement_unlock(Judgement &lock);

} // namespace index
