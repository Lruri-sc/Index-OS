#pragma once

#include <stdint.h>

#include "index/anti_skill.hpp"

namespace index {

// The single global scheduler spinlock. Exposed for callers that need to do
// a compound operation across multiple Esper-state writes -- e.g. linux_futex
// WAIT must hold one lock across the *uaddr==val check AND the state=waiting
// + queue-insert, otherwise a FUTEX_WAKE between the check and the insert
// goes lost (the classic lost-wakeup race). For the common case of "one
// state transition + reschedule" the helper functions below already take
// the lock internally and callers should NOT take it themselves.
extern AntiSkill g_esper_lock;

// Esper: an Academy City ability-user. Here it is a user-space process -- an
// EL0 program with its own identity (pid), lifecycle, exit status, and a full
// saved EL0 context so several Espers can run concurrently (cooperatively, by
// yielding) in their own arena slots. Kernel threads are Sisters; user
// processes are Espers. With fork/exec an Esper can spawn children, and wait()
// blocks a parent until a child exits -- so `waiting` is a real blocked state.
enum class EsperState { free, ready, running, waiting, exited, faulted };

// Which syscall ABI this Esper presents to its EL0 code. Index is the native
// 1..15 svc table that all userprog/*.cpp targets. Linux is the AArch64 Linux
// numbering (write=64, exit_group=94, ...) used by binaries built with a
// stock Linux toolchain. The dispatcher branches on this once per svc; the
// two tables never share numbers, so cross-ABI calls are impossible.
enum class Abi { Index, Linux };

// A virtual memory area: a contiguous user-VA range with a uniform protection
// and a backing source. The page-fault handler (pr2_handle_fault, defined in
// personal_reality_v2.cpp) consults this list to decide whether a fault is
// real or just a missing page it should populate on demand. Phase B uses
// these for Linux Espers; Index Espers still go through the legacy pool in
// personal_reality.cpp.
enum class VmaKind : uint8_t {
    Free = 0,
    Anon,   // zero-filled on first touch (heap/stack/bss/anonymous mmap)
    File,   // copied from file_src + (fault_va - start) on first touch
    Stack,  // like Anon but a sentinel kind so we can grow/find it later
    Brk,    // like Anon but the upper bound moves on brk()
};

constexpr uint8_t kVmaProtR = 1; // readable from EL0
constexpr uint8_t kVmaProtW = 2; // writable from EL0
constexpr uint8_t kVmaProtX = 4; // executable from EL0

// 32 sufficed for busybox/sshd, but a JVM maps far more regions: ~10 shared
// libs (each several PT_LOADs, now further split by RELRO mprotect), the Java
// heap + metaspace + code cache, and a VMA per thread stack + many malloc
// mmaps. 32 exhausted -> mmap -ENOMEM -> "Unable to load libjava.so: Out of
// memory". Bumped to 512 (Vma ~56 B * 512 * kMaxEspers ~ 0.5 MiB bss).
constexpr uint32_t kMaxVmas = 512;

struct Vma {
    uint64_t start = 0;          // inclusive (page-aligned)
    uint64_t end = 0;            // exclusive (page-aligned); start..end is the VA span
    // File-backed bookkeeping: file_src points to the ELF buffer at file_off,
    // and the segment's content (file_size bytes) lives at VA seg_vaddr, which
    // may be ABOVE start because we page-align start down to the nearest
    // 4 KiB boundary. seg_pad = seg_vaddr - start is the number of bytes
    // at the front of the VMA that are NOT in the file (they stay zero).
    const uint8_t *file_src = nullptr;
    uint64_t file_off = 0;       // offset into file_src for the segment's first byte
    uint64_t file_size = 0;      // bytes in file (= p_filesz)
    uint64_t seg_pad = 0;        // bytes between start and the segment's actual VA
    uint8_t prot = 0;            // bitmask of kVmaProtR/W/X
    VmaKind kind = VmaKind::Free;
    // Demand-paged file-backed mmap: when file_src==nullptr but file_path[0]!=0,
    // the fault handler reads each page from this path via lateran_pread instead
    // of pre-reading the whole file into the kernel heap (so a 33 MB rt.jar costs
    // no heap and isn't bounded by the old 8-buffer table). ELF segments leave
    // this empty and use the resident file_src buffer.
    char file_path[96] = {};
};

// PersonalReality (パーソナルリアリティ): an esper's private, self-consistent
// world -- the very source of its power. Here it is a refcounted address space,
// the Index analogue of Linux's mm_struct. The VMA list + brk/mmap bookkeeping
// + page-table base (ttbr0) live
// HERE, jointly owned by every thread (CLONE_VM Esper) that shares the space,
// rather than being duplicated in each Esper. This gives all threads ONE
// authoritative VMA map (a region mmap'd by any thread is instantly visible to
// its siblings -- the fix for the CLONE_VM "VMA list divergence" that killed
// the multi-threaded JVM) and ties the map's lifetime to the address space, not
// to any single thread's Esper slot. `refs` counts sharers: fork() makes a
// fresh one (refs=1), clone(CLONE_VM) shares the parent's (refs++). When the
// last sharer exits and refs hits 0 the page tables are reclaimed
// (pr2_destroy). Allocated from a fixed pool (reality_alloc); see
// personal_reality_v2.
struct PersonalReality {
    uint64_t ttbr0 = 0;          // L1 phys (page-table base); the authoritative copy
    Vma vmas[kMaxVmas] = {};      // the VMA list shared by all threads in this space
    uint32_t vma_count = 0;
    uint64_t brk_start = 0;       // base of the heap (Brk) VMA
    uint64_t brk_cur = 0;         // current program break
    uint64_t mmap_next = 0;       // bump pointer for anonymous mmap()
    int32_t refs = 0;             // sharer count (threads); 0 => slot free
    bool in_use = false;          // pool-slot occupancy
};

// A per-Esper file descriptor. 0/1/2 are the console; open() hands out file
// descriptors that name a file (in Lateran/Bookshelf/GrimoireFS) and a read
// offset -- read() re-reads the file and serves the next slice (small files).
// pipe() hands out two more: a pipe_read end and a pipe_write end, both
// pointing at the same Aiwass slot by index.
constexpr uint32_t kMaxFds = 16;
constexpr uint32_t kFdPathCap = 96;
enum class FdKind { closed, console, file, pipe_read, pipe_write, socket,
                     devnull, devzero, devrandom, devtty,
                     unix_sock /*AF_UNIX via ImaginaryNumberChannel*/,
                     eventfd /*eventfd2 u64 counter*/,
                     epoll /*epoll_create1 set*/,
                     timerfd /*timerfd_create: CNTPCT deadline -> readable*/,
                     signalfd /*signalfd4: pending signals -> readable*/,
                     inotify /*inotify_init1: Kazakiri FS-watch event queue*/,
                     pty_master /*master end of a SisterRelay pair*/,
                     pty_slave  /*slave end (looks like a tty)*/ };
struct Fd {
    FdKind kind = FdKind::closed;
    char path[kFdPathCap] = {};
    uint64_t off = 0;
    int pipe_idx = -1;     // valid iff kind is pipe_read or pipe_write
    int sock_idx = -1;     // valid iff kind is socket (Antenna table index)
    bool writable = false; // file opened for writing (O_WRONLY/O_RDWR)
    bool cloexec = false;  // O_CLOEXEC/FD_CLOEXEC: released on execve, not fork
};

// Linux cwd buffer. Stored as an absolute, normalized path ("/" by default).
// chdir validates the new path through Lateran (must be a directory) before
// installing it. openat/unlinkat/mkdirat/newfstatat with a relative path
// prepend this when no dirfd overrides it.
constexpr uint32_t kCwdCap = 256;

// Pending execve()/initial-run argv/envp staging: linux_build_startup_stack
// reads from here so kSysExec and the kernel shell's `linuxrun` can hand the
// new Linux process a real (argc, argv, envp). exec_argv[0..exec_argc) are
// borrowed pointers that must stay valid through load_elf_into_slot.
// Bumped well past a shell's needs for the OpenJDK launcher: `java -cp ... Main
// args...` has many argv, and the launcher re-execs itself after setenv'ing
// LD_LIBRARY_PATH -- if envp is truncated below where LD_LIBRARY_PATH lands, the
// re-exec'd java never sees it, re-sets it, and re-execs forever (infinite loop).
constexpr uint32_t kExecArgvCap = 64;
constexpr uint32_t kExecEnvpCap = 64;

struct Esper {
    uint32_t pid = 0;
    char name[24] = {};
    // Full path of the running program image (absolute), set by load_elf_into_slot
    // and inherited across fork/clone. Backs readlink("/proc/self/exe"), which the
    // musl dynamic loader reads to resolve $ORIGIN in a binary's RUNPATH (the
    // OpenJDK launcher's libjli.so lives at $ORIGIN/../lib/aarch64/jli).
    char exe_path[kCwdCap] = {};

    // Process group + session. fork inherits both; setsid sets sid=pid and
    // pgrp=pid (becomes a session leader without a controlling tty); setpgid
    // moves the caller into another group within the same session. The
    // foreground pgrp of the controlling terminal (tcsetpgrp) decides who
    // receives kernel-generated SIGINT/SIGQUIT/SIGTSTP from VINTR/etc. on the
    // PL011 IRQ path -- the heart of async Ctrl-C.
    uint32_t pgrp = 0;        // 0 = inherit-on-create
    uint32_t sid = 0;

    // Phase H identity. uid 0 = crowley (Aleister Crowley, Academy City's
    // hidden supreme authority; not called "root" -- the user explicitly asked
    // for this). real / effective / saved uids and gids are tracked separately
    // so setresuid/setresgid can mutate them independently. fork/clone inherit
    // all three pairs; execve(set-uid binaries) would adjust them but we
    // don't honour the setuid mode bit yet.
    uint32_t uid = 0, euid = 0, suid = 0;
    uint32_t gid = 0, egid = 0, sgid = 0;
    // File-creation mode mask (umask). Default 022 matches Linux/musl.
    uint32_t umask = 0022;

    // POSIX rlimits. Linux defines 16 resources (RLIMIT_NLIMITS = 16) on
    // aarch64. Each is a pair { rlim_cur, rlim_max } -- soft/hard caps. Most
    // programs only ever query (`ulimit -a` / glibc's stdio probing RLIMIT_
    // NOFILE) -- we accept setrlimit changes but don't enforce them yet.
    struct Rlim { uint64_t cur, max; };
    Rlim rlimits[16] = {};
    EsperState state = EsperState::free;
    int64_t exit_code = 0;
    int parent = -1;          // parent Esper slot, or -1 (for wait())
    uint32_t code_pages = 0;  // code pages mapped (for fork's address-space copy)

    // EL0 execution context, valid once `started`.
    bool started = false;
    uint64_t entry = 0;      // initial PC
    uint64_t stack_top = 0;  // initial SP_EL0
    uint64_t ttbr0 = 0;      // this process's address space (private page table)
    uint64_t regs[31] = {};  // x0..x30
    uint64_t sp_el0 = 0;
    uint64_t elr = 0;        // resume PC
    uint64_t spsr = 0;       // resume PSTATE
    // FP/SIMD (NEON) state: q0..q31 (512 bytes) then FPSR, FPCR (8 bytes).
    // The kernel is built -mgeneral-regs-only so it never touches these; only
    // EL0 does. They must still be saved/restored on every EL0<->EL0 switch
    // (preempt + park + resume), or a process preempted mid-NEON (e.g. OpenSSL
    // SHA/curve25519/chacha20 during the SSH KEX) resumes with another Esper's
    // vector registers and computes a corrupted result. See fpsimd_save /
    // fpsimd_restore (user_switch.S). alignas(16) for the stp/ldp q pairs.
    alignas(16) uint8_t fpsimd[528] = {};

    // wait() bookkeeping: when a child exits and wakes this (waiting) parent,
    // the child's exit code is written to *wait_status_ptr the next time this
    // Esper's address space is loaded (it is not active at the child's exit).
    uint64_t wait_status_ptr = 0;
    bool has_pending_status = false;
    int64_t pending_status = 0;
    // When set, the woken waiter is a Linux wait4 and *wait_status_ptr should
    // receive the Linux-encoded 32-bit status ((code & 0xff) << 8), not the
    // raw int64 code that Index wait() writes.
    bool wait_status_is_linux = false;
    // True for an Esper created by clone(CLONE_VM) -- a thread sharing its
    // creator's address space. Its mm/images are shared, so teardown must not
    // free them while siblings still run (mm is refcounted, images too).
    bool is_thread = false;
    // The address space (Linux mm_struct analogue): the VMA list, brk/mmap
    // bookkeeping and the authoritative ttbr0 all live in this shared,
    // refcounted PersonalReality. Every thread that shares the space (CLONE_VM)
    // points at the SAME PersonalReality, so they all see one consistent VMA
    // map. nullptr for an Index legacy-pool Esper, which has no VMA list --
    // ensure_user treats a null mm as "already fully mapped" (this replaces the
    // old vma_count==0 sentinel). e->ttbr0 mirrors mm->ttbr0 as a fast-path
    // cache so the context-switch path needn't dereference mm.
    PersonalReality *mm = nullptr;
    // Thread bookkeeping (Wave F4). clear_child_tid: if non-zero, on exit the
    // kernel writes 0 to *clear_child_tid and futex-wakes it (waking a joiner
    // in pthread_join). wait_futex/wait_futex_phys mark an Esper blocked in
    // futex(FUTEX_WAIT) on a particular physical word.
    uint64_t clear_child_tid = 0;
    bool wait_futex = false;
    uint64_t wait_futex_phys = 0;

    // rt_sigsuspend bookkeeping. When wait_sigsuspend is true, the Esper is
    // parked in rt_sigsuspend with sig_mask temporarily replaced by the
    // user-supplied mask; sigsuspend_saved_mask holds the original mask, to
    // be restored on signal delivery so the handler (and the post-
    // rt_sigreturn EL0 state) sees the caller's pre-sigsuspend mask.
    bool wait_sigsuspend = false;
    uint64_t sigsuspend_saved_mask = 0;

    // Canonical-mode TTY line accumulator. Linux keeps the in-progress line in
    // the tty struct (n_tty's read_buf) so park-on-no-input never loses what
    // the user has typed so far. Our svc retry path otherwise restarts the
    // read syscall with a fresh stack -> any local `buf[0..n)` would vanish.
    // console_read_tty stages bytes here as they're read+echoed; the buffer
    // is drained into the caller's user buffer on newline (complete line).
    // 256 covers typical shell input (login + busybox interactive).
    uint32_t tty_line_n = 0;
    char tty_line_buf[256] = {};
    // Thread-local storage base (TPIDR_EL0). Set by clone(CLONE_SETTLS) and
    // restored on every context load so each thread sees its own TLS.
    uint64_t tpidr = 0;

    // Pipe-blocked bookkeeping: when SYS_read/SYS_write on an Aiwass pipe would
    // block, the dispatcher rewinds ELR by 4 (so the svc re-executes on wake),
    // saves the EL0 context, marks this Esper waiting, and records *which*
    // pipe it is parked on. A waker that frees space (read) or adds data (write)
    // on that pipe just flips this Esper back to ready; on resume the svc runs
    // again and the now-unblocked read/write returns its result. -1 means this
    // Esper is not parked on a pipe (could still be waiting for a child via
    // wait()).
    int wait_pipe_idx = -1;
    bool wait_pipe_is_write = false;

    // nanosleep/clock_nanosleep parks the Esper with state=waiting and stores
    // the deadline (in CNTPCT ticks) here. esper_pick_ready scans waiting
    // Espers and flips ready as soon as cntpct >= wake_cntpct. 0 = not sleeping.
    uint64_t wake_cntpct = 0;

    // Generic in-kernel IPC park. The svc handler that needs to wait for
    // another Esper (accept of an incoming connection; recv of bytes/datagrams
    // not yet produced; connect handshake's reply) saves its EL0 context with
    // ELR-4 (so the svc re-fires on wake), sets state=waiting, and stores
    // *what* it's waiting on here. A producer-side function (deliver_tcp,
    // inc_send, inc_sendto, ...) calls ipc_wake(kind, id) to flip matching
    // waiters back to ready. -1 / IpcWaitKind::None means not parked.
    enum class IpcWaitKind : uint8_t {
        None = 0,
        AntennaAccept = 1,   // listen socket id; wake when an Established child queues
        AntennaRecv = 2,     // socket id; wake when rx_avail > 0 or peer/state changes
        AntennaConnect = 3,  // socket id; wake when handshake completes / RST
        ChannelAccept = 4,   // listen UNIX socket id; wake on inc_connect
        ChannelRecv = 5,     // UNIX socket id; wake on inc_send / inc_sendto
        EventfdRead = 6,     // eventfd id; wake when counter > 0
        EpollWait = 7,       // epoll fd id; wake when any registered fd became ready
        PtyMasterRead = 8,   // SisterRelay id; wake on slave write
        PtySlaveRead = 9,    // SisterRelay id; wake on master write
        ConsoleRead = 10,    // PL011 console; wake on timer-tick PL011 has_input
        VforkDone = 11,      // child pid; vfork parent suspended until child
                             // exec/exit (CLONE_VFORK). id = the child's pid.
        PollWait = 12,       // ppoll/pselect waiter; woken by the 100 Hz
                             // network_tick after the NIC is drained -- a
                             // generic "something may be ready" tick, exactly
                             // how Linux poll() is driven by scheduler+softirq.
        InotifyRead = 13,    // inotify (Kazakiri) fd id; wake when an FS
                             // mutation queues an event (kazakiri_notify).
        PtraceStop = 14,     // ptrace (Mental Out) stop: tracee parked here is
                             // NOT woken by any generic path -- only the tracer's
                             // PTRACE_CONT/SYSCALL/SINGLESTEP/DETACH resumes it.
    };
    IpcWaitKind ipc_wait_kind = IpcWaitKind::None;
    int ipc_wait_id = -1;
    uint32_t last_syscall = 0xffffffff; // [WD] most recent EL0 syscall nr (hang diag)
    // ppoll/pselect timeout, carried across park-replays: the syscall re-fires
    // from scratch on every wake, so a local deadline would reset each tick.
    bool poll_armed = false;
    uint64_t poll_deadline = 0; // 0 = infinite; else a last_order_ticks() value
    // ppoll's effective sigmask (its 4th arg, the sigset swapped in for the
    // wait), cached on the FIRST entry of a ppoll and reused across every
    // park-replay. Linux swaps the mask in once via set_user_sigmask; Index
    // used to re-read frame[3] on every replay, so a single failed prefault of
    // the user sigset pointer would silently fall back to the (SIGCHLD-blocking)
    // process mask -> the reaper's ppoll then treats SIGCHLD as masked and never
    // delivers it -> logout hangs. Reading once removes that per-replay race.
    uint64_t poll_eff_mask = 0;

    // Which syscall table this Esper presents to EL0. Set by sniff_abi at
    // exec time; defaults to Index so any code path that hasn't been taught
    // about ABIs keeps the existing behaviour. Phase A only switches the
    // svc dispatch on this; Phases C/D/E build the Linux startup stack and
    // syscall surface on top of the same field.
    Abi abi = Abi::Index;
    // ELF metadata captured at load time, used by Phase C's startup-stack
    // builder to fill AT_PHDR/AT_PHNUM/AT_PHENT and AT_BASE/AT_ENTRY in the
    // Linux aux vector. Zero for Index processes (the existing layout has
    // no such concept).
    uint64_t elf_phdr_va = 0;
    uint16_t elf_phnum = 0;
    uint16_t elf_phentsize = 0;
    uint64_t elf_base = 0;
    uint64_t elf_entry = 0;

    // (The per-process VMA list now lives in the shared PersonalReality `mm`
    // above -- see struct PersonalReality. A null mm == an Index legacy-pool
    // Esper with no VMA list.)
    // The Esper's ELF file image (kept alive while the Esper is running so
    // file-backed VMAs can keep reading bytes out of it on each fault). Owned
    // by the Esper -- freed on exec/exit. Index Espers leave this null.
    uint8_t *linux_elf_image = nullptr;
    uint64_t linux_elf_image_size = 0;
    // For dynamically-linked Linux binaries: the PT_INTERP interpreter
    // (ld-musl) ELF image, kept resident the same way (its file-backed VMAs
    // read from it on fault). Null for static binaries and Index Espers.
    uint8_t *linux_interp_image = nullptr;
    // (file-backed mmap is demand-paged now -- pages fault in from the
    // filesystem via lateran_pread; no per-Esper whole-file buffers, no 8-cap.)
    // (brk/mmap bookkeeping -- brk_start/brk_cur/mmap_next -- moved into the
    // shared PersonalReality `mm` so all threads share one heap + mmap arena.)

    // Linux signal handling (Wave F2). Per-signal user-handler VA (0 = SIG_DFL,
    // 1 = SIG_IGN), the program's restorer trampoline (musl's __restore_rt),
    // and flags. Only signals 1..kNumSignals-1 are tracked. Set by
    // rt_sigaction; consulted when a signal is delivered (kill/tgkill/raise).
    uint64_t sig_handler[64] = {};
    uint64_t sig_restorer[64] = {};
    uint64_t sig_flags[64] = {};
    uint64_t sig_act_mask[64] = {}; // sa_mask: extra signals blocked during handler
    // Real signal mask + pending queue (item 13). sig_mask is the bitmask of
    // currently blocked signals (bit i-1 for signal i). sig_pending records
    // blocked-but-arrived signals waiting for the mask to drop. SIGKILL (9)
    // and SIGSTOP (19) can't be masked and are always delivered immediately.
    uint64_t sig_mask = 0;
    uint64_t sig_pending = 0;

    // ptrace "Mental Out": debugger control of another Esper. A tracee records
    // its tracer; a ptrace-stop parks the tracee (state=waiting, ipc_wait_kind=
    // PtraceStop -- nothing generic wakes that kind, only the tracer's CONT/etc.)
    // and reports to the tracer's wait4. See mental_out.cpp.
    int ptrace_tracer = -1;        // tracer's Esper slot, or -1 (not traced)
    bool ptrace_stopped = false;   // currently in a ptrace-stop (parked)
    bool ptrace_report = false;    // a stop is pending for the tracer's wait4
    uint32_t ptrace_options = 0;   // PTRACE_O_* (only TRACESYSGOOD acted on)
    uint32_t ptrace_event = 0;     // event code for the current stop (PTRACE_EVENT_*)
    int32_t ptrace_stop_sig = 0;   // signal/cause reported by the current stop
    int32_t ptrace_pending_stop = 0; // a signal that must trigger a stop at the next safe point (0 = none)
    bool ptrace_syscall = false;   // resumed via PTRACE_SYSCALL: stop at next syscall entry/exit
    bool ptrace_singlestep = false;// resumed via PTRACE_SINGLESTEP
    bool ptrace_in_syscall = false;// syscall-stop toggle: false=>next stop is entry, true=>exit
    int32_t ptrace_inject_sig = 0; // signal to deliver to the tracee on resume (CONT/SYSCALL data arg)
    // Tracer-side: a stopped tracee reports a Linux WSTOPPED status. Reuses the
    // wait machinery but needs the (sig<<8)|0x7f encoding (vs exit's (code&0xff)<<8).
    bool wait_status_is_stop = false;

    Fd fds[kMaxFds] = {};    // 0/1/2 set to console by esper_create

    // Linux working directory (absolute, normalized). Initialised to "/" by
    // esper_create; chdir installs a new value after validating it's a real
    // directory. Inherited across fork/clone (CLONE_FS would matter for thread
    // semantics; we keep it shared per address space, which is what musl asks
    // for in practice).
    char cwd[kCwdCap] = {};

    // Pending argv/envp for the next program image built on this slot. Set by
    // kSysExec / `linuxrun` / linux_execve right before load_elf_into_slot;
    // consumed by linux_build_startup_stack which writes the SysV initial
    // stack from these. Reset to {} on consume; empty argv falls back to a
    // single argv[0] = e->name (preserves the old single-shot behaviour).
    const char *exec_argv[kExecArgvCap] = {};
    uint32_t exec_argc = 0;
    const char *exec_envp[kExecEnvpCap] = {};
    uint32_t exec_envc = 0;
};

constexpr uint32_t kMaxEspers = 16;

// Create an Esper. Returns its table slot index, or -1 if full. The slot
// starts in EsperState::waiting (no wait predicate set, so the scheduler
// will not pick it) so the caller can populate ttbr0 / regs / entry / etc.
// without racing a concurrent picker on another CPU. Once setup is done,
// call esper_make_ready(slot) to atomically transition the slot to ready;
// only then does it become eligible for scheduling.
int esper_create(const char *name);
Esper *esper_at(uint32_t index);

// Atomic state transition: EsperState::waiting -> EsperState::ready.
// Used by clone/fork/spawn after the new Esper's fields are fully written.
// Takes g_esper_lock so the write is a release-ordered publication: any
// CPU that subsequently observes state=ready via the lock also observes
// every prior field write on this CPU.
void esper_make_ready(int idx);

// Per-CPU "what is this CPU running" lookup. Reads only this CPU's slot, so
// it is a lock-free snapshot of an exclusive writer; safe in any context.
int esper_running_index();

// [WD] SMP hang watchdog: dump every non-free Esper's state + what it is parked
// on (ipc_wait_kind/id, pipe idx) + resume PC. Called periodically from
// network_tick while diagnosing the post-auth SSH hang.
void esper_watchdog_dump();

// --- SMP multi-core EL0 plumbing ---
// Tell the scheduler how many CPUs participate in EL0 (set by misaka_network
// once secondaries are online) and how to wake a specific secondary from WFI
// (the GICv3 reschedule SGI). esper_kick_secondaries pulses all secondaries
// to re-scan after an Esper becomes runnable, so work spreads across cores.
void esper_set_online_cpus(uint32_t n);
void esper_set_kick(void (*fn)(uint32_t cpu));
void esper_kick_secondaries();
int esper_running_on(uint32_t cpu); // [dbg] g_running[cpu]

// --- SMP-safe scheduler primitives (g_esper_lock held internally) ---
//
// Each of these collapses a multi-step state transition into one critical
// section so that two CPUs cannot both observe an Esper as ready, both flip
// it to running, and both `resume_user_eret` it -- the bug that prevented
// secondary cores from participating in EL0 scheduling. Mirrors Linux's
// pattern of holding rq->lock across pick_next_task + activate, exit + wake
// parent, and the preempt save+pick+switch.

// Pick a ready Esper (round-robin from after+1) and ATOMICALLY claim it as
// running on the current CPU (state=running, g_running[cpu]=idx). Returns
// the Esper index, or -1 if none ready. Replaces the legacy pick_ready ->
// set_running pair: those left an inter-lock window in which another CPU
// could claim the same idx.
int esper_pick_and_claim(int after);

// Mark cur_idx as waiting (caller has already populated the wait predicate
// fields -- wait_pipe_idx / ipc_wait_kind / wake_cntpct / wait_futex /
// wait_status_ptr / regs -- which are private to cur_idx so no lock is
// needed for those writes), clear g_running[cpu], then pick + claim next.
// Returns next idx or -1 if no one else is ready (caller does leave_user).
int esper_park_and_pick(int cur_idx);

// IRQ preemption: cur_idx was running; caller has already saved its EL0
// registers into cur->regs/elr/spsr/sp_el0/tpidr from the IRQ frame.
// If a DIFFERENT ready Esper exists, flip cur to ready + pick + claim it,
// returning its idx. Otherwise return -1 and leave cur as running (no
// preempt needed). This avoids the gratuitous self-flip when cur is the
// only runnable Esper on its CPU.
int esper_preempt_and_pick(int cur_idx);

// Exit + wake wait-blocked parent + pick next, all under g_esper_lock.
// Caller has already released pipe / image / address-space refs outside the
// lock. Sets me=exited+code, clears g_running[cpu], wakes a wait4-blocked
// parent (matches the legacy "wait_pipe_idx < 0" predicate so a nanosleep /
// IPC / futex blocker is NOT wrongly woken), and picks+claims the next
// Esper. Returns next idx, or -1 if no one runnable (caller does leave_user).
int esper_exit_and_pick(int me_idx, int64_t code);
// Thread-group exit for a fatal signal (SIGSEGV) in one CLONE_VM thread: marks
// every Esper sharing `mm` exited + wakes their parents, then picks next once.
int esper_group_exit_and_pick(int fault_idx, const void *mm, int64_t code);

// ptrace "Mental Out": detach all tracees of a dying tracer (slot dead_tracer)
// so none is orphaned (a ptrace-stopped tracee would never resume otherwise).
// Untraces each + wakes any that is stopped. Caller MUST hold g_esper_lock.
// Called from every tracer-death path (exit, fault, fatal-signal kill).
void esper_detach_tracees_locked(int dead_tracer);

// Same as exit_and_pick but transitions to faulted (parent is NOT woken --
// matches existing single-core behaviour where esper_fault never woke
// wait4; the parent reaps the fault via its next wait4 syscall).
int esper_fault_and_pick(int me_idx);

// Fault the running Esper WITHOUT picking a successor (caller's scheduler loop
// re-picks). For the resume-time corrupt-context guard in run_one_esper.
void esper_fault_current(int me_idx);

// "Already-locked" variants: caller holds g_esper_lock via
// anti_skill_lock_irqsave and wants to combine the pick / park / claim with
// additional reads or writes inside the same critical section (futex
// WAIT's value-check is the canonical user). Do NOT acquire or release
// the lock themselves.
int esper_pick_and_claim_locked(int after);
int esper_park_and_pick_locked(int cur_idx);

// vfork hand-off: atomically make the child runnable, park the parent (its
// wait predicate + saved context must already be set by the caller), and pick
// the next Esper on this CPU (claiming the child). Does NOT kick secondaries:
// parent and child share one stack (vfork child_stack=0), so execution must
// stay serial -- the child runs here while the parent is suspended.
int esper_vfork_handoff(int parent_idx, int child_idx);

// --- Legacy single-step primitives ---
// These DO NOT atomically claim the Esper and are only safe during init,
// before SMP EL0 scheduling is active. After misaka_network_set_user_mode
// is true, every caller MUST use the *_and_pick variants instead -- otherwise
// the pick_ready -> set_running window is open and two CPUs can race.
void esper_set_running(int index);
int esper_pick_ready(int after); // round-robin from after+1; -1 if none

// Identity + lifecycle of the running Esper.
uint32_t esper_current_pid();
void esper_exit(int64_t code); // running -> exited (legacy; prefer exit_and_pick)
void esper_fault();            // running -> faulted (legacy; prefer fault_and_pick)

// fork/wait helpers (read-only scans; safe to call without lock).
int esper_find_exited_child(int parent_index); // an exited/faulted child slot, or -1
int esper_child_count(int parent_index);       // live children (ready/running/waiting)

void esper_report();

} // namespace index
