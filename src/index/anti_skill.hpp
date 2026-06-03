#pragma once

#include <stdint.h>

namespace index {

// AntiSkill (アンチスキル) is Academy City's armed police force. Where Judgement
// (the student disciplinary committee) keeps order at the social layer, Anti-Skill
// is the hard, low-level enforcement that shows up with guns. Here it is the
// kernel spinlock: the raw, busy-wait counterpart to the Judgement mutex.
//
// Judgement sleeps a Sister via the scheduler when the lock is busy; AntiSkill
// must NOT sleep, because it guards the scheduler and the allocators themselves
// (the very machinery a sleeping lock would need). A core that loses the race
// spins -- parking on WFE so it does not hammer the interconnect -- until the
// holder releases with a store-release and an SEV.
//
// On a single core the lock is always uncontended; the irqsave variant still
// masks IRQs so the critical section stays atomic against this core's own
// interrupt handlers.
struct AntiSkill {
    volatile uint32_t lock = 0;
    volatile int32_t owner_cpu = -1; // [dbg] cpu holding it, -1 = free
    volatile uint64_t owner_site = 0; // [dbg] LR of the acquiring call site
};

void anti_skill_init(AntiSkill &m);

// Acquire / release without touching the interrupt mask. Use only where IRQs
// are already masked (e.g. inside the scheduler's switch path).
void anti_skill_lock(AntiSkill &m);
void anti_skill_unlock(AntiSkill &m);

// Acquire after masking IRQs on this core (returns the prior DAIF to restore);
// release then restore that DAIF. This is the form the scheduler, allocators,
// and console use: masking first prevents a same-core IRQ handler from trying
// to re-take a lock this core already holds (self-deadlock).
uint64_t anti_skill_lock_irqsave(AntiSkill &m);
void anti_skill_unlock_irqrestore(AntiSkill &m, uint64_t flags);

} // namespace index
