#include "index/anti_skill.hpp"

#include "arch/aarch64/cpu.hpp"
#include "index/imaginary_number_district.hpp"

namespace index {

void anti_skill_init(AntiSkill &m) {
    m.lock = 0;
}

void anti_skill_lock(AntiSkill &m) {
    uint32_t held = 0;
    uint32_t failed = 0;
    // ldaxr/stxr must stay in one asm block so the exclusive monitor set by the
    // load survives to the store. `sevl` primes the local event register so the
    // first WFE falls through; thereafter a contended core sleeps on WFE and is
    // woken by the holder's store-release + SEV (see anti_skill_unlock).
    asm volatile(
        "   sevl\n"
        "1: wfe\n"
        "2: ldaxr   %w0, [%2]\n"   // load-acquire the lock word
        "   cbnz    %w0, 1b\n"     // already held -> wait for an event, retry
        "   stxr    %w1, %w3, [%2]\n" // try to claim it (store 1)
        "   cbnz    %w1, 2b\n"     // lost the store race -> retry without WFE
        : "=&r"(held), "=&r"(failed)
        : "r"(&m.lock), "r"(1u)
        : "memory");
    // [dbg] record holder for the deadlock watchdog (see _irqsave below).
    m.owner_cpu = static_cast<int32_t>(arch::this_cpu_id());
}

void anti_skill_unlock(AntiSkill &m) {
    m.owner_cpu = -1; // [dbg] mark free before releasing
    // Store-release publishes the critical section's writes before the unlock,
    // then SEV wakes any core parked on WFE in anti_skill_lock.
    asm volatile("stlr wzr, [%0]" ::"r"(&m.lock) : "memory");
    arch::send_event();
}

uint64_t anti_skill_lock_irqsave(AntiSkill &m) {
    const uint64_t flags = arch::read_daif();
    arch::disable_irq();
    // [dbg] deadlock watchdog: if the lock stays held implausibly long, the
    // holder probably leaked it (e.g. left the critical section via leave_user
    // without unlocking). Dump who holds it + the acquiring call site so the
    // leaking path is identifiable, then keep trying.
    uint64_t spins = 0;
    while (m.lock != 0) {
        // ~8e8 spins (~hundreds of ms) before flagging: g_fs_lock legitimately
        // holds for a while when a core reads a multi-MB ELF/.so off virtio-blk
        // while sibling cores exec concurrently, so a lower threshold false-
        // positived on parallel exec. A genuine deadlock is permanent and will
        // keep re-flagging past this window.
        if (++spins >= 800000000ull) {
            namespace d = imaginary_number_district;
            d::write("[DEADLOCK lock@"); d::hex(reinterpret_cast<uint64_t>(&m));
            d::write(" held_cpu="); d::dec(static_cast<uint64_t>(static_cast<uint32_t>(m.owner_cpu)));
            d::write(" site="); d::hex(m.owner_site);
            d::write(" spinner="); d::dec(arch::this_cpu_id());
            d::write("]\n");
            spins = 0;
        }
    }
    anti_skill_lock(m);
    m.owner_site = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    return flags;
}

void anti_skill_unlock_irqrestore(AntiSkill &m, uint64_t flags) {
    anti_skill_unlock(m);
    arch::write_daif(flags);
}

} // namespace index
