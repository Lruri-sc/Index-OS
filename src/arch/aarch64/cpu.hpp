#pragma once

#include <stdint.h>

namespace index::arch {

inline uint64_t current_el() {
    uint64_t value = 0;
    asm volatile("mrs %0, CurrentEL" : "=r"(value));
    return (value >> 2) & 3;
}

inline void enable_irq() {
    asm volatile("msr daifclr, #2" ::: "memory");
}

inline void disable_irq() {
    asm volatile("msr daifset, #2" ::: "memory");
}

inline void wait_for_interrupt() {
    asm volatile("wfi");
}

// Wait for / signal an event: the WFE/SEV pair AntiSkill uses to park and wake
// a core spinning on a contended lock instead of burning the bus.
inline void wait_for_event() {
    asm volatile("wfe");
}

inline void send_event() {
    asm volatile("sev");
}

// DAIF (interrupt mask) save/restore for irqsave-style critical sections.
inline uint64_t read_daif() {
    uint64_t value = 0;
    asm volatile("mrs %0, daif" : "=r"(value));
    return value;
}

inline void write_daif(uint64_t value) {
    asm volatile("msr daif, %0" ::"r"(value) : "memory");
}

// Barriers in the inner-shareable domain (the domain the page tables and locks
// live in once SMP is up): order memory (dmb), drain it (dsb), flush the
// pipeline so new system-register state takes effect (isb).
inline void dmb_ish() {
    asm volatile("dmb ish" ::: "memory");
}

inline void dsb_ish() {
    asm volatile("dsb ish" ::: "memory");
}

inline void isb() {
    asm volatile("isb" ::: "memory");
}

// Raw MPIDR_EL1 affinity (Aff0..Aff3), the firmware-visible identity of a core
// and the target PSCI CPU_ON expects. Mask off the RES1/U/MT flag bits.
inline uint64_t read_mpidr() {
    uint64_t value = 0;
    asm volatile("mrs %0, mpidr_el1" : "=r"(value));
    return value & 0xFF00FFFFFFULL;
}

// Per-core scratch register: we stash this core's logical id (0..n-1) here at
// bring-up so any code can ask "which core am I?" in one instruction.
inline void write_tpidr_el1(uint64_t value) {
    asm volatile("msr tpidr_el1, %0" ::"r"(value) : "memory");
}

inline uint64_t read_tpidr_el1() {
    uint64_t value = 0;
    asm volatile("mrs %0, tpidr_el1" : "=r"(value));
    return value;
}

inline uint32_t this_cpu_id() {
    return static_cast<uint32_t>(read_tpidr_el1());
}

[[noreturn]] inline void halt() {
    while (true) {
        asm volatile("wfe");
    }
}

} // namespace index::arch
