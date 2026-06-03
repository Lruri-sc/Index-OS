#include "index/fortis931.hpp"

#include "index/aiwass.hpp"
#include "index/anti_skill.hpp"
#include "index/esper.hpp"
#include "index/linux_abi.hpp"
#include "index/usermode.hpp"

namespace index {

namespace {

// If a signal interrupts a syscall that has parked the Esper (nanosleep,
// wait4, console read, futex, ...), POSIX requires the syscall to return
// -EINTR after the handler runs. We can't synthesise that at handler exit
// time (rt_sigreturn restores the saved trap-frame regs, which were saved
// before the syscall finished), so pre-set X0 = -EINTR on the saved context
// before linux_deliver_signal copies it into the sigframe. Caller holds
// g_esper_lock so this state mutation is atomic w.r.t. the picker on
// other CPUs (otherwise a CPU might pick the Esper as ready between the
// state-flip and the wait-predicate clears and observe inconsistent fields).
void prep_interrupt_wait_locked(Esper *e) {
    if (e->state != EsperState::waiting) return;
    e->regs[0] = static_cast<uint64_t>(-4); // -EINTR
    e->ipc_wait_kind = Esper::IpcWaitKind::None;
    e->ipc_wait_id = -1;
    e->wait_pipe_idx = -1;
    e->wake_cntpct = 0; // any nanosleep deadline is moot once we EINTR
    e->state = EsperState::ready;
}

// One Esper's kill step under g_esper_lock. Returns the post-kill action:
// signal handler delivered (target keeps running, no pipe release needed),
// fatal teardown (parent wake recorded, caller must call release_esper_pipes
// outside the lock), or no-op (already dead / invalid).
enum class KillAction { Delivered, Killed, Nothing };

KillAction kill_one_locked(Esper *t, int sig) {
    if (t == nullptr || t->state == EsperState::free ||
        t->state == EsperState::exited || t->state == EsperState::faulted) {
        return KillAction::Nothing;
    }

    if (t->abi == Abi::Linux && linux_deliver_signal_locked(t, sig, nullptr)) {
        return KillAction::Delivered;
    }

    // Fatal: record exit, wake a wait4-blocked parent, leave pipe cleanup
    // for the caller to do outside the lock (pipe-side wake takes the lock
    // itself and we mustn't re-enter).
    t->exit_code = 128 + sig;
    t->state = EsperState::exited;
    t->wait_pipe_idx = -1;

    const int parent_idx = t->parent;
    if (parent_idx >= 0 && static_cast<uint32_t>(parent_idx) < kMaxEspers) {
        Esper *p = esper_at(static_cast<uint32_t>(parent_idx));
        // Match the parent-wake predicate in esper_exit_and_pick: only a
        // genuine wait4-blocked parent should be woken. Use wait_pipe_idx<0
        // for back-compat with the legacy single-core code path (a parent
        // in nanosleep/IPC/futex has wait_pipe_idx==-1 too -- pre-existing
        // hazard; out of scope for SMP locking).
        if (p != nullptr && p->state == EsperState::waiting && p->wait_pipe_idx < 0) {
            p->regs[0] = t->pid;
            p->pending_status = t->exit_code;
            p->has_pending_status = (p->wait_status_ptr != 0);
            p->state = EsperState::ready;
            t->state = EsperState::free; // reaped by the just-woken wait()
        }
    }
    return KillAction::Killed;
}

} // namespace

int fortis931_kill_pgrp(uint32_t pgrp, int sig) {
    if (pgrp == 0) return 0;
    int n = 0;
    // Hold g_esper_lock across the whole pgrp scan so the kill of each
    // member is atomic w.r.t. SMP scheduling decisions. release_esper_pipes
    // calls wake_pipe_* which also takes the lock; we defer the pipe
    // cleanup to a second pass with the lock released. The targets are
    // already state=exited by then, so no other CPU runs them and the fd
    // writes can't race.
    Esper *to_release[kMaxEspers] = {};
    uint32_t to_release_n = 0;
    {
        const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
        for (uint32_t i = 0; i < kMaxEspers; ++i) {
            Esper *e = esper_at(i);
            if (e == nullptr || e->pgrp != pgrp) continue;
            if (e->state == EsperState::free ||
                e->state == EsperState::exited ||
                e->state == EsperState::faulted) {
                continue;
            }
            prep_interrupt_wait_locked(e);
            const KillAction act = kill_one_locked(e, sig);
            if (act == KillAction::Delivered || act == KillAction::Killed) {
                ++n;
            }
            if (act == KillAction::Killed) {
                to_release[to_release_n++] = e;
            }
        }
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
    }
    for (uint32_t i = 0; i < to_release_n; ++i) {
        release_esper_pipes(to_release[i]);
    }
    return n;
}

bool fortis931_kill(int target_slot, int sig) {
    if (target_slot < 0) return false;
    Esper *t = esper_at(static_cast<uint32_t>(target_slot));
    if (t == nullptr) return false;

    KillAction act;
    {
        const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
        act = kill_one_locked(t, sig);
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
    }
    if (act == KillAction::Killed) {
        release_esper_pipes(t); // safe outside lock: target is exited
    }
    return act != KillAction::Nothing;
}

} // namespace index
