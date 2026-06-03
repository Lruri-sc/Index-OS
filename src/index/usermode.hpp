#pragma once

#include "index/esper.hpp" // Esper::IpcWaitKind enum

namespace index {

struct Esper;

// Park the running Esper in svc context: rewinds ELR by 4 so the syscall
// re-fires on wake, records (kind, id) as the wait reason, then switches to
// the next ready Esper. Returns the next slot index or -1 if no one else is
// runnable. Producers wake matching waiters by calling linux_ipc_wake.
int linux_ipc_park(int idx, Esper::IpcWaitKind kind, int id, uint64_t *frame);
int linux_ipc_wake(Esper::IpcWaitKind kind, int id);

// Snapshot the monotonic poll generation (bumped on every linux_ipc_wake). A
// multi-fd ppoll waiter snapshots this before scanning its fds, then parks via
// ipc_park_unless_ready with a probe that re-reads it under g_esper_lock -- so a
// producer that fired in the check-then-park window is caught (gen changed) and
// the poll re-scans instead of sleeping through it. O(1) under the lock, unlike
// re-scanning all N fds (which regressed KEX timing and was reverted).
uint32_t linux_poll_gen();

// Park on (kind,id) but re-check readiness under g_esper_lock first, closing the
// lost-wakeup race against a producer that delivers + wakes on another CPU
// between a non-blocking try and the park. `ready` is a non-consuming readiness
// probe invoked under the lock (must not itself take g_esper_lock). Returns true
// if it parked (caller returns the kFdParked sentinel); false if the fd became
// ready in the window (caller should retry the non-blocking read/recv).
//
// park_sig_mask: the set of signals that are BLOCKED for the duration of this
// wait (so they must NOT abort the park). If any pending signal is outside this
// mask (i.e. deliverable), the park is refused -- the caller re-loops and its
// in-syscall signal-check delivers it (-> handler -> EINTR). Checking this under
// the SAME g_esper_lock a signal sender takes closes the park-vs-signal race
// (Linux's signal_pending_state() recheck before schedule()). ppoll passes its
// effective sigmask (the ppoll sigset, which UNblocks SIGCHLD even though the
// process mask blocks it); the default ~0 means "no signal aborts the park",
// preserving the legacy behaviour of the non-ppoll callers.
bool ipc_park_unless_ready(int idx, Esper::IpcWaitKind kind, int id,
                           uint64_t *frame, bool (*ready)(int),
                           uint64_t park_sig_mask = ~0ULL);

// Commit a park while the caller ALREADY holds g_esper_lock (flags = the DAIF
// returned by anti_skill_lock_irqsave). For multi-condition waiters (ppoll) that
// must re-scan their fds under the lock before sleeping; closes the lost-wakeup
// race the same way ipc_park_unless_ready does for single-fd waiters. RELEASES
// the lock and switches away; does not return in the no-runnable case.
void ipc_park_locked(int idx, Esper::IpcWaitKind kind, int id, uint64_t *frame,
                     uint64_t flags);

// Drop every pipe ref this Esper holds and wake any peers blocked on those
// pipes (writers see broken pipe; readers see EOF). Used by SYS_close,
// SYS_exit, the EL0 fault path, dup2's overwrite, and Fortis931 when killing
// an Esper.
void release_esper_pipes(Esper *e);

// File helpers shared with the Linux ABI (linux_abi.cpp). They reuse the same
// per-Esper fd table and Lateran/Bookshelf/GrimoireFS lookup the Index open()
// path uses, so Linux openat/read/close/lseek/fstat behave like Index open.
int linux_file_open(Esper *e, const char *path);                       // fd>=3, or -1
// Open with create/truncate/write intent (Linux openat). create makes a fresh
// FAT file if absent; trunc empties it; writable marks the fd for write().
int linux_file_open_ex(Esper *e, const char *path, bool create, bool trunc, bool writable);
int64_t linux_file_read(Esper *e, uint32_t fd, char *buf, uint64_t cap); // bytes, 0=EOF, -1 err
// Write to a writable file fd: read-modify-writes the whole FAT file (small
// files), splicing `buf` at the fd's offset and advancing it. Returns bytes
// written or -1.
int64_t linux_file_write(Esper *e, uint32_t fd, const char *buf, uint64_t len);
int64_t linux_file_size(const char *path);                             // bytes, or -1 not found
bool linux_fd_is_file(Esper *e, uint32_t fd);
void linux_fd_seek(Esper *e, uint32_t fd, uint64_t off);
uint64_t linux_fd_tell(Esper *e, uint32_t fd);
int64_t linux_fd_size(Esper *e, uint32_t fd); // size of the file behind an open fd, or -1
const char *linux_fd_path(Esper *e, uint32_t fd); // stored path of an open file fd, or ""
int linux_dir_open(Esper *e, const char *path);   // fd for an existing directory (getdents)
void linux_fd_close(Esper *e, uint32_t fd);

// Terminate the running Esper `idx` with `code` and schedule the next one,
// from within a syscall trap (shares the exit path Index/Linux exit use). Used
// by the Linux signal path when a fatal signal has no handler (e.g. abort()).
void linux_exit_running(int idx, int64_t code, uint64_t *frame);

// pipe2(fds, flags): create an Aiwass-backed pipe and install its two ends in
// `e->fds`. Writes the read/write fd into out_fds[0/1]. Returns 0 on success
// or -errno (mirroring Linux). flags currently ignored (no O_CLOEXEC).
int linux_pipe2(Esper *e, int *out_fds, uint64_t flags);

// Write/read to a Linux Esper's pipe fd. Mirrors the Index ABI's pipe path:
// blocking writes park the Esper until the reader drains, blocking reads park
// until a writer fills. Returns bytes transferred, -errno (-EPIPE on broken
// write), or the kFdParked sentinel meaning the caller has been parked and
// frame[0] will be set on resume. linux_abi.cpp's fd_*_dispatch calls these.
int64_t linux_pipe_write(int idx, uint32_t fd, const char *buf, uint64_t len,
                         uint64_t *frame);
int64_t linux_pipe_read(int idx, uint32_t fd, char *buf, uint64_t len,
                        uint64_t *frame);

// dup(oldfd) / dup3(oldfd, newfd, flags): copy a Linux-visible fd entry,
// updating pipe refcounts. dup picks the lowest free slot >= 0. dup3 closes
// any prior occupant of newfd first. Returns the new fd or -errno.
int linux_dup_fd(Esper *e, int oldfd);
int linux_dup3_fd(Esper *e, int oldfd, int newfd, uint64_t flags);

// Backend refcount helpers for an *individual* Fd value (not tied to an fd
// table slot). linux_ref_fd_backend takes one extra reference on whatever the
// Fd points at (Antenna / Channel / SisterRelay / Aiwass / eventfd / epoll);
// linux_release_fd_backend drops one (and wakes pipe peers, like close()).
// These are the dispatch the dup/close paths already inline -- factored out so
// SCM_RIGHTS fd passing (ImaginaryNumberChannel) can ref a sent fd, drop an
// undelivered one, and install a received one without duplicating the table.
void linux_ref_fd_backend(const Fd &f);
void linux_release_fd_backend(const Fd &f);
// Install an already-ref'd Fd value into the lowest free slot of e->fds. Does
// NOT take a reference (ownership transfers from the caller, e.g. an in-flight
// SCM_RIGHTS queue entry). Returns the new fd number or -EMFILE.
int linux_install_fd(Esper *e, const Fd &f);

// Park the running Esper until cntpct >= deadline (nanosleep). Saves the EL0
// context, switches to the next ready Esper, and on wake resumes after the
// svc with frame[0] = 0. If no other Esper is runnable, busy-yields here
// (still safe: IRQs are masked in the svc, but the timer keeps advancing).
void linux_nanosleep_park(int idx, uint64_t deadline_cntpct, uint64_t *frame);

// execve(path, argv, envp): replace the running Esper's image with the ELF
// at `path`, then resume it at the new entry with the SysV initial stack
// built from argv/envp. argv/envp arrays must be NULL-terminated; the strings
// they point to must stay valid through linux_build_startup_stack. Returns
// false on failure (the old image stays intact); does not return on success
// (it switches into the new context via load_ctx).
bool linux_execve_replace(int idx, const char *path, const char *const *argv,
                          uint32_t argc, const char *const *envp, uint32_t envc,
                          uint64_t *frame);

// Read the whole file behind open fd `fd` into a fresh buffer that stays
// resident until the Esper exits (tracked in e->mmap_bufs), for file-backed
// mmap. Returns the buffer and sets *out_size to the file size, or nullptr on
// failure (bad fd / out of heap / too many mmaps).
uint8_t *linux_file_mmap_buf(Esper *e, uint32_t fd, uint64_t *out_size);

// Run the embedded EL0 user program (prints via syscalls, then exits).
void run_user();

// Run a user program that violates the EL0/EL1 boundary, to show the kernel
// catching the fault and surviving.
void run_user_fault();

// Load a position-independent ELF from the Lateran disk and run it at EL0.
void run_elf(const char *name);

// Like run_elf, but passes a real argv/envp into the new process's SysV
// initial stack. Used by Necessarius `linuxrun` so busybox can pick the
// applet from argv[0].
void run_elf_argv(const char *name, const char *const *argv, uint32_t argc,
                  const char *const *envp, uint32_t envc);

// Load several ELF programs (space-separated names) and run them concurrently
// (cooperatively, switching on yield) as separate Espers.
void run_coexec(const char *names);

// EL0 SMP scheduling helpers. run_one_esper switches the current CPU into a
// chosen Esper; returns when the Esper parks / exits via leave_user. The
// idle Sister on each CPU calls el0_try_run_one in its loop to participate.
void run_one_esper(int next);
bool el0_try_run_one();

// Active address-space / image refcount-table entries (leak monitoring via
// /proc/index_resources). Should plateau across repeated fork/exec/exit.
uint32_t as_ref_active_count();
uint32_t image_ref_active_count();

} // namespace index
