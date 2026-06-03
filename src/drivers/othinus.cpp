#include "drivers/othinus.hpp"

#include "arch/aarch64/cpu.hpp"

namespace index::drivers {

namespace {

// PSCI v0.2 function IDs. SYSTEM_OFF/RESET use the SMC32 view; CPU_ON uses the
// SMC64 view (0xC4...) because it passes 64-bit MPIDR and entry-point arguments.
constexpr uint32_t kPsciSystemOff = 0x84000008;
constexpr uint32_t kPsciSystemReset = 0x84000009;
constexpr uint32_t kPsciCpuOn = 0xC4000003;
// CPU_SUSPEND -- spec allows both SMC32 and SMC64 IDs. SMC64 carries the
// 64-bit entry_point + context_id which we leave 0 since we only use the
// standby state type (returns to caller on wake, no entry needed).
constexpr uint32_t kPsciCpuSuspend = 0xC4000001;
// power_state for "standby state" (StateType = 0, StateID = 0, PowerLevel = 0).
// In the original ("v0.2") power_state format used by the QEMU/HVF firmware
// emulation: bits 16 = StateType (0=standby/retention, 1=powerdown); we want
// 0 to keep context. State ID 0 means "use the lowest-power state available".
constexpr uint32_t kPowerStateStandby = 0x00000000;

Othinus g_world;
bool g_world_installed = false;

uint64_t psci_call(uint32_t function, PsciConduit conduit, uint64_t a1 = 0,
                   uint64_t a2 = 0, uint64_t a3 = 0) {
    register uint64_t x0 asm("x0") = function;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;

    // Per SMCCC the SMC/HVC callee may clobber x0..x17 (only x18..x30 are
    // preserved). The old asm declared x1..x3 as plain inputs and left x4..x17
    // un-clobbered, so the compiler could keep a live value in any of those
    // registers across the call -- PSCI then overwrote it, and on CPU_SUSPEND
    // that intermittently smashed a value the caller later branched through
    // (the cpu-N wild-jump to 0x80000345). Mark x0..x3 in/out and clobber
    // x4..x17, matching Linux's __arm_smccc_smc.
    if (conduit == PsciConduit::smc) {
        asm volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
                       "x11", "x12", "x13", "x14", "x15", "x16", "x17");
    } else {
        asm volatile("hvc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
                       "x11", "x12", "x13", "x14", "x15", "x16", "x17");
    }
    return x0;
}

} // namespace

[[noreturn]] void othinus_system_off(const Othinus &world) {
    if (world.available && world.conduit != PsciConduit::none) {
        psci_call(kPsciSystemOff, world.conduit);
    }
    // PSCI returns only on failure; idle if the world refuses to end.
    arch::halt();
}

[[noreturn]] void othinus_system_reset(const Othinus &world) {
    if (world.available && world.conduit != PsciConduit::none) {
        psci_call(kPsciSystemReset, world.conduit);
    }
    arch::halt();
}

int othinus_cpu_on(const Othinus &world, uint64_t target_mpidr, uint64_t entry_phys,
                   uint64_t context_id) {
    if (!world.available || world.conduit == PsciConduit::none) {
        return -1;
    }
    const uint64_t status =
        psci_call(kPsciCpuOn, world.conduit, target_mpidr, entry_phys, context_id);
    return static_cast<int>(status); // low 32 bits carry the signed PSCI status
}

void othinus_install(const Othinus &world) {
    g_world = world;
    g_world_installed = true;
}

void othinus_cpu_suspend() {
    if (!g_world_installed || !g_world.available || g_world.conduit == PsciConduit::none) {
        asm volatile("wfi" ::: "memory");
        return;
    }
    // Issue PSCI CPU_SUSPEND with a STANDBY (retention, StateType=0) state as
    // a power hint, then ALWAYS execute WFI as the real sleep. Rationale:
    // under Apple HVF + GICv3 the hypervisor's CPU_SUSPEND for a standby state
    // returns 0 *without blocking* (it's a no-op accept), so the old "only
    // fall back to WFI when status != 0" logic left run_espers spinning at
    // ~245k iter/s = 99% host CPU (bisected 2026-06-01: bare WFI here = 0.0%,
    // PSCI-only = 99%). A standby suspend is WFI-equivalent and returns to the
    // caller, so following it with WFI is always correct:
    //   - HVF no-op accept (returns 0, didn't sleep) -> WFI does the sleep.
    //   - real retention suspend (slept, woke on IRQ) -> the pending IRQ makes
    //     WFI return immediately; no extra latency, no spin.
    //   - error return (NOT_SUPPORTED / INVALID_PARAMS) -> WFI does the sleep.
    // (A powerdown StateType=1 would NOT return; we deliberately use standby.)
    (void)psci_call(kPsciCpuSuspend, g_world.conduit, kPowerStateStandby, 0, 0);
    asm volatile("wfi" ::: "memory");
}

const char *psci_conduit_name(PsciConduit conduit) {
    switch (conduit) {
    case PsciConduit::hvc:
        return "hvc";
    case PsciConduit::smc:
        return "smc";
    case PsciConduit::none:
        return "none";
    }
    return "none";
}

} // namespace index::drivers
