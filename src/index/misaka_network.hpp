#pragma once

#include <stdint.h>

#include "index/artificial_heaven.hpp"
#include "drivers/othinus.hpp"

namespace index {

// The Misaka Network is the field of parallel Sisters all running at once,
// kept in step by Last Order (the timer). Here it is a preemptive,
// strict-priority round-robin scheduler that now runs across ALL cores: each
// core pulls ready Sisters from one shared runqueue, protected by an AntiSkill
// spinlock, and every Last Order beat may switch a core to the next Sister.
// Each Sister is a thread with its own stack and saved context.
void misaka_network_init();

// Wake the secondary cores discovered in `heaven` via PSCI CPU_ON (through
// Othinus) and fold them into the network: each runs its own idle Sister and is
// driven by its own timer. Call once, after misaka_network_init, on the boot core.
void misaka_network_bring_up_secondaries(const ArtificialHeaven &heaven,
                                         const drivers::Othinus &world);

// Number of cores currently online in the network (>= 1).
uint32_t misaka_network_online_cpus();

// Print the per-core roster: each online core's id, MPIDR, and current Sister.
void misaka_network_smp_report();

// Spawn a demo worker Sister that busies a per-Sister counter, so preemptive
// progress is observable. Returns false if the network is full.
bool misaka_network_spawn_demo();

// Spawn a Sister that bumps its counter once, then sleeps ~1s, and repeats.
// Its counters grow slowly because it blocks instead of busy-spinning.
bool misaka_network_spawn_sleeper();

// Spawn a generic named Sister at kLevelDefault running `entry(arg)`. Used by
// drivers that need a continuous background pump (Tsuchimikado / QGA agent).
bool misaka_network_spawn_named(void (*entry)(void *), void *arg, const char *name);

// Voluntarily give up the core to the next ready Sister (cooperative switch).
void misaka_network_yield();

// Block the current Sister for `ticks` LastOrder beats, then become ready.
void misaka_network_sleep(uint64_t ticks);

// Spawn a producer/consumer pair sharing one Imprimatur, to show blocking
// hand-off between Sisters. Returns false if already running or network full.
bool misaka_network_spawn_prodcons();

// Spawn two busy workers at Levels 4 and 2 to show strict-priority scheduling:
// the higher-Level Sister's `work` outruns the lower one's.
bool misaka_network_spawn_levels();

// Spawn two Sisters that contend on one Judgement mutex around a shared
// counter, to show mutual exclusion (no lost updates).
bool misaka_network_spawn_judgement();

// Spawn a priority-inversion scenario (Lv1 holder, Lv3 hog, Lv4 waiter). The
// Lv4 waiter only makes progress because Level Upper (priority inheritance)
// boosts the holder past the hog. Returns false if running or not enough slots.
bool misaka_network_spawn_priority_inversion();

// Spawn a sender/receiver pair communicating through a bounded RadioNoise
// mailbox (semaphores + mutex). The receiver verifies FIFO order.
bool misaka_network_spawn_radio();

// Current value of the Judgement-protected shared counter.
uint64_t misaka_network_shared_counter();

// Number of priority-inheritance boosts that actually raised a holder's Level.
uint64_t misaka_network_level_upper_count();

// While set, timer ticks preempt EL0 user processes (via `hook`) instead of
// switching Sisters. usermode registers the hook and toggles the mode.
void misaka_network_set_user_tick(void (*hook)(uint64_t *frame));
void misaka_network_set_user_mode(bool active);
bool misaka_network_user_mode_active();

// --- Low-level hooks used by synchronization primitives (Imprimatur/Judgement/
// RadioNoise) ---
// Identity of the Sister running on the calling core. Call under the lock.
uint32_t misaka_network_current_id();

// The shared scheduler lock. A sync primitive takes it so that "test my own
// state, then decide to block" is atomic against the scheduler on every core
// (without it, a post on another core could slip in and be lost). lock() masks
// IRQs and returns the prior DAIF; pass it back to unlock().
uint64_t misaka_network_lock();
void misaka_network_unlock(uint64_t flags);

// Block / unblock a Sister. PRECONDITION: the caller holds the scheduler lock
// via misaka_network_lock(). block_current_locked switches away and returns
// (with the lock still held) once this Sister is unblocked and rescheduled;
// unblock_locked only marks a blocked Sister ready (no immediate switch).
void misaka_network_block_current_locked();
void misaka_network_unblock_locked(uint32_t sister_id);

// Effective Level of the running Sister (for priority inheritance). Under lock.
uint8_t misaka_network_my_level();

// Level Upper: boost a lock holder's effective Level to a blocked waiter's, so
// it is not starved by mid-Level threads (priority inheritance). No-op if the
// holder is already at or above `to_level`.
void misaka_network_level_upper(uint32_t sister_id, uint8_t to_level);

// Restore a Sister's effective Level back to its own base Level.
void misaka_network_level_restore(uint32_t sister_id);

// Print the current roster of Sisters and their counters.
void misaka_network_report();

// Number of live Sisters.
uint32_t misaka_network_count();

} // namespace index
