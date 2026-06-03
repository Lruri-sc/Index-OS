#include "index/esper.hpp"

#include "arch/aarch64/cpu.hpp"
#include "index/anti_skill.hpp"
#include "index/artificial_heaven.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/linux_abi.hpp" // linux_deliver_signal_locked (SIGCHLD on child exit)

namespace index {

// Scheduler-wide spinlock. Protects the per-Esper state-transition + the
// g_running array against concurrent picks on different CPUs. Equivalent to
// Linux's per-rq spinlock collapsed to a single global one (fine here --
// kMaxEspers = 16 keeps contention low and lookups O(N)). Exported via the
// header so callers that need a wider critical section (futex value-check +
// queue-insert, signal delivery + frame rewrite, address-space refcount)
// can hold it across additional writes.
AntiSkill g_esper_lock;

namespace {

Esper g_espers[kMaxEspers];
uint32_t g_next_pid = 1;
// Per-CPU "currently running" Esper, indexed by arch::this_cpu_id(). Mirrors
// Linux's per-CPU `current` (or `cpu_rq(cpu)->curr`): each CPU owns one slot
// and only writes its own. Cross-CPU reads (e.g. esper_pick_ready checking
// "is this Esper already running somewhere?") need the scheduler spinlock.
// Initialised to -1 (= no Esper running) because the slot 0 is a valid
// Esper index, not a sentinel.
int g_running[kMaxCpus] = {-1, -1, -1, -1, -1, -1, -1, -1};

// Number of online CPUs participating in EL0 scheduling (set by
// misaka_network once secondaries are up). 1 until then.
uint32_t g_online_cpus = 1;
// Reschedule-IPI hook (misaka_network_kick_cpu): wakes a secondary CPU from
// WFI so it re-checks for runnable Espers. Null until registered.
void (*g_kick_cpu)(uint32_t cpu) = nullptr;

void copy_name(char *dst, const char *src) {
    uint32_t i = 0;
    for (; src[i] && i + 1 < sizeof(Esper::name); ++i) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

const char *state_name(EsperState s) {
    switch (s) {
    case EsperState::ready:
        return "ready";
    case EsperState::running:
        return "running";
    case EsperState::waiting:
        return "waiting";
    case EsperState::exited:
        return "exited";
    case EsperState::faulted:
        return "faulted";
    case EsperState::free:
        return "free";
    }
    return "?";
}

} // namespace

int esper_create(const char *name) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    int slot = -1;
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        if (g_espers[i].state == EsperState::free) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        // Recycle the lowest-pid finished process.
        for (uint32_t i = 0; i < kMaxEspers; ++i) {
            if (g_espers[i].state == EsperState::exited ||
                g_espers[i].state == EsperState::faulted) {
                if (slot < 0 || g_espers[i].pid < g_espers[slot].pid) {
                    slot = static_cast<int>(i);
                }
            }
        }
    }
    if (slot < 0) {
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        return -1;
    }

    Esper &e = g_espers[slot];
    e = Esper{};
    e.pid = g_next_pid++;
    copy_name(e.name, name);
    e.pgrp = e.pid; // sole member of its own group by default; setpgid moves it
    e.sid = e.pid;  // own session leader by default; setsid bumps it explicitly
    // Leave the slot in waiting (with no wait predicate set so wake_sleepers
    // skips it): this hides the half-initialised Esper from concurrent
    // schedulers on other CPUs until the caller calls esper_make_ready.
    // On a single-core build the user-visible behaviour is unchanged.
    e.state = EsperState::waiting;
    e.fds[0].kind = FdKind::console; // stdin
    e.fds[1].kind = FdKind::console; // stdout
    e.fds[2].kind = FdKind::console; // stderr
    e.cwd[0] = '/';
    e.cwd[1] = 0;
    // Default rlimits (matches Linux glibc baseline). Indexes follow Linux's
    // RLIMIT_* enum:  0 CPU / 1 FSIZE / 2 DATA / 3 STACK / 4 CORE / 5 RSS /
    // 6 NPROC / 7 NOFILE / 8 MEMLOCK / 9 AS / 10 LOCKS / 11 SIGPENDING /
    // 12 MSGQUEUE / 13 NICE / 14 RTPRIO / 15 RTTIME.
    constexpr uint64_t kInf = ~uint64_t(0);
    e.rlimits[0]  = { kInf, kInf };          // CPU seconds: unlimited
    e.rlimits[1]  = { kInf, kInf };          // FSIZE
    e.rlimits[2]  = { kInf, kInf };          // DATA
    e.rlimits[3]  = { 8 * 1024 * 1024, kInf }; // STACK 8 MiB soft
    e.rlimits[4]  = { 0, kInf };             // CORE: 0 (no dumps by default)
    e.rlimits[5]  = { kInf, kInf };
    e.rlimits[6]  = { 32, 32 };              // NPROC -- fewer Espers than Linux
    e.rlimits[7]  = { kMaxFds, kMaxFds };    // NOFILE matches our fd table
    e.rlimits[8]  = { 64 * 1024, kInf };
    e.rlimits[9]  = { kInf, kInf };          // AS
    e.rlimits[10] = { kInf, kInf };
    e.rlimits[11] = { 64, 64 };
    e.rlimits[12] = { 819200, 819200 };
    e.rlimits[13] = { 0, 0 };
    e.rlimits[14] = { 0, 0 };
    e.rlimits[15] = { kInf, kInf };
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return slot;
}

void esper_make_ready(int idx) {
    if (idx < 0 || static_cast<uint32_t>(idx) >= kMaxEspers) {
        return;
    }
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    Esper &e = g_espers[idx];
    if (e.state == EsperState::waiting) {
        e.state = EsperState::ready;
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    // A freshly-runnable Esper (clone child, spawned program): wake the
    // secondaries so one of them can pick it up in parallel with this core.
    esper_kick_secondaries();
}

Esper *esper_at(uint32_t index) {
    return index < kMaxEspers ? &g_espers[index] : nullptr;
}

int esper_running_index() {
    const uint32_t cpu = arch::this_cpu_id();
    return cpu < kMaxCpus ? g_running[cpu] : -1;
}

void esper_set_online_cpus(uint32_t n) {
    g_online_cpus = (n == 0) ? 1 : (n > kMaxCpus ? kMaxCpus : n);
}

void esper_set_kick(void (*fn)(uint32_t cpu)) { g_kick_cpu = fn; }

int esper_running_on(uint32_t cpu) {
    return cpu < kMaxCpus ? g_running[cpu] : -1;
}

// Wake the secondary CPUs (reschedule IPI) so they re-scan for runnable
// Espers. Called after an Esper becomes ready (clone, IPC wake, child-exit
// parent wake, ...) so a parked secondary picks it up promptly instead of
// only at its next 100 Hz timer wake. Boot core (cpu 0) is always running
// the scheduler so it needs no kick. Safe to call with or without
// g_esper_lock held -- it only issues SGIs (no Esper-state access).
void esper_kick_secondaries() {
    if (g_kick_cpu == nullptr || g_online_cpus <= 1) return;
    // Kick ONE secondary, round-robin -- NOT all of them. Each wake makes one
    // Esper runnable, so waking one idle core to pick it is enough. Kicking
    // every core on every wake was a thundering herd: mt_cxx's mutex
    // contention fires futex_wake ~1000x, and 7 cores storming awake to pick
    // a single runnable (6 finding nothing, re-WFI) pinned ~6 cores (~600%).
    // If several Espers become runnable, successive wake calls round-robin to
    // different cores. (Linux likewise targets the IPI at one CPU.)
    static volatile uint32_t rr = 0;
    const uint32_t span = g_online_cpus - 1; // secondaries are cpu 1..online-1
    const uint32_t cpu = 1 + (__atomic_fetch_add(&rr, 1, __ATOMIC_RELAXED) % span);
    g_kick_cpu(cpu);
}

// --- Internal helpers (require g_esper_lock held) -------------------------

namespace {

// Walk waiting Espers and wake any whose nanosleep deadline has passed.
// Called from any pick-side primitive so a parked nanosleeper rejoins the
// runqueue at the moment its absolute wake time arrives.
void wake_sleepers_locked() {
    uint64_t now = 0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        Esper &e = g_espers[i];
        if (e.state == EsperState::waiting && e.wake_cntpct != 0 &&
            now >= e.wake_cntpct) {
            e.wake_cntpct = 0;
            e.state = EsperState::ready;
        }
    }
}

// Atomically pick + claim a ready Esper for THIS CPU. Caller holds
// g_esper_lock.
//
// SMP nanosleep policy: wake_sleepers is CENTRALISED on the boot core only.
// If every idle secondary woke nanosleepers in its own pick, the busybox
// prompt-poll nanosleeper would be resurrected ~constantly and all cores
// would race to run it -> it immediately re-sleeps -> thundering-herd
// livelock (measured ~98% idle when secondaries did this). Confining
// wake_sleepers to cpu 0 means a secondary's pick only ever sees Espers
// some other path made ready (clone, IPC wake, ...) -- never a sleeper it
// resurrects itself -- so an idle secondary finds nothing and WFIs. Boot
// core handles all timer-based wakeups centrally (its 100 Hz preempt +
// run_espers loop both run wake_sleepers).
int pick_and_claim_locked(int after) {
    if (arch::this_cpu_id() == 0) wake_sleepers_locked();
    for (uint32_t step = 1; step <= kMaxEspers; ++step) {
        const int idx = static_cast<int>((after + step) % kMaxEspers);
        if (g_espers[idx].state == EsperState::ready) {
            g_espers[idx].state = EsperState::running;
            const uint32_t cpu = arch::this_cpu_id();
            if (cpu < kMaxCpus) g_running[cpu] = idx;
            return idx;
        }
    }
    return -1;
}

// Legacy parent-wake predicate. Existing single-core code uses
// wait_pipe_idx < 0 as a proxy for "in wait4()". A nanosleeper or
// IPC/futex blocker has wait_pipe_idx == -1 too, so we tighten the check
// here to "no wait predicate set at all" -- otherwise a child exiting
// could erroneously inject its exit code into an unrelated blocker's
// regs[0]. (This is also what makes safe a future caller that wakes
// parents from cross-CPU contexts.)
bool parent_is_in_wait4_locked(const Esper &p) {
    return p.state == EsperState::waiting && p.wait_pipe_idx < 0 &&
           p.ipc_wait_kind == Esper::IpcWaitKind::None && !p.wait_futex &&
           p.wake_cntpct == 0;
}

void maybe_wake_parent_locked(Esper &me) {
    const int parent_idx = me.parent;
    if (parent_idx < 0 || static_cast<uint32_t>(parent_idx) >= kMaxEspers) {
        return;
    }
    Esper &p = g_espers[parent_idx];
    if (parent_is_in_wait4_locked(p)) {
        p.regs[0] = me.pid;
        p.pending_status = me.exit_code;
        p.has_pending_status = (p.wait_status_ptr != 0);
        p.state = EsperState::ready;
        me.state = EsperState::free; // reaped by the just-woken wait()
        return;
    }
    // Parent isn't blocked in wait4. Deliver SIGCHLD so a parent that installed
    // a handler (e.g. sshd, otherwise parked in ppoll) learns the child exited,
    // reaps it via wait4, and tears the session down. Without this the child
    // lingers as a zombie and sshd never sends exit-status/CHANNEL_CLOSE -- the
    // ssh client then hangs after every command/logout. Mirrors Linux's "child
    // exit -> SIGCHLD to parent". `me` stays exited (a zombie) for the parent's
    // wait4 to collect (linux_wait4 scans for exited children). If the parent
    // uses the default/ignored SIGCHLD disposition, do nothing (correct: the
    // default action for SIGCHLD is to ignore it).
    const uint64_t chld = p.sig_handler[17 /*SIGCHLD*/];
    if (chld == 0 /*SIG_DFL*/ || chld == 1 /*SIG_IGN*/) {
        return;
    }
    if (p.state == EsperState::waiting) {
        // Interrupt the parked syscall: it returns -EINTR so the handler runs
        // (this is what makes sshd's parked ppoll wake, run its SIGCHLD reaper,
        // and tear the session down -- without it the child stays a zombie and
        // the ssh client hangs after `exit`). NB: ppoll REPLAYS on wake and
        // re-reads frame[0] as its `ufds` pollfd pointer, so this -4 can surface
        // there as ufds=0xfffffffffffffffc; the ppoll handler guards against a
        // non-user ufds (ufds < user-ceiling) and skips the fd scan instead of
        // dereferencing it. Mirrors fortis931's prep_interrupt_wait_locked.
        p.regs[0] = static_cast<uint64_t>(-4); // -EINTR
        p.ipc_wait_kind = Esper::IpcWaitKind::None;
        p.ipc_wait_id = -1;
        p.wait_pipe_idx = -1;
        p.wake_cntpct = 0;
        p.state = EsperState::ready;
    }
    linux_deliver_signal_locked(&p, 17 /*SIGCHLD*/, nullptr);
}

void clear_running_slot_locked(int idx) {
    const uint32_t cpu = arch::this_cpu_id();
    if (cpu < kMaxCpus && g_running[cpu] == idx) {
        g_running[cpu] = -1;
    }
}

} // namespace

// --- Public SMP-safe scheduler primitives ---------------------------------

int esper_pick_and_claim(int after) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    const int next = pick_and_claim_locked(after);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return next;
}

int esper_park_and_pick(int cur_idx) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    if (cur_idx >= 0 && static_cast<uint32_t>(cur_idx) < kMaxEspers &&
        g_espers[cur_idx].state == EsperState::running) {
        // Only park if still running: a concurrent fortis931_kill on
        // another CPU may have transitioned cur to exited/faulted just
        // before this svc-side park. In that case don't revive the
        // dying Esper -- just pick next.
        g_espers[cur_idx].state = EsperState::waiting;
        clear_running_slot_locked(cur_idx);
    }
    const int next = pick_and_claim_locked(cur_idx);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return next;
}

int esper_preempt_and_pick(int cur_idx) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    if (arch::this_cpu_id() == 0) wake_sleepers_locked(); // central nanosleep wake
    int next = -1;
    for (uint32_t step = 1; step <= kMaxEspers; ++step) {
        const int idx = static_cast<int>((cur_idx + step) % kMaxEspers);
        if (idx == cur_idx) continue;
        if (g_espers[idx].state == EsperState::ready) {
            next = idx;
            break;
        }
    }
    if (next < 0) {
        // No one else ready -- keep cur running; no state flip.
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        return -1;
    }
    if (cur_idx >= 0 && static_cast<uint32_t>(cur_idx) < kMaxEspers &&
        g_espers[cur_idx].state == EsperState::running) {
        // Only flip to ready if still running -- a concurrent kill may
        // have transitioned us to exited/faulted; don't resurrect that.
        g_espers[cur_idx].state = EsperState::ready;
    }
    g_espers[next].state = EsperState::running;
    const uint32_t cpu = arch::this_cpu_id();
    if (cpu < kMaxCpus) g_running[cpu] = next;
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return next;
}

int esper_exit_and_pick(int me_idx, int64_t code) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    if (me_idx >= 0 && static_cast<uint32_t>(me_idx) < kMaxEspers) {
        Esper &me = g_espers[me_idx];
        me.state = EsperState::exited;
        me.exit_code = code;
        clear_running_slot_locked(me_idx);
        maybe_wake_parent_locked(me);
    }
    const int next = pick_and_claim_locked(me_idx);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    // Parent may have become runnable (wait4 wakeup): let a secondary grab it.
    esper_kick_secondaries();
    return next;
}

int esper_fault_and_pick(int me_idx) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    if (me_idx >= 0 && static_cast<uint32_t>(me_idx) < kMaxEspers) {
        g_espers[me_idx].state = EsperState::faulted;
        clear_running_slot_locked(me_idx);
        // Faulted children do not wake wait4 -- matches the legacy
        // single-core behaviour where esper_fault never touched the parent.
    }
    const int next = pick_and_claim_locked(me_idx);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return next;
}

void esper_fault_current(int me_idx) {
    // Mark the running Esper faulted + release this CPU's run slot, WITHOUT
    // picking a successor (unlike esper_fault_and_pick). The scheduler-loop
    // caller (run_one_esper's resume guard) just returns; el0_try_run_one /
    // run_espers then re-pick from a clean g_running. Used when a corrupt
    // saved context is detected before resume (can't safely eret into it).
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    if (me_idx >= 0 && static_cast<uint32_t>(me_idx) < kMaxEspers) {
        g_espers[me_idx].state = EsperState::faulted;
        clear_running_slot_locked(me_idx);
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
}

int esper_vfork_handoff(int parent_idx, int child_idx) {
    // vfork hand-off, done atomically under one lock so the child never runs
    // while the parent is still running on another CPU -- they share the same
    // SP_EL0 (vfork passes child_stack=0), so concurrent execution would
    // corrupt the stack. Sequence: make child ready, park parent (caller has
    // already written the parent's wait predicate + saved regs), then pick on
    // THIS cpu (which claims the child it just readied). NO secondary kick:
    // vfork is strictly serial (parent suspended until child exec/exit), so we
    // want the child to run here, not be stolen by another core.
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    if (child_idx >= 0 && static_cast<uint32_t>(child_idx) < kMaxEspers &&
        g_espers[child_idx].state == EsperState::waiting) {
        g_espers[child_idx].state = EsperState::ready;
    }
    if (parent_idx >= 0 && static_cast<uint32_t>(parent_idx) < kMaxEspers &&
        g_espers[parent_idx].state == EsperState::running) {
        g_espers[parent_idx].state = EsperState::waiting;
        clear_running_slot_locked(parent_idx);
    }
    const int next = pick_and_claim_locked(parent_idx);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return next;
}

int esper_pick_and_claim_locked(int after) {
    return pick_and_claim_locked(after);
}

int esper_park_and_pick_locked(int cur_idx) {
    if (cur_idx >= 0 && static_cast<uint32_t>(cur_idx) < kMaxEspers) {
        g_espers[cur_idx].state = EsperState::waiting;
        clear_running_slot_locked(cur_idx);
    }
    return pick_and_claim_locked(cur_idx);
}

// --- Legacy primitives ----------------------------------------------------

void esper_set_running(int index) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    const uint32_t cpu = arch::this_cpu_id();
    if (cpu < kMaxCpus) g_running[cpu] = index;
    if (index >= 0) {
        g_espers[index].state = EsperState::running;
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
}

int esper_pick_ready(int after) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    wake_sleepers_locked();
    for (uint32_t step = 1; step <= kMaxEspers; ++step) {
        const int idx = static_cast<int>((after + step) % kMaxEspers);
        if (g_espers[idx].state == EsperState::ready) {
            anti_skill_unlock_irqrestore(g_esper_lock, flags);
            return idx;
        }
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return -1;
}

uint32_t esper_current_pid() {
    const int idx = esper_running_index();
    return idx >= 0 ? g_espers[idx].pid : 0;
}

void esper_exit(int64_t code) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    const uint32_t cpu = arch::this_cpu_id();
    if (cpu < kMaxCpus && g_running[cpu] >= 0) {
        const int idx = g_running[cpu];
        g_espers[idx].state = EsperState::exited;
        g_espers[idx].exit_code = code;
        g_running[cpu] = -1;
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
}

void esper_fault() {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    const uint32_t cpu = arch::this_cpu_id();
    if (cpu < kMaxCpus && g_running[cpu] >= 0) {
        const int idx = g_running[cpu];
        g_espers[idx].state = EsperState::faulted;
        g_running[cpu] = -1;
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
}

int esper_find_exited_child(int parent_index) {
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        if (g_espers[i].parent == parent_index &&
            (g_espers[i].state == EsperState::exited ||
             g_espers[i].state == EsperState::faulted)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int esper_child_count(int parent_index) {
    int n = 0;
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        if (g_espers[i].parent == parent_index &&
            g_espers[i].state != EsperState::free) {
            ++n;
        }
    }
    return n;
}

void esper_report() {
    namespace district = imaginary_number_district;

    district::writeln("Espers (user processes):");
    bool any = false;
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        const Esper &e = g_espers[i];
        if (e.state == EsperState::free) {
            continue;
        }
        any = true;
        district::write("  [pid ");
        district::dec(e.pid);
        district::write("] ");
        district::write(e.name);
        district::write(" state=");
        district::write(state_name(e.state));
        if (e.state == EsperState::exited) {
            district::write(" code=");
            district::dec(static_cast<uint64_t>(e.exit_code));
        }
        district::write("\n");
    }
    if (!any) {
        district::writeln("  (none yet; run 'user', 'exec <name>' or 'coexec ...')");
    }
}

// [WD] Compact one-line-per-Esper dump for the SMP hang watchdog: who is parked
// on what (ipc_wait_kind/id, pipe idx) and the resume PC. Pair with
// antenna_report()/inc_report() to see whether the data a poller is waiting for
// is actually sitting in a socket/channel ring (-> wakeup/poll bug) or absent
// (-> delivery bug).
void esper_watchdog_dump() {
    namespace district = imaginary_number_district;
    district::writeln("[WD] ==== esper states ====");
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        const Esper &e = g_espers[i];
        if (e.state == EsperState::free) continue;
        district::write("[WD] i="); district::dec(i);
        district::write(" pid="); district::dec(e.pid);
        district::write(" st="); district::write(state_name(e.state));
        district::write(" wk="); district::dec(static_cast<uint64_t>(static_cast<uint32_t>(e.ipc_wait_kind)));
        district::write(" wid="); district::dec(static_cast<uint64_t>(static_cast<uint32_t>(e.ipc_wait_id)));
        district::write(" pipe="); district::dec(static_cast<uint64_t>(static_cast<uint32_t>(e.wait_pipe_idx)));
        district::write(e.wait_pipe_is_write ? "(w)" : "(r)");
        district::write(" sys="); district::dec(static_cast<uint64_t>(e.last_syscall));
        district::write(" elr="); district::hex(e.elr);
        district::write(" ttbr0="); district::hex(e.ttbr0);
        district::write(" "); district::write(e.name);
        district::write("\n");
    }
}

} // namespace index
