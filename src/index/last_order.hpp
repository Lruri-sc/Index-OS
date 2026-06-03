#pragma once

#include <stdint.h>

namespace index {

// Last Order (MISAKA 20001) is the administrator that keeps the whole Misaka
// Network beating in sync. Here she is the ARM generic timer: the kernel's
// heartbeat. Each beat arrives as a timer interrupt routed through Aleister and
// advances a tick counter, the foundation a scheduler will later stand on.
struct LastOrder {
    uint64_t frequency = 0; // counter frequency (Hz), from CNTFRQ_EL0
    uint64_t interval = 0;  // counts between beats
    uint32_t hz = 0;        // configured beats per second
    bool armed = false;
};

// Arm the EL1 physical timer at `hz` beats/second, routed through Aleister.
// Requires Aleister to be initialised first; IRQs must be unmasked by the
// caller for beats to be delivered.
void last_order_init(LastOrder &clock, uint32_t hz);

// Arm the calling secondary core's own virtual timer (PPI 27 is banked per
// core), reusing the boot core's configured interval. Only the boot core
// advances the global tick counter; secondaries beat purely to drive their own
// preemption. Requires last_order_init to have run on the boot core first.
void last_order_arm_secondary();

// Total beats counted since arming (advanced from IRQ context).
uint64_t last_order_ticks();

// Configured beats per second, or 0 if the heartbeat was never armed.
uint32_t last_order_hz();

// Counter cycles between regular beats (frequency / hz). 0 before arming.
uint64_t last_order_interval_cycles();

// Override the next timer beat to fire after `cycles` (relative to now,
// counted in CNTV cycles). Used by run_espers's tickless-idle path to extend
// the next wake-up to the nearest Esper deadline rather than ticking every
// 10 ms while the box has nothing to do. The next on_beat call re-arms the
// regular periodic interval, so this only affects one upcoming beat.
void last_order_set_oneshot_cycles(uint64_t cycles);

} // namespace index
