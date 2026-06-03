#pragma once

#include <stdint.h>

namespace index {

struct Esper;

// Phase A entry: dispatch a Linux AArch64 svc for the running Esper. The
// trap frame layout matches the existing Index dispatcher (x0..x30 in
// frame[0..30]; x8 holds the syscall number for Linux just like for Index,
// so the caller can pass it via frame[8]).
//
// Returns nothing; sets frame[0] to the syscall result (or -errno) just like
// Index syscalls. The dispatcher returns -ENOSYS for any syscall not yet
// implemented -- musl is generally robust to that and falls back where it
// can. Phase A only wires write(64), exit(93) and exit_group(94); subsequent
// phases extend the table per the plan.
void linux_syscall_dispatch(uint64_t *frame);

// Phase C: build the SysV AArch64 process startup stack (argc/argv/envp/auxv)
// for a Linux Esper near the top of its stack VMA, and return the resulting
// initial SP_EL0 (16-byte aligned). The Esper must already have its VMA list
// and elf_* metadata set. v1 passes argc=1 (argv[0] = the program name),
// empty envp, and the aux vector musl needs (AT_PHDR/PHENT/PHNUM, AT_ENTRY,
// AT_PAGESZ, AT_RANDOM, ...). Returns 0 on failure.
uint64_t linux_build_startup_stack(Esper *e);

// Wave F2: deliver signal `sig` to a Linux Esper that has a handler registered
// for it. Builds a Linux aarch64 rt_sigframe on the target's user stack from
// its current EL0 context and redirects it to the handler; the handler's
// rt_sigreturn (syscall 139) restores the frame.
//
// If `frame` is non-null the target is the *running* Esper interrupted in a
// syscall (its live context is in the trap frame + system registers); the
// frame is rewritten in place. If `frame` is null the target is some other
// Esper and its saved context (regs/sp_el0/elr/spsr) is rewritten instead.
//
// Returns true if a handler was installed and the signal was delivered; false
// if the signal is SIG_DFL/SIG_IGN (caller applies the default action, e.g.
// terminate). Non-Linux Espers always return false.
bool linux_deliver_signal(Esper *e, int sig, uint64_t *frame);

// Same as linux_deliver_signal but assumes caller already holds g_sched_lock
// (e.g. fortis931_kill, which needs to atomically check abi/state, deliver
// the signal, and tear down the Esper if it's an uncaught fatal signal).
bool linux_deliver_signal_locked(Esper *e, int sig, uint64_t *frame);

} // namespace index
