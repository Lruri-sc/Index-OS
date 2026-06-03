#include "index/judgement.hpp"

#include "index/misaka_network.hpp"

namespace index {

namespace {

void enqueue(Judgement &lock, uint32_t sister) {
    if (lock.waiting >= kJudgementMaxWaiters) {
        return;
    }
    lock.waiters[lock.tail] = sister;
    lock.tail = (lock.tail + 1) % kJudgementMaxWaiters;
    ++lock.waiting;
}

uint32_t dequeue(Judgement &lock) {
    const uint32_t sister = lock.waiters[lock.head];
    lock.head = (lock.head + 1) % kJudgementMaxWaiters;
    --lock.waiting;
    return sister;
}

} // namespace

void judgement_init(Judgement &lock) {
    lock.held = false;
    lock.owner = kJudgementNoOwner;
    lock.head = 0;
    lock.tail = 0;
    lock.waiting = 0;
}

void judgement_lock(Judgement &lock) {
    // The scheduler lock makes "test held, then take or block" atomic across all
    // cores, so two cores cannot both believe they acquired a free lock.
    const uint64_t flags = misaka_network_lock();
    if (!lock.held) {
        lock.held = true;
        lock.owner = misaka_network_current_id();
        misaka_network_unlock(flags);
        return;
    }

    // Held by someone else: queue and block. We resume here already owning the
    // lock, handed to us directly by judgement_unlock (ownership transferred).
    // Lend our Level to the holder (priority inheritance) so a mid-Level thread
    // cannot starve it and leave us blocked forever (priority inversion).
    misaka_network_level_upper(lock.owner, misaka_network_my_level());
    enqueue(lock, misaka_network_current_id());
    misaka_network_block_current_locked(); // switches away; lock re-held on resume
    misaka_network_unlock(flags);
}

void judgement_unlock(Judgement &lock) {
    const uint64_t flags = misaka_network_lock();
    if (lock.owner != misaka_network_current_id()) {
        misaka_network_unlock(flags); // only the owner may unlock; ignore otherwise
        return;
    }

    // Done with the critical section: drop any inherited Level boost.
    misaka_network_level_restore(misaka_network_current_id());

    if (lock.waiting > 0) {
        const uint32_t next = dequeue(lock);
        lock.owner = next;            // transfer ownership without releasing
        misaka_network_unblock_locked(next);
    } else {
        lock.held = false;
        lock.owner = kJudgementNoOwner;
    }
    misaka_network_unlock(flags);
}

} // namespace index
