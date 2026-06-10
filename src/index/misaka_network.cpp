#include "index/misaka_network.hpp"

#include "arch/aarch64/cpu.hpp"
#include "drivers/aleister.hpp"
#include "drivers/misaka_mail.hpp"
#include "drivers/othinus.hpp"
#include "index/aiwass.hpp"                   // [WD] aiwass_report
#include "index/anti_skill.hpp"
#include "index/antenna.hpp"                  // [WD] antenna_report
#include "index/esper.hpp"
#include "index/imaginary_number_channel.hpp" // [WD] inc_report
#include "index/imaginary_number_district.hpp"
#include "index/imprimatur.hpp"
#include "index/judgement.hpp"
#include "index/last_order.hpp"
#include "index/radio_noise.hpp"
#include "index/sister_relay.hpp"             // [WD] sr_report
#include "index/teleport.hpp"
#include "index/usermode.hpp"

// Implemented in arch/aarch64/misaka_switch.S.
extern "C" void misaka_switch(uint64_t *prev_ctx, uint64_t *next_ctx);
extern "C" void sister_trampoline();
// Implemented in arch/aarch64/boot.S: the physical entry a woken core lands on.
extern "C" void secondary_entry();
// Implemented in arch/aarch64/exceptions.S.
extern "C" void install_exception_vectors();

namespace index {

namespace {

constexpr uint32_t kMaxSisters = 24; // room for one idle per core + workers + shell
// Sister kernel stacks. These MUST be sized for running EL0 Espers, not just
// pure kernel-thread work: the boot core's idle Sister (idle_entry) calls
// el0_try_run_one(), so an Esper's syscall/IRQ traps onto THIS stack and
// descends deep C chains (execve -> load_elf -> ELF parse + file reads; dlopen
// mmap; the full C++ runtime; signal-frame builds; and a 100 Hz preempt can
// nest another). At 8 KiB that overflowed mid-syscall and smashed the saved
// frame -> `ret` to 0 (EL1 instruction abort, PC=0) and adjacent-stack
// corruption -- the broad SMP wild-write seen as the 8-core reconnect crash.
// The 64 KiB fix was applied to secondaries (kSecondaryStackSize) but the boot
// core's idle Sister was left at 8 KiB; match it here. Linux per-CPU THREAD_SIZE
// is in the same ballpark. (24 * 64 KiB = 1.5 MiB .bss; RAM is 2 GiB.)
constexpr uint32_t kStackSize = 64 * 1024;
constexpr uint32_t kSecondaryStackSize = 64 * 1024;

// Scheduling priority, borrowed from Academy City's esper ranks (Level 0..5):
// the scheduler always runs the highest-Level ready Sister, round-robin among
// equals. The shell sits at the top so it is never starved; idle at the bottom.
constexpr uint8_t kLevelIdle = 0;
constexpr uint8_t kLevelDefault = 2;
constexpr uint8_t kLevelShell = 5;

// Context slot indices, matching misaka_switch.S.
constexpr uint32_t kCtxX19 = 0;  // entry function on a fresh Sister
constexpr uint32_t kCtxX20 = 1;  // argument on a fresh Sister
constexpr uint32_t kCtxLr = 11;  // x30
constexpr uint32_t kCtxSp = 12;
constexpr uint32_t kCtxWords = 13;

enum class State { free, ready, running, sleeping, blocked, dead };

struct Sister {
    uint64_t context[kCtxWords];
    State state = State::free;
    uint8_t base_level = kLevelDefault; // the Sister's own Level
    uint8_t level = kLevelDefault;      // effective Level (may be boosted)
    uint32_t owner = 0;      // cpu running this Sister, valid when state==running
    uint64_t runs = 0;       // times scheduled onto a core
    uint64_t wake_at = 0;    // LastOrder beat to wake at, when sleeping
    volatile uint64_t work = 0;
    const char *name = "";
};

// One descriptor per physical core. `current` is the Sister this core is running
// (each core has its own); MPIDR is the firmware identity.
struct PerCpu {
    uint32_t current = 0;
    uint64_t mpidr = 0;
    volatile bool online = false;
};

// Count of Level Upper boosts that actually raised a holder's Level, so the
// priority-inheritance demo can prove inheritance fired.
uint64_t g_level_upper_boosts = 0;

// One AntiSkill spinlock guards ALL of the shared scheduler state below: every
// Sister's state/owner, pick_next, each core's `current`, spawning, and the
// sleep/block/unblock transitions. See the handoff protocol on switch_to.
AntiSkill g_sched_lock;

alignas(16) uint8_t g_stacks[kMaxSisters][kStackSize];      // spawned Sister stacks
alignas(16) uint8_t g_secondary_stacks[kMaxCpus][kSecondaryStackSize]; // secondaries' kernel stacks (run EL0 syscalls)
Sister g_sisters[kMaxSisters];
PerCpu g_percpu[kMaxCpus];
uint32_t g_count = 0;

// --- Kernel-stack overflow guard ----------------------------------------
// Every kernel stack grows DOWN; an overflow walks off the bottom and
// silently smashes whatever .bss sits below it (that was the 8 KiB
// idle-Sister overflow that produced the broad SMP wild-write / ELR=0).
// We can't cheaply unmap a true hardware guard page (the stacks live inside
// a 1 GiB RAM block; splitting it on the live kernel map is risky), so we
// poison the bottom 16 bytes of every stack with a canary and re-check it
// from the 100 Hz tick: a runaway descent writes through the bottom canary
// just before it leaves the stack, so a clobbered canary is a deterministic
// "this stack overflowed" signal that names the offending stack -- any future
// "stack too small" regression now reports loudly instead of corrupting RAM.
extern "C" char __stack_top[]; // boot core's main stack top (linker.ld; 64 KiB below)
constexpr uint64_t kStackCanary = 0x5354414B47554152ULL; // "STAKGUAR"
constexpr uint64_t kBootStackSize = 64 * 1024;            // linker.ld: `. += 64K`

void stack_canary_arm() {
    for (uint32_t i = 0; i < kMaxSisters; ++i) {
        auto *p = reinterpret_cast<uint64_t *>(g_stacks[i]);
        p[0] = kStackCanary; p[1] = kStackCanary;
    }
    for (uint32_t i = 0; i < kMaxCpus; ++i) {
        auto *p = reinterpret_cast<uint64_t *>(g_secondary_stacks[i]);
        p[0] = kStackCanary; p[1] = kStackCanary;
    }
    auto *b = reinterpret_cast<uint64_t *>(__stack_top - kBootStackSize);
    b[0] = kStackCanary; b[1] = kStackCanary;
}

// Returns -1 if all canaries intact; else a code: 0..kMaxSisters-1 = sister
// stack slot, 1000+cpu = secondary stack, 2000 = boot main stack.
int stack_canary_check() {
    for (uint32_t i = 0; i < kMaxSisters; ++i) {
        const auto *p = reinterpret_cast<volatile uint64_t *>(g_stacks[i]);
        if (p[0] != kStackCanary || p[1] != kStackCanary) return static_cast<int>(i);
    }
    for (uint32_t i = 0; i < kMaxCpus; ++i) {
        const auto *p = reinterpret_cast<volatile uint64_t *>(g_secondary_stacks[i]);
        if (p[0] != kStackCanary || p[1] != kStackCanary) return 1000 + static_cast<int>(i);
    }
    const auto *b = reinterpret_cast<volatile uint64_t *>(__stack_top - kBootStackSize);
    if (b[0] != kStackCanary || b[1] != kStackCanary) return 2000;
    return -1;
}
volatile uint32_t g_online_cpus = 0;

Sister *current_sister() {
    return &g_sisters[g_percpu[arch::this_cpu_id()].current];
}

// Strip the higher-half base so a kernel symbol's address becomes the physical
// address a secondary needs while its MMU is still off. Physical RAM lives far
// below kHighHalfBase, so this is a no-op when the value is already physical.
uint64_t phys_of(uint64_t va) {
    return va & ~kHighHalfBase;
}

const char *idle_name(uint32_t cpu) {
    static const char *const names[kMaxCpus] = {
        "idle0", "idle1", "idle2", "idle3", "idle4", "idle5", "idle6", "idle7"};
    return cpu < kMaxCpus ? names[cpu] : "idle";
}

uint32_t pick_next(uint32_t from) {
    // Highest Level among all ready Sisters wins. Sisters that are `running` on a
    // core are not `ready`, so they are never picked by another core.
    int best_level = -1;
    for (uint32_t i = 0; i < kMaxSisters; ++i) {
        if (g_sisters[i].state == State::ready &&
            static_cast<int>(g_sisters[i].level) > best_level) {
            best_level = static_cast<int>(g_sisters[i].level);
        }
    }
    if (best_level < 0) {
        return from; // nobody ready; caller stays put
    }

    // Round-robin among the ready Sisters at that Level, starting after `from`.
    for (uint32_t step = 1; step <= kMaxSisters; ++step) {
        const uint32_t idx = (from + step) % kMaxSisters;
        if (g_sisters[idx].state == State::ready &&
            static_cast<int>(g_sisters[idx].level) == best_level) {
            return idx;
        }
    }
    return from;
}

int alloc_slot() {
    for (uint32_t i = 0; i < kMaxSisters; ++i) {
        if (g_sisters[i].state == State::free) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Create a ready Sister. PRECONDITION: caller holds g_sched_lock (or we are in
// single-core init before IRQs are on).
void spawn(void (*entry)(void *), void *arg, const char *name, uint8_t level) {
    const int slot = alloc_slot();
    if (slot < 0) {
        return;
    }

    Sister &s = g_sisters[slot];
    for (uint32_t i = 0; i < kCtxWords; ++i) {
        s.context[i] = 0;
    }
    // Run every Sister entirely in the kernel high half (TTBR1): entry code, the
    // trampoline return address, and the stack. spawn() is usually called while
    // the boot core still executes kmain at LOW/identity VAs, so &entry,
    // &sister_trampoline and &g_stacks[slot] come out as LOW aliases -- a Sister
    // launched from them runs kernel code and pushes its kernel stack through
    // TTBR0. When that Sister then runs an EL0 Esper (the idle Sisters call
    // el0_try_run_one) and the Esper traps to EL1, TTBR0 is the Esper's page
    // table, so the kernel's low code/stack VAs resolve through IT -> kernel
    // return addresses get written into the Esper's user pages and kernel fetches
    // go wild (the intermittent SMP EL0-bad-PC / cpu-N wild-jump corruption).
    // Forcing the high aliases makes kernel execution independent of TTBR0,
    // matching the secondaries (secondary_main) and mainstream kernels (the
    // kernel lives in TTBR1; TTBR0 is purely the user address space). `arg` is
    // left as-is: it is opaque data (often null / a non-pointer), not code.
    s.context[kCtxX19] = teleport_high_alias(reinterpret_cast<uint64_t>(entry));
    s.context[kCtxX20] = reinterpret_cast<uint64_t>(arg);
    s.context[kCtxLr] = teleport_high_alias(reinterpret_cast<uint64_t>(&sister_trampoline));
    const uint64_t top =
        teleport_high_alias(reinterpret_cast<uint64_t>(g_stacks[slot] + kStackSize));
    s.context[kCtxSp] = top & ~uint64_t(0xF);
    s.state = State::ready;
    s.base_level = level;
    s.level = level;
    s.owner = 0;
    s.runs = 0;
    s.work = 0;
    s.name = name;
    ++g_count;
}

// Switch this core to `next`. PRECONDITION: g_sched_lock held and IRQs masked.
// The lock is held ACROSS the context switch and is released by whichever
// context we switch into -- a resumed context returns through its own
// unlock_irqrestore, a fresh Sister releases it in sister_trampoline. We return
// here only when some core later switches back to us, with the lock held again.
void switch_to(uint32_t next) {
    const uint32_t cpu = arch::this_cpu_id();
    const uint32_t prev = g_percpu[cpu].current;
    if (next == prev) {
        g_sisters[prev].state = State::running; // nobody else ready; keep running it
        g_sisters[prev].owner = cpu;
        return;
    }
    g_sisters[next].state = State::running;
    g_sisters[next].owner = cpu;
    g_sisters[next].runs = g_sisters[next].runs + 1;
    g_percpu[cpu].current = next;
    misaka_switch(g_sisters[prev].context, g_sisters[next].context);
}

// Wake any Sister whose sleep deadline has passed. Caller holds g_sched_lock.
void wake_due_sleepers() {
    const uint64_t now = last_order_ticks();
    for (uint32_t i = 0; i < kMaxSisters; ++i) {
        if (g_sisters[i].state == State::sleeping && now >= g_sisters[i].wake_at) {
            g_sisters[i].state = State::ready;
        }
    }
}

// Run from IRQ context after the timer beat is EOI'd: wake sleepers, then have
// this core round-robin to the next ready Sister.
void resched() {
    const uint64_t flags = anti_skill_lock_irqsave(g_sched_lock);
    wake_due_sleepers();
    const uint32_t cpu = arch::this_cpu_id();
    const uint32_t cur = g_percpu[cpu].current;
    if (g_sisters[cur].state == State::running) {
        g_sisters[cur].state = State::ready; // demote so it can be re-picked
    }
    switch_to(pick_next(cur));
    anti_skill_unlock_irqrestore(g_sched_lock, flags);
}

// While EL0 user processes are running (only on the boot core), the timer must
// preempt *Espers* (via the saved IRQ frame), not switch Sisters. usermode
// registers that hook here.
void (*g_user_tick)(uint64_t *frame) = nullptr;
bool g_user_mode = false;

// Reschedule IPI (GICv3 SGI 0). The handler itself is a near no-op: the value
// is in the *delivery* -- it pulls the target CPU out of WFI and back through
// irq_dispatch -> after_eoi (network_tick), where the idle loop re-checks its
// runqueue. Mirrors Linux's scheduler_ipi(): the IPI need do nothing beyond
// forcing a return to the scheduler. g_resched_ipi_count is a self-test
// counter proving SGIs actually arrive on the target core.
constexpr uint32_t kReschedSgi = 0;
volatile uint64_t g_resched_ipi_count = 0;
void resched_ipi_handler(void *) {
    __atomic_fetch_add(&g_resched_ipi_count, 1, __ATOMIC_RELAXED);
}

// The after-EOI hook: route the timer tick to the right scheduler. Boot core
// drains virtio-net (vmnet single-core RX path). Any core preempts its running
// Esper when user mode is active: esper_preempt early-returns if the saved
// frame is an EL1 (kernel/idle) interrupt, so calling it on an idle secondary
// is a safe no-op; on a secondary running an Esper it round-robins to the next
// runnable one for that cpu.
// Defined in kernel.cpp's exception_report: once any core takes a fatal
// exception we stop emitting [WD] so the crash backtrace is the last clean
// thing on the console (instead of being buried under recurring [WD] blocks).
extern "C" volatile bool g_panicked;
void network_tick(uint64_t *frame) {
    if (arch::this_cpu_id() == 0) {
        // Kernel-stack overflow guard: cheap canary scan every tick (~40 stacks
        // x 2 loads). A clobbered bottom canary means that stack overran -- log
        // it loudly ONCE (latched) so the offending stack is named instead of
        // the failure showing up later as a random wild-write / ELR=0 crash.
        {
            static bool sg_reported = false;
            if (!sg_reported) {
                const int bad = stack_canary_check();
                if (bad >= 0) {
                    sg_reported = true;
                    namespace district = imaginary_number_district;
                    district::write("\n[STACKGUARD] KERNEL STACK OVERFLOW -- ");
                    if (bad >= 2000) district::write("boot main stack");
                    else if (bad >= 1000) { district::write("secondary cpu "); district::dec(static_cast<uint64_t>(bad - 1000)); }
                    else { district::write("sister stack slot "); district::dec(static_cast<uint64_t>(bad)); }
                    district::writeln(" bottom canary clobbered (stack too small / runaway recursion -- bump kStackSize/kSecondaryStackSize)");
                }
            }
        }
        drivers::misaka_mail_drain();
        // Generic poll wakeup: a tick may have ingested socket data; even when
        // it hasn't, parked ppoll/pselect waiters must re-check periodically
        // (mirrors Linux poll() being driven by the scheduler + softirqs).
        linux_ipc_wake(Esper::IpcWaitKind::PollWait, -1);
        // Same for epoll_pwait: a timerfd registered in an epoll set becomes
        // readable purely by CNTPCT advancing (no producer event, no IRQ), so a
        // parked epoll waiter must be re-kicked every tick to re-scan + honour its
        // timeout -- without this an epoll on a timerfd hangs forever (SMP=1 froze).
        linux_ipc_wake(Esper::IpcWaitKind::EpollWait, -1);
        // [WD] Hang diagnosis: while a TCP connection is live (not just the
        // listen socket -- antenna_has_active_socket excludes Listen), snapshot
        // every esper's wait state + the pty rings + sockets every ~2 s. The
        // dump appearing at all also proves CPU0 is still ticking (vs a wedged
        // core). Silent when no connection. Remove once the pty hang is fixed.
        // Keep large downloads (apt) from stalling on HVF: while a TCP connection
        // is live, re-ring the virtio-net RX doorbell each tick to force a vm-exit
        // so qemu's main loop gets the BQL, runs SLIRP, and delivers pending RX.
        // Without this a busy-spinning guest (apt http recv + doorbell-poll ext2
        // write) starves qemu's main thread and the download stalls. This is the
        // clean replacement for the old [WD] console-dump, which inadvertently
        // supplied these vm-exit kicks -- disabling that dump made apt-get update
        // stall at "Get:1 InRelease". (The verbose [WD] esper-state dump for the
        // pty hang is gone; that hang is fixed.)
        if (!g_panicked && antenna_has_active_socket()) {
            // Re-ring the virtio-net RX doorbell each tick while a TCP socket is
            // live: a vm-exit that lets qemu deliver any SLIRP-buffered RX into the
            // ring (we drain it just above). Got apt-get update's download flowing
            // from "Get:1" to ~97% of InRelease. (The remaining ~3% is the server
            // truncating apt's connection -- busybox wget completes the same 270KB
            // download over this same TCP stack, so it is NOT an Index TCP bug; see
            // apt-chroot.md part8.)
            drivers::misaka_mail_kick_rx();
            // NOTE: the *tail* of a large real-mirror download (apt InRelease ~97%
            // / "261KB" stall) is delivered unreliably while the reader is parked:
            // virtio-net RX IRQ is gated off on HVF GICv3 (enabling SPI 35-38 hangs
            // probe), so RX-during-park rides this single doorbell poll, which is
            // enough for the bulk but not the trailing segments. An in-tick yield-
            // spin "fix" worked once but was unreliable + heavy in the IRQ; the real
            // fix needs the virtio RX IRQ (currently blocked). DNS + bulk download
            // work; large-file completion on real mirrors is the remaining gap.
        }
    }
    if (g_user_mode && g_user_tick != nullptr) {
        g_user_tick(frame);
        return;
    }
    resched();
}

void idle_entry(void *) {
    // SMP EL0 idle loop (IPI-driven). Each iteration tries to grab a runnable
    // Esper for THIS cpu; if none, WFI until something wakes us. Two things
    // make this not burn a core (unlike the earlier naive attempt):
    //  1. A secondary's pick (pick_and_claim, cpu!=0) does NOT wake_sleepers,
    //     so it never resurrects the busybox prompt-poll nanosleeper and never
    //     livelocks running+resleeping it. Idle secondaries see only Espers
    //     some other path made ready (clone/IPC/futex), so at true idle
    //     el0_try_run_one returns false and we WFI.
    //  2. Wakeups arrive as a reschedule SGI (esper_kick_secondaries) which is
    //     pending at the GIC; WFI returns immediately if an IRQ is pending, so
    //     there is no lost-wakeup window between the failed pick and the WFI.
    // The 100 Hz timer also wakes us, but a no-work pick is O(kMaxEspers) under
    // the lock -- cheap -- so periodic ticks don't heat the core.
    for (;;) {
        // CRITICAL: re-enable IRQs each iteration. Running an Esper that then
        // parks returns here via leave_user, which does NOT restore DAIF -- so
        // we arrive with IRQs masked (the EL0 syscall trap masked them). A
        // masked WFI does not sleep on a pending-but-masked IRQ: it returns
        // immediately, and because the timer IRQ is never taken its handler
        // never re-arms, leaving it permanently pending -> 100% spin (measured
        // 11M idle-loop iters/s). enable_irq lets the pending timer be taken
        // and cleared, then WFI genuinely sleeps until the next IRQ / SGI.
        arch::enable_irq();
        if (el0_try_run_one()) continue;
        // Restore the kernel address space before idling. After running an EL0
        // Esper this core's TTBR0 still points at that (possibly freed) per-
        // process page table; leaving it installed across a deep idle made the
        // HVF vCPU thread fail to halt (~100%/core post-multithread). Switch
        // back to the kernel TTBR0 (TTBR1 always maps the kernel; TTBR0 should
        // be benign) before suspend.
        asm volatile("msr ttbr0_el1, %0" ::"r"(teleport_kernel_ttbr0()));
        asm volatile("dsb ish; tlbi vmalle1is; dsb ish; isb" ::: "memory");
        // PSCI standby suspend (not bare WFI): on Apple HVF a bare WFI on a
        // secondary that has run EL0 does not reliably halt the backing vCPU
        // thread, whereas CPU_SUSPEND parks it (same reason boot uses it).
        drivers::othinus_cpu_suspend();
    }
}

void worker_entry(void *) {
    // current_sister() is this worker at first entry; capture it once -- the
    // pointer is to the Sister's own slot, so it stays valid even if preemption
    // later resumes this worker on a different core.
    Sister *self = current_sister();
    for (;;) {
        self->work = self->work + 1;
        for (volatile uint32_t spin = 0; spin < 200000; spin = spin + 1) {
        }
    }
}

void sleeper_entry(void *) {
    Sister *self = current_sister();
    for (;;) {
        self->work = self->work + 1;
        misaka_network_sleep(100); // ~1 second at 100 Hz, then wake and repeat
    }
}

Imprimatur g_relay;            // shared permit between the demo pair
bool g_prodcons_started = false;

void producer_entry(void *) {
    Sister *self = current_sister();
    for (;;) {
        misaka_network_sleep(100); // produce one item per ~second
        self->work = self->work + 1;
        imprimatur_post(g_relay); // grant the consumer a permit
    }
}

void consumer_entry(void *) {
    Sister *self = current_sister();
    for (;;) {
        imprimatur_wait(g_relay); // block until the producer grants a permit
        self->work = self->work + 1;
    }
}

bool g_levels_started = false;

// Two busy workers at different Levels. With strict-priority scheduling the
// higher-Level one hogs a core, so its `work` races ahead of the lower one.
void busy_entry(void *) {
    Sister *self = current_sister();
    for (;;) {
        self->work = self->work + 1;
        for (volatile uint32_t spin = 0; spin < 200000; spin = spin + 1) {
        }
    }
}

Judgement g_crit;             // mutex guarding the shared counter below
volatile uint64_t g_shared = 0;
bool g_judgement_started = false;

// Two Sisters hammer a shared counter under one Judgement lock. If the lock
// works, g_shared always equals the sum of both Sisters' local `work`.
void critical_entry(void *) {
    Sister *self = current_sister();
    for (;;) {
        judgement_lock(g_crit);
        // Critical section: read-modify-write the shared counter with a
        // deliberate gap, so a missing lock would lose updates.
        const uint64_t v = g_shared;
        for (volatile uint32_t spin = 0; spin < 5000; spin = spin + 1) {
        }
        g_shared = v + 1;
        judgement_unlock(g_crit);

        self->work = self->work + 1;
        misaka_network_sleep(10); // ~100 ms, let the other contend
    }
}

// Priority-inversion demo. pi-low (Lv1) holds g_pi while doing CPU work;
// pi-mid (Lv3) is a busy hog that would normally starve pi-low; pi-high (Lv4)
// periodically needs g_pi. Without priority inheritance, pi-mid starves pi-low
// so pi-low never releases and pi-high (Lv4) is stuck forever. With Level Upper,
// pi-low inherits Lv4, outruns pi-mid, releases, and pi-high makes progress.
Judgement g_pi;
bool g_pi_started = false;

void pi_low_entry(void *) {
    Sister *self = current_sister();
    for (;;) {
        judgement_lock(g_pi);
        // Critical section bounded by WALL CLOCK (not spin count, which differs
        // wildly between TCG and HVF). We can only observe the deadline and
        // release if we actually run; under pi-mid's hog that happens only once
        // pi-high's wait boosts us above pi-mid via Level Upper.
        const uint64_t deadline = last_order_ticks() + 20;
        while (last_order_ticks() < deadline) {
            // busy-hold the lock
        }
        self->work = self->work + 1;
        judgement_unlock(g_pi);
        // No sleep: re-acquire within this same time slice so the lock is held
        // again before pi-mid preempts us, sustaining the inversion each round.
    }
}

void pi_mid_entry(void *) {
    Sister *self = current_sister();
    misaka_network_sleep(15); // let pi-low take the lock first
    for (;;) {
        self->work = self->work + 1; // pure hog: never sleeps, never yields
        for (volatile uint32_t spin = 0; spin < 100000; spin = spin + 1) {
        }
    }
}

void pi_high_entry(void *) {
    Sister *self = current_sister();
    misaka_network_sleep(25); // start after the hog is running, so we contend
    for (;;) {
        judgement_lock(g_pi);
        self->work = self->work + 1;
        judgement_unlock(g_pi);
        misaka_network_sleep(10);
    }
}

// RadioNoise demo: a sender streams an incrementing sequence through a bounded
// mailbox to a slower receiver. The small buffer means the sender blocks on
// "full" and the receiver blocks on "empty"; the receiver checks each message
// is the next expected value, so errors stays 0 iff the queue is FIFO-correct.
RadioNoise g_mailbox;
volatile uint64_t g_rn_sent = 0;
volatile uint64_t g_rn_recv = 0;
volatile uint64_t g_rn_errors = 0;
bool g_radio_started = false;

void radio_sender_entry(void *) {
    uint64_t seq = 0;
    for (;;) {
        radio_noise_send(g_mailbox, seq);
        ++seq;
        g_rn_sent = seq;
        misaka_network_sleep(5); // produce faster than the receiver drains
    }
}

void radio_receiver_entry(void *) {
    uint64_t expected = 0;
    for (;;) {
        const uint64_t got = radio_noise_recv(g_mailbox);
        if (got == expected) {
            ++expected;
            g_rn_recv = expected;
        } else {
            g_rn_errors = g_rn_errors + 1;
        }
        misaka_network_sleep(20); // slower drain -> sender blocks on a full queue
    }
}

const char *state_name(State s) {
    switch (s) {
    case State::ready:
        return "ready";
    case State::running:
        return "run";
    case State::sleeping:
        return "sleep";
    case State::blocked:
        return "block";
    case State::dead:
        return "dead";
    case State::free:
        return "free";
    }
    return "?";
}

// Turn the calling (already-running) secondary context into this core's idle
// Sister, then idle. PRECONDITION: IRQs masked on entry (a freshly woken core).
[[noreturn]] void become_idle(uint32_t cpu) {
    const uint64_t flags = anti_skill_lock_irqsave(g_sched_lock);
    const int slot = alloc_slot();
    if (slot >= 0) {
        Sister &s = g_sisters[slot];
        for (uint32_t i = 0; i < kCtxWords; ++i) {
            s.context[i] = 0; // saved when this context is first switched away
        }
        s.state = State::running;
        s.owner = cpu;
        s.base_level = kLevelIdle;
        s.level = kLevelIdle;
        s.runs = 1;
        s.work = 0;
        s.name = idle_name(cpu);
        ++g_count;
        g_percpu[cpu].current = static_cast<uint32_t>(slot);
    }
    g_percpu[cpu].online = true;
    g_online_cpus = g_online_cpus + 1;
    arch::dsb_ish(); // publish online + current before the boot core observes us
    anti_skill_unlock_irqrestore(g_sched_lock, flags);

    arch::enable_irq(); // current is set, so a timer IRQ now reschedules safely
    // SMP EL0 idle loop: same IPI-driven model as idle_entry. enable_irq each
    // iteration because leave_user (after an Esper parks) returns with IRQs
    // masked -- see idle_entry's comment; without it a masked WFI spins.
    for (;;) {
        arch::enable_irq();
        if (el0_try_run_one()) continue;
        // Restore the kernel address space before idling. After running an EL0
        // Esper this core's TTBR0 still points at that (possibly freed) per-
        // process page table; leaving it installed across a deep idle made the
        // HVF vCPU thread fail to halt (~100%/core post-multithread). Switch
        // back to the kernel TTBR0 (TTBR1 always maps the kernel; TTBR0 should
        // be benign) before suspend.
        asm volatile("msr ttbr0_el1, %0" ::"r"(teleport_kernel_ttbr0()));
        asm volatile("dsb ish; tlbi vmalle1is; dsb ish; isb" ::: "memory");
        // PSCI standby suspend (not bare WFI): on Apple HVF a bare WFI on a
        // secondary that has run EL0 does not reliably halt the backing vCPU
        // thread, whereas CPU_SUSPEND parks it (same reason boot uses it).
        drivers::othinus_cpu_suspend();
    }
}

} // namespace

extern "C" void sister_finish() {
    // A Sister's entry returned. Mark it dead under the lock and move this core
    // on; we never return here (a dead Sister is never rescheduled), so the lock
    // is released by the context we switch into -- not restored here.
    anti_skill_lock_irqsave(g_sched_lock);
    const uint32_t cpu = arch::this_cpu_id();
    g_sisters[g_percpu[cpu].current].state = State::dead;
    switch_to(pick_next(g_percpu[cpu].current));
    for (;;) {
        arch::wait_for_interrupt();
    }
}

// Called from sister_trampoline (assembly) the first time a fresh Sister runs:
// release the scheduler lock that the switching-out core handed us, before the
// trampoline unmasks IRQs and runs the Sister's entry.
extern "C" void misaka_network_trampoline_unlock() {
    anti_skill_unlock(g_sched_lock);
}

// Entry for a freshly woken secondary core (called from boot.S secondary_entry,
// MMU still off, on this core's own stack). Never returns.
// The high-half continuation of secondary bringup. Reached via an explicit jump
// to its high-half (TTBR1) alias so that from here on EVERY instruction fetch on
// this secondary goes through TTBR1 -- like the boot core and mainstream kernels.
// Previously secondaries ran the kernel (idle loop, syscall handlers) at LOW
// identity VAs via TTBR0 = the live EL0 process's page table; when that table
// was freed/reused under SMP the next kernel fetch hit garbage -> the cpu-N
// PC-alignment wild-jump to 0x80000345. Running high-half makes kernel execution
// independent of whatever TTBR0 holds.
static void secondary_main_high() {
    // Match the boot core: vectors at the higher-half alias.
    install_exception_vectors();
    uint64_t vbar = 0;
    asm volatile("mrs %0, vbar_el1" : "=r"(vbar));
    vbar = teleport_high_alias(vbar);
    asm volatile("msr vbar_el1, %0; isb" ::"r"(vbar) : "memory");

    // Find our logical id by matching the MPIDR the boot core published.
    const uint64_t mpidr = arch::read_mpidr();
    uint32_t cpu = 0;
    for (uint32_t i = 1; i < kMaxCpus; ++i) {
        if (g_percpu[i].mpidr == mpidr) {
            cpu = i;
            break;
        }
    }
    arch::write_tpidr_el1(cpu);

    drivers::aleister_init_cpu_interface(); // this core's GICC
    last_order_arm_secondary();             // this core's preemption timer

    become_idle(cpu); // adopt this core's idle Sister and start scheduling
}

extern "C" void secondary_main() {
    teleport_enable_secondary(); // MMU on, sharing the boot core's page tables
    // Re-base this core's stack pointer from the LOW/identity (TTBR0) address
    // boot.S set out of the PSCI context_id to its high-half (TTBR1) alias. A
    // kernel stack VA must NOT live in the TTBR0 range: once this core runs a
    // user Esper (el0_try_run_one installs the Esper's page table in TTBR0),
    // every EL1 stack push -- syscall/IRQ/fault handlers operating on SP_EL1 --
    // would translate the low stack VA through the *Esper's* page table,
    // colliding the secondary's kernel stack with user memory. That is the cpu-N
    // wild jump (faulting PC == SP, both in the 0x44xxxxxx stack band) and the
    // EL0 bad-PC SMP corruption. The high alias is the same physical stack mapped
    // via TTBR1 -- always the kernel map -- so stack access no longer depends on
    // whatever TTBR0 holds. 续13 moved kernel *code* to the high half but left the
    // stack low; this completes it. (Linux keeps secondary kernel stacks in the
    // kernel map for exactly this reason.) The switch is safe mid-function: the
    // high and low aliases are the same physical RAM, so the live frame stays
    // valid, and TTBR0 is still the kernel identity map here (no Esper yet).
    {
        uint64_t sp_now;
        asm volatile("mov %0, sp" : "=r"(sp_now));
        const uint64_t sp_high = teleport_high_alias(sp_now);
        asm volatile("mov sp, %0" ::"r"(sp_high) : "memory");
    }
    // Jump to the high half (TTBR1) for the rest of this core's life. Taking the
    // function's address at this LOW PC yields its low/physical VA (PC-relative);
    // OR in the high-half base to get the TTBR1 alias, then call it -- the same
    // trick the boot core uses to leave the identity map (see kernel.cpp). From
    // there kernel instruction fetch no longer depends on TTBR0.
    using Fn = void (*)();
    Fn cont = reinterpret_cast<Fn>(
        teleport_high_alias(reinterpret_cast<uint64_t>(&secondary_main_high)));
    cont(); // never returns (secondary_main_high -> become_idle loops forever)
}

void misaka_network_init() {
    arch::write_tpidr_el1(0); // boot core is logical cpu 0 (TPIDR is UNKNOWN at reset)
    anti_skill_init(g_sched_lock);
    stack_canary_arm(); // poison every kernel stack's bottom for overflow detection

    for (uint32_t i = 0; i < kMaxSisters; ++i) {
        g_sisters[i].state = State::free;
    }

    // The running boot/shell context becomes Sister 0; misaka_switch fills in
    // its saved context the first time it is switched away from.
    g_sisters[0].state = State::running;
    g_sisters[0].owner = 0;
    g_sisters[0].name = "main";
    g_sisters[0].base_level = kLevelShell; // shell never starves
    g_sisters[0].level = kLevelShell;
    g_sisters[0].runs = 1;
    g_count = 1;

    g_percpu[0].current = 0;
    g_percpu[0].mpidr = arch::read_mpidr();
    g_percpu[0].online = true;
    g_online_cpus = 1;

    drivers::aleister_set_after_eoi(network_tick);
    drivers::aleister_register(kReschedSgi, resched_ipi_handler, nullptr);
    spawn(idle_entry, nullptr, "idle0", kLevelIdle); // boot core's fallback idle
}

// Send the reschedule IPI to a logical CPU (wakes it from WFI to re-check its
// runqueue). No-op for the calling CPU itself (it's already running) and for
// offline / out-of-range CPUs. Used by the wake paths when they enqueue an
// Esper onto another core's runqueue.
void misaka_network_kick_cpu(uint32_t cpu) {
    if (cpu >= kMaxCpus || cpu == arch::this_cpu_id()) return;
    if (!g_percpu[cpu].online) return;
    drivers::aleister_send_sgi(g_percpu[cpu].mpidr, kReschedSgi);
}

uint64_t misaka_network_resched_ipi_count() { return g_resched_ipi_count; }

void misaka_network_bring_up_secondaries(const ArtificialHeaven &heaven,
                                         const drivers::Othinus &world) {
    const uint64_t boot_mpidr = arch::read_mpidr();
    const uint64_t entry = phys_of(reinterpret_cast<uint64_t>(&secondary_entry));

    uint32_t next_logical = 1;
    for (uint32_t i = 0; i < heaven.cpu_count && next_logical < kMaxCpus; ++i) {
        const uint64_t mpidr = heaven.cpus[i];
        if (mpidr == boot_mpidr) {
            continue; // the boot core is already logical cpu 0
        }
        const uint32_t cpu = next_logical;
        const uint64_t stack_top = phys_of(
            reinterpret_cast<uint64_t>(&g_secondary_stacks[cpu][kSecondaryStackSize]) & ~uint64_t(0xF));

        // Publish this core's identity so it can find its own logical id; the
        // PSCI context_id is the physical stack top so boot.S can set sp at once.
        g_percpu[cpu].mpidr = mpidr;
        g_percpu[cpu].online = false;
        arch::dsb_ish();

        if (drivers::othinus_cpu_on(world, mpidr, entry, stack_top) != 0) {
            continue; // firmware refused to start this core; leave it parked
        }

        // Wait (bounded) until the secondary marks itself online.
        bool up = false;
        for (uint64_t spin = 0; spin < 200000000ull; ++spin) {
            if (g_percpu[cpu].online) {
                up = true;
                break;
            }
            arch::dmb_ish();
        }
        if (up) {
            ++next_logical;
        }
    }

    // [P1.1 self-test] Verify the reschedule IPI path: send SGI 0 to every
    // online secondary and confirm the per-core handler ran. Each secondary is
    // in WFI (idle_entry); the SGI must wake it, irq_dispatch must route INTID
    // 0 to resched_ipi_handler (bumping g_resched_ipi_count), then it re-WFIs.
    // Counted globally; expect == number of secondaries after a short wait.
    namespace district = imaginary_number_district;
    const uint64_t before = g_resched_ipi_count;
    for (uint32_t cpu = 1; cpu < next_logical; ++cpu) {
        drivers::aleister_send_sgi(g_percpu[cpu].mpidr, kReschedSgi);
    }
    for (uint64_t spin = 0; spin < 50000000ull; ++spin) {
        if (g_resched_ipi_count >= before + (next_logical - 1)) break;
        arch::dmb_ish();
    }
    district::write("  Aleister (IPI)     : reschedule SGI delivered to ");
    district::dec(g_resched_ipi_count - before);
    district::write(" / ");
    district::dec(next_logical - 1);
    district::writeln(" secondaries");

    // Register the reschedule-IPI hook + online CPU count with the EL0
    // scheduler so wake paths can pull secondaries out of WFI to run Espers
    // in parallel (esper_kick_secondaries -> misaka_network_kick_cpu -> SGI).
    esper_set_kick(&misaka_network_kick_cpu);
    esper_set_online_cpus(g_online_cpus);
}

void misaka_network_set_user_tick(void (*hook)(uint64_t *frame)) {
    g_user_tick = hook;
}

void misaka_network_set_user_mode(bool active) {
    g_user_mode = active;
}

bool misaka_network_user_mode_active() {
    return g_user_mode;
}

bool misaka_network_spawn_demo() {
    const uint64_t flags = misaka_network_lock();
    bool ok = false;
    if (alloc_slot() >= 0) {
        spawn(worker_entry, nullptr, "worker", kLevelDefault);
        ok = true;
    }
    misaka_network_unlock(flags);
    return ok;
}

bool misaka_network_spawn_sleeper() {
    const uint64_t flags = misaka_network_lock();
    bool ok = false;
    if (alloc_slot() >= 0) {
        spawn(sleeper_entry, nullptr, "sleeper", kLevelDefault);
        ok = true;
    }
    misaka_network_unlock(flags);
    return ok;
}

bool misaka_network_spawn_named(void (*entry)(void *), void *arg, const char *name) {
    const uint64_t flags = misaka_network_lock();
    bool ok = false;
    if (alloc_slot() >= 0) {
        spawn(entry, arg, name, kLevelDefault);
        ok = true;
    }
    misaka_network_unlock(flags);
    return ok;
}

bool misaka_network_spawn_levels() {
    if (g_levels_started) {
        return false;
    }
    const uint64_t flags = misaka_network_lock();
    bool ok = false;
    if (alloc_slot() >= 0) {
        spawn(busy_entry, nullptr, "lv4-high", 4);
        if (alloc_slot() >= 0) {
            spawn(busy_entry, nullptr, "lv2-low", 2);
            g_levels_started = true;
            ok = true;
        }
    }
    misaka_network_unlock(flags);
    return ok;
}

bool misaka_network_spawn_judgement() {
    if (g_judgement_started) {
        return false;
    }
    const uint64_t flags = misaka_network_lock();
    bool ok = false;
    if (alloc_slot() >= 0) {
        judgement_init(g_crit);
        g_shared = 0;
        spawn(critical_entry, nullptr, "judge-A", kLevelDefault);
        if (alloc_slot() >= 0) {
            spawn(critical_entry, nullptr, "judge-B", kLevelDefault);
            g_judgement_started = true;
            ok = true;
        }
    }
    misaka_network_unlock(flags);
    return ok;
}

bool misaka_network_spawn_priority_inversion() {
    if (g_pi_started) {
        return false;
    }
    const uint64_t flags = misaka_network_lock();
    uint32_t free = 0;
    for (uint32_t i = 0; i < kMaxSisters; ++i) {
        if (g_sisters[i].state == State::free) {
            ++free;
        }
    }
    bool ok = false;
    if (free >= 3) {
        judgement_init(g_pi);
        spawn(pi_low_entry, nullptr, "pi-low", 1);
        spawn(pi_mid_entry, nullptr, "pi-mid", 3);
        spawn(pi_high_entry, nullptr, "pi-high", 4);
        g_pi_started = true;
        ok = true;
    }
    misaka_network_unlock(flags);
    return ok;
}

bool misaka_network_spawn_radio() {
    if (g_radio_started) {
        return false;
    }
    const uint64_t flags = misaka_network_lock();
    bool ok = false;
    if (alloc_slot() >= 0) {
        radio_noise_init(g_mailbox);
        g_rn_sent = 0;
        g_rn_recv = 0;
        g_rn_errors = 0;
        spawn(radio_sender_entry, nullptr, "rn-send", kLevelDefault);
        if (alloc_slot() >= 0) {
            spawn(radio_receiver_entry, nullptr, "rn-recv", kLevelDefault);
            g_radio_started = true;
            ok = true;
        }
    }
    misaka_network_unlock(flags);
    return ok;
}

bool misaka_network_spawn_prodcons() {
    if (g_prodcons_started) {
        return false;
    }
    const uint64_t flags = misaka_network_lock();
    bool ok = false;
    if (alloc_slot() >= 0) {
        imprimatur_init(g_relay, 0);
        spawn(producer_entry, nullptr, "producer", kLevelDefault);
        if (alloc_slot() >= 0) {
            spawn(consumer_entry, nullptr, "consumer", kLevelDefault);
            g_prodcons_started = true;
            ok = true;
        }
    }
    misaka_network_unlock(flags);
    return ok;
}

uint64_t misaka_network_shared_counter() {
    return g_shared;
}

uint64_t misaka_network_level_upper_count() {
    return g_level_upper_boosts;
}

void misaka_network_yield() {
    const uint64_t flags = anti_skill_lock_irqsave(g_sched_lock);
    const uint32_t cpu = arch::this_cpu_id();
    if (g_sisters[g_percpu[cpu].current].state == State::running) {
        g_sisters[g_percpu[cpu].current].state = State::ready;
    }
    switch_to(pick_next(g_percpu[cpu].current));
    anti_skill_unlock_irqrestore(g_sched_lock, flags);
}

void misaka_network_sleep(uint64_t ticks) {
    const uint64_t flags = anti_skill_lock_irqsave(g_sched_lock);
    const uint32_t cpu = arch::this_cpu_id();
    g_sisters[g_percpu[cpu].current].wake_at = last_order_ticks() + ticks;
    g_sisters[g_percpu[cpu].current].state = State::sleeping;
    switch_to(pick_next(g_percpu[cpu].current));
    anti_skill_unlock_irqrestore(g_sched_lock, flags);
}

uint32_t misaka_network_current_id() {
    return g_percpu[arch::this_cpu_id()].current;
}

uint64_t misaka_network_lock() {
    return anti_skill_lock_irqsave(g_sched_lock);
}

void misaka_network_unlock(uint64_t flags) {
    anti_skill_unlock_irqrestore(g_sched_lock, flags);
}

void misaka_network_block_current_locked() {
    const uint32_t cpu = arch::this_cpu_id();
    g_sisters[g_percpu[cpu].current].state = State::blocked;
    switch_to(pick_next(g_percpu[cpu].current));
}

void misaka_network_unblock_locked(uint32_t sister_id) {
    if (sister_id < kMaxSisters && g_sisters[sister_id].state == State::blocked) {
        g_sisters[sister_id].state = State::ready;
    }
}

uint8_t misaka_network_my_level() {
    return g_sisters[g_percpu[arch::this_cpu_id()].current].level;
}

void misaka_network_level_upper(uint32_t sister_id, uint8_t to_level) {
    // Priority inheritance: raise a lock holder to a waiter's Level so it can
    // run past mid-Level hogs, finish its critical section, and release.
    if (sister_id < kMaxSisters && to_level > g_sisters[sister_id].level) {
        g_sisters[sister_id].level = to_level;
        ++g_level_upper_boosts;
    }
}

void misaka_network_level_restore(uint32_t sister_id) {
    // Drop any inherited boost back to the Sister's own Level.
    if (sister_id < kMaxSisters) {
        g_sisters[sister_id].level = g_sisters[sister_id].base_level;
    }
}

void misaka_network_report() {
    namespace district = imaginary_number_district;

    district::write("MisakaNetwork: ");
    district::dec(g_count);
    district::write(" sister(s), ");
    district::dec(g_online_cpus);
    district::writeln(" core(s)");
    const uint32_t here = g_percpu[arch::this_cpu_id()].current;
    for (uint32_t i = 0; i < kMaxSisters; ++i) {
        if (g_sisters[i].state == State::free) {
            continue;
        }
        district::write("  [");
        district::dec(i);
        district::write("] ");
        district::write(g_sisters[i].name);
        district::write(" Lv");
        district::dec(g_sisters[i].base_level);
        if (g_sisters[i].level != g_sisters[i].base_level) {
            district::write("->"); // currently boosted by priority inheritance
            district::dec(g_sisters[i].level);
        }
        district::write(" state=");
        if (g_sisters[i].state == State::running) {
            district::write("run@");
            district::dec(g_sisters[i].owner); // which core it is running on
        } else {
            district::write(state_name(g_sisters[i].state));
        }
        district::write(" runs=");
        district::dec(g_sisters[i].runs);
        district::write(" work=");
        district::dec(g_sisters[i].work);
        district::writeln(i == here ? " <= here" : "");
    }
    if (g_judgement_started) {
        district::write("  Judgement shared counter = ");
        district::dec(g_shared);
        district::writeln(" (should equal judge-A.work + judge-B.work)");
    }
    if (g_pi_started) {
        district::write("  Level Upper boosts = ");
        district::dec(g_level_upper_boosts);
        district::writeln(" (priority inheritance; pi-high progresses only via these)");
    }
    if (g_radio_started) {
        district::write("  RadioNoise: sent=");
        district::dec(g_rn_sent);
        district::write(" recv=");
        district::dec(g_rn_recv);
        district::write(" errors=");
        district::dec(g_rn_errors);
        district::writeln(" (FIFO mailbox; errors must stay 0)");
    }
}

void misaka_network_smp_report() {
    namespace district = imaginary_number_district;

    district::write("MisakaNetwork SMP: ");
    district::dec(g_online_cpus);
    district::writeln(" core(s) online");
    for (uint32_t c = 0; c < kMaxCpus; ++c) {
        if (!g_percpu[c].online) {
            continue;
        }
        district::write("  cpu");
        district::dec(c);
        district::write(c == arch::this_cpu_id() ? "* mpidr=" : "  mpidr=");
        district::hex(g_percpu[c].mpidr);
        district::write(" running=");
        district::writeln(g_sisters[g_percpu[c].current].name);
    }
}

uint32_t misaka_network_online_cpus() {
    return g_online_cpus;
}

uint32_t misaka_network_count() {
    return g_count;
}

// Public wrapper so the crash dumper (kernel.cpp exception_report) can name a
// clobbered kernel-stack canary AT fault time -- the real check + the canary
// globals live in this file's anonymous namespace, still in scope here.
int kernel_stack_canary_check() { return stack_canary_check(); }

} // namespace index
