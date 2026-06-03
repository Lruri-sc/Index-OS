#pragma once

#include <stdint.h>

namespace index::drivers {

// Othinus rewrites the world: she can end it and remake it.
// Here she is the PSCI interface of the UTM / QEMU `virt` machine, the
// firmware door that powers the whole VM off (end the world) or resets it
// (remake the world). PSCI is reached through a `hvc` or `smc` trap; which
// conduit to use is declared by the firmware in the device tree `/psci`
// node's `method` property, so the kernel reads it instead of guessing.
enum class PsciConduit {
    none,
    hvc,
    smc,
};

struct Othinus {
    PsciConduit conduit = PsciConduit::none;
    bool available = false;
};

[[noreturn]] void othinus_system_off(const Othinus &world);
[[noreturn]] void othinus_system_reset(const Othinus &world);
const char *psci_conduit_name(PsciConduit conduit);

// Othinus does not only end the world -- she remakes it. PSCI CPU_ON powers on a
// secondary core: it begins executing at `entry_phys` (a physical address, since
// the woken core starts with its MMU off) with x0 = `context_id`. Returns the
// PSCI status (0 = SUCCESS, negative = error such as ALREADY_ON / INVALID_PARAMS).
int othinus_cpu_on(const Othinus &world, uint64_t target_mpidr, uint64_t entry_phys,
                   uint64_t context_id);

// Architectural standby idle: PSCI CPU_SUSPEND with state_type=standby. Unlike
// a bare WFI, this is a hypercall the hypervisor can recognise as "guest is
// doing real cpuidle, park the host vCPU thread for real" -- which is what
// finally lets HVF avoid the constant mainloop wake storm seen with naked
// WFI on Apple Silicon UTM. Returns when an interrupt wakes the core (just
// like WFI). Falls back to bare WFI if PSCI was never installed.
void othinus_cpu_suspend();

// Install the world handle so othinus_cpu_suspend() can find it after
// artificial_heaven goes out of scope. Call once during kmain.
void othinus_install(const Othinus &world);

} // namespace index::drivers
