#include "index/linux_abi.hpp"

#include "arch/aarch64/cpu.hpp"
#include "drivers/random_vector.hpp"
#include "index/aiwass.hpp"
#include "index/event_queue.hpp"
#include "index/antenna.hpp"
#include "index/imaginary_number_channel.hpp"
#include "index/esper.hpp"
#include "index/ext2.hpp"
#include "index/fortis931.hpp"
#include "index/idol_theory.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/last_order.hpp"
#include "drivers/misaka_mail.hpp"
#include "index/lateran.hpp"
#include "index/personal_reality_v2.hpp"
#include "index/sister_relay.hpp"
#include "index/tree_diagram.hpp"
#include "index/usermode.hpp"

namespace index {

namespace {

// Linux conventionally returns a negative errno on failure; we use -38
// (-ENOSYS) for any syscall number we don't recognise yet. Specific syscall
// numbers are written inline as `case N /*name*/` in the dispatch switch
// below (the table is large enough that a name-per-constant block adds noise
// without adding clarity).
constexpr int64_t kEnosys = -38;

} // namespace

// AArch64 ELF auxv types we populate (from elf.h). Only the ones musl reads
// during early startup matter; the rest can be omitted and musl copes.
constexpr uint64_t kAtNull = 0;
constexpr uint64_t kAtPhdr = 3;
constexpr uint64_t kAtPhent = 4;
constexpr uint64_t kAtPhnum = 5;
constexpr uint64_t kAtPagesz = 6;
constexpr uint64_t kAtBase = 7;
constexpr uint64_t kAtFlags = 8;
constexpr uint64_t kAtEntry = 9;
constexpr uint64_t kAtUid = 11;
constexpr uint64_t kAtEuid = 12;
constexpr uint64_t kAtGid = 13;
constexpr uint64_t kAtEgid = 14;
constexpr uint64_t kAtHwcap = 16;
constexpr uint64_t kAtRandom = 25;

uint64_t read_cntpct() {
    uint64_t v = 0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

uint64_t linux_build_startup_stack(Esper *e) {
    if (e == nullptr) {
        return 0;
    }
    const uint64_t top = e->stack_top;

    // 16 bytes of "randomness" for AT_RANDOM (musl seeds its stack canary +
    // malloc from this). Not cryptographic -- a counter mix is fine for now.
    uint8_t rnd[16];
    uint64_t a = read_cntpct();
    uint64_t b = a * 6364136223846793005ULL + 1442695040888963407ULL;
    for (int i = 0; i < 8; ++i) {
        rnd[i] = static_cast<uint8_t>(a >> (i * 8));
        rnd[i + 8] = static_cast<uint8_t>(b >> (i * 8));
    }
    const uint64_t random_va = (top - 16) & ~uint64_t(15);

    // Collect the argv/envp source. exec_argv is staged by kSysExec / kernel
    // shell / linux_execve right before load_elf_into_slot. When empty, fall
    // back to the legacy single-shot behaviour (argv[0] = process name) so
    // run_elf without argv still works.
    const char *argv_local0 = e->name;
    const char *fallback_argv[1] = { argv_local0 };
    const char *const *argv = (e->exec_argc > 0)
                                  ? e->exec_argv
                                  : fallback_argv;
    const uint32_t argc = (e->exec_argc > 0) ? e->exec_argc : 1;
    const char *const *envp = e->exec_envp;
    const uint32_t envc = e->exec_envc;

    // Step 1: lay out strings (argv[i], envp[j]) high-to-low below random_va.
    // Each string is written verbatim with a trailing NUL; we remember its
    // user VA so the pointer block can reference it.
    uint64_t cursor = random_va;
    uint64_t argv_va[kExecArgvCap] = {};
    uint64_t envp_va[kExecEnvpCap] = {};
    auto push_string = [&](const char *s, uint64_t *out_va) -> bool {
        uint32_t n = 0;
        while (s != nullptr && s[n] != 0) ++n;
        cursor -= (n + 1);
        *out_va = cursor;
        if (!pr2_write_user(e, cursor, s, n + 1)) return false;
        return true;
    };
    for (uint32_t i = 0; i < argc && i < kExecArgvCap; ++i) {
        if (!push_string(argv[i], &argv_va[i])) return 0;
    }
    for (uint32_t i = 0; i < envc && i < kExecEnvpCap; ++i) {
        if (!push_string(envp[i], &envp_va[i])) return 0;
    }
    cursor &= ~uint64_t(15);

    // The aux vector. Pairs of (type, value), AT_NULL last.
    const uint64_t aux[][2] = {
        {kAtPhdr, e->elf_phdr_va},
        {kAtPhent, e->elf_phentsize},
        {kAtPhnum, e->elf_phnum},
        {kAtPagesz, 4096},
        {kAtBase, e->elf_base},
        {kAtFlags, 0},
        {kAtEntry, e->elf_entry},
        {kAtUid, e->uid},
        {kAtEuid, e->euid},
        {kAtGid, e->gid},
        {kAtEgid, e->egid},
        {kAtHwcap, 0},
        {kAtRandom, random_va},
        {kAtNull, 0},
    };
    const uint32_t aux_pairs = sizeof(aux) / sizeof(aux[0]);

    // Stack block, low to high: argc, argv[argc] pointers, NULL, envp[envc]
    // pointers, NULL, auxv pairs.
    const uint64_t block_bytes = 8 + (argc + 1) * 8 + (envc + 1) * 8 + aux_pairs * 16;
    uint64_t sp = (cursor - block_bytes) & ~uint64_t(15);

    // Write the random block first (strings already written above).
    if (!pr2_write_user(e, random_va, rnd, 16)) {
        return 0;
    }

    // Now lay down the pointer block at sp.
    uint64_t cur = sp;
    auto put64 = [&](uint64_t value) -> bool {
        const bool ok = pr2_write_user(e, cur, &value, 8);
        cur += 8;
        return ok;
    };
    if (!put64(argc)) return 0;
    for (uint32_t i = 0; i < argc; ++i) {
        if (!put64(argv_va[i])) return 0;
    }
    if (!put64(0)) return 0; // argv terminator
    for (uint32_t i = 0; i < envc; ++i) {
        if (!put64(envp_va[i])) return 0;
    }
    if (!put64(0)) return 0; // envp terminator
    for (uint32_t i = 0; i < aux_pairs; ++i) {
        if (!put64(aux[i][0])) return 0;
        if (!put64(aux[i][1])) return 0;
    }

    // Consume the staging so the next exec on this slot starts clean.
    e->exec_argc = 0;
    e->exec_envc = 0;
    for (uint32_t i = 0; i < kExecArgvCap; ++i) e->exec_argv[i] = nullptr;
    for (uint32_t i = 0; i < kExecEnvpCap; ++i) e->exec_envp[i] = nullptr;
    return sp;
}

// --- helpers shared by the Phase D syscall handlers ---------------------

Esper *cur_esper() {
    const int idx = esper_running_index();
    return idx >= 0 ? esper_at(static_cast<uint32_t>(idx)) : nullptr;
}

// Ensure [va, va+len) is mapped so the kernel can read/write it at EL1. For a
// VMA-backed (Phase B) Linux Esper this faults the pages in; otherwise it's a
// no-op (legacy pool Espers are already fully mapped).
bool ensure_user(Esper *e, uint64_t va, uint64_t len) {
    if (e == nullptr || va == 0 || len == 0) {
        return va == 0 ? false : true;
    }
    // Reject ranges that overflow or escape the 39-bit user VA window (mirrors
    // Linux access_ok). Without this, pr2_prefault_range's `va + len - 1` wraps:
    // first > last, the fault loop never runs, the range is "validated" with
    // nothing faulted in, and the caller's kernel-side access then takes an
    // unhandled EL1 abort. va<ceiling && len<=ceiling => va+len can't wrap u64.
    constexpr uint64_t kUserCeiling = 0x8000000000ULL; // 1<<39
    if (va >= kUserCeiling || len > kUserCeiling || va + len > kUserCeiling) {
        return false;
    }
    if (e->mm == nullptr) {
        return true; // Index legacy pool: no VMA list, already fully mapped
    }
    return pr2_prefault_range(e, va, len);
}

// Copy a user C-string into a kernel buffer, prefaulting one page at a time so
// the caller never dereferences an unmapped user page at EL1 (an unhandled data
// abort -> kernel crash). Stops at NUL or cap-1; returns false on a fault before
// NUL (a page the string runs into has no VMA). Mirrors Linux strncpy_from_user.
// dst is always NUL-terminated on a true return.
bool copy_user_cstr(Esper *e, uint64_t va, char *dst, uint32_t cap) {
    if (e == nullptr || va == 0 || dst == nullptr || cap == 0) {
        return false;
    }
    constexpr uint64_t kUserCeiling = 0x8000000000ULL;
    uint64_t mapped_page = ~0ULL;
    for (uint32_t i = 0; i + 1 < cap; ++i) {
        const uint64_t a = va + i;
        if (a >= kUserCeiling) return false;
        const uint64_t pg = a & ~uint64_t(0xFFF);
        if (pg != mapped_page) {
            if (!ensure_user(e, pg, 1)) return false; // prefault this page or fail
            mapped_page = pg;
        }
        const char c = *reinterpret_cast<const char *>(a);
        dst[i] = c;
        if (c == 0) return true;
    }
    dst[cap - 1] = 0; // path longer than cap -> truncate
    return true;
}

// --- TTY / termios (the console line discipline) ---------------------------
// One console, so one termios state. Linux aarch64 struct termios is
// { u32 c_iflag, c_oflag, c_cflag, c_lflag; u8 c_line; u8 c_cc[19]; }. We keep
// a 64-byte blob so TCGETS/TCSETS round-trip whatever the program sets, and
// read the lflag (offset 12) to decide canonical-vs-raw / echo / signals.
constexpr uint32_t kICANON = 0x0002;
constexpr uint32_t kECHO = 0x0008;
constexpr uint32_t kISIG = 0x0001;
// kSigInt (= 2) comes from fortis931.hpp.

uint8_t g_termios[64];
bool g_termios_init = false;

// Scratch buffer for getdents64 (file scope to avoid a function-local static).
// /bin alone already has ~95 entries (busybox applet symlinks); bumped from 64
// after `ls /bin | wc -l` truncated. Each entry is ~104 bytes so 256 ~= 26 KiB.
// PER-CPU: getdents64 fills this then packs it into the user buffer within one
// syscall. EL1 syscalls are never preempted/migrated (esper_preempt bails when
// it interrupts EL1), so this_cpu_id() is stable across fill+pack -- but two
// CPUs running getdents64 concurrently would otherwise clobber a single shared
// buffer (concurrent `ls` in two SSH sessions => corrupted listings). One row
// per CPU makes it race-free with no lock. ~26 KiB * 8 = ~208 KiB bss.
constexpr uint32_t kGetdentsScratchCpus = 8; // >= kMaxCpus (artificial_heaven.hpp)
LateranEntry g_dirents[kGetdentsScratchCpus][256];

// AT_FDCWD: openat/unlinkat/etc. dirfd value that means "resolve relative
// paths against the caller's cwd". Linux defines it as -100.
constexpr int32_t kAtFdCwd = -100;

// Reusable scratch for path resolution. Two slots so the cwd/relative input
// and the normalized result don't have to share. File-scope (the same reason
// g_dirents is): a freestanding C++ build has no __cxa_guard_* for function
// statics.
char g_path_join[kCwdCap];
char g_path_norm[kCwdCap];

// Resolve `path` against the Esper's cwd (or `dirfd_path` if non-null and the
// caller passed a real dirfd). Writes a normalized absolute path to `out` and
// returns true. Absolute paths get normalized in place; relative paths are
// prepended with the base. The normalizer collapses "//" / "." / ".." per
// POSIX, but does not touch symlinks (none here).
bool resolve_path(Esper *e, const char *dirfd_path, const char *path,
                  char *out, uint32_t cap) {
    if (path == nullptr || cap < 2 || out == nullptr) {
        return false;
    }
    // Stage 1: build a flat "base + / + path" into g_path_join.
    uint32_t n = 0;
    if (path[0] == '/') {
        // Absolute: ignore the base entirely.
        while (path[n] != 0 && n + 1 < kCwdCap) {
            g_path_join[n] = path[n];
            ++n;
        }
        g_path_join[n] = 0;
    } else {
        const char *base = (dirfd_path != nullptr && dirfd_path[0] != 0)
                               ? dirfd_path
                               : (e != nullptr ? e->cwd : "/");
        uint32_t bi = 0;
        while (base[bi] != 0 && n + 1 < kCwdCap) {
            g_path_join[n++] = base[bi++];
        }
        if (n == 0 || g_path_join[n - 1] != '/') {
            if (n + 1 < kCwdCap) g_path_join[n++] = '/';
        }
        uint32_t pi = 0;
        while (path[pi] != 0 && n + 1 < kCwdCap) {
            g_path_join[n++] = path[pi++];
        }
        g_path_join[n] = 0;
    }

    // Stage 2: normalize. Walk components, build a stack of offsets into out
    // so ".." can pop the previous component cheaply. out always begins with
    // exactly one '/'.
    out[0] = '/';
    uint32_t out_n = 1;
    uint32_t starts[kCwdCap / 2];
    uint32_t depth = 0;

    uint32_t i = 0;
    while (g_path_join[i] != 0) {
        while (g_path_join[i] == '/') ++i;
        if (g_path_join[i] == 0) break;
        // [i .. j) is the next component.
        uint32_t j = i;
        while (g_path_join[j] != 0 && g_path_join[j] != '/') ++j;
        const uint32_t clen = j - i;
        const bool is_dot = clen == 1 && g_path_join[i] == '.';
        const bool is_dotdot = clen == 2 && g_path_join[i] == '.' && g_path_join[i + 1] == '.';
        if (is_dot) {
            // no-op
        } else if (is_dotdot) {
            if (depth > 0) {
                --depth;
                out_n = starts[depth];
            }
        } else {
            if (out_n + clen + 1 >= cap) return false;
            // Reserve a '/' separator: out always has a trailing slash *between*
            // components. The first component is appended directly after the
            // root '/' (out_n == 1). Subsequent components get their own '/'.
            if (out_n > 1) out[out_n++] = '/';
            starts[depth++] = out_n;
            for (uint32_t k = 0; k < clen; ++k) out[out_n++] = g_path_join[i + k];
        }
        i = j;
    }
    out[out_n] = 0;
    if (out_n == 1) {
        // Root: ensure it's just "/".
        out[0] = '/';
        out[1] = 0;
    }
    return true;
}

// openat-style: pick the right base path for a dirfd. AT_FDCWD or any dirfd
// that isn't a real open dir → use cwd via resolve_path(e, nullptr, ...).
bool resolve_at(Esper *e, int32_t dirfd, const char *path, char *out, uint32_t cap) {
    // Copy the user path into a kernel buffer up front (prefaulting page by page)
    // so resolve_path never dereferences an unmapped user page at EL1. Every
    // caller passes a user pointer (a syscall's path_va).
    char kpath[kCwdCap];
    if (!copy_user_cstr(e, reinterpret_cast<uint64_t>(path), kpath, sizeof(kpath))) {
        return false;
    }
    path = kpath;
    if (path[0] == '/') {
        return resolve_path(e, nullptr, path, out, cap);
    }
    if (dirfd == kAtFdCwd) {
        return resolve_path(e, nullptr, path, out, cap);
    }
    if (e != nullptr && static_cast<uint32_t>(dirfd) < kMaxFds &&
        e->fds[dirfd].kind == FdKind::file) {
        return resolve_path(e, e->fds[dirfd].path, path, out, cap);
    }
    return resolve_path(e, nullptr, path, out, cap);
}

uint32_t *termios_lflag() { return reinterpret_cast<uint32_t *>(g_termios + 12); }

void termios_default() {
    for (auto &b : g_termios) b = 0;
    *reinterpret_cast<uint32_t *>(g_termios + 0) = 0x0500;   // c_iflag: ICRNL|IXON
    *reinterpret_cast<uint32_t *>(g_termios + 4) = 0x0005;   // c_oflag: OPOST|ONLCR
    *reinterpret_cast<uint32_t *>(g_termios + 8) = 0x00bf;   // c_cflag: B38400|CS8|CREAD
    *reinterpret_cast<uint32_t *>(g_termios + 12) = kISIG | kICANON | kECHO | 0x10 | 0x8000;
    g_termios[16] = 0;        // c_line
    g_termios[17 + 0] = 3;    // VINTR = Ctrl-C
    g_termios[17 + 2] = 0x7f; // VERASE = DEL
    g_termios[17 + 4] = 4;    // VEOF  = Ctrl-D
    g_termios[17 + 6] = 1;    // VMIN  = 1
    g_termios_init = true;
}

uint8_t *termios_blob() {
    if (!g_termios_init) termios_default();
    return g_termios;
}

// Sentinel: the read was interrupted by Ctrl-C (VINTR) and the caller should
// deliver SIGINT. Distinct from a normal byte count / EOF.
constexpr int64_t kTtyInterrupted = -100;

// Sentinel: this syscall parked (load_ctx already swapped the trap frame to
// another Esper); caller should propagate up the chain and NOT write frame[0].
// Defined earlier than its original site (fd_read_dispatch precedent) because
// console_read_tty now uses it too. The numeric value must match line 466.
constexpr int64_t kFdParked = -1000;

// Read from the console honouring the current termios. Canonical mode does
// line editing (backspace) + optional echo and returns at newline; raw mode
// returns the first available byte(s) with no echo. Ctrl-C with ISIG set
// returns kTtyInterrupted so the caller can raise SIGINT.
int64_t console_read_tty(int idx, char *buf, uint64_t cap, uint64_t *frame) {
    namespace district = imaginary_number_district;
    const uint32_t lflag = *termios_lflag();
    const bool canon = (lflag & kICANON) != 0;
    const bool echo = (lflag & kECHO) != 0;
    const bool isig = (lflag & kISIG) != 0;

    if (!canon) {
        // Raw mode: block for one byte, then drain whatever else is ready (up
        // to cap). No echo -- the program is responsible for display.
        // No-data path used to yield-spin (this was the idle CPU heat source
        // -- busybox interactive reads land here). Now we park on ConsoleRead
        // and the 100 Hz network_tick PL011 poll wakes us when a byte arrives.
        uint64_t n = 0;
        for (;;) {
            const int ch = district::try_read();
            if (ch < 0) {
                if (n > 0) break; // nothing more pending
                if (linux_ipc_park(idx, Esper::IpcWaitKind::ConsoleRead, -1,
                                   frame) >= 0) {
                    return kFdParked;
                }
                __builtin_unreachable();
            }
            if (ch == 3 && isig) return kTtyInterrupted; // Ctrl-C
            if (n < cap) buf[n++] = static_cast<char>(ch);
            if (n >= cap) break;
        }
        return static_cast<int64_t>(n);
    }

    // Linux's n_tty keeps the in-progress canonical line in tty->read_buf
    // (a tty-struct field, not the syscall's stack) so blocking on more
    // input never loses what's been typed. Mirror that: accumulate into
    // me->tty_line_buf, park when the ring empties, return the whole line
    // on '\n'. Without this the busybox login on a secondary core hangs --
    // svc retry after park restarts console_read_tty with n=0 and the
    // already-typed bytes (which were popped from the ring during the
    // previous attempt) are gone forever.
    Esper *me = esper_at(static_cast<uint32_t>(idx));
    if (me == nullptr) return -14; // -EFAULT
    uint32_t n = me->tty_line_n;
    constexpr uint32_t kLineCap = sizeof(me->tty_line_buf);
    for (;;) {
        const int ch = district::try_read();
        if (ch < 0) {
            // No bytes ready. Park: the in-progress line lives in the Esper
            // struct so it survives the svc-retry on wake. PL011 RX IRQ's
            // linux_ipc_wake(ConsoleRead, -1) flips us back to ready.
            me->tty_line_n = n;
            if (linux_ipc_park(idx, Esper::IpcWaitKind::ConsoleRead, -1,
                               frame) >= 0) {
                return kFdParked;
            }
            __builtin_unreachable();
        }
        if (ch == 3 && isig) {
            me->tty_line_n = 0; // SIGINT discards the line
            return kTtyInterrupted;
        }
        if (ch == '\r' || ch == '\n') {
            if (echo) district::putc('\n');
            if (n < kLineCap) me->tty_line_buf[n++] = '\n';
            // Hand the completed line to the caller's buffer; truncate to cap.
            const uint64_t out = n < cap ? n : cap;
            for (uint64_t i = 0; i < out; ++i) buf[i] = me->tty_line_buf[i];
            me->tty_line_n = 0;
            return static_cast<int64_t>(out);
        }
        if ((ch == '\b' || ch == 0x7f) && n > 0) {
            --n;
            if (echo) district::write("\b \b");
            continue;
        }
        if (ch != '\t' && ch != 0x1b && (ch < 0x20 || ch == 0x7f || ch > 0xff)) {
            continue;
        }
        if (n < kLineCap) {
            me->tty_line_buf[n++] = static_cast<char>(ch);
            if (echo && ch >= 0x20 && ch <= 0x7e) district::putc(static_cast<char>(ch));
        } else {
            break;
        }
    }
    // Line buffer full (no newline) -- flush what we have and reset.
    const uint64_t out = n < cap ? n : cap;
    for (uint64_t i = 0; i < out; ++i) buf[i] = me->tty_line_buf[i];
    me->tty_line_n = 0;
    return static_cast<int64_t>(out);
}

// Ctrl-C (VINTR) was typed: raise SIGINT on the running Esper. Sets frame[0]
// itself (caller must not write it after). Makes the interrupted read look
// like -EINTR; runs a SIGINT handler if installed, else terminates the process.
void deliver_sigint(Esper *me, uint64_t *frame) {
    frame[0] = static_cast<uint64_t>(-4); // -EINTR (saved into the sigframe)
    if (me != nullptr && linux_deliver_signal(me, kSigInt, frame)) {
        return; // handler entered; frame already set up by linux_deliver_signal
    }
    const int idx = esper_running_index();
    if (idx >= 0) {
        linux_exit_running(idx, 128 + kSigInt, frame);
    }
}

// Console read for fd 0 that also handles Ctrl-C, setting frame[0] itself.
void console_read_signalled(Esper *me, int idx, char *buf, uint64_t cap,
                             uint64_t *frame) {
    const int64_t n = console_read_tty(idx, buf, cap, frame);
    if (n == kTtyInterrupted) {
        deliver_sigint(me, frame);
    } else if (n == kFdParked) {
        // frame[0] must NOT be set: ipc_park's load_ctx already swapped the
        // trap frame to the next Esper's saved context, and the dispatcher
        // returns kFdParked so the syscall path won't overwrite frame[0]
        // either. When this Esper later wakes (timer-tick ConsoleRead wake),
        // run_espers calls resume_user and the svc re-executes (elr -= 4),
        // landing back in console_read_tty with the byte now drainable.
        return;
    } else {
        frame[0] = static_cast<uint64_t>(n);
    }
}

// Write `len` bytes from a kernel buffer to fd 1/2 (the console). Returns the
// count written or -EBADF.
int64_t console_write(uint64_t fd, const char *buf, uint64_t len) {
    if (fd != 1 && fd != 2) {
        return -9; // -EBADF
    }
    for (uint64_t i = 0; i < len; ++i) {
        imaginary_number_district::putc(buf[i]);
    }
    return static_cast<int64_t>(len);
}

// Forward decl: defined in usermode.cpp. The Linux ABI dispatcher delegates to
// these so a Linux write/read to a pipe-redirected fd 1/0 actually crosses the
// pipe instead of bypassing it to the console. Mirrors the Index ABI's
// kind-aware dispatch path.
int64_t linux_pipe_write(int idx, uint32_t fd, const char *buf, uint64_t len,
                         uint64_t *frame);
int64_t linux_pipe_read(int idx, uint32_t fd, char *buf, uint64_t len,
                        uint64_t *frame);

// Kind-aware write to fd `fd` for the running Esper. Routes to console / pipe
// / file based on what's actually installed in e->fds[fd] (so dup3'd
// redirections take effect). Returns the byte count, -errno, or kFdParked
// (defined earlier; means "parked, caller must just return").

bool str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}
bool str_prefix(const char *s, const char *pfx) {
    while (*pfx) { if (*s != *pfx) return false; ++s; ++pfx; }
    return true;
}

// inotify IN_* event bits (Linux uapi/inotify.h).
constexpr uint32_t IN_MODIFY      = 0x00000002;
constexpr uint32_t IN_MOVED_FROM  = 0x00000040;
constexpr uint32_t IN_MOVED_TO    = 0x00000080;
constexpr uint32_t IN_CREATE      = 0x00000100;
constexpr uint32_t IN_DELETE      = 0x00000200;
constexpr uint32_t IN_ISDIR       = 0x40000000;

// Kazakiri: the inotify producer. Called from the VFS mutation syscalls after a
// successful change to `abs`. Scans every inotify instance's watches: a watch on
// the parent directory of `abs` gets a child event carrying the basename; a watch
// on `abs` itself gets a name-less self event. Cheap when no watches exist (the
// common case) -- it only enqueues + wakes when an interested watch is present.
static void kazakiri_notify(const char *abs, uint32_t mask, uint32_t cookie = 0) {
    if (abs == nullptr || abs[0] == 0) return;
    if (!inotify_any_watches()) return; // fast path: nobody is watching
    uint32_t len = 0; while (abs[len] != 0) ++len;
    int slash = -1;
    for (int i = static_cast<int>(len) - 1; i >= 0; --i)
        if (abs[i] == '/') { slash = i; break; }
    char dir[kInotifyPathCap];
    char base[kInotifyNameCap];
    if (slash < 0) {
        dir[0] = 0; // relative path -> no parent to match (absolute watches won't hit)
        uint32_t j = 0; for (; j + 1 < kInotifyNameCap && abs[j]; ++j) base[j] = abs[j]; base[j] = 0;
    } else {
        const uint32_t dl = static_cast<uint32_t>(slash);
        if (dl == 0) { dir[0] = '/'; dir[1] = 0; }          // parent is root
        else { uint32_t j = 0; for (; j + 1 < kInotifyPathCap && j < dl; ++j) dir[j] = abs[j]; dir[j] = 0; }
        const char *b = abs + slash + 1;
        uint32_t j = 0; for (; j + 1 < kInotifyNameCap && b[j]; ++j) base[j] = b[j]; base[j] = 0;
    }
    const uint32_t core = mask & 0xfffu; // selector bits (exclude IN_ISDIR etc.)
    bool any = false;
    for (uint32_t i = 0; i < kMaxInotify; ++i) {
        Inotify *in = inotify_at(static_cast<int>(i));
        if (in == nullptr) continue;
        for (uint32_t w = 0; w < kInotifyWatches; ++w) {
            InotifyWatch &wt = in->watches[w];
            if (!wt.in_use || (wt.mask & core) == 0) continue;
            if (str_eq(wt.path, dir)) { inotify_enqueue(in, wt.wd, mask, cookie, base); any = true; }
            else if (str_eq(wt.path, abs)) { inotify_enqueue(in, wt.wd, mask, cookie, ""); any = true; }
        }
    }
    if (any) {
        linux_ipc_wake(Esper::IpcWaitKind::InotifyRead, -1);
        linux_ipc_wake(Esper::IpcWaitKind::PollWait, -1);
        linux_ipc_wake(Esper::IpcWaitKind::EpollWait, -1);
    }
}
// Resolve fd -> the pathname /proc/<pid>/fd/<n> symlinks to. This is the Linux
// mechanism musl/glibc ttyname() relies on: a pty slave fd resolves to the
// "/dev/pts/N" string it then stats. Returns false for fds with no stable path
// (sockets, pipes, eventfds -- Linux uses "socket:[ino]" etc., not needed here).
bool fd_link_target(Esper *me, int fdn, char *out, uint32_t cap) {
    if (me == nullptr || fdn < 0 || static_cast<uint32_t>(fdn) >= kMaxFds || cap < 2) {
        return false;
    }
    auto put = [&](const char *s) {
        uint32_t i = 0;
        while (s[i] != 0 && i + 1 < cap) { out[i] = s[i]; ++i; }
        out[i] = 0;
    };
    const Fd &f = me->fds[fdn];
    switch (f.kind) {
    case FdKind::pty_slave: {
        // "/dev/pts/<sock_idx>" (sock_idx == SisterRelay id == pts number).
        char buf[24];
        const char *pfx = "/dev/pts/";
        uint32_t i = 0;
        while (pfx[i] != 0) { buf[i] = pfx[i]; ++i; }
        int n = f.sock_idx < 0 ? 0 : f.sock_idx;
        char d[12]; int k = 0;
        if (n == 0) d[k++] = '0';
        while (n > 0) { d[k++] = static_cast<char>('0' + n % 10); n /= 10; }
        while (k > 0 && i + 1 < sizeof(buf)) buf[i++] = d[--k];
        buf[i] = 0;
        put(buf);
        return true;
    }
    case FdKind::pty_master: put("/dev/ptmx");   return true;
    case FdKind::devnull:    put("/dev/null");   return true;
    case FdKind::devzero:    put("/dev/zero");   return true;
    case FdKind::devrandom:  put("/dev/urandom"); return true;
    case FdKind::devtty:     put("/dev/tty");    return true;
    case FdKind::console:    put("/dev/console"); return true;
    case FdKind::file: {
        const char *p = linux_fd_path(me, static_cast<uint32_t>(fdn));
        if (p == nullptr || p[0] == 0) return false;
        put(p);
        return true;
    }
    default:
        return false;
    }
}
int64_t fd_write_dispatch(Esper *me, int idx, uint32_t fd, const char *buf,
                          uint64_t len, uint64_t *frame) {
    if (me == nullptr) return -14; // -EFAULT
    if (fd >= kMaxFds) return -9;
    const FdKind kind = me->fds[fd].kind;
    if (kind == FdKind::file) {
        if (!me->fds[fd].writable) return -9; // -EBADF: opened read-only
        const int64_t w = linux_file_write(me, fd, buf, len);
        if (w > 0 && me->fds[fd].path[0] != 0) kazakiri_notify(me->fds[fd].path, IN_MODIFY);
        return w;
    }
    if (kind == FdKind::pipe_write) {
        return linux_pipe_write(idx, fd, buf, len, frame);
    }
    if (kind == FdKind::socket) {
        // TCP socket: stream send via antenna. UDP write() is unusual --
        // sendto is the conventional API for it; return -EOPNOTSUPP.
        return antenna_tcp_send(me->fds[fd].sock_idx,
                                reinterpret_cast<const uint8_t *>(buf),
                                static_cast<uint32_t>(len));
    }
    if (kind == FdKind::unix_sock) {
        return inc_send(me->fds[fd].sock_idx,
                        reinterpret_cast<const uint8_t *>(buf),
                        static_cast<uint32_t>(len));
    }
    if (kind == FdKind::eventfd) {
        if (len < 8) return -22; // -EINVAL: eventfd write needs a u64
        EventFd *e = eventfd_at(me->fds[fd].sock_idx);
        if (e == nullptr) return -9;
        const uint64_t v = *reinterpret_cast<const uint64_t *>(buf);
        if (v == 0xffffffffffffffffULL) return -22; // reserved sentinel
        // Saturate just below the sentinel.
        const uint64_t max = 0xfffffffffffffffeULL;
        if (max - e->counter < v) e->counter = max;
        else e->counter += v;
        if (e->counter > 0) {
            linux_ipc_wake(Esper::IpcWaitKind::EventfdRead,
                           me->fds[fd].sock_idx);
            linux_ipc_wake(Esper::IpcWaitKind::EpollWait, -1); // wildcard
        }
        return 8;
    }
    if (kind == FdKind::pty_master) {
        return sr_master_write(me->fds[fd].sock_idx,
                               reinterpret_cast<const uint8_t *>(buf),
                               static_cast<uint32_t>(len));
    }
    if (kind == FdKind::pty_slave) {
        return sr_slave_write(me->fds[fd].sock_idx,
                              reinterpret_cast<const uint8_t *>(buf),
                              static_cast<uint32_t>(len));
    }
    if (kind == FdKind::console || kind == FdKind::devtty || fd <= 2) {
        return console_write(fd, buf, len);
    }
    if (kind == FdKind::devnull || kind == FdKind::devzero) {
        return static_cast<int64_t>(len); // discard
    }
    if (kind == FdKind::devrandom) {
        return static_cast<int64_t>(len); // entropy sink; accept silently
    }
    return -9; // -EBADF
}

// Non-consuming readiness probes for ipc_park_unless_ready (invoked under
// g_esper_lock; must not take any lock themselves). The POLLIN bit (0x1) covers
// both "bytes available" and "peer gone -> EOF is readable".
static bool channel_readable(int s) { return (inc_poll(s) & 0x1u) != 0; }
static bool pty_master_readable(int s) { return (sr_poll_master(s) & 0x1u) != 0; }
static bool pty_slave_readable(int s) { return (sr_poll_slave(s) & 0x1u) != 0; }
// ppoll's "readiness" probe: not a per-fd check but "did any producer fire since
// the snapshot `snap`?". A changed generation means an fd may have become ready
// in the check-then-park window, so ipc_park_unless_ready returns false (don't
// sleep) and ppoll re-scans. O(1) -- no under-lock fd rescan. (`snap` is the
// uint32_t generation round-tripped through the int `id` parameter.)
static bool poll_gen_changed(int snap) {
    return linux_poll_gen() != static_cast<uint32_t>(snap);
}

int64_t fd_read_dispatch(Esper *me, int idx, uint32_t fd, char *buf,
                         uint64_t len, uint64_t *frame) {
    if (me == nullptr) return -14;
    if (fd >= kMaxFds) return -9;
    const FdKind kind = me->fds[fd].kind;
    if (kind == FdKind::file) {
        return linux_file_read(me, fd, buf, len);
    }
    if (kind == FdKind::pipe_read) {
        return linux_pipe_read(idx, fd, buf, len, frame);
    }
    if (kind == FdKind::socket) {
        // Try non-blocking first to short-circuit synchronous loopback (where
        // the producer side just delivered into our rx ring); fall back to
        // antenna's pump-and-wait loop only if there's actually no data and
        // the peer hasn't closed.
        const int s = me->fds[fd].sock_idx;
        bool peer_gone = false;
        const int64_t r = antenna_tcp_try_recv(s,
                                               reinterpret_cast<uint8_t *>(buf),
                                               static_cast<uint32_t>(len),
                                               &peer_gone);
        if (r > 0) return r;
        if (peer_gone) return 0;
        // Park the Esper on AntennaRecv; deliver_tcp wakes us when data lands.
        if (linux_ipc_park(idx, Esper::IpcWaitKind::AntennaRecv, s, frame) >= 0) {
            return kFdParked;
        }
        // Nothing else runnable: surface as the legacy blocking recv would.
        return antenna_tcp_recv(s, reinterpret_cast<uint8_t *>(buf),
                                static_cast<uint32_t>(len), 1000);
    }
    if (kind == FdKind::unix_sock) {
        const int s = me->fds[fd].sock_idx;
        for (;;) {
            bool peer_gone = false;
            const int64_t r = inc_try_recv(s,
                                           reinterpret_cast<uint8_t *>(buf),
                                           static_cast<uint32_t>(len),
                                           &peer_gone);
            if (r > 0) return r;
            if (peer_gone) return 0;
            // Park on ChannelRecv, but re-check readiness atomically under
            // g_esper_lock so a peer that delivered + woke between inc_try_recv
            // and the park (on another CPU) can't be missed -- the privsep
            // monitor<->child RPC lost-wakeup hang on -smp >1. If it became
            // ready in that window, loop and re-read instead of sleeping.
            if (ipc_park_unless_ready(idx, Esper::IpcWaitKind::ChannelRecv, s,
                                      frame, channel_readable)) {
                return kFdParked;
            }
        }
    }
    if (kind == FdKind::timerfd) {
        if (len < 8) return -22;
        TimerFd *t = timerfd_at(me->fds[fd].sock_idx);
        if (t == nullptr) return -9;
        uint64_t exp = 0;
        const uint64_t now = read_cntpct();
        if (t->expire_cnt != 0 && now >= t->expire_cnt) {
            exp = 1;
            if (t->interval_cnt != 0) {
                exp += (now - t->expire_cnt) / t->interval_cnt;
                t->expire_cnt += exp * t->interval_cnt; // advance past all missed periods
            } else {
                t->expire_cnt = 0; // one-shot: disarm
            }
        }
        if (exp == 0) return -11; // -EAGAIN: not expired (wait via poll/epoll)
        *reinterpret_cast<uint64_t *>(buf) = exp;
        return 8;
    }
    if (kind == FdKind::signalfd) {
        SignalFd *s = signalfd_at(me->fds[fd].sock_idx);
        if (s == nullptr) return -9;
        const uint64_t deliverable = me->sig_pending & s->mask;
        if (deliverable == 0) return -11; // -EAGAIN: nothing pending in the mask
        if (len < 128) return -22;        // struct signalfd_siginfo is 128 bytes
        const int sig = __builtin_ctzll(deliverable) + 1;
        me->sig_pending &= ~(1ULL << (sig - 1)); // consume: delivered via the fd
        for (uint32_t i = 0; i < 128; ++i) buf[i] = 0;
        *reinterpret_cast<uint32_t *>(buf) = static_cast<uint32_t>(sig); // ssi_signo
        return 128;
    }
    if (kind == FdKind::inotify) {
        Inotify *in = inotify_at(me->fds[fd].sock_idx);
        if (in == nullptr) return -9;
        const int64_t r = inotify_read_one(in, reinterpret_cast<uint8_t *>(buf), len);
        if (r != -11) return r; // an event (>0) or -EINVAL (buffer too small)
        if (in->nonblock) return -11; // -EAGAIN: ring empty, non-blocking
        // Block until a mutation queues an event (kazakiri_notify wakes us).
        if (linux_ipc_park(idx, Esper::IpcWaitKind::InotifyRead,
                           me->fds[fd].sock_idx, frame) >= 0) {
            return kFdParked;
        }
        return -11;
    }
    if (kind == FdKind::eventfd) {
        if (len < 8) return -22;
        const int s = me->fds[fd].sock_idx;
        EventFd *e = eventfd_at(s);
        if (e == nullptr) return -9;
        if (e->counter == 0) {
            if (e->nonblock) return -11; // -EAGAIN
            if (linux_ipc_park(idx, Esper::IpcWaitKind::EventfdRead, s, frame) >= 0) {
                return kFdParked;
            }
            return -11;
        }
        const uint64_t v = e->semaphore ? 1ULL : e->counter;
        e->counter -= v;
        *reinterpret_cast<uint64_t *>(buf) = v;
        return 8;
    }
    if (kind == FdKind::pty_master) {
        const int s = me->fds[fd].sock_idx;
        for (;;) {
            bool peer_gone = false;
            const int64_t r = sr_master_try_read(s, reinterpret_cast<uint8_t *>(buf),
                                                 static_cast<uint32_t>(len), &peer_gone);
            if (r > 0) return r;
            if (peer_gone) return 0;
            // Recheck-under-lock park: a slave write + wake on another CPU
            // between the try and the park must not be lost (SMP pty hang).
            if (ipc_park_unless_ready(idx, Esper::IpcWaitKind::PtyMasterRead, s,
                                      frame, pty_master_readable))
                return kFdParked;
        }
    }
    if (kind == FdKind::pty_slave) {
        const int s = me->fds[fd].sock_idx;
        for (;;) {
            bool peer_gone = false;
            const int64_t r = sr_slave_try_read(s, reinterpret_cast<uint8_t *>(buf),
                                                static_cast<uint32_t>(len), &peer_gone);
            if (r > 0) return r;
            if (peer_gone) return 0;
            if (ipc_park_unless_ready(idx, Esper::IpcWaitKind::PtySlaveRead, s,
                                      frame, pty_slave_readable))
                return kFdParked;
        }
    }
    if (kind == FdKind::devnull) {
        return 0; // immediate EOF
    }
    if (kind == FdKind::devzero) {
        for (uint64_t i = 0; i < len; ++i) buf[i] = 0;
        return static_cast<int64_t>(len);
    }
    if (kind == FdKind::devrandom) {
        // Prefer hardware entropy from the RandomVector (virtio-rng); fall
        // back to a CNTPCT-mixed LCG when the device isn't present.
        const uint32_t hw = drivers::random_vector_read(
            reinterpret_cast<uint8_t *>(buf), static_cast<uint32_t>(len));
        if (hw > 0) return static_cast<int64_t>(hw);
        uint64_t s = read_cntpct() ^ 0x9e3779b97f4a7c15ULL;
        for (uint64_t i = 0; i < len; ++i) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            buf[i] = static_cast<char>(s >> 33);
        }
        return static_cast<int64_t>(len);
    }
    if (kind == FdKind::console || kind == FdKind::devtty || fd == 0) {
        // Sets frame[0] itself (or parks; either way the syscall dispatcher
        // must not touch frame[0] -- that's the contract of kFdParked).
        console_read_signalled(me, idx, buf, len, frame);
        return kFdParked;
    }
    return 0; // EOF for unbacked fds (1/2 etc.)
}

// Linux struct iovec { void *iov_base; size_t iov_len; } -- 16 bytes each.
struct IoVec {
    uint64_t base;
    uint64_t len;
};

// EL0t, IRQ enabled, F/A/D masked -- the PSTATE a signal handler starts with
// (matches how the loader starts EL0 code).
constexpr uint64_t kSpsrEl0Handler = 0x340;

// Signal numbers that cannot be blocked, ignored, or caught (POSIX).
constexpr uint64_t kSigKillBit = 1ULL << (9 - 1);
constexpr uint64_t kSigStopBit = 1ULL << (19 - 1);
constexpr uint64_t kNonmaskable = kSigKillBit | kSigStopBit;

// Internal helper: assumes caller holds g_esper_lock. Used by both the
// public linux_deliver_signal wrapper below and by fortis931_kill, which
// needs to hold the lock across the abi/state check + the signal delivery
// + the teardown writes as one atomic block.
bool linux_deliver_signal_locked(Esper *e, int sig, uint64_t *frame) {
    if (e == nullptr || e->abi != Abi::Linux || sig <= 0 || sig >= 64) {
        return false;
    }
    const uint64_t bit = 1ULL << (sig - 1);

    // ptrace "Mental Out" intercept: a traced tracee turns most signals into a
    // pending ptrace signal-stop (the syscall-return drain parks it + reports to
    // the tracer's wait4) instead of delivering them. SIGKILL is never trapped.
    // A signal injected by the tracer (CONT/SYSCALL data arg) passes through once
    // via ptrace_inject_sig so the tracer can actually deliver a signal. Setting
    // the sig_pending bit makes an interruptible blocking syscall (ppoll/read)
    // wake so the tracee reaches the drain and stops.
    if (e->ptrace_tracer >= 0 && sig != 9 /*SIGKILL*/) {
        if (e->ptrace_inject_sig == sig) {
            e->ptrace_inject_sig = 0; // one-shot pass-through -> deliver normally
        } else {
            if (e->ptrace_pending_stop == 0) e->ptrace_pending_stop = sig;
            e->sig_pending |= bit;
            return true; // consumed: turned into a pending ptrace-stop
        }
    }

    // rt_sigsuspend wake: a signal arriving at a sigsuspend-parked Esper
    // restores its pre-sigsuspend mask and flips it back to ready. The
    // signal is then delivered to the handler unconditionally (the kernel
    // unblocked it for sigsuspend's duration by definition). Mirrors Linux:
    // restore task->blocked = saved_sigmask BEFORE the handler is set up,
    // so the sigframe gets the original mask and rt_sigreturn restores it
    // after the handler returns.
    const bool waking_sigsuspend = e->wait_sigsuspend;
    if (waking_sigsuspend) {
        e->sig_mask = e->sigsuspend_saved_mask;
        e->wait_sigsuspend = false;
        if (e->state == EsperState::waiting) {
            const uint32_t cpu = arch::this_cpu_id();
            (void)cpu;
            e->state = EsperState::ready;
        }
    }

    // Blocked? Queue as pending unless this is SIGKILL/SIGSTOP which never
    // get blocked. The pending bit is drained when the mask clears (in
    // rt_sigprocmask / rt_sigreturn). The sigsuspend-wake path skips this
    // mask check -- that signal is what we were waiting for, deliver it.
    if (!waking_sigsuspend && !(bit & kNonmaskable) && (e->sig_mask & bit)) {
        e->sig_pending |= bit;
        return true; // "delivered" by being queued
    }

    const uint64_t handler = e->sig_handler[sig];
    if (handler == 0 /*SIG_DFL*/ || handler == 1 /*SIG_IGN*/) {
        return false; // caller applies the default action
    }

    // Any non-current target (frame == nullptr): just mark the signal pending
    // and let the TARGET build its own signal frame on ITS cpu, at its next
    // preempt / syscall-return signal drain. Mainstream kernels deliver signals
    // on the target's return-to-user path -- the sender never writes the
    // target's user stack or mutates its page tables (CoW) off-core. The old
    // code did that off-core write for waiting/ready targets; under SMP that
    // races the target's own faults/execution and intermittently corrupts its
    // user memory (the cpu-N user-pointer smashes 0x11 / 0x40081318). frame !=
    // nullptr means the target IS this cpu's running Esper, so building now is
    // safe and immediate.
    if (frame == nullptr) {
        e->sig_pending |= bit;
        return true;
    }

    // Gather the interrupted EL0 context. For the running Esper (frame != null)
    // it's the live trap frame + system registers; otherwise it's the saved
    // context in the Esper struct (stable under g_esper_lock since target
    // is not running on another CPU -- ruled out above).
    uint64_t regs[31];
    uint64_t user_sp, user_pc, user_pstate;
    if (frame != nullptr) {
        for (uint32_t i = 0; i < 31; ++i) regs[i] = frame[i];
        asm volatile("mrs %0, sp_el0" : "=r"(user_sp));
        asm volatile("mrs %0, elr_el1" : "=r"(user_pc));
        asm volatile("mrs %0, spsr_el1" : "=r"(user_pstate));
    } else {
        if (!e->started) return false;
        for (uint32_t i = 0; i < 31; ++i) regs[i] = e->regs[i];
        user_sp = e->sp_el0;
        user_pc = e->elr;
        user_pstate = e->spsr;
    }

    // Lay out an rt_sigframe below the current SP: { siginfo info(128);
    // ucontext uc; } with uc.uc_sigmask at uc+40 (the current process mask
    // we save here so rt_sigreturn can restore it) and uc.uc_mcontext at
    // uc+168 holding the saved regs.
    constexpr uint64_t kFrameSize = 0x600;
    const uint64_t base = (user_sp - kFrameSize) & ~uint64_t(15);
    const uint64_t uc = base + 128;
    const uint64_t mc = uc + 168;

    // Zero the fixed region, then fill siginfo.si_signo and the mcontext.
    // pr2_write_user only touches page_ref/page_unref (atomic, no lock) so
    // it's safe to invoke while holding g_esper_lock. Zero in 64-byte chunks
    // rather than from a kFrameSize (0x600 = 1.5 KiB) stack buffer: this runs on
    // the boot core's idle-Sister kernel stack (only 8 KiB, vs 64 KiB on
    // secondaries) deep in the syscall/IRQ chain (-> pr2_write_user -> fault ->
    // page alloc), and a preempt here can nest a SECOND signal build -- a 1.5 KiB
    // frame doubled the kernel-stack high-water mark and risked overrunning the
    // 8 KiB stack into adjacent kernel memory (the broad SMP wild-write class).
    uint8_t zero[64] = {};
    for (uint64_t off = 0; off < kFrameSize; off += sizeof(zero)) {
        const uint64_t chunk = (kFrameSize - off < sizeof(zero)) ? kFrameSize - off : sizeof(zero);
        if (!pr2_write_user(e, base + off, zero, chunk)) return false;
    }
    const uint32_t signo = static_cast<uint32_t>(sig);
    if (!pr2_write_user(e, base, &signo, 4)) return false; // siginfo.si_signo

    // Save current mask in uc_sigmask (the low 8 bytes; the full sigset_t is
    // 128 bytes but we only ever track signals 1..64).
    const uint64_t saved_mask = e->sig_mask;
    if (!pr2_write_user(e, uc + 40, &saved_mask, 8)) return false;

    // Linux semantics: while the handler runs, block this signal itself plus
    // anything in sigaction's sa_mask (SA_NODEFER would suppress the first;
    // we don't honour that flag yet).
    e->sig_mask = (saved_mask | e->sig_act_mask[sig] | bit) & ~kNonmaskable;

    // mcontext: fault_address(0), regs[31], sp, pc, pstate.
    if (!pr2_write_user(e, mc + 8, regs, 31 * 8)) return false;
    if (!pr2_write_user(e, mc + 8 + 31 * 8, &user_sp, 8)) return false;
    if (!pr2_write_user(e, mc + 8 + 32 * 8, &user_pc, 8)) return false;
    if (!pr2_write_user(e, mc + 8 + 33 * 8, &user_pstate, 8)) return false;

    // Build the new EL0 state: enter the handler with (sig, &info, &uc),
    // SP at the frame base, LR = the program's restorer (musl __restore_rt,
    // which issues rt_sigreturn).
    uint64_t nregs[31] = {};
    nregs[0] = static_cast<uint64_t>(sig);
    nregs[1] = base;        // siginfo*
    nregs[2] = uc;          // ucontext*
    nregs[30] = e->sig_restorer[sig]; // LR -> restorer
    if (frame != nullptr) {
        for (uint32_t i = 0; i < 31; ++i) frame[i] = nregs[i];
        asm volatile("msr sp_el0, %0" ::"r"(base));
        asm volatile("msr elr_el1, %0" ::"r"(handler));
        asm volatile("msr spsr_el1, %0" ::"r"(kSpsrEl0Handler));
    } else {
        for (uint32_t i = 0; i < 31; ++i) e->regs[i] = nregs[i];
        e->sp_el0 = base;
        e->elr = handler;
        e->spsr = kSpsrEl0Handler;
    }
    return true;
}

// Public entry point: acquires/releases g_esper_lock around the locked
// helper. Use this from non-fortis931 call sites (rt_sigreturn,
// kSysTgkill / kSysKill, raise) where the caller is not already holding
// the lock.
bool linux_deliver_signal(Esper *e, int sig, uint64_t *frame) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    const bool delivered = linux_deliver_signal_locked(e, sig, frame);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return delivered;
}

// Poll/epoll readiness mask for one fd (POLLIN=1, POLLOUT=4, POLLERR=8,
// POLLHUP=0x10, POLLNVAL=0x20), non-consuming. Real poll() needs per-fd
// readiness instead of the old "always ready"; the socket case unblocks sshd.
static uint32_t linux_fd_revents(Esper *me, int fd) {
    if (fd < 0 || static_cast<uint32_t>(fd) >= kMaxFds) return 0x20; // POLLNVAL
    switch (me->fds[fd].kind) {
    case FdKind::socket:     return antenna_poll(me->fds[fd].sock_idx);
    case FdKind::unix_sock:  return inc_poll(me->fds[fd].sock_idx);
    case FdKind::eventfd: {
        EventFd *e = eventfd_at(me->fds[fd].sock_idx);
        uint32_t re = 0x4; if (e != nullptr && e->counter > 0) re |= 0x1; return re;
    }
    case FdKind::timerfd: {
        TimerFd *t = timerfd_at(me->fds[fd].sock_idx);
        return (t != nullptr && t->expire_cnt != 0 && read_cntpct() >= t->expire_cnt) ? 0x1u : 0u;
    }
    case FdKind::signalfd: {
        SignalFd *s = signalfd_at(me->fds[fd].sock_idx);
        return (s != nullptr && (me->sig_pending & s->mask) != 0) ? 0x1u : 0u;
    }
    case FdKind::inotify: {
        Inotify *in = inotify_at(me->fds[fd].sock_idx);
        return inotify_has_events(in) ? 0x1u : 0u;
    }
    case FdKind::pipe_read:  return aiwass_readable(me->fds[fd].pipe_idx) ? 0x1u : 0u;
    case FdKind::pipe_write: return aiwass_writable(me->fds[fd].pipe_idx) ? 0x4u : 0u;
    case FdKind::console:
    case FdKind::devtty:
        // POLLOUT always writable; POLLIN only when a byte is actually waiting.
        // Returning 0x5 (always POLLIN-ready) made ppoll(stdin, POLLIN) report
        // ready every iteration -- busybox ash in SCRIPT mode ppoll's fd 0 and
        // busy-looped forever (`sh file.sh` / `#!/bin/sh` hung at 100% CPU, never
        // running the script). has_input() is a non-destructive PL011 FIFO peek;
        // with no byte we report not-readable so ppoll parks (100 Hz network_tick
        // re-scans + wakes PollWait), and the parked reader drains via try_read.
        return 0x4u | (imaginary_number_district::has_input() ? 0x1u : 0u);
    case FdKind::pty_master: return sr_poll_master(me->fds[fd].sock_idx);
    case FdKind::pty_slave:  return sr_poll_slave(me->fds[fd].sock_idx);
    case FdKind::closed:     return 0x20;
    default:                 return 0x5; // file/dev*: always ready
    }
}

void linux_syscall_dispatch(uint64_t *frame) {
    namespace district = imaginary_number_district;
    const uint64_t nr = frame[8];
    Esper *me = cur_esper();
    if (me != nullptr) me->last_syscall = static_cast<uint32_t>(nr); // [WD] hang diag

    switch (nr) {

    // ---- I/O ----------------------------------------------------------
    case 64 /*write*/: {
        const uint32_t fd = static_cast<uint32_t>(frame[0]);
        const uint64_t buf = frame[1];
        const uint64_t len = frame[2];
        if (buf != 0 && !ensure_user(me, buf, len)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        const int idx = esper_running_index();
        const int64_t r = fd_write_dispatch(me, idx, fd,
                                            reinterpret_cast<const char *>(buf),
                                            len, frame);
        if (r == kFdParked) return; // frame[0] is set on resume
        frame[0] = static_cast<uint64_t>(r);
        return;
    }
    case 66 /*writev*/: {
        const uint32_t fd = static_cast<uint32_t>(frame[0]);
        const uint64_t iov_va = frame[1];
        const uint64_t cnt = frame[2];
        if (cnt > 1024) { frame[0] = static_cast<uint64_t>(-22); return; } // UIO_MAXIOV: bound before cnt*16 can overflow
        if (!ensure_user(me, iov_va, cnt * sizeof(IoVec))) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        const IoVec *iov = reinterpret_cast<const IoVec *>(iov_va);
        const int idx = esper_running_index();
        int64_t total = 0;
        for (uint64_t i = 0; i < cnt; ++i) {
            if (iov[i].len == 0) continue;
            if (!ensure_user(me, iov[i].base, iov[i].len)) {
                frame[0] = static_cast<uint64_t>(-14); return;
            }
            const char *vb = reinterpret_cast<const char *>(iov[i].base);
            const int64_t n = fd_write_dispatch(me, idx, fd, vb, iov[i].len, frame);
            if (n == kFdParked) return;
            if (n < 0) {
                frame[0] = static_cast<uint64_t>(total > 0 ? total : n);
                return;
            }
            total += n;
        }
        frame[0] = static_cast<uint64_t>(total);
        return;
    }
    case 63 /*read*/: {
        const uint32_t fd = static_cast<uint32_t>(frame[0]);
        const uint64_t buf = frame[1];
        const uint64_t len = frame[2];
        if (!ensure_user(me, buf, len)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        const int idx = esper_running_index();
        const int64_t r = fd_read_dispatch(me, idx, fd, reinterpret_cast<char *>(buf), len, frame);
        if (r == kFdParked) return; // frame[0] is set (console or pipe)
        frame[0] = static_cast<uint64_t>(r);
        return;
    }
    case 65 /*readv*/: {
        // readv(fd, iov, cnt): read sequentially into each iovec. Used by
        // musl's buffered stdio fread path. Backed by the same fd dispatch.
        const uint32_t fd = static_cast<uint32_t>(frame[0]);
        const uint64_t iov_va = frame[1];
        const uint64_t cnt = frame[2];
        if (cnt > 1024) { frame[0] = static_cast<uint64_t>(-22); return; } // UIO_MAXIOV: bound before cnt*16 can overflow
        if (!ensure_user(me, iov_va, cnt * sizeof(IoVec))) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        const IoVec *iov = reinterpret_cast<const IoVec *>(iov_va);
        const int idx = esper_running_index();
        int64_t total = 0;
        for (uint64_t i = 0; i < cnt; ++i) {
            if (iov[i].len == 0) continue;
            if (!ensure_user(me, iov[i].base, iov[i].len)) {
                frame[0] = static_cast<uint64_t>(-14); return;
            }
            char *b = reinterpret_cast<char *>(iov[i].base);
            const int64_t n = fd_read_dispatch(me, idx, fd, b, iov[i].len, frame);
            if (n == kFdParked) return;
            if (n <= 0) break;
            total += n;
            if (static_cast<uint64_t>(n) < iov[i].len) break; // short read
        }
        frame[0] = static_cast<uint64_t>(total);
        return;
    }
    case 67 /*pread64*/: {
        // pread64(fd, buf, count, offset): read at `offset` WITHOUT moving the
        // fd position (POSIX). openjdk25's nio/zip uses it where openjdk8 used a
        // plain read. Implement by save-seek-read-restore on the existing fd
        // cursor (same primitives lseek uses). Mirrors Linux's pread64 contract.
        const uint32_t fd = static_cast<uint32_t>(frame[0]);
        const uint64_t buf = frame[1];
        const uint64_t len = frame[2];
        const uint64_t offset = frame[3];
        if (!ensure_user(me, buf, len)) { frame[0] = static_cast<uint64_t>(-14); return; }
        if (!linux_fd_is_file(me, fd)) { frame[0] = static_cast<uint64_t>(-29); return; } // -ESPIPE
        const uint64_t save = linux_fd_tell(me, fd);
        linux_fd_seek(me, fd, offset);
        const int64_t r = linux_file_read(me, fd, reinterpret_cast<char *>(buf), len);
        linux_fd_seek(me, fd, save); // restore: pread must not move the cursor
        frame[0] = static_cast<uint64_t>(r);
        return;
    }
    case 168 /*getcpu*/: {
        // getcpu(*cpu, *node, tcache): current CPU + NUMA node. openjdk25's
        // runtime queries it for thread/heap placement (openjdk8 didn't). tcache
        // is ignored (Linux has since 2.6.24). Index is a single NUMA node (0).
        const uint64_t cpu_va = frame[0];
        const uint64_t node_va = frame[1];
        if (cpu_va != 0) {
            if (!ensure_user(me, cpu_va, 4)) { frame[0] = static_cast<uint64_t>(-14); return; }
            *reinterpret_cast<uint32_t *>(cpu_va) = arch::this_cpu_id();
        }
        if (node_va != 0) {
            if (!ensure_user(me, node_va, 4)) { frame[0] = static_cast<uint64_t>(-14); return; }
            *reinterpret_cast<uint32_t *>(node_va) = 0;
        }
        frame[0] = 0;
        return;
    }
    case 293 /*rseq*/:
        // rseq (restartable sequences): glibc 2.35+ registers it for fast per-CPU
        // ops. Index has no rseq; return -ENOSYS so glibc uses its non-rseq path
        // (handled gracefully). Explicit so it skips the unknown-syscall warning.
        // Mirrors a Linux kernel built without CONFIG_RSEQ.
        frame[0] = static_cast<uint64_t>(-38); // -ENOSYS
        return;
    case 435 /*clone3*/:
        // clone3: newer clone ABI. Index implements clone(220); glibc/musl fall
        // back to clone() on -ENOSYS, so threads use the supported path. Mirrors
        // an older Linux kernel that predates clone3.
        frame[0] = static_cast<uint64_t>(-38); // -ENOSYS
        return;
    case 62 /*lseek*/: {
        const uint64_t fd = frame[0];
        const int64_t off = static_cast<int64_t>(frame[1]);
        const uint64_t whence = frame[2];
        if (!linux_fd_is_file(me, static_cast<uint32_t>(fd))) {
            frame[0] = static_cast<uint64_t>(-29); // -ESPIPE for console/pipe
            return;
        }
        uint64_t base = 0;
        if (whence == 1 /*SEEK_CUR*/) {
            base = linux_fd_tell(me, static_cast<uint32_t>(fd));
        } else if (whence == 2 /*SEEK_END*/) {
            const int64_t sz = linux_fd_size(me, static_cast<uint32_t>(fd));
            base = (sz > 0) ? static_cast<uint64_t>(sz) : 0;
        }
        const uint64_t pos = base + static_cast<uint64_t>(off);
        linux_fd_seek(me, static_cast<uint32_t>(fd), pos);
        frame[0] = pos;
        return;
    }
    case 29 /*ioctl*/: {
        const uint64_t fd = frame[0];
        // musl passes the request as a signed int that gets sign-extended into
        // x1 -- ioctls with bit 31 set (any _IOR with non-zero size) arrive as
        // 0xFFFFFFFF........  Mask to the canonical 32-bit value so the
        // comparisons below stay readable.
        const uint64_t req = frame[1] & 0xFFFFFFFFULL;
        const uint64_t arg = frame[2];
        // "Is this fd a tty?"  An explicit openat("/dev/console") / "/dev/tty"
        // / "/dev/pts/N" returns one of the FdKind tty variants; fd 0/1/2
        // start out as FdKind::console at exec time and dup2(pipe_w, 1) etc.
        // will overwrite that. Checking the kind (not the fd number) is what
        // lets `cmd | wc` look like a pipe to the child and not a terminal.
        const FdKind kk = (me != nullptr && fd < kMaxFds) ? me->fds[fd].kind
                                                           : FdKind::closed;
        const bool is_tty = kk == FdKind::console || kk == FdKind::devtty ||
                            kk == FdKind::pty_master || kk == FdKind::pty_slave;
        const bool is_master = (kk == FdKind::pty_master);
        if (req == 0x5413 /*TIOCGWINSZ*/ && is_tty && arg != 0 && ensure_user(me, arg, 8)) {
            // Must reject non-tty fds: musl's isatty() probes by issuing this
            // ioctl, so always-succeed had ls/grep treating piped stdout as
            // a terminal and emitting columnar+color output instead of one
            // file per line, breaking `ls | wc -l` (94 files reported as 9).
            uint16_t *ws = reinterpret_cast<uint16_t *>(arg); // {row,col,xpixel,ypixel}
            ws[2] = 0; ws[3] = 0;
            if (kk == FdKind::pty_master || kk == FdKind::pty_slave) {
                SisterRelay *r = sr_at(me->fds[fd].sock_idx);
                ws[0] = r ? r->rows : 24;
                ws[1] = r ? r->cols : 80;
            } else {
                ws[0] = 24; ws[1] = 80;
            }
            frame[0] = 0;
        } else if (req == 0x5414 /*TIOCSWINSZ*/ && is_tty && arg != 0 && ensure_user(me, arg, 8)) {
            const uint16_t *ws = reinterpret_cast<const uint16_t *>(arg);
            if (kk == FdKind::pty_master || kk == FdKind::pty_slave) {
                SisterRelay *r = sr_at(me->fds[fd].sock_idx);
                if (r) { r->rows = ws[0]; r->cols = ws[1]; }
            }
            frame[0] = 0;
        } else if (req == 0x80045430 /*TIOCGPTN*/ && is_master && arg != 0 &&
                   ensure_user(me, arg, 4)) {
            // Master fd -> slave number for /dev/pts/N lookup.
            *reinterpret_cast<int32_t *>(arg) = me->fds[fd].sock_idx;
            frame[0] = 0;
        } else if (req == 0x40045431 /*TIOCSPTLCK*/ && is_master && arg != 0 &&
                   ensure_user(me, arg, 4)) {
            const int lock = *reinterpret_cast<const int32_t *>(arg);
            SisterRelay *r = sr_at(me->fds[fd].sock_idx);
            if (r) r->slave_locked = (lock != 0);
            frame[0] = 0;
        } else if (req == 0x5401 /*TCGETS*/ && is_tty && arg != 0 && ensure_user(me, arg, 64)) {
            // For pty fds, return that pty's saved termios; for console use the global.
            const uint8_t *t = termios_blob();
            if (kk == FdKind::pty_master || kk == FdKind::pty_slave) {
                SisterRelay *r = sr_at(me->fds[fd].sock_idx);
                if (r) t = r->termios;
            }
            for (uint32_t i = 0; i < 44; ++i) reinterpret_cast<uint8_t *>(arg)[i] = t[i];
            frame[0] = 0;
        } else if ((req == 0x5402 || req == 0x5403 || req == 0x5404) /*TCSETS/W/F*/ &&
                   is_tty && arg != 0 && ensure_user(me, arg, 64)) {
            uint8_t *t = termios_blob();
            if (kk == FdKind::pty_master || kk == FdKind::pty_slave) {
                SisterRelay *r = sr_at(me->fds[fd].sock_idx);
                if (r) t = r->termios;
            }
            for (uint32_t i = 0; i < 44; ++i) t[i] = reinterpret_cast<const uint8_t *>(arg)[i];
            // If this TCSETS is for the console (not a pty), republish ISIG +
            // VINTR for the PL011 IRQ-path SIGINT generator. c_lflag at byte
            // 12 (bit 0 = ISIG); c_cc[VINTR] at byte 17.
            if (kk == FdKind::console) {
                const uint32_t lflag = *reinterpret_cast<const uint32_t *>(t + 12);
                const bool isig_on = (lflag & 0x01) != 0;
                const char vintr = static_cast<char>(t[17]);
                district::set_console_isig(isig_on, vintr != 0 ? vintr : 3);
            }
            frame[0] = 0;
        } else if (req == 0x540E /*TIOCSCTTY*/ && is_tty) {
            // Acquire the controlling tty. Linux sets the tty's foreground
            // process group (and session) to the acquirer's right here: the
            // session leader that opens a pty becomes its foreground group.
            // WITHOUT this, the pty kept a STALE fg pgrp (the sshd session's,
            // e.g. 8) so the freshly setsid+exec'd shell (pgrp 9) ran its
            // job-control init -- while (tcgetpgrp(tty) != getpgrp()) kill(0,
            // SIGTTIN); -- forever, SIGTTIN'd itself and died (no prompt). It
            // "worked" before only when the fg pgrp raced to the right value.
            // Stamping it here at ctty-acquire makes it deterministic. ptys
            // carry it in the SisterRelay; the console keeps its existing
            // (working) global behaviour.
            if ((kk == FdKind::pty_master || kk == FdKind::pty_slave) &&
                me != nullptr) {
                SisterRelay *r = sr_at(me->fds[fd].sock_idx);
                if (r != nullptr) {
                    r->fg_pgid = static_cast<int>(me->pgrp);
                    r->sid = static_cast<int>(me->sid);
                }
            }
            frame[0] = 0;
        } else if (req == 0x540F /*TIOCGPGRP*/ && is_tty && arg != 0 && ensure_user(me, arg, 4)) {
            // Foreground process group of the *controlling tty* -- MUST be
            // per-tty. A single global value let the console shell's pgrp leak
            // onto the SSH pty, so the pty shell's job-control init loop
            //   while (tcgetpgrp(tty) != getpgrp()) kill(0, SIGTTIN);
            // never matched and spun forever (ioctl<->kill), so it never printed
            // its prompt. ptys carry their own fg pgrp in the SisterRelay; the
            // console keeps the global (the PL011 VINTR IRQ path reads it for
            // SIGINT). If this tty's fg pgrp is unset, report the CALLER as
            // foreground so the loop exits on its first iteration.
            int32_t fg = 0;
            if (kk == FdKind::pty_master || kk == FdKind::pty_slave) {
                SisterRelay *r = sr_at(me->fds[fd].sock_idx);
                if (r != nullptr && r->fg_pgid > 0) fg = r->fg_pgid;
            } else {
                fg = static_cast<int32_t>(district::get_fg_pgrp());
            }
            *reinterpret_cast<int32_t *>(arg) =
                (fg > 0) ? fg : (me != nullptr ? static_cast<int32_t>(me->pgrp) : 1);
            frame[0] = 0;
        } else if (req == 0x5410 /*TIOCSPGRP*/ && is_tty && arg != 0 && ensure_user(me, arg, 4)) {
            // tcsetpgrp: install the new fg pgrp on THIS tty. ptys store it in
            // their own SisterRelay; the console updates the global the VINTR
            // IRQ path reads when delivering SIGINT.
            const uint32_t newpg = *reinterpret_cast<const uint32_t *>(arg);
            if (kk == FdKind::pty_master || kk == FdKind::pty_slave) {
                SisterRelay *r = sr_at(me->fds[fd].sock_idx);
                if (r != nullptr) r->fg_pgid = static_cast<int>(newpg);
            } else {
                district::set_fg_pgrp(newpg);
            }
            frame[0] = 0;
        } else if (req == 0x5421 /*FIONBIO*/) {
            // Set non-blocking mode -- accept silently.
            frame[0] = 0;
        } else if (req == 0x540B /*TCFLSH*/) {
            // Flush tty input/output: we have no pending buffers per-tty yet.
            frame[0] = 0;
        } else {
            frame[0] = static_cast<uint64_t>(-25); // -ENOTTY
        }
        return;
    }
    case 56 /*openat*/: {
        // openat(dirfd, path, flags, mode): resolve cwd-relative paths against
        // either the dirfd's stored path (a real open dir) or the caller's
        // cwd (AT_FDCWD or anything else). Honours O_CREAT/O_TRUNC/O_WRONLY/
        // O_RDWR/O_APPEND so programs can create + write files.
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t path_va = frame[1];
        const uint64_t flags = frame[2];
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        const bool writable = (flags & 0x3) != 0;       // O_WRONLY(1) | O_RDWR(2)
        const bool create = (flags & 0x40) != 0;        // O_CREAT
        const bool trunc = (flags & 0x200) != 0;        // O_TRUNC
        const bool append = (flags & 0x400) != 0;       // O_APPEND
        char abs[kCwdCap];
        if (!resolve_at(me, dirfd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }

        // /dev/* synthetic devices come ahead of the on-disk lookup so they
        // work even when the rootfs has no device nodes.
        {
            FdKind dk = FdKind::closed;
            if (str_eq(abs, "/dev/null")) dk = FdKind::devnull;
            else if (str_eq(abs, "/dev/zero")) dk = FdKind::devzero;
            else if (str_eq(abs, "/dev/random") || str_eq(abs, "/dev/urandom"))
                dk = FdKind::devrandom;
            else if (str_eq(abs, "/dev/tty") || str_eq(abs, "/dev/console"))
                dk = FdKind::devtty;
            if (dk != FdKind::closed) {
                int dfd = -1;
                for (uint32_t i = 3; i < kMaxFds; ++i) {
                    if (me->fds[i].kind == FdKind::closed) { dfd = static_cast<int>(i); break; }
                }
                if (dfd < 0) { frame[0] = static_cast<uint64_t>(-24); return; } // -EMFILE
                me->fds[dfd] = Fd{};
                me->fds[dfd].kind = dk;
                me->fds[dfd].writable = writable || (dk != FdKind::devnull); // /dev/null read returns EOF anyway
                frame[0] = static_cast<uint64_t>(dfd);
                return;
            }
        }

        // /dev/ptmx -- allocate a fresh SisterRelay pair, hand back the master.
        // /dev/pts/N -- open the slave end of pair N (must be unlocked).
        if (str_eq(abs, "/dev/ptmx")) {
            const int si = sr_alloc();
            if (si < 0) { frame[0] = static_cast<uint64_t>(-23); return; }
            int dfd = -1;
            for (uint32_t i = 3; i < kMaxFds; ++i) {
                if (me->fds[i].kind == FdKind::closed) { dfd = static_cast<int>(i); break; }
            }
            if (dfd < 0) {
                sr_close_master(si);
                frame[0] = static_cast<uint64_t>(-24); return;
            }
            me->fds[dfd] = Fd{};
            me->fds[dfd].kind = FdKind::pty_master;
            me->fds[dfd].sock_idx = si;
            frame[0] = static_cast<uint64_t>(dfd);
            return;
        }
        if (abs[0] == '/' && abs[1] == 'd' && abs[2] == 'e' && abs[3] == 'v' &&
            abs[4] == '/' && abs[5] == 'p' && abs[6] == 't' && abs[7] == 's' &&
            abs[8] == '/') {
            int n = 0;
            const char *p = abs + 9;
            if (*p < '0' || *p > '9') { frame[0] = static_cast<uint64_t>(-2); return; }
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); ++p; }
            if (*p != 0) { frame[0] = static_cast<uint64_t>(-2); return; }
            const int si = sr_open_slave(n);
            if (si < 0) { frame[0] = static_cast<uint64_t>(-13); return; } // -EACCES (locked or invalid)
            int dfd = -1;
            for (uint32_t i = 3; i < kMaxFds; ++i) {
                if (me->fds[i].kind == FdKind::closed) { dfd = static_cast<int>(i); break; }
            }
            if (dfd < 0) {
                sr_close_slave(si);
                frame[0] = static_cast<uint64_t>(-24); return;
            }
            me->fds[dfd] = Fd{};
            me->fds[dfd].kind = FdKind::pty_slave;
            me->fds[dfd].sock_idx = si;
            frame[0] = static_cast<uint64_t>(dfd);
            return;
        }

        // inotify IN_CREATE fires only when O_CREAT actually makes a new file, so
        // record prior existence -- but only for O_CREAT opens, so the common
        // non-creating open pays nothing.
        bool pre_exists = true;
        if (create) { LateranEntry cst; pre_exists = lateran_is_dir(abs) || lateran_stat(abs, &cst); }

        int fd = (writable || create)
                     ? linux_file_open_ex(me, abs, create, trunc, writable)
                     : linux_file_open(me, abs);
        if (fd < 0 && !writable && !create && lateran_is_dir(abs)) {
            // Opening a directory (e.g. for getdents64): give it an fd whose
            // path is the directory, even though it has no file content.
            fd = linux_dir_open(me, abs);
        }
        if (fd >= 0 && append) {
            const int64_t sz = linux_fd_size(me, static_cast<uint32_t>(fd));
            if (sz > 0) linux_fd_seek(me, static_cast<uint32_t>(fd), static_cast<uint64_t>(sz));
        }
        if (fd >= 0 && create && !pre_exists) kazakiri_notify(abs, IN_CREATE);
        frame[0] = (fd >= 0) ? static_cast<uint64_t>(fd) : static_cast<uint64_t>(-2); // -ENOENT
        return;
    }
    case 35 /*unlinkat*/: {
        // unlinkat(dirfd, path, flags): remove a file (dirfd ignored except
        // for AT_FDCWD-vs-real-dirfd path resolution).
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t path_va = frame[1];
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char abs[kCwdCap];
        if (!resolve_at(me, dirfd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        const bool was_dir = lateran_is_dir(abs); // capture before removal (IN_ISDIR)
        const bool ok = lateran_unlink(abs);
        if (ok) kazakiri_notify(abs, IN_DELETE | (was_dir ? IN_ISDIR : 0u));
        frame[0] = ok ? 0 : static_cast<uint64_t>(-2); // -ENOENT
        return;
    }
    case 34 /*mkdirat*/: {
        // mkdirat(dirfd, path, mode): create a directory (dirfd → cwd base).
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t path_va = frame[1];
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char abs[kCwdCap];
        if (!resolve_at(me, dirfd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        const bool mok = lateran_mkdir(abs);
        if (mok) kazakiri_notify(abs, IN_CREATE | IN_ISDIR);
        frame[0] = mok ? 0 : static_cast<uint64_t>(-17); // -EEXIST / failure
        return;
    }
    case 78 /*readlinkat*/: {
        // readlinkat(dirfd, path, buf, bufsiz): copy the symlink target into
        // `buf` (NOT NUL-terminated -- musl handles that). Returns the byte
        // count, or -EINVAL if the path isn't a symlink.
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t path_va = frame[1];
        const uint64_t buf_va = frame[2];
        const uint64_t cap = frame[3];
        if (!ensure_user(me, path_va, 1) || !ensure_user(me, buf_va, cap)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        char abs[kCwdCap];
        if (!resolve_at(me, dirfd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        // /proc/self/exe (and /proc/<pid>/exe for self) -> the running image's
        // absolute path. The musl dynamic loader readlink()s this to resolve
        // $ORIGIN in a binary's RUNPATH -- without it the OpenJDK launcher can't
        // locate libjli.so ($ORIGIN/../lib/aarch64/jli) and dies at startup.
        if (me != nullptr && me->exe_path[0] != 0) {
            bool is_exe = str_eq(abs, "/proc/self/exe");
            if (!is_exe && str_prefix(abs, "/proc/")) {
                const char *q = abs + 6;
                uint32_t pid = 0;
                while (*q >= '0' && *q <= '9') { pid = pid * 10 + (*q - '0'); ++q; }
                is_exe = (q != abs + 6 && pid == me->pid &&
                          q[0] == '/' && q[1] == 'e' && q[2] == 'x' && q[3] == 'e' && q[4] == 0);
            }
            if (is_exe) {
                uint32_t ln = 0; while (me->exe_path[ln] != 0) ++ln;
                const uint32_t w = (ln < cap) ? ln : static_cast<uint32_t>(cap);
                uint8_t *out = reinterpret_cast<uint8_t *>(buf_va);
                for (uint32_t i = 0; i < w; ++i) out[i] = static_cast<uint8_t>(me->exe_path[i]);
                frame[0] = w;
                return;
            }
        }
        // /proc/self/fd/<N> and /proc/<pid>/fd/<N> are symlinks to the open
        // file's path -- the Linux mechanism musl/glibc ttyname() uses to turn
        // a pty slave fd into "/dev/pts/N". Resolve the fd before Lateran.
        {
            const char *rest = nullptr;
            if (str_prefix(abs, "/proc/self/fd/")) {
                rest = abs + 14;
            } else if (str_prefix(abs, "/proc/")) {
                const char *q = abs + 6;
                uint32_t pid = 0;
                while (*q >= '0' && *q <= '9') { pid = pid * 10 + (*q - '0'); ++q; }
                if (q != abs + 6 && q[0] == '/' && q[1] == 'f' && q[2] == 'd' &&
                    q[3] == '/' && me != nullptr && pid == me->pid) {
                    rest = q + 4;
                }
            }
            if (rest != nullptr && *rest >= '0' && *rest <= '9') {
                int fdn = 0;
                const char *q = rest;
                while (*q >= '0' && *q <= '9') { fdn = fdn * 10 + (*q - '0'); ++q; }
                char fp[kCwdCap];
                if (*q == 0 && fd_link_target(me, fdn, fp, kCwdCap)) {
                    uint32_t ln = 0; while (fp[ln] != 0) ++ln;
                    const uint32_t w = (ln < cap) ? ln : static_cast<uint32_t>(cap);
                    uint8_t *out = reinterpret_cast<uint8_t *>(buf_va);
                    for (uint32_t i = 0; i < w; ++i) out[i] = static_cast<uint8_t>(fp[i]);
                    frame[0] = w;
                    return;
                }
                if (*q == 0) { frame[0] = static_cast<uint64_t>(-2); return; } // -ENOENT
            }
        }
        char tgt[256];
        const int32_t n = lateran_readlink(abs, tgt, sizeof(tgt));
        if (n < 0) { frame[0] = static_cast<uint64_t>(-22); return; } // -EINVAL
        const uint32_t w = (static_cast<uint32_t>(n) < cap) ? static_cast<uint32_t>(n)
                                                            : static_cast<uint32_t>(cap);
        uint8_t *out = reinterpret_cast<uint8_t *>(buf_va);
        for (uint32_t i = 0; i < w; ++i) out[i] = static_cast<uint8_t>(tgt[i]);
        frame[0] = w;
        return;
    }
    case 36 /*symlinkat*/: {
        // symlinkat(target, newdirfd, linkpath): create a symbolic link. The
        // target string is stored as-is (no resolution at create time).
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t target_va = frame[0];
        const int32_t dirfd = static_cast<int32_t>(frame[1]);
        const uint64_t path_va = frame[2];
        if (!ensure_user(me, target_va, 1) || !ensure_user(me, path_va, 1)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        char abs[kCwdCap];
        if (!resolve_at(me, dirfd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        const bool ok = lateran_symlink(reinterpret_cast<const char *>(target_va), abs);
        frame[0] = ok ? 0 : static_cast<uint64_t>(-17); // -EEXIST or unsupported
        return;
    }
    case 38 /*renameat*/:
    case 276 /*renameat2*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t old_dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t old_va = frame[1];
        const int32_t new_dirfd = static_cast<int32_t>(frame[2]);
        const uint64_t new_va = frame[3];
        if (!ensure_user(me, old_va, 1) || !ensure_user(me, new_va, 1)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        char old_abs[kCwdCap], new_abs[kCwdCap];
        if (!resolve_at(me, old_dirfd, reinterpret_cast<const char *>(old_va), old_abs, kCwdCap) ||
            !resolve_at(me, new_dirfd, reinterpret_cast<const char *>(new_va), new_abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        const bool rdir = lateran_is_dir(old_abs); // capture before the move (IN_ISDIR)
        const bool rok = lateran_rename(old_abs, new_abs);
        if (rok) {
            // MOVED_FROM + MOVED_TO carry a shared nonzero cookie so a watcher can
            // pair the two halves of a rename (Linux semantics).
            static uint32_t g_mv_cookie = 0;
            uint32_t cookie = __atomic_add_fetch(&g_mv_cookie, 1, __ATOMIC_RELAXED);
            if (cookie == 0) cookie = 1;
            const uint32_t dirbit = rdir ? IN_ISDIR : 0u;
            kazakiri_notify(old_abs, IN_MOVED_FROM | dirbit, cookie);
            kazakiri_notify(new_abs, IN_MOVED_TO | dirbit, cookie);
        }
        frame[0] = rok ? 0 : static_cast<uint64_t>(-2);
        return;
    }
    case 45 /*truncate*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t path_va = frame[0];
        const uint64_t new_size = frame[1];
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char abs[kCwdCap];
        if (!resolve_at(me, -100, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        frame[0] = lateran_truncate(abs, new_size) ? 0 : static_cast<uint64_t>(-2);
        return;
    }
    case 46 /*ftruncate*/: {
        if (me == nullptr || frame[0] >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const uint32_t fd = static_cast<uint32_t>(frame[0]);
        const uint64_t new_size = frame[1];
        if (!linux_fd_is_file(me, fd)) { frame[0] = static_cast<uint64_t>(-9); return; }
        frame[0] = lateran_truncate(linux_fd_path(me, fd), new_size) ? 0 : static_cast<uint64_t>(-2);
        return;
    }
    case 52 /*fchmod*/: {
        if (me == nullptr || frame[0] >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const uint32_t fd = static_cast<uint32_t>(frame[0]);
        const uint32_t mode = static_cast<uint32_t>(frame[1]);
        if (!linux_fd_is_file(me, fd)) { frame[0] = static_cast<uint64_t>(-9); return; }
        frame[0] = lateran_chmod(linux_fd_path(me, fd), mode) ? 0 : static_cast<uint64_t>(-2);
        return;
    }
    case 53 /*fchmodat*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t path_va = frame[1];
        const uint32_t mode = static_cast<uint32_t>(frame[2]);
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char abs[kCwdCap];
        if (!resolve_at(me, dirfd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        // Dynamic pty char devices have no ext2 inode; sshd's pty_setowner
        // chmod()s /dev/pts/N -- accept it as a no-op (the SisterRelay owns perms).
        if (str_prefix(abs, "/dev/pts/") || str_eq(abs, "/dev/ptmx")) { frame[0] = 0; return; }
        frame[0] = lateran_chmod(abs, mode) ? 0 : static_cast<uint64_t>(-2);
        return;
    }
    case 55 /*fchown*/: {
        if (me == nullptr || frame[0] >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const uint32_t fd = static_cast<uint32_t>(frame[0]);
        const uint32_t uid = static_cast<uint32_t>(frame[1]);
        const uint32_t gid = static_cast<uint32_t>(frame[2]);
        if (!linux_fd_is_file(me, fd)) { frame[0] = static_cast<uint64_t>(-9); return; }
        frame[0] = lateran_chown(linux_fd_path(me, fd), uid, gid) ? 0 : static_cast<uint64_t>(-2);
        return;
    }
    case 54 /*fchownat*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t path_va = frame[1];
        const uint32_t uid = static_cast<uint32_t>(frame[2]);
        const uint32_t gid = static_cast<uint32_t>(frame[3]);
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char abs[kCwdCap];
        if (!resolve_at(me, dirfd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        if (str_prefix(abs, "/dev/pts/") || str_eq(abs, "/dev/ptmx")) { frame[0] = 0; return; }
        frame[0] = lateran_chown(abs, uid, gid) ? 0 : static_cast<uint64_t>(-2);
        return;
    }
    case 37 /*linkat*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t old_dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t old_va = frame[1];
        const int32_t new_dirfd = static_cast<int32_t>(frame[2]);
        const uint64_t new_va = frame[3];
        if (!ensure_user(me, old_va, 1) || !ensure_user(me, new_va, 1)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        char old_abs[kCwdCap], new_abs[kCwdCap];
        if (!resolve_at(me, old_dirfd, reinterpret_cast<const char *>(old_va), old_abs, kCwdCap) ||
            !resolve_at(me, new_dirfd, reinterpret_cast<const char *>(new_va), new_abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        frame[0] = lateran_link(old_abs, new_abs) ? 0 : static_cast<uint64_t>(-2);
        return;
    }
    case 81 /*sync*/:
    case 82 /*fsync*/:
    case 83 /*fdatasync*/:
        // QEMU is mounted cache=directsync, so guest writes already hit the
        // host image atomically; nothing buffered to flush. Honest answer is 0.
        frame[0] = 0;
        return;
    case 88 /*utimensat*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t path_va = frame[1];
        const uint64_t times_va = frame[2];
        // path NULL means "the fd is the file" (Linux extension). We don't
        // support that without an fd-only path -- emulate by reading the fd's
        // stored path.
        char abs[kCwdCap];
        if (path_va != 0) {
            if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
            if (!resolve_at(me, dirfd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
                frame[0] = static_cast<uint64_t>(-2); return;
            }
        } else {
            if (dirfd < 0 || static_cast<uint32_t>(dirfd) >= kMaxFds ||
                !linux_fd_is_file(me, static_cast<uint32_t>(dirfd))) {
                frame[0] = static_cast<uint64_t>(-9); return;
            }
            const char *fp = linux_fd_path(me, static_cast<uint32_t>(dirfd));
            uint32_t i = 0;
            for (; fp[i] && i + 1 < kCwdCap; ++i) abs[i] = fp[i];
            abs[i] = 0;
        }
        // times[2] = { atime, mtime }, each timespec{tv_sec, tv_nsec}. NULL
        // means UTIME_NOW for both. Special tv_nsec values: UTIME_NOW
        // (1073741823), UTIME_OMIT (1073741822).
        int64_t atime = -1, mtime = -1;
        const uint32_t kUtimeNow = 0x3fffffffu;
        const uint32_t kUtimeOmit = 0x3ffffffeu;
        const uint32_t now = static_cast<uint32_t>(idol_theory_epoch_seconds());
        if (times_va == 0) {
            atime = now; mtime = now;
        } else if (ensure_user(me, times_va, 32)) {
            const int64_t *ts = reinterpret_cast<const int64_t *>(times_va);
            const uint32_t a_ns = static_cast<uint32_t>(ts[1]);
            const uint32_t m_ns = static_cast<uint32_t>(ts[3]);
            if (a_ns == kUtimeNow) atime = now;
            else if (a_ns != kUtimeOmit) atime = ts[0];
            if (m_ns == kUtimeNow) mtime = now;
            else if (m_ns != kUtimeOmit) mtime = ts[2];
        }
        frame[0] = lateran_utime(abs, atime, mtime) ? 0 : static_cast<uint64_t>(-2);
        return;
    }
    case 48 /*faccessat*/:
    case 439 /*faccessat2*/: {
        // faccessat(dirfd, path, mode, flags). We don't track per-process
        // permission bits properly yet, so the truthful test is just "does
        // the file exist?" -- map any mode check to existence.
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t dirfd = static_cast<int32_t>(frame[0]);
        const uint64_t path_va = frame[1];
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char abs[kCwdCap];
        if (!resolve_at(me, dirfd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        LateranEntry meta = {};
        if (lateran_stat(abs, &meta) || lateran_is_dir(abs)) {
            frame[0] = 0;
        } else {
            const int64_t sz = linux_file_size(abs);
            frame[0] = (sz >= 0) ? 0 : static_cast<uint64_t>(-2);
        }
        return;
    }
    case 61 /*getdents64*/: {
        // getdents64(fd, dirp, count): list the directory the fd was opened on.
        // We don't keep a real dir fd, so we list by the fd's stored path; the
        // fd's offset doubles as a "已经返回到第几个条目" cursor so repeated
        // calls eventually return 0 (end). struct linux_dirent64 is
        // { u64 d_ino; i64 d_off; u16 d_reclen; u8 d_type; char d_name[]; }.
        if (me == nullptr || frame[0] >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const uint32_t fd = static_cast<uint32_t>(frame[0]);
        const uint64_t dirp = frame[1];
        const uint64_t cap = frame[2];
        if (!linux_fd_is_file(me, fd)) { frame[0] = static_cast<uint64_t>(-20); return; } // -ENOTDIR
        if (!ensure_user(me, dirp, cap)) { frame[0] = static_cast<uint64_t>(-14); return; }
        // Per-CPU scratch row (race-free vs a concurrent getdents64 on another
        // CPU; EL1 syscalls don't migrate, so this_cpu_id() is stable here).
        const uint32_t cpu = (arch::this_cpu_id() < kGetdentsScratchCpus)
                                 ? arch::this_cpu_id() : 0;
        LateranEntry *dents = g_dirents[cpu];
        const uint32_t total = lateran_list_dir(linux_fd_path(me, fd), dents, 256);
        const uint64_t start = linux_fd_tell(me, fd); // resume cursor
        uint8_t *out = reinterpret_cast<uint8_t *>(dirp);
        uint64_t used = 0;
        uint32_t i = static_cast<uint32_t>(start);
        for (; i < total; ++i) {
            uint32_t nlen = 0;
            while (dents[i].name[nlen]) ++nlen;
            const uint64_t reclen = (19 + nlen + 1 + 7) & ~uint64_t(7); // align 8
            if (used + reclen > cap) break;
            uint8_t *d = out + used;
            for (uint32_t b = 0; b < reclen; ++b) d[b] = 0;
            *reinterpret_cast<uint64_t *>(d + 0) = dents[i].first_cluster + 2; // d_ino
            *reinterpret_cast<int64_t *>(d + 8) = static_cast<int64_t>(i + 1); // d_off
            *reinterpret_cast<uint16_t *>(d + 16) = static_cast<uint16_t>(reclen);
            d[18] = dents[i].is_dir ? 4 /*DT_DIR*/ : 8 /*DT_REG*/;
            for (uint32_t b = 0; b < nlen; ++b) d[19 + b] = static_cast<uint8_t>(dents[i].name[b]);
            used += reclen;
        }
        linux_fd_seek(me, fd, i); // advance cursor past what we returned
        frame[0] = used;
        return;
    }
    case 57 /*close*/:
        if (me != nullptr) {
            linux_fd_close(me, static_cast<uint32_t>(frame[0]));
        }
        frame[0] = 0;
        return;

    // ---- memory -------------------------------------------------------
    case 214 /*brk*/: {
        const uint64_t req = frame[0];
        if (me == nullptr || me->mm == nullptr) { frame[0] = 0; return; }
        PersonalReality *mm = me->mm; // brk lives in the shared address space
        if (req == 0) {
            frame[0] = mm->brk_cur; // query
            return;
        }
        if (req >= mm->brk_start && req <= mm->brk_start + 0x4000000ULL) {
            mm->brk_cur = req; // pages fault in (zero) on demand
            frame[0] = mm->brk_cur;
        } else {
            frame[0] = mm->brk_cur; // refuse out-of-range; return unchanged break
        }
        return;
    }
    case 222 /*mmap*/: {
        // mmap(addr, len, prot, flags, fd, off). Anonymous (MAP_ANONYMOUS=0x20)
        // bumps a fresh VMA out of the per-process mmap region; file-backed
        // (MAP_PRIVATE) snapshots the file into a resident buffer and backs
        // the VMA with it. MAP_FIXED (0x10) honours the caller's `addr`
        // verbatim and displaces any overlapping mapping -- ld-musl's PT_LOAD
        // layout (wide R|X cover, then RW MAP_FIXED on top for .data/.bss)
        // doesn't work without this.
        const uint64_t addr = frame[0];
        const uint64_t len = frame[1];
        const uint64_t prot = frame[2];
        const uint64_t flags = frame[3];
        const uint64_t fd = frame[4];
        const uint64_t offset = frame[5];
        if (me == nullptr || me->mm == nullptr || len == 0) { frame[0] = static_cast<uint64_t>(-12); return; }
        const uint64_t alen = (len + 0xFFF) & ~uint64_t(0xFFF);
        // Bound the length: the page-round must not wrap (alen < len), and the
        // mapping must fit under the 39-bit EL0 ceiling. Mainstream mmap caps
        // length to TASK_SIZE; without this a len near 2^64 wraps alen to 0 /
        // garbage and pr2_add_vma's own (end+mask) round then stores a bogus or
        // inverted VMA. (#3-class integer overflow on a user-controlled length.)
        constexpr uint64_t kUserCeiling = 0x8000000000ULL;
        if (alen == 0 || alen < len || alen > kUserCeiling) {
            frame[0] = static_cast<uint64_t>(-12); return; // -ENOMEM
        }
        const bool fixed = (flags & 0x10) != 0;
        uint64_t at;
        if (fixed) {
            if (addr == 0) { frame[0] = static_cast<uint64_t>(-22); return; } // -EINVAL
            at = addr & ~uint64_t(0xFFF);
        } else if (addr != 0) {
            // Honor a non-NULL hint when that range is free (Linux mmap places a
            // mapping at the hint if possible). HotSpot reserves its heap at a
            // computed narrow-oop base via a hint; a bump-only allocator that
            // ignores it makes HotSpot retry across the whole address space and
            // eventually MAP_FIXED over the dynamic linker (gap 10). Fall back to
            // the bump pointer if the hint range is already taken.
            const uint64_t h = addr & ~uint64_t(0xFFF);
            // Don't honor a low hint (Linux mmap_min_addr): placing a mapping
            // at/near 0 would defeat NULL-deref protection. Fall back to the
            // bump pointer for low or already-taken hints.
            constexpr uint64_t kMmapMinAddr = 0x10000; // 64 KiB
            at = (h >= kMmapMinAddr && h <= kUserCeiling - alen && pr2_range_free(me, h, h + alen))
                     ? h : me->mm->mmap_next;
        } else {
            at = me->mm->mmap_next;
        }
        if (at > kUserCeiling - alen) { // at + alen would overflow / exceed the ceiling
            frame[0] = static_cast<uint64_t>(-12); return; // -ENOMEM
        }
        if (fixed) {
            pr2_remove_vma_range(me, at, at + alen);
        }
        uint8_t vprot = kVmaProtR;
        if (prot & 0x2) vprot |= kVmaProtW; // PROT_WRITE
        if (prot & 0x4) vprot |= kVmaProtX; // PROT_EXEC
        bool ok;
        if (flags & 0x20 /*MAP_ANONYMOUS*/) {
            ok = pr2_add_vma(me, at, at + alen, vprot,
                             static_cast<uint8_t>(VmaKind::Anon), nullptr, 0, 0, 0);
        } else if (linux_fd_is_file(me, static_cast<uint32_t>(fd))) {
            // Demand-paged file mmap: record path + offset and fault pages in from
            // the filesystem on first touch, instead of slurping the whole file
            // into the kernel heap (rt.jar is 33 MB; the old whole-file buffer
            // OOM'd the 64 MB heap and was capped at 8 buffers). file_src=nullptr
            // + file_path marks the VMA demand-paged (see pr2_handle_fault).
            const char *fpath = me->fds[static_cast<uint32_t>(fd)].path;
            const int64_t fsz = linux_file_size(fpath);
            if (fsz < 0) { frame[0] = static_cast<uint64_t>(-12); return; }
            const uint64_t avail = (offset < static_cast<uint64_t>(fsz))
                                       ? (static_cast<uint64_t>(fsz) - offset) : 0;
            ok = pr2_add_vma(me, at, at + alen, vprot,
                             static_cast<uint8_t>(VmaKind::File), nullptr, offset, avail, at, fpath);
        } else {
            frame[0] = static_cast<uint64_t>(-9); // -EBADF
            return;
        }
        if (!ok) {
            frame[0] = static_cast<uint64_t>(-12); // -ENOMEM (VMA table full)
            return;
        }
        // Bump the pointer past this mapping. With a honored hint `at` may be
        // above the old mmap_next, so advance to at+alen (not just +=alen) to
        // keep later anonymous mmaps from colliding with the hinted placement.
        if (!fixed && at + alen > me->mm->mmap_next) {
            me->mm->mmap_next = at + alen;
        }
        frame[0] = at;
        return;
    }
    case 215 /*munmap*/: {
        // munmap(addr, len): tear down the VMA(s) covering this range AND
        // free the leaf pages they backed. Was a silent no-op (bump-allocator
        // grew mmap_next monotonically + VMA table filled at kMaxVmas=32) --
        // exhausted with ~20 shell commands because busybox malloc rounds
        // every alloc to a fresh mmap. pr2_remove_vma_range handles both
        // the VMA table compaction and the page_unref for any leaf pages
        // that were faulted in.
        if (me == nullptr) { frame[0] = 0; return; }
        const uint64_t addr = frame[0];
        const uint64_t len = frame[1];
        const uint64_t alen = (len + 0xFFF) & ~uint64_t(0xFFF);
        if (addr == 0 || alen == 0) { frame[0] = 0; return; }
        pr2_remove_vma_range(me, addr, addr + alen);
        frame[0] = 0;
        return;
    }
    case 226 /*mprotect*/: {
        // mprotect(addr, len, prot): actually apply it. musl maps a thread
        // stack and then mprotects it writable; ignoring that loops forever.
        const uint64_t addr = frame[0];
        const uint64_t len = frame[1];
        const uint64_t prot = frame[2];
        uint8_t vprot = 0;
        if (prot & 0x1) vprot |= kVmaProtR; // PROT_READ
        if (prot & 0x2) vprot |= kVmaProtW | kVmaProtR; // PROT_WRITE (imply R)
        if (prot & 0x4) vprot |= kVmaProtX; // PROT_EXEC
        if (me != nullptr) {
            pr2_mprotect(me, addr, len, vprot);
        }
        frame[0] = 0;
        return;
    }

    // ---- stat (minimal; musl checks stdout to pick buffering) ---------
    case 80 /*fstat*/:
    case 79 /*newfstatat*/: {
        // struct stat is 128 bytes on aarch64. fds 0/1/2 report as a character
        // device (tty); an open file fd reports S_IFREG/S_IFDIR with its real
        // size at st_size (offset 48). newfstatat(dirfd, path, statbuf, flags)
        // resolves a path through Lateran when the empty-path flag is unset.
        const uint64_t fd = frame[0];
        uint64_t st_va = (nr == 80) ? frame[1] : frame[2];
        bool by_path = false;
        char abs[kCwdCap];
        if (nr == 79) {
            const uint64_t path_va = frame[1];
            const uint64_t flags = frame[3];
            st_va = frame[2];
            // AT_EMPTY_PATH (0x1000): stat the dirfd itself; else resolve path.
            if (!(flags & 0x1000) && path_va != 0) {
                if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
                const char *path = reinterpret_cast<const char *>(path_va);
                if (path[0] != 0) {
                    if (!resolve_at(me, static_cast<int32_t>(fd), path, abs, kCwdCap)) {
                        frame[0] = static_cast<uint64_t>(-2); return;
                    }
                    by_path = true;
                }
            }
        }
        if (!ensure_user(me, st_va, 128)) { frame[0] = static_cast<uint64_t>(-14); return; }
        uint8_t *st = reinterpret_cast<uint8_t *>(st_va);
        for (uint32_t i = 0; i < 128; ++i) st[i] = 0;
        const uint32_t S_IFCHR = 0020000, S_IFDIR = 0040000, S_IFREG = 0100000;
        uint64_t *st_dev = reinterpret_cast<uint64_t *>(st + 0);
        uint64_t *st_ino = reinterpret_cast<uint64_t *>(st + 8);
        uint32_t *mode = reinterpret_cast<uint32_t *>(st + 16);
        int64_t *size = reinterpret_cast<int64_t *>(st + 48);
        int64_t *st_atime = reinterpret_cast<int64_t *>(st + 72);
        int64_t *st_mtime = reinterpret_cast<int64_t *>(st + 88);
        int64_t *st_ctime = reinterpret_cast<int64_t *>(st + 104);
        uint32_t *st_uid = reinterpret_cast<uint32_t *>(st + 24);
        uint32_t *st_gid = reinterpret_cast<uint32_t *>(st + 28);
        uint32_t *st_nlink = reinterpret_cast<uint32_t *>(st + 20);
        *st_nlink = 1; // baseline; ext2_stat could override
        // Single virtual device for Lateran. Non-zero so musl ld's dlopen
        // dedup (which keys on (st_dev, st_ino)) doesn't collapse distinct
        // files to the same identity.
        *st_dev = 1;
        if (by_path) {
            // Dynamic char devices with no ext2 inode: /dev/pts/<N> + /dev/ptmx.
            // ttyname() stats the path /proc/self/fd gave it and matches st_rdev
            // against fstat(fd) -- synthesize a char device with the SAME rdev
            // (Linux pts major 136, ptmx 5:2).
            if (str_prefix(abs, "/dev/pts/")) {
                int n = 0; const char *p = abs + 9; bool digit = (*p >= '0' && *p <= '9');
                while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); ++p; }
                if (digit && *p == 0) {
                    *mode = S_IFCHR | 0620;
                    *reinterpret_cast<uint64_t *>(st + 32) =
                        (136u << 8) | (static_cast<uint32_t>(n) & 0xffu);
                    frame[0] = 0; return;
                }
            }
            if (str_eq(abs, "/dev/ptmx")) {
                *mode = S_IFCHR | 0666;
                *reinterpret_cast<uint64_t *>(st + 32) = (5u << 8) | 2u;
                frame[0] = 0; return;
            }
            LateranEntry meta = {};
            if (lateran_stat(abs, &meta)) {
                *mode = meta.mode != 0 ? meta.mode : (meta.is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
                *size = meta.is_dir ? 0 : meta.size;
                *st_uid = meta.uid;
                *st_gid = meta.gid;
                *st_nlink = meta.nlink;
                *st_atime = meta.atime;
                *st_mtime = meta.mtime;
                *st_ctime = meta.ctime;
                *st_ino = meta.ino;
            } else if (lateran_is_dir(abs)) {
                *mode = S_IFDIR | 0755;
                *size = 0;
            } else {
                const int64_t sz = linux_file_size(abs);
                if (sz < 0) { frame[0] = static_cast<uint64_t>(-2); return; }
                *mode = S_IFREG | 0644;
                *size = sz;
            }
        } else if (fd <= 2) {
            *mode = S_IFCHR | 0644;
        } else if (me != nullptr && fd < kMaxFds &&
                   (me->fds[fd].kind == FdKind::devnull ||
                    me->fds[fd].kind == FdKind::devzero ||
                    me->fds[fd].kind == FdKind::devrandom ||
                    me->fds[fd].kind == FdKind::devtty)) {
            *mode = S_IFCHR | 0666;
        } else if (linux_fd_is_file(me, static_cast<uint32_t>(fd))) {
            const char *fp = linux_fd_path(me, static_cast<uint32_t>(fd));
            if (fp[0] != 0 && lateran_is_dir(fp)) {
                *mode = S_IFDIR | 0755;
                *size = 0;
            } else {
                // Same lateran_stat call the path branch uses, so st_ino is
                // a real per-file value when ext2 has it.
                LateranEntry meta = {};
                if (fp[0] != 0 && lateran_stat(fp, &meta)) {
                    *mode = meta.mode != 0 ? meta.mode : (S_IFREG | 0644);
                    *size = meta.is_dir ? 0 : meta.size;
                    *st_uid = meta.uid;
                    *st_gid = meta.gid;
                    *st_nlink = meta.nlink;
                    *st_atime = meta.atime;
                    *st_mtime = meta.mtime;
                    *st_ctime = meta.ctime;
                    *st_ino = meta.ino;
                } else {
                    *mode = S_IFREG | 0644;
                    const int64_t sz = linux_fd_size(me, static_cast<uint32_t>(fd));
                    *size = (sz > 0) ? sz : 0;
                }
            }
        } else if (me != nullptr && fd < kMaxFds &&
                   (me->fds[fd].kind == FdKind::pty_slave ||
                    me->fds[fd].kind == FdKind::pty_master)) {
            // pty endpoints are char devices. st_rdev must equal what the path
            // stat of /dev/pts/N (pts major 136) or /dev/ptmx (5:2) reports, so
            // musl ttyname()'s rdev comparison passes.
            *mode = S_IFCHR | 0620;
            if (me->fds[fd].kind == FdKind::pty_slave) {
                const uint32_t n = static_cast<uint32_t>(me->fds[fd].sock_idx) & 0xffu;
                *reinterpret_cast<uint64_t *>(st + 32) = (136u << 8) | n;
            } else {
                *reinterpret_cast<uint64_t *>(st + 32) = (5u << 8) | 2u;
            }
        } else {
            *mode = S_IFREG | 0644;
        }
        frame[0] = 0;
        return;
    }

    case 291 /*statx*/:
        // statx: the JDK's NIO prefers it but falls back to newfstatat(79) on
        // -ENOSYS -- and javac works fine via that fallback (verified end-to-end:
        // the triple-indirect build below compiled + ran a class). A full statx
        // that returned SUCCESS changed the JDK's file-attribute path and
        // *deterministically* tripped a separate crash in the EL0 context-restore
        // (FAR=user garbage), so we deliberately stay on the proven fallback and
        // answer -ENOSYS *quietly*: the explicit case suppresses the per-call
        // unknown-syscall console spam javac produced by the hundred. Mirrors a
        // kernel built without statx; revisit a real impl with careful debugging.
        frame[0] = static_cast<uint64_t>(-38); // -ENOSYS
        return;

    // ---- process / thread identity & signals (mostly no-ops) ----------
    case 96 /*set_tid_address*/:
        // Store the clear_child_tid pointer; the kernel zeroes + futex-wakes it
        // on this Esper's exit (pthread_join rendezvous). Returns the tid.
        if (me != nullptr) {
            me->clear_child_tid = frame[0];
            frame[0] = me->pid;
        } else {
            frame[0] = 1;
        }
        return;
    case 99 /*set_robust_list*/:
        frame[0] = 0;
        return;
    case 134 /*rt_sigaction*/: {
        // rt_sigaction(sig, const struct sigaction* act, struct sigaction* old,
        // size_t setsize). Kernel struct sigaction: {handler@0, flags@8,
        // restorer@16, mask@24}. We record handler/restorer/flags per signal.
        const uint64_t sig = frame[0];
        const uint64_t act_va = frame[1];
        const uint64_t old_va = frame[2];
        if (me == nullptr || sig == 0 || sig >= 64) {
            frame[0] = static_cast<uint64_t>(-22); // -EINVAL
            return;
        }
        if (old_va != 0 && ensure_user(me, old_va, 32)) {
            uint64_t *o = reinterpret_cast<uint64_t *>(old_va);
            o[0] = me->sig_handler[sig];
            o[1] = me->sig_flags[sig];
            o[2] = me->sig_restorer[sig];
            o[3] = 0;
        }
        if (act_va != 0 && ensure_user(me, act_va, 32)) {
            const uint64_t *a = reinterpret_cast<const uint64_t *>(act_va);
            me->sig_handler[sig] = a[0];
            me->sig_flags[sig] = a[1];
            me->sig_restorer[sig] = a[2];
            // sa_mask is at offset 24 in struct sigaction. Only the low 64
            // signals are tracked; SIGKILL/SIGSTOP are silently stripped.
            me->sig_act_mask[sig] = a[3] & ~kNonmaskable;
        }
        frame[0] = 0;
        return;
    }
    case 135 /*rt_sigprocmask*/: {
        // rt_sigprocmask(how, *set, *oldset, sigsetsize). On aarch64 sigset_t
        // is 128 bytes / 1024 bits; we track only the low 64 (the rest stays
        // zeroed). how: 0=SIG_BLOCK, 1=SIG_UNBLOCK, 2=SIG_SETMASK.
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-22); return; }
        const uint64_t how = frame[0];
        const uint64_t set_va = frame[1];
        const uint64_t old_va = frame[2];
        if (old_va != 0 && ensure_user(me, old_va, 8)) {
            *reinterpret_cast<uint64_t *>(old_va) = me->sig_mask;
        }
        if (set_va != 0 && ensure_user(me, set_va, 8)) {
            const uint64_t newset = *reinterpret_cast<const uint64_t *>(set_va) & ~kNonmaskable;
            switch (how) {
            case 0 /*SIG_BLOCK*/:   me->sig_mask |= newset; break;
            case 1 /*SIG_UNBLOCK*/: me->sig_mask &= ~newset; break;
            case 2 /*SIG_SETMASK*/: me->sig_mask = newset; break;
            default: frame[0] = static_cast<uint64_t>(-22); return;
            }
            // Mask just dropped -- deliver any pending signals it was blocking.
            uint64_t deliverable = me->sig_pending & ~me->sig_mask;
            while (deliverable) {
                // Find the lowest set bit (lowest signal number).
                int sig = 1;
                uint64_t d = deliverable;
                while (!(d & 1)) { d >>= 1; ++sig; }
                const uint64_t bit = 1ULL << (sig - 1);
                if (linux_deliver_signal(me, sig, frame)) {
                    // Handler entered (frame built): consume the pending bit now.
                    // Subsequent pending signals deliver after rt_sigreturn.
                    me->sig_pending &= ~bit;
                    return;
                }
                // Delivery returned false. Distinguish two cases -- the old code
                // pre-cleared the bit and on SIGCHLD just continued, so a TRANSIENT
                // frame-build failure (pr2_write_user couldn't fault in the user
                // stack page under SMP) silently DROPPED SIGCHLD -> sshd never
                // reaped the exited shell -> ssh logout hung. Only consume the bit
                // for a genuine SIG_DFL/SIG_IGN; for an installed handler, leave it
                // pending so it is redelivered on the next signal-check / return.
                const uint64_t h = me->sig_handler[sig];
                if (h != 0 /*SIG_DFL*/ && h != 1 /*SIG_IGN*/) {
                    break; // installed handler, build failed transiently -> keep pending
                }
                me->sig_pending &= ~bit; // genuine default/ignore disposition
                const int idx = esper_running_index();
                if (idx >= 0 && sig != 17 /*SIGCHLD ignored by default*/) {
                    linux_exit_running(idx, 128 + sig, frame);
                    return;
                }
                deliverable = me->sig_pending & ~me->sig_mask;
            }
        }
        frame[0] = 0;
        return;
    }
    case 136 /*rt_sigpending*/: {
        // rt_sigpending(*set, sigsetsize): copy the bitmask of signals raised
        // while blocked. We track 64; the wire-format sigset is 128 bytes so
        // we write 8 bytes then zero the rest (already 0 from frame init).
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-22); return; }
        const uint64_t set_va = frame[0];
        const uint64_t sz = frame[1];
        if (set_va != 0 && ensure_user(me, set_va, sz < 128 ? sz : 128)) {
            uint8_t *p = reinterpret_cast<uint8_t *>(set_va);
            const uint64_t pend = me->sig_pending;
            for (uint32_t i = 0; i < (sz < 128 ? sz : 128); ++i) p[i] = 0;
            *reinterpret_cast<uint64_t *>(p) = pend;
        }
        frame[0] = 0;
        return;
    }
    case 139 /*rt_sigreturn*/: {
        // Restore the EL0 context saved in the rt_sigframe at sp_el0. Layout:
        // sp -> rt_sigframe{ siginfo(128); ucontext }. uc_sigmask is at
        // sp+128+40 (which we restore to revert mask changes from delivery);
        // uc_mcontext at sp+128+168 holds the saved regs/sp/pc/pstate.
        uint64_t sp = 0;
        asm volatile("mrs %0, sp_el0" : "=r"(sp));
        const uint64_t uc = sp + 128;
        const uint64_t mc = uc + 168;
        if (!ensure_user(me, mc, 280)) {
            frame[0] = static_cast<uint64_t>(-14);
            return;
        }
        // Restore mask first, then registers. This way if a still-pending
        // signal becomes deliverable post-restore the loop below can chain.
        if (me != nullptr) {
            const uint64_t saved_mask = *reinterpret_cast<const uint64_t *>(uc + 40);
            me->sig_mask = saved_mask & ~kNonmaskable;
        }
        const uint64_t *m = reinterpret_cast<const uint64_t *>(mc);
        // m[0] = fault_address; m[1..31] = x0..x30; m[32] = sp; m[33] = pc;
        // m[34] = pstate.
        for (uint32_t i = 0; i < 31; ++i) {
            frame[i] = m[1 + i];
        }
        const uint64_t user_sp = m[32];
        const uint64_t user_pc = m[33];
        const uint64_t user_pstate = m[34];
        // SECURITY: the rt_sigframe lives on the user stack and is fully
        // attacker-controllable. Restoring spsr_el1 verbatim would let EL0 set the
        // PSTATE mode bits to EL1 -- on the eret out of this syscall the CPU would
        // then run the (also attacker-chosen) PC at EL1 = full EL0->EL1 privilege
        // escalation (classic sigreturn / SROP). It would also let EL0 set DAIF to
        // mask IRQs and run un-preemptibly (scheduler-starving DoS). Sanitize like
        // Linux valid_user_regs(): keep only the user-owned condition flags (NZCV)
        // and force every other bit to the canonical EL0 PSTATE (kSpsrEl0 = EL0t,
        // IRQ-enabled/preemptible, AArch64). NZCV must survive so the interrupted
        // code resumes with its arithmetic flags (POSIX).
        constexpr uint64_t kNzcvMask = 0xF0000000ULL; // N,Z,C,V (bits 31..28)
        const uint64_t safe_pstate = (user_pstate & kNzcvMask) |
                                     (kSpsrEl0Handler & ~kNzcvMask); // 0x340 = EL0t, preemptible
        asm volatile("msr sp_el0, %0" ::"r"(user_sp));
        asm volatile("msr elr_el1, %0" ::"r"(user_pc));
        asm volatile("msr spsr_el1, %0" ::"r"(safe_pstate));
        // If the mask dropped enough that a pending signal is now deliverable,
        // chain-deliver one -- otherwise the handler we just returned from
        // could leave queued signals stranded forever.
        if (me != nullptr) {
            uint64_t deliverable = me->sig_pending & ~me->sig_mask;
            if (deliverable) {
                int sig = 1;
                while (!(deliverable & 1)) { deliverable >>= 1; ++sig; }
                const uint64_t bit = 1ULL << (sig - 1);
                if (linux_deliver_signal(me, sig, frame)) {
                    me->sig_pending &= ~bit; // chained handler entered -> consume
                } else if (me->sig_handler[sig] == 0 || me->sig_handler[sig] == 1) {
                    me->sig_pending &= ~bit; // genuine SIG_DFL/SIG_IGN
                }
                // else: handler installed but frame build failed transiently ->
                // keep pending so it is redelivered (never silently drop it).
            }
        }
        // frame[0] (x0) was restored from the saved context above, so the
        // interrupted code resumes with its original x0.
        return;
    }
    case 129 /*kill*/:    // kill(pid, sig)
    case 130 /*tkill*/:   // tkill(tid, sig)
    case 131 /*tgkill*/: {// tgkill(tgid, tid, sig)
        // Normalise to (tid, sig): tgkill has an extra leading tgid arg.
        const uint64_t tid = (nr == 131) ? frame[1] : frame[0];
        const uint64_t sig = (nr == 131) ? frame[2] : frame[1];
        // Self-signal (e.g. raise()): the live trap frame is the interrupted
        // context, so deliver directly with it -- the handler runs on eret.
        if (me != nullptr && me->pid == tid) {
            frame[0] = 0; // tgkill "succeeds"; saved x0 in the sigframe is 0
            if (sig != 0 && !linux_deliver_signal(me, static_cast<int>(sig), frame)) {
                // No handler: default action. For most signals that's terminate.
                const int idx = esper_running_index();
                if (idx >= 0) {
                    linux_exit_running(idx, 128 + static_cast<int64_t>(sig), frame);
                }
            }
            return;
        }
        int target = -1;
        for (uint32_t i = 0; i < kMaxEspers; ++i) {
            Esper *c = esper_at(i);
            if (c != nullptr && c->pid == tid && c->state != EsperState::free &&
                c->state != EsperState::exited && c->state != EsperState::faulted) {
                target = static_cast<int>(i);
                break;
            }
        }
        frame[0] = (target >= 0 && fortis931_kill(target, static_cast<int>(sig)))
                       ? 0 : static_cast<uint64_t>(-3); // -ESRCH
        return;
    }
    case 172 /*getpid*/:
        frame[0] = me != nullptr ? me->pid : 1;
        return;
    case 173 /*getppid*/:
        frame[0] = (me != nullptr && me->parent >= 0)
                       ? esper_at(static_cast<uint32_t>(me->parent))->pid
                       : 1;
        return;
    case 178 /*gettid*/:
        frame[0] = me != nullptr ? me->pid : 1;
        return;
    case 179 /*sysinfo*/: {
        // sysinfo(struct sysinfo *info). aarch64 layout: long uptime; ulong
        // loads[3]; ulong totalram/freeram/sharedram/bufferram/totalswap/
        // freeswap; u16 procs; u16 pad; (4 bytes implicit pad to align);
        // ulong totalhigh/freehigh; u32 mem_unit; char _f[0]. Total 112 bytes.
        const uint64_t st_va = frame[0];
        if (st_va == 0 || !ensure_user(me, st_va, 112)) {
            frame[0] = static_cast<uint64_t>(-14); // -EFAULT
            return;
        }
        uint8_t *st = reinterpret_cast<uint8_t *>(st_va);
        for (uint32_t i = 0; i < 112; ++i) st[i] = 0;
        // Uptime: LastOrder ticks (100 Hz) / 100.
        const uint64_t up_seconds = last_order_ticks() / 100;
        *reinterpret_cast<int64_t *>(st + 0) = static_cast<int64_t>(up_seconds);
        // Loads: Index doesn't compute load averages; leave the three slots 0.
        // Memory: TreeDiagram bookkeeping is in 4 KiB units; mem_unit reports
        // that so userland can convert back to bytes.
        const uint64_t total_pages = tree_diagram_total_pages();
        const uint64_t used_pages = tree_diagram_used_pages();
        const uint64_t free_pages = total_pages > used_pages ? total_pages - used_pages : 0;
        *reinterpret_cast<uint64_t *>(st + 32) = total_pages; // totalram
        *reinterpret_cast<uint64_t *>(st + 40) = free_pages;  // freeram
        // sharedram (48), bufferram (56), totalswap (64), freeswap (72) stay 0.
        // procs: non-free Esper slots.
        uint16_t procs = 0;
        for (uint32_t i = 0; i < kMaxEspers; ++i) {
            Esper *e = esper_at(i);
            if (e != nullptr && e->state != EsperState::free) ++procs;
        }
        *reinterpret_cast<uint16_t *>(st + 80) = procs;
        // pad (82), implicit pad (84-87), totalhigh (88), freehigh (96) all 0.
        *reinterpret_cast<uint32_t *>(st + 104) = static_cast<uint32_t>(kTreeDiagramPageSize);
        frame[0] = 0;
        return;
    }
    case 174 /*getuid*/:
        frame[0] = me != nullptr ? me->uid : 0;
        return;
    case 175 /*geteuid*/:
        frame[0] = me != nullptr ? me->euid : 0;
        return;
    case 176 /*getgid*/:
        frame[0] = me != nullptr ? me->gid : 0;
        return;
    case 177 /*getegid*/:
        frame[0] = me != nullptr ? me->egid : 0;
        return;
    case 146 /*setuid*/: {
        const uint32_t u = static_cast<uint32_t>(frame[0]);
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-1); return; }
        // Privileged (euid 0 = crowley) can set all three; otherwise only the
        // effective uid, and only to the real or saved value.
        if (me->euid == 0) {
            me->uid = me->euid = me->suid = u;
            frame[0] = 0;
        } else if (u == me->uid || u == me->suid) {
            me->euid = u;
            frame[0] = 0;
        } else {
            frame[0] = static_cast<uint64_t>(-1); // -EPERM
        }
        return;
    }
    case 144 /*setgid*/: {
        const uint32_t g = static_cast<uint32_t>(frame[0]);
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-1); return; }
        if (me->euid == 0) {
            me->gid = me->egid = me->sgid = g;
            frame[0] = 0;
        } else if (g == me->gid || g == me->sgid) {
            me->egid = g;
            frame[0] = 0;
        } else {
            frame[0] = static_cast<uint64_t>(-1);
        }
        return;
    }
    case 147 /*setresuid*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-1); return; }
        const uint32_t r = static_cast<uint32_t>(frame[0]);
        const uint32_t e = static_cast<uint32_t>(frame[1]);
        const uint32_t s = static_cast<uint32_t>(frame[2]);
        // -1 (UINT32_MAX) preserves the current value of the corresponding id.
        const uint32_t nr = (r == 0xffffffff) ? me->uid : r;
        const uint32_t ne = (e == 0xffffffff) ? me->euid : e;
        const uint32_t ns = (s == 0xffffffff) ? me->suid : s;
        if (me->euid != 0) {
            auto allowed = [&](uint32_t v) {
                return v == me->uid || v == me->euid || v == me->suid;
            };
            if (!allowed(nr) || !allowed(ne) || !allowed(ns)) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
        }
        me->uid = nr; me->euid = ne; me->suid = ns;
        frame[0] = 0;
        return;
    }
    case 149 /*setresgid*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-1); return; }
        const uint32_t r = static_cast<uint32_t>(frame[0]);
        const uint32_t e = static_cast<uint32_t>(frame[1]);
        const uint32_t s = static_cast<uint32_t>(frame[2]);
        const uint32_t nr = (r == 0xffffffff) ? me->gid : r;
        const uint32_t ne = (e == 0xffffffff) ? me->egid : e;
        const uint32_t ns = (s == 0xffffffff) ? me->sgid : s;
        if (me->euid != 0) {
            auto allowed = [&](uint32_t v) {
                return v == me->gid || v == me->egid || v == me->sgid;
            };
            if (!allowed(nr) || !allowed(ne) || !allowed(ns)) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
        }
        me->gid = nr; me->egid = ne; me->sgid = ns;
        frame[0] = 0;
        return;
    }
    case 148 /*getresuid*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-1); return; }
        const uint64_t r_va = frame[0];
        const uint64_t e_va = frame[1];
        const uint64_t s_va = frame[2];
        if (r_va && ensure_user(me, r_va, 4)) *reinterpret_cast<uint32_t *>(r_va) = me->uid;
        if (e_va && ensure_user(me, e_va, 4)) *reinterpret_cast<uint32_t *>(e_va) = me->euid;
        if (s_va && ensure_user(me, s_va, 4)) *reinterpret_cast<uint32_t *>(s_va) = me->suid;
        frame[0] = 0;
        return;
    }
    case 150 /*getresgid*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-1); return; }
        const uint64_t r_va = frame[0];
        const uint64_t e_va = frame[1];
        const uint64_t s_va = frame[2];
        if (r_va && ensure_user(me, r_va, 4)) *reinterpret_cast<uint32_t *>(r_va) = me->gid;
        if (e_va && ensure_user(me, e_va, 4)) *reinterpret_cast<uint32_t *>(e_va) = me->egid;
        if (s_va && ensure_user(me, s_va, 4)) *reinterpret_cast<uint32_t *>(s_va) = me->sgid;
        frame[0] = 0;
        return;
    }
    case 43 /*statfs*/:
    case 44 /*fstatfs*/: {
        // statfs(path, *buf) or fstatfs(fd, *buf). aarch64 struct statfs is
        // 120 bytes. We fill from the ext2 superblock; FAT/procfs synth.
        const uint64_t buf_va = frame[1];
        if (!ensure_user(me, buf_va, 120)) { frame[0] = static_cast<uint64_t>(-14); return; }
        Ext2FsStats st = {};
        const bool have = ext2_fs_stats(&st);
        if (!have) {
            // Synthesise: 4K block, 256 MB capacity, 80% free.
            st.block_size = 4096;
            st.total_blocks = 65536;
            st.free_blocks = 52428;
            st.total_inodes = 16384;
            st.free_inodes = 16000;
            st.namelen_max = 255;
        }
        uint8_t *b = reinterpret_cast<uint8_t *>(buf_va);
        for (uint32_t i = 0; i < 120; ++i) b[i] = 0;
        auto p64 = [&](uint32_t off, uint64_t v) {
            *reinterpret_cast<uint64_t *>(b + off) = v;
        };
        p64(0,  0xEF53);            // f_type (ext2 magic)
        p64(8,  st.block_size);     // f_bsize
        p64(16, st.total_blocks);
        p64(24, st.free_blocks);
        p64(32, st.free_blocks);    // f_bavail == f_bfree (no root reserve)
        p64(40, st.total_inodes);
        p64(48, st.free_inodes);
        p64(64, st.namelen_max);
        p64(72, st.block_size);     // f_frsize
        p64(80, 0);                 // f_flags
        frame[0] = 0;
        return;
    }
    case 166 /*umask*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-1); return; }
        const uint32_t old = me->umask;
        me->umask = static_cast<uint32_t>(frame[0]) & 0777;
        frame[0] = old;
        return;
    }
    case 40 /*mount*/: {
        // mount(source, target, fstype, flags, data). "tmpfs" creates a real
        // in-memory (Testament) mount at the resolved target. Other fstypes
        // (proc/sysfs/9p) are already visible via Lateran, so accept silently
        // (busybox `mount -a` / alpine init expect 0, not ENOSYS).
        if (me != nullptr) {
            char fstype[16] = {};
            copy_user_cstr(me, frame[2], fstype, sizeof(fstype));
            const bool is_tmpfs = fstype[0] == 't' && fstype[1] == 'm' &&
                                  fstype[2] == 'p' && fstype[3] == 'f' &&
                                  fstype[4] == 's' && fstype[5] == 0;
            char abs[kCwdCap];
            if (is_tmpfs && frame[1] != 0 &&
                resolve_at(me, -100 /*AT_FDCWD*/,
                           reinterpret_cast<const char *>(frame[1]), abs, kCwdCap)) {
                lateran_tmpfs_mount(abs);
            }
        }
        frame[0] = 0;
        return;
    }
    case 39 /*umount2*/: {
        // umount2(target, flags). If `target` is a tmpfs mount, tear it down;
        // otherwise accept silently (nothing to unmount for the baked layout).
        if (me != nullptr && frame[0] != 0) {
            char abs[kCwdCap];
            if (resolve_at(me, -100 /*AT_FDCWD*/,
                           reinterpret_cast<const char *>(frame[0]), abs, kCwdCap)) {
                lateran_tmpfs_umount(abs);
            }
        }
        frame[0] = 0;
        return;
    }
    case 154 /*setpgid*/: {
        // setpgid(pid, pgid). pid=0 means self; pgid=0 means "make pgid==pid"
        // (start a new group led by self). Real POSIX requires same session,
        // not yet exec'd, etc.; we accept any caller and just store.
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-1); return; }
        const uint32_t pid = static_cast<uint32_t>(frame[0]);
        const uint32_t newpgid = static_cast<uint32_t>(frame[1]);
        Esper *target = me;
        if (pid != 0 && pid != me->pid) {
            target = nullptr;
            for (uint32_t i = 0; i < kMaxEspers; ++i) {
                Esper *e = esper_at(i);
                if (e != nullptr && e->pid == pid) { target = e; break; }
            }
            if (target == nullptr) { frame[0] = static_cast<uint64_t>(-3); return; } // -ESRCH
        }
        target->pgrp = (newpgid != 0) ? newpgid : target->pid;
        frame[0] = 0;
        return;
    }
    case 155 /*getpgid*/: {
        if (me == nullptr) { frame[0] = 1; return; }
        const uint64_t pid = frame[0];
        if (pid == 0 || pid == me->pid) { frame[0] = me->pgrp; return; }
        for (uint32_t i = 0; i < kMaxEspers; ++i) {
            Esper *e = esper_at(i);
            if (e != nullptr && e->pid == pid) { frame[0] = e->pgrp; return; }
        }
        frame[0] = static_cast<uint64_t>(-3); // -ESRCH
        return;
    }
    case 156 /*setsid*/:
        // setsid: caller becomes session leader of a new session (and a new
        // pgrp). Real Linux refuses if caller is already a pgrp leader; we
        // skip that check.
        if (me == nullptr) { frame[0] = 1; return; }
        me->sid = me->pid;
        me->pgrp = me->pid;
        frame[0] = me->pid;
        return;
    case 157 /*getsid*/:
        if (me != nullptr) {
            const uint64_t pid = frame[0];
            if (pid == 0 || pid == me->pid) { frame[0] = me->sid; return; }
            for (uint32_t i = 0; i < kMaxEspers; ++i) {
                Esper *e = esper_at(i);
                if (e != nullptr && e->pid == pid) { frame[0] = e->sid; return; }
            }
            frame[0] = static_cast<uint64_t>(-3); // -ESRCH
            return;
        }
        frame[0] = 1;
        return;
    case 137 /*rt_sigtimedwait*/:
        // Stub: report timeout immediately so callers (busybox init's signal
        // loop, getty waiting for SIGCHLD) don't spin on ENOSYS.  Programs
        // that actually need timed signal waits will degrade gracefully.
        frame[0] = static_cast<uint64_t>(-11); // -EAGAIN: nothing arrived
        return;
    case 102 /*getitimer*/:
    case 103 /*setitimer*/:
    case 140 /*setpriority*/:
    case 141 /*getpriority*/:
    case 142 /*reboot*/:
    case 118 /*sched_setparam*/:
    case 119 /*sched_setscheduler*/:
    case 120 /*sched_getscheduler*/:
    case 121 /*sched_getparam*/:
    case 122 /*sched_setaffinity*/:
    case 125 /*sched_get_priority_max*/:
    case 126 /*sched_get_priority_min*/:
        // Stubs: accept silently / return 0.  busybox getty/login probe these
        // and don't actually rely on real behaviour at our scale. setaffinity
        // is accepted (we don't pin EL0 threads to cores).
        frame[0] = 0;
        return;
    case 123 /*sched_getaffinity*/: {
        // sched_getaffinity(pid, cpusetsize, mask): OpenSSL/sshd probe this for
        // the online CPU count. Report all SMP cores set; return bytes written.
        const uint64_t cpusetsize = frame[1];
        const uint64_t mask_va = frame[2];
        const uint64_t n = cpusetsize < 8 ? cpusetsize : 8;
        if (mask_va != 0 && ensure_user(me, mask_va, n)) {
            uint8_t *m = reinterpret_cast<uint8_t *>(mask_va);
            for (uint64_t i = 0; i < n; ++i) m[i] = (i == 0) ? 0x0f : 0; // 4 cores
        }
        frame[0] = static_cast<uint64_t>(n);
        return;
    }
    case 158 /*getgroups*/:
        // We don't track supplementary groups (just the primary gid). Report
        // an empty supplementary group set, which is legal.
        frame[0] = 0;
        return;
    case 159 /*setgroups*/:
        // Privileged stub: accept (no enforcement); reject for non-crowley so
        // glibc/musl don't get a false sense of having set anything.
        if (me != nullptr && me->euid != 0) {
            frame[0] = static_cast<uint64_t>(-1);
        } else {
            frame[0] = 0;
        }
        return;

    // ---- misc ---------------------------------------------------------
    case 278 /*getrandom*/: {
        const uint64_t buf = frame[0];
        const uint64_t len = frame[1];
        if (!ensure_user(me, buf, len)) { frame[0] = static_cast<uint64_t>(-14); return; }
        uint8_t *p = reinterpret_cast<uint8_t *>(buf);
        const uint32_t hw = drivers::random_vector_read(p, static_cast<uint32_t>(len));
        if (hw == len) { frame[0] = hw; return; }
        // Fallback PRNG for the unfilled tail (virtio-rng absent or short read).
        uint64_t s = read_cntpct();
        for (uint64_t i = hw; i < len; ++i) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            p[i] = static_cast<uint8_t>(s >> 33);
        }
        frame[0] = len;
        return;
    }
    case 113 /*clock_gettime*/: {
        const uint64_t clk_id = frame[0];
        const uint64_t ts_va = frame[1];
        if (!ensure_user(me, ts_va, 16)) { frame[0] = static_cast<uint64_t>(-14); return; }
        int64_t *ts = reinterpret_cast<int64_t *>(ts_va);
        if (clk_id == 0 /*CLOCK_REALTIME*/ || clk_id == 5 /*CLOCK_REALTIME_COARSE*/) {
            uint64_t sec = 0, nsec = 0;
            idol_theory_epoch_nanos(&sec, &nsec);
            ts[0] = static_cast<int64_t>(sec);
            ts[1] = static_cast<int64_t>(nsec);
        } else {
            // CLOCK_MONOTONIC / MONOTONIC_COARSE / RAW: CNTPCT derived seconds.
            const uint64_t cnt = read_cntpct();
            uint64_t freq = 1;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
            if (freq == 0) freq = 62500000;
            ts[0] = static_cast<int64_t>(cnt / freq);
            ts[1] = static_cast<int64_t>(((cnt % freq) * 1000000000ULL) / freq);
        }
        frame[0] = 0;
        return;
    }
    case 114 /*clock_getres*/: {
        // Resolution probe. HotSpot's os::Linux::clock_init() calls this for
        // CLOCK_MONOTONIC; an ENOSYS made it warn "No monotonic clock available"
        // and fall back to gettimeofday. Report the real granularity: ~1 ns for
        // the RTC-derived realtime clock, the CNTPCT period (~16 ns @ 62.5 MHz)
        // for the monotonic clock.
        const uint64_t clk_id = frame[0];
        const uint64_t ts_va = frame[1];
        if (ts_va != 0) {
            if (!ensure_user(me, ts_va, 16)) { frame[0] = static_cast<uint64_t>(-14); return; }
            int64_t *ts = reinterpret_cast<int64_t *>(ts_va);
            ts[0] = 0;
            if (clk_id == 0 /*REALTIME*/ || clk_id == 5 /*REALTIME_COARSE*/) {
                ts[1] = 1; // 1 ns
            } else {
                uint64_t freq = 0; asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
                if (freq == 0) freq = 62500000;
                const int64_t r = static_cast<int64_t>(1000000000ULL / freq);
                ts[1] = (r > 0) ? r : 1;
            }
        }
        frame[0] = 0;
        return;
    }
    case 169 /*gettimeofday*/: {
        const uint64_t tv_va = frame[0];
        if (tv_va != 0 && ensure_user(me, tv_va, 16)) {
            uint64_t sec = 0, nsec = 0;
            idol_theory_epoch_nanos(&sec, &nsec);
            int64_t *tv = reinterpret_cast<int64_t *>(tv_va);
            tv[0] = static_cast<int64_t>(sec);
            tv[1] = static_cast<int64_t>(nsec / 1000); // tv_usec
        }
        frame[0] = 0;
        return;
    }
    case 160 /*uname*/: {
        // struct utsname has 6 fields of 65 bytes: sysname, nodename, release,
        // version, machine, domainname.
        const uint64_t va = frame[0];
        if (!ensure_user(me, va, 6 * 65)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char *u = reinterpret_cast<char *>(va);
        for (uint32_t i = 0; i < 6 * 65; ++i) u[i] = 0;
        // This is the LINUX-ABI uname; anything that reaches it is a Linux binary,
        // and many platform-detect on sysname (JDK os.name, glibc/musl) -- so it
        // returns "Linux". Index's OWN tools (e.g. /bin/uname) are native-ABI
        // programs that don't call this and report "Index" themselves. So the
        // identity splits by ABI, not by a kernel-side guess. nodename + release
        // ("5.0.0-Index") still carry the Index brand here.
        const char *fields[6] = {"Linux", "index", "5.0.0-Index", "#1", "aarch64", "(none)"};
        for (uint32_t f = 0; f < 6; ++f) {
            const char *s = fields[f];
            char *d = u + f * 65;
            for (uint32_t i = 0; i < 64 && s[i]; ++i) d[i] = s[i];
        }
        frame[0] = 0;
        return;
    }
    case 32 /*flock*/: {
        // flock(fd, op): advisory whole-file lock. Index serializes all FS
        // access under FsGuard (single-threaded disk path), so advisory locks
        // are always uncontended -- accept as a no-op. openjdk's hsperfdata
        // (perf data file) uses it; without it -version warned + skipped perf.
        frame[0] = 0;
        return;
    }
    case 25 /*fcntl*/: {
        // F_GETFD/F_SETFD track FD_CLOEXEC (sshd marks its listener/monitor/pty
        // fds close-on-exec; execve drops them). F_GETFL/F_SETFL and the rest
        // accept silently as before (we don't model the open-flag set).
        const uint64_t fd = frame[0];
        const uint64_t cmd = frame[1];
        const uint64_t arg = frame[2];
        if (me != nullptr && fd < kMaxFds && me->fds[fd].kind != FdKind::closed) {
            if (cmd == 1 /*F_GETFD*/) {
                frame[0] = me->fds[fd].cloexec ? 1u /*FD_CLOEXEC*/ : 0u;
                return;
            }
            if (cmd == 2 /*F_SETFD*/) {
                me->fds[fd].cloexec = (arg & 1u /*FD_CLOEXEC*/) != 0;
                frame[0] = 0;
                return;
            }
        }
        if (cmd == 0 /*F_DUPFD*/ || cmd == 1030 /*F_DUPFD_CLOEXEC*/) {
            // Real dup to the lowest free fd >= arg. The old stub returned 0,
            // which made busybox ash (which relocates its script fd to a high
            // number via F_DUPFD, then closes the original) read its script from
            // fd 0 = the console -> interactive ppoll loop -> `sh file.sh` hung.
            const int nfd = linux_dup_fd_from(me, static_cast<int>(fd), static_cast<int>(arg));
            if (nfd >= 0) me->fds[nfd].cloexec = (cmd == 1030);
            frame[0] = static_cast<uint64_t>(nfd);
            return;
        }
        frame[0] = 0; // F_GETFL/F_SETFL/... : legacy silent success
        return;
    }
    case 261 /*prlimit64*/: {
        // prlimit64(pid, resource, new_limit, old_limit). pid 0 = self. We
        // only support self (multi-process rlimit lookup is a Phase H+ thing).
        const uint64_t pid = frame[0];
        const uint64_t res = frame[1];
        const uint64_t new_va = frame[2];
        const uint64_t old_va = frame[3];
        Esper *target = me;
        if (pid != 0 && (me == nullptr || me->pid != pid)) {
            // Resolve by pid for other processes.
            target = nullptr;
            for (uint32_t i = 0; i < kMaxEspers; ++i) {
                Esper *e = esper_at(i);
                if (e != nullptr && e->pid == pid &&
                    e->state != EsperState::free) { target = e; break; }
            }
        }
        if (target == nullptr || res >= 16) {
            frame[0] = static_cast<uint64_t>(-3); // -ESRCH / -EINVAL
            return;
        }
        if (old_va != 0 && ensure_user(me, old_va, 16)) {
            uint64_t *o = reinterpret_cast<uint64_t *>(old_va);
            o[0] = target->rlimits[res].cur;
            o[1] = target->rlimits[res].max;
        }
        if (new_va != 0 && ensure_user(me, new_va, 16)) {
            const uint64_t *n = reinterpret_cast<const uint64_t *>(new_va);
            // Soft can move freely up to hard; hard can only shrink (unless
            // privileged). We don't enforce privilege yet -- accept the value.
            if (n[1] < target->rlimits[res].max && me != nullptr && me->euid != 0) {
                // non-privileged tried to lower then raise hard -- reject
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            target->rlimits[res].cur = n[0];
            target->rlimits[res].max = n[1];
        }
        frame[0] = 0;
        return;
    }
    case 163 /*getrlimit*/: {
        const uint64_t res = frame[0];
        const uint64_t va = frame[1];
        if (me == nullptr || res >= 16 || !ensure_user(me, va, 16)) {
            frame[0] = static_cast<uint64_t>(-22);
            return;
        }
        uint64_t *r = reinterpret_cast<uint64_t *>(va);
        r[0] = me->rlimits[res].cur;
        r[1] = me->rlimits[res].max;
        frame[0] = 0;
        return;
    }
    case 164 /*setrlimit*/: {
        const uint64_t res = frame[0];
        const uint64_t va = frame[1];
        if (me == nullptr || res >= 16 || !ensure_user(me, va, 16)) {
            frame[0] = static_cast<uint64_t>(-22);
            return;
        }
        const uint64_t *r = reinterpret_cast<const uint64_t *>(va);
        me->rlimits[res].cur = r[0];
        me->rlimits[res].max = r[1];
        frame[0] = 0;
        return;
    }
    case 283 /*membarrier*/:
        frame[0] = 0; // memory ordering is already strong enough on our model
        return;
    case 233 /*madvise*/:
        frame[0] = 0; // advisory only; safe to ignore
        return;
    case 73 /*ppoll*/: {
        // Real poll: per-fd readiness; if nothing is ready, park until a producer
        // wakes us. We never drain the NIC here -- doing so re-enters deliver_tcp
        // on the poller's stack and races KEX byte assembly; the 100 Hz
        // network_tick is the single RX drain site. pollfd = {int fd; short ev;
        // short re}.
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t ufds = frame[0];
        const uint64_t nfds = frame[1];
        const uint64_t tmo_va = frame[2];
        if (nfds > 256) { frame[0] = static_cast<uint64_t>(-22); return; }
        const int pidx = esper_running_index();
        // ppoll's 4th arg is a sigmask applied for the wait's duration (Linux
        // swaps it in atomically). A signal deliverable under THAT mask must
        // interrupt the wait with -EINTR (checked each iteration below).
        //
        // Read the sigset ONCE, on the first entry of this ppoll (poll_armed is
        // still false), and cache it; every park-replay then reuses the cache
        // instead of re-prefaulting the user pointer. This mirrors Linux's
        // set_user_sigmask (swap the mask in once for the syscall's lifetime).
        // The old code re-read frame[3] on every replay: if a single prefault of
        // the sigset pointer failed (SMP / demand-paging timing), eff_mask fell
        // back to the process mask -- which BLOCKS SIGCHLD (sshd blocks it and
        // relies on ppoll's sigset to unblock it) -- so that replay treated the
        // pending SIGCHLD as masked, the reaper never ran, and logout hung. This
        // was the Heisenbug: any extra console latency widened the window and
        // hid it. Reading once, when sshd has just written the sigset and the
        // stack page is definitely resident, removes the per-replay race.
        uint64_t eff_mask;
        if (me->poll_armed) {
            eff_mask = me->poll_eff_mask; // replay: reuse the first-entry value
        } else {
            eff_mask = me->sig_mask;
            if (frame[3] != 0 && ensure_user(me, frame[3], 8)) {
                eff_mask = *reinterpret_cast<const uint64_t *>(frame[3]);
            }
            me->poll_eff_mask = eff_mask;
        }
        for (;;) {
            // Snapshot the poll generation BEFORE scanning. Any producer that
            // makes one of our fds ready after this point bumps g_poll_gen under
            // g_esper_lock; the under-lock recheck below (poll_gen_changed) then
            // catches it and we re-scan instead of sleeping through the wake.
            // This closes the check-then-park lost-wakeup window that hung the
            // SMP pty relay -- sshd polling {pty-master, TCP} while the shell
            // wrote the prompt on another core, whose PtyMasterRead wake landed
            // in the gap before sshd parked and was lost (the 100 Hz fallback
            // only re-kicks PollWait from CPU0, so a poller parked on another
            // core could sleep until the next NIC packet). All without the
            // O(nfds) under-lock fd rescan that was tried and reverted for
            // regressing KEX timing -- the recheck here is a single counter load.
            const uint32_t gen0 = linux_poll_gen();
            // A deliverable signal (unmasked by the ppoll sigmask) interrupts the
            // wait with -EINTR and runs its handler -- this is how sshd's SIGCHLD
            // reaper fires at logout. CHECK IT BEFORE the fd scan: an fd that is
            // perpetually POLLHUP/POLLERR (e.g. the pty master once the shell's
            // slave side closed -- 0x18 is always reported) keeps count>0 and
            // would starve this check forever, so the SIGCHLD that should reap the
            // exited shell never gets delivered and the ssh client hangs after
            // `exit`. Mirrors deliver_sigint: set -EINTR, then enter the handler.
            {
                const uint64_t deliverable = me->sig_pending & ~eff_mask;
                if (deliverable != 0) {
                    const int sig = __builtin_ctzll(deliverable) + 1;
                    frame[0] = static_cast<uint64_t>(-4); // -EINTR (saved in sigframe if delivered)
                    if (linux_deliver_signal(me, sig, frame)) {
                        me->poll_armed = false;
                        return; // handler entered; ppoll yields -EINTR via sigreturn
                    }
                    // Delivery returned false. Restore the pollfd-array arg (we
                    // tentatively set frame[0]=-EINTR). Only CONSUME the pending
                    // bit for a genuine SIG_DFL/SIG_IGN; for an installed handler a
                    // false means a TRANSIENT frame-build failure (pr2_write_user
                    // could not fault in the user stack page under SMP) -- dropping
                    // it here silently lost SIGCHLD and hung the ssh logout, so we
                    // leave it pending to be redelivered on the next iteration.
                    frame[0] = ufds;
                    const uint64_t h = me->sig_handler[sig];
                    if (h == 0 /*SIG_DFL*/ || h == 1 /*SIG_IGN*/) {
                        me->sig_pending &= ~(1ULL << (sig - 1));
                    }
                }
            }
            uint64_t count = 0;
            // ufds must be a real user pointer. Reject a wild/kernel value (>= the
            // 39-bit user ceiling) so a corrupt arg skips the scan -- and the
            // signal check above still runs -- instead of dereferencing it (the
            // ufds=0xfffffffffffffffc EL1 data abort seen at logout).
            if (ufds != 0 && nfds > 0 && nfds <= 4096 && ufds < 0x8000000000ULL &&
                ensure_user(me, ufds, nfds * 8)) { // nfds bound: keep nfds*8 from overflowing
                for (uint64_t i = 0; i < nfds; ++i) {
                    const int fd = *reinterpret_cast<const int32_t *>(ufds + i*8);
                    const int16_t ev = *reinterpret_cast<const int16_t *>(ufds + i*8 + 4);
                    uint32_t re = linux_fd_revents(me, fd);
                    re &= static_cast<uint32_t>(static_cast<uint16_t>(ev)) | 0x18u;
                    *reinterpret_cast<int16_t *>(ufds + i*8 + 6) = static_cast<int16_t>(re);
                    if (re != 0) ++count;
                }
            }
            if (count > 0) { me->poll_armed = false; frame[0] = count; return; }
            // Arm the timeout once; it must survive park-replays (each wake re-runs
            // this syscall from the top, so a local deadline would reset every tick).
            if (!me->poll_armed) {
                me->poll_armed = true;
                if (tmo_va == 0 || !ensure_user(me, tmo_va, 16)) {
                    me->poll_deadline = 0; // infinite
                } else {
                    const uint64_t sec  = *reinterpret_cast<const uint64_t *>(tmo_va);
                    const uint64_t nsec = *reinterpret_cast<const uint64_t *>(tmo_va + 8);
                    me->poll_deadline = last_order_ticks() + sec * 100 + nsec / 10000000 + 1;
                }
            }
            if (me->poll_deadline != 0 && last_order_ticks() >= me->poll_deadline) {
                me->poll_armed = false; frame[0] = 0; return; // timed out
            }
            if (pidx < 0) { me->poll_armed = false; frame[0] = 0; return; } // no one else runnable
            // Park ONLY if no producer fired since gen0 AND no signal deliverable
            // under eff_mask is pending (both re-checked under the same
            // g_esper_lock every waker / signal sender takes -> park-vs-wake and
            // park-vs-signal are atomic). If a producer fired or a signal arrived
            // in the check-then-park window, ipc_park_unless_ready returns false
            // and we loop to re-scan / re-run the signal-check, never sleeping
            // through it. Passing eff_mask (not the process mask) is essential:
            // sshd blocks SIGCHLD in its process mask but ppoll's sigset unblocks
            // it, so only eff_mask correctly says "SIGCHLD must abort this park".
            if (ipc_park_unless_ready(pidx, Esper::IpcWaitKind::PollWait,
                                      static_cast<int>(gen0), frame,
                                      poll_gen_changed, eff_mask)) {
                return; // parked; svc replays the ppoll on wake
            }
            // gen changed in the park window -> re-scan (loop).
        }
    }
    case 72 /*pselect6*/: {
        // Same shape as ppoll: claim all fds in readfds/writefds are ready.
        // Args: nfds, readfds, writefds, exceptfds, timeout, sigmask. We
        // leave the bitmaps alone (caller already set them); just return nfds.
        frame[0] = frame[0]; // nfds passes through unchanged
        return;
    }
    case 71 /*sendfile*/: {
        // No fast-path: tell the caller it's not supported so musl falls
        // through to its read/write copy. busybox cat tries sendfile first.
        frame[0] = static_cast<uint64_t>(-38); // -ENOSYS, but quiet
        return;
    }
    case 124 /*sched_yield*/:
        // Cooperative yield. The 100 Hz timer tick does the real preemption
        // (esper_preempt); a plain YIELD lets this core re-pick if another Esper
        // is ready. Always succeeds (Linux sched_yield returns 0). java's VM
        // threads call this during spin/shutdown; returning -ENOSYS spammed the
        // log -- returning 0 is both correct and quiet.
        asm volatile("yield");
        frame[0] = 0;
        return;
    case 153 /*times*/:
    case 165 /*getrusage*/:
        // Quiet "not yet" replies for common timing/accounting syscalls so
        // interactive busybox doesn't fill the log with ENOSYS. Programs
        // generally degrade gracefully.
        frame[0] = static_cast<uint64_t>(-38);
        return;
        // utimensat(88) and faccessat(48) moved up to real implementations.

    case 93 /*exit*/:
    case 94 /*exit_group*/:
        // Handled by the usermode.cpp wrapper (it recognises 93/94 and calls
        // exit_and_schedule). Nothing to do here.
        return;

    // ---- cwd / chdir / getcwd -----------------------------------------
    case 17 /*getcwd*/: {
        // getcwd(buf, size): copy the Esper's cwd into a user buffer; returns
        // the buf VA on success or -ERANGE if the cwd is longer than size.
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t buf_va = frame[0];
        const uint64_t size = frame[1];
        uint32_t n = 0;
        while (me->cwd[n] != 0 && n + 1 < kCwdCap) ++n;
        if (size < n + 1) { frame[0] = static_cast<uint64_t>(-34); return; } // -ERANGE
        if (!ensure_user(me, buf_va, n + 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char *dst = reinterpret_cast<char *>(buf_va);
        for (uint32_t i = 0; i < n; ++i) dst[i] = me->cwd[i];
        dst[n] = 0;
        frame[0] = buf_va;
        return;
    }
    case 49 /*chdir*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t path_va = frame[0];
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char abs[kCwdCap];
        if (!resolve_path(me, nullptr, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        // Must exist and be a directory. "/" always exists.
        const bool is_root = (abs[0] == '/' && abs[1] == 0);
        if (!is_root && !lateran_is_dir(abs)) {
            frame[0] = static_cast<uint64_t>(-20); return; // -ENOTDIR / -ENOENT
        }
        uint32_t i = 0;
        for (; i + 1 < kCwdCap && abs[i] != 0; ++i) me->cwd[i] = abs[i];
        me->cwd[i] = 0;
        frame[0] = 0;
        return;
    }
    case 50 /*fchdir*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        if (fd >= kMaxFds || me->fds[fd].kind != FdKind::file) {
            frame[0] = static_cast<uint64_t>(-9); return; // -EBADF
        }
        const char *fp = me->fds[fd].path;
        if (fp[0] == 0 || !lateran_is_dir(fp)) {
            frame[0] = static_cast<uint64_t>(-20); return; // -ENOTDIR
        }
        char abs[kCwdCap];
        if (!resolve_path(me, nullptr, fp, abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        uint32_t i = 0;
        for (; i + 1 < kCwdCap && abs[i] != 0; ++i) me->cwd[i] = abs[i];
        me->cwd[i] = 0;
        frame[0] = 0;
        return;
    }

    // ---- dup / dup3 / pipe2 -------------------------------------------
    case 23 /*dup*/: {
        const int r = linux_dup_fd(me, static_cast<int>(frame[0]));
        frame[0] = (r >= 0) ? static_cast<uint64_t>(r) : static_cast<uint64_t>(static_cast<int64_t>(r));
        return;
    }
    case 24 /*dup3*/: {
        const int oldfd = static_cast<int>(frame[0]);
        const int newfd = static_cast<int>(frame[1]);
        const uint64_t flags = frame[2];
        const int r = linux_dup3_fd(me, oldfd, newfd, flags);
        frame[0] = (r >= 0) ? static_cast<uint64_t>(r) : static_cast<uint64_t>(static_cast<int64_t>(r));
        return;
    }
    case 59 /*pipe2*/: {
        const uint64_t va = frame[0];
        if (!ensure_user(me, va, 8)) { frame[0] = static_cast<uint64_t>(-14); return; }
        int fds[2] = {-1, -1};
        const int r = linux_pipe2(me, fds, frame[1]);
        if (r < 0) {
            frame[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
            return;
        }
        int *out = reinterpret_cast<int *>(va);
        out[0] = fds[0];
        out[1] = fds[1];
        frame[0] = 0;
        return;
    }

    // ---- sleep --------------------------------------------------------
    case 101 /*nanosleep*/:
    case 115 /*clock_nanosleep*/: {
        // nanosleep(req, rem) / clock_nanosleep(clk, flags, req, rem). req is a
        // struct timespec {tv_sec, tv_nsec}. We park the Esper until cntpct
        // reaches the deadline; the scheduler wakes it. Doesn't honour rem
        // (always returns 0).
        const uint64_t req_va = (nr == 101) ? frame[0] : frame[2];
        if (!ensure_user(me, req_va, 16)) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int64_t *req = reinterpret_cast<const int64_t *>(req_va);
        const uint64_t sec = static_cast<uint64_t>(req[0] < 0 ? 0 : req[0]);
        const uint64_t nsec = static_cast<uint64_t>(req[1] < 0 ? 0 : req[1]);
        uint64_t freq = 1;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        if (freq == 0) freq = 62500000;
        const uint64_t now = read_cntpct();
        const uint64_t ticks = sec * freq + (nsec * freq) / 1000000000ULL;
        if (ticks == 0) { frame[0] = 0; return; }
        const int idx = esper_running_index();
        if (idx < 0) { frame[0] = 0; return; }
        linux_nanosleep_park(idx, now + ticks, frame);
        return;
    }

    // ---- execve -------------------------------------------------------
    case 221 /*execve*/: {
        // execve(path, argv, envp): replace this Esper's image with the ELF at
        // `path`, then resume at the new entry with a SysV initial stack built
        // from argv/envp. Resolves the path against cwd.
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t path_va = frame[0];
        const uint64_t argv_va = frame[1];
        const uint64_t envp_va = frame[2];
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        char abs[kCwdCap];
        if (!resolve_path(me, nullptr, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        const char *const *user_argv = reinterpret_cast<const char *const *>(argv_va);
        uint32_t argc = 0;
        if (argv_va != 0) {
            ensure_user(me, argv_va, kExecArgvCap * 8);
            while (argc < kExecArgvCap && user_argv[argc] != nullptr) {
                ensure_user(me, reinterpret_cast<uint64_t>(user_argv[argc]), 1);
                ++argc;
            }
        }
        const char *const *user_envp = reinterpret_cast<const char *const *>(envp_va);
        uint32_t envc = 0;
        if (envp_va != 0) {
            ensure_user(me, envp_va, kExecEnvpCap * 8);
            while (envc < kExecEnvpCap && user_envp[envc] != nullptr) {
                ensure_user(me, reinterpret_cast<uint64_t>(user_envp[envc]), 1);
                ++envc;
            }
        }
        const int idx = esper_running_index();
        if (!linux_execve_replace(idx, abs, user_argv, argc, user_envp, envc, frame)) {
            frame[0] = static_cast<uint64_t>(-2); // -ENOENT (old image intact)
            return;
        }
        // linux_execve_replace already switched contexts via load_ctx; eret resumes new image.
        return;
    }

    // ---- sockets (Antenna): IPv4 UDP only for now ---------------------
    // struct sockaddr_in is 16 bytes on Linux aarch64:
    //   u16 sin_family (=2 AF_INET), u16 sin_port (big-endian),
    //   u32 sin_addr (big-endian), u8 sin_zero[8].
    case 198 /*socket*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t domain = frame[0];
        const uint64_t type = frame[1] & 0x7f; // strip SOCK_NONBLOCK/CLOEXEC
        const bool stream = (type == 1);
        const bool dgram  = (type == 2);
        if (!stream && !dgram) {
            frame[0] = static_cast<uint64_t>(-93); // -EPROTONOSUPPORT
            return;
        }
        int idx = -1;
        FdKind kind = FdKind::socket;
        if (domain == 2 /*AF_INET*/) {
            idx = stream ? antenna_socket_tcp() : antenna_socket_udp();
            kind = FdKind::socket;
        } else if (domain == 1 /*AF_UNIX*/) {
            idx = inc_socket(static_cast<uint8_t>(type));
            kind = FdKind::unix_sock;
        } else {
            frame[0] = static_cast<uint64_t>(-97); // -EAFNOSUPPORT
            return;
        }
        if (idx < 0) { frame[0] = static_cast<uint64_t>(-23); return; }
        int fd = -1;
        for (uint32_t i = 0; i < kMaxFds; ++i) {
            if (me->fds[i].kind == FdKind::closed) { fd = static_cast<int>(i); break; }
        }
        if (fd < 0) {
            if (kind == FdKind::socket) antenna_close(idx); else inc_close(idx);
            frame[0] = static_cast<uint64_t>(-24); return;
        }
        me->fds[fd] = Fd{};
        me->fds[fd].kind = kind;
        me->fds[fd].sock_idx = idx;
        frame[0] = static_cast<uint64_t>(fd);
        return;
    }
    case 200 /*bind*/:
    case 203 /*connect*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t sa_va = frame[1];
        const uint64_t sa_len = frame[2];
        if (fd >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const FdKind k = me->fds[fd].kind;
        if (k != FdKind::socket && k != FdKind::unix_sock) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        if (sa_len < 2 || !ensure_user(me, sa_va, sa_len < 110 ? sa_len : 110)) {
            frame[0] = static_cast<uint64_t>(-22); return;
        }
        const uint8_t *sa = reinterpret_cast<const uint8_t *>(sa_va);
        const uint16_t family = static_cast<uint16_t>(sa[0] | (sa[1] << 8));
        const int s = me->fds[fd].sock_idx;
        if (family == 2 /*AF_INET*/) {
            if (k != FdKind::socket) { frame[0] = static_cast<uint64_t>(-97); return; }
            const uint16_t port = static_cast<uint16_t>((sa[2] << 8) | sa[3]);
            const uint8_t *ip = sa + 4;
            const bool ok = (nr == 200) ? antenna_bind(s, port) : antenna_connect(s, ip, port);
            frame[0] = ok ? 0 : static_cast<uint64_t>(nr == 200 ? -98 : -22);
            return;
        }
        if (family == 1 /*AF_UNIX*/) {
            if (k != FdKind::unix_sock) { frame[0] = static_cast<uint64_t>(-97); return; }
            // sun_path is the rest of the struct. Compute length: caller's
            // addrlen minus the 2-byte family. Cap at sun_path size.
            uint32_t plen = (sa_len > 2) ? sa_len - 2 : 0;
            if (plen > 108) plen = 108;
            // Many libcs pass a NUL-terminated path with addrlen = offsetof+
            // strlen(path)+1; strip a trailing NUL so the path matches the
            // bind name exactly.
            const char *raw = reinterpret_cast<const char *>(sa + 2);
            while (plen > 0 && raw[plen - 1] == 0) --plen;
            const bool ok = (nr == 200) ? inc_bind(s, raw, plen)
                                         : inc_connect(s, raw, plen, 100);
            frame[0] = ok ? 0 : static_cast<uint64_t>(nr == 200 ? -98 : -111);
            return;
        }
        frame[0] = static_cast<uint64_t>(-97); // -EAFNOSUPPORT
        return;
    }
    case 204 /*getsockname*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t sa_va = frame[1];
        const uint64_t len_va = frame[2];
        if (fd >= kMaxFds || me->fds[fd].kind != FdKind::socket) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        if (!ensure_user(me, sa_va, 16) || !ensure_user(me, len_va, 4)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        uint8_t local_ip[4] = {};
        uint16_t local_port = 0;
        if (!antenna_get_local(me->fds[fd].sock_idx, local_ip, &local_port)) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        uint8_t *sa = reinterpret_cast<uint8_t *>(sa_va);
        sa[0] = 2; sa[1] = 0;
        sa[2] = static_cast<uint8_t>(local_port >> 8);
        sa[3] = static_cast<uint8_t>(local_port & 0xff);
        for (uint32_t i = 0; i < 4; ++i) sa[4 + i] = local_ip[i];
        for (uint32_t i = 0; i < 8; ++i) sa[8 + i] = 0;
        *reinterpret_cast<uint32_t *>(len_va) = 16;
        frame[0] = 0;
        return;
    }
    case 205 /*getpeername*/: {
        // Remote endpoint of a connected/accepted socket. sshd calls this to
        // log "Connection from <ip> port <n>". Mirrors getsockname(204) but
        // fills the peer address from the Antenna's remote_ip/remote_port.
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t sa_va = frame[1];
        const uint64_t len_va = frame[2];
        if (fd >= kMaxFds || me->fds[fd].kind != FdKind::socket) {
            frame[0] = static_cast<uint64_t>(-9); return; // -EBADF / -ENOTSOCK
        }
        if (!ensure_user(me, sa_va, 16) || !ensure_user(me, len_va, 4)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        uint8_t peer_ip[4] = {};
        uint16_t peer_port = 0;
        if (!antenna_get_remote(me->fds[fd].sock_idx, peer_ip, &peer_port)) {
            frame[0] = static_cast<uint64_t>(-107); return; // -ENOTCONN
        }
        uint8_t *sa = reinterpret_cast<uint8_t *>(sa_va);
        sa[0] = 2; sa[1] = 0;                              // AF_INET
        sa[2] = static_cast<uint8_t>(peer_port >> 8);      // port (network order)
        sa[3] = static_cast<uint8_t>(peer_port & 0xff);
        for (uint32_t i = 0; i < 4; ++i) sa[4 + i] = peer_ip[i];
        for (uint32_t i = 0; i < 8; ++i) sa[8 + i] = 0;
        *reinterpret_cast<uint32_t *>(len_va) = 16;
        frame[0] = 0;
        return;
    }
    case 206 /*sendto*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t buf = frame[1];
        const uint64_t len = frame[2];
        const uint64_t dst_va = frame[4];
        const uint64_t dst_len = frame[5];
        if (fd >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const FdKind k = me->fds[fd].kind;
        if (k != FdKind::socket && k != FdKind::unix_sock) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        if (buf != 0 && !ensure_user(me, buf, len)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        if (k == FdKind::socket) {
            const uint8_t *dst_ip = nullptr;
            uint16_t dst_port = 0;
            if (dst_va != 0 && dst_len >= 8 && ensure_user(me, dst_va, 16)) {
                const uint8_t *sa = reinterpret_cast<const uint8_t *>(dst_va);
                const uint16_t family = static_cast<uint16_t>(sa[0] | (sa[1] << 8));
                if (family != 2) { frame[0] = static_cast<uint64_t>(-97); return; }
                dst_port = static_cast<uint16_t>((sa[2] << 8) | sa[3]);
                dst_ip = sa + 4;
            }
            frame[0] = static_cast<uint64_t>(
                antenna_sendto(me->fds[fd].sock_idx, dst_ip, dst_port,
                               reinterpret_cast<const uint8_t *>(buf),
                               static_cast<uint32_t>(len)));
            return;
        }
        // AF_UNIX dgram.
        if (dst_va == 0 || dst_len < 2 ||
            !ensure_user(me, dst_va, dst_len < 110 ? dst_len : 110)) {
            frame[0] = static_cast<uint64_t>(-22); return;
        }
        const uint8_t *sa = reinterpret_cast<const uint8_t *>(dst_va);
        const uint16_t family = static_cast<uint16_t>(sa[0] | (sa[1] << 8));
        if (family != 1) { frame[0] = static_cast<uint64_t>(-97); return; }
        uint32_t plen = (dst_len > 2) ? dst_len - 2 : 0;
        if (plen > 108) plen = 108;
        const char *path = reinterpret_cast<const char *>(sa + 2);
        while (plen > 0 && path[plen - 1] == 0) --plen;
        frame[0] = static_cast<uint64_t>(
            inc_sendto(me->fds[fd].sock_idx, path, plen,
                       reinterpret_cast<const uint8_t *>(buf),
                       static_cast<uint32_t>(len)));
        return;
    }
    case 207 /*recvfrom*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t buf = frame[1];
        const uint64_t len = frame[2];
        const uint64_t src_va = frame[4];
        const uint64_t srclen_va = frame[5];
        if (fd >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const FdKind k = me->fds[fd].kind;
        if (k != FdKind::socket && k != FdKind::unix_sock) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        if (!ensure_user(me, buf, len)) { frame[0] = static_cast<uint64_t>(-14); return; }
        int64_t r = 0;
        if (k == FdKind::socket) {
            uint8_t src_ip[4] = {};
            uint16_t src_port = 0;
            r = antenna_recvfrom(me->fds[fd].sock_idx,
                                  reinterpret_cast<uint8_t *>(buf),
                                  static_cast<uint32_t>(len),
                                  src_ip, &src_port, 1000);
            if (r > 0 && src_va != 0 && ensure_user(me, src_va, 16)) {
                uint8_t *sa = reinterpret_cast<uint8_t *>(src_va);
                sa[0] = 2; sa[1] = 0;
                sa[2] = static_cast<uint8_t>(src_port >> 8);
                sa[3] = static_cast<uint8_t>(src_port & 0xff);
                for (uint32_t i = 0; i < 4; ++i) sa[4 + i] = src_ip[i];
                for (uint32_t i = 0; i < 8; ++i) sa[8 + i] = 0;
                if (srclen_va != 0 && ensure_user(me, srclen_va, 4)) {
                    *reinterpret_cast<uint32_t *>(srclen_va) = 16;
                }
            }
        } else {
            char src_path[108] = {};
            uint32_t src_plen = 0;
            r = inc_recvfrom(me->fds[fd].sock_idx,
                             reinterpret_cast<uint8_t *>(buf),
                             static_cast<uint32_t>(len),
                             src_path, &src_plen, 1000);
            if (r >= 0 && src_va != 0 && ensure_user(me, src_va, 110)) {
                uint8_t *sa = reinterpret_cast<uint8_t *>(src_va);
                sa[0] = 1; sa[1] = 0;
                const uint32_t cap = src_plen > 108 ? 108 : src_plen;
                for (uint32_t i = 0; i < cap; ++i) sa[2 + i] = static_cast<uint8_t>(src_path[i]);
                if (cap < 108) sa[2 + cap] = 0;
                if (srclen_va != 0 && ensure_user(me, srclen_va, 4)) {
                    *reinterpret_cast<uint32_t *>(srclen_va) = 2 + cap + 1;
                }
            }
        }
        frame[0] = static_cast<uint64_t>(r);
        return;
    }
    case 201 /*listen*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t backlog = frame[1];
        if (fd >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const FdKind k = me->fds[fd].kind;
        const int s = me->fds[fd].sock_idx;
        bool ok = false;
        if (k == FdKind::socket) ok = antenna_tcp_listen(s, static_cast<uint32_t>(backlog));
        else if (k == FdKind::unix_sock) ok = inc_listen(s, static_cast<uint32_t>(backlog));
        else { frame[0] = static_cast<uint64_t>(-9); return; }
        frame[0] = ok ? 0 : static_cast<uint64_t>(-22);
        return;
    }
    case 202 /*accept*/:
    case 242 /*accept4*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t sa_va = frame[1];
        const uint64_t len_va = frame[2];
        if (fd >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const FdKind k = me->fds[fd].kind;
        if (k != FdKind::socket && k != FdKind::unix_sock) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        const int s = me->fds[fd].sock_idx;
        int child = -1;
        uint8_t peer_ip[4] = {};
        uint16_t peer_port = 0;
        char peer_path[108] = {};
        uint32_t peer_plen = 0;
        if (k == FdKind::socket) {
            child = antenna_tcp_try_accept(s, peer_ip, &peer_port);
        } else {
            child = inc_try_accept(s, peer_path, &peer_plen);
        }
        if (child < 0) {
            // Empty queue: park the Esper on the listen socket and let the
            // producer side (deliver_tcp / inc_connect) wake us via
            // linux_ipc_wake when a connection lands.
            const int idx = esper_running_index();
            const auto kind = (k == FdKind::socket)
                                  ? Esper::IpcWaitKind::AntennaAccept
                                  : Esper::IpcWaitKind::ChannelAccept;
            if (idx >= 0 && linux_ipc_park(idx, kind, s, frame) >= 0) {
                return; // resumed when a peer fires the wake; svc re-fires
            }
            frame[0] = static_cast<uint64_t>(-11); // -EAGAIN: nothing to switch to
            return;
        }
        int newfd = -1;
        for (uint32_t i = 0; i < kMaxFds; ++i) {
            if (me->fds[i].kind == FdKind::closed) { newfd = static_cast<int>(i); break; }
        }
        if (newfd < 0) {
            if (k == FdKind::socket) antenna_close(child); else inc_close(child);
            frame[0] = static_cast<uint64_t>(-24); return;
        }
        me->fds[newfd] = Fd{};
        me->fds[newfd].kind = k;
        me->fds[newfd].sock_idx = child;
        // Optional peer sockaddr (sockaddr_in for AF_INET, sockaddr_un for AF_UNIX).
        if (sa_va != 0 && ensure_user(me, sa_va, 110)) {
            uint8_t *sa = reinterpret_cast<uint8_t *>(sa_va);
            if (k == FdKind::socket) {
                sa[0] = 2; sa[1] = 0;
                sa[2] = static_cast<uint8_t>(peer_port >> 8);
                sa[3] = static_cast<uint8_t>(peer_port & 0xff);
                for (uint32_t i = 0; i < 4; ++i) sa[4 + i] = peer_ip[i];
                for (uint32_t i = 0; i < 8; ++i) sa[8 + i] = 0;
                if (len_va != 0 && ensure_user(me, len_va, 4)) {
                    *reinterpret_cast<uint32_t *>(len_va) = 16;
                }
            } else {
                sa[0] = 1; sa[1] = 0; // AF_UNIX
                const uint32_t cap = peer_plen > 108 ? 108 : peer_plen;
                for (uint32_t i = 0; i < cap; ++i) sa[2 + i] = static_cast<uint8_t>(peer_path[i]);
                if (cap < 108) sa[2 + cap] = 0;
                if (len_va != 0 && ensure_user(me, len_va, 4)) {
                    *reinterpret_cast<uint32_t *>(len_va) = 2 + cap + 1;
                }
            }
        }
        frame[0] = static_cast<uint64_t>(newfd);
        return;
    }
    case 19 /*eventfd2*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint32_t initval = static_cast<uint32_t>(frame[0]);
        const uint32_t flags = static_cast<uint32_t>(frame[1]);
        const bool semaphore = (flags & 0x01) != 0; // EFD_SEMAPHORE
        const bool nonblock  = (flags & 0x800) != 0; // EFD_NONBLOCK
        const int eid = eventfd_alloc(initval, semaphore, nonblock);
        if (eid < 0) { frame[0] = static_cast<uint64_t>(-23); return; }
        int newfd = -1;
        for (uint32_t i = 0; i < kMaxFds; ++i) {
            if (me->fds[i].kind == FdKind::closed) { newfd = static_cast<int>(i); break; }
        }
        if (newfd < 0) { eventfd_close(eid); frame[0] = static_cast<uint64_t>(-24); return; }
        me->fds[newfd] = Fd{};
        me->fds[newfd].kind = FdKind::eventfd;
        me->fds[newfd].sock_idx = eid;
        frame[0] = static_cast<uint64_t>(newfd);
        return;
    }
    case 85 /*timerfd_create*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint32_t flags = static_cast<uint32_t>(frame[1]); // arg0=clockid (CNTPCT-based here)
        const int tid = timerfd_alloc((flags & 0x800) != 0 /*TFD_NONBLOCK*/);
        if (tid < 0) { frame[0] = static_cast<uint64_t>(-23); return; }
        int newfd = -1;
        for (uint32_t i = 0; i < kMaxFds; ++i)
            if (me->fds[i].kind == FdKind::closed) { newfd = static_cast<int>(i); break; }
        if (newfd < 0) { timerfd_close(tid); frame[0] = static_cast<uint64_t>(-24); return; }
        me->fds[newfd] = Fd{};
        me->fds[newfd].kind = FdKind::timerfd;
        me->fds[newfd].sock_idx = tid;
        frame[0] = static_cast<uint64_t>(newfd);
        return;
    }
    case 86 /*timerfd_settime*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint32_t flags = static_cast<uint32_t>(frame[1]); // TFD_TIMER_ABSTIME=1
        const uint64_t newv = frame[2], oldv = frame[3];
        if (fd >= kMaxFds || me->fds[fd].kind != FdKind::timerfd) { frame[0] = static_cast<uint64_t>(-9); return; }
        TimerFd *t = timerfd_at(me->fds[fd].sock_idx);
        if (t == nullptr || !ensure_user(me, newv, 32)) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int64_t *its = reinterpret_cast<const int64_t *>(newv);
        uint64_t freq = 0; asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        if (freq == 0) freq = 62500000;
        auto ticks = [&](int64_t s, int64_t ns) -> uint64_t {
            return static_cast<uint64_t>(s) * freq +
                   (static_cast<uint64_t>(ns) * freq) / 1000000000ull;
        };
        if (oldv != 0 && ensure_user(me, oldv, 32))
            for (uint32_t i = 0; i < 4; ++i) reinterpret_cast<int64_t *>(oldv)[i] = 0;
        if (its[2] == 0 && its[3] == 0) { t->expire_cnt = 0; t->interval_cnt = 0; } // disarm
        else {
            const uint64_t vt = ticks(its[2], its[3]);
            t->expire_cnt = (flags & 1) ? vt : (read_cntpct() + vt); // ABSTIME: monotonic CNTPCT
            t->interval_cnt = ticks(its[0], its[1]);
        }
        frame[0] = 0;
        return;
    }
    case 87 /*timerfd_gettime*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0], curv = frame[1];
        if (fd >= kMaxFds || me->fds[fd].kind != FdKind::timerfd) { frame[0] = static_cast<uint64_t>(-9); return; }
        TimerFd *t = timerfd_at(me->fds[fd].sock_idx);
        if (t == nullptr || !ensure_user(me, curv, 32)) { frame[0] = static_cast<uint64_t>(-14); return; }
        uint64_t freq = 0; asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        if (freq == 0) freq = 62500000;
        int64_t *its = reinterpret_cast<int64_t *>(curv);
        its[0] = static_cast<int64_t>(t->interval_cnt / freq);
        its[1] = static_cast<int64_t>((t->interval_cnt % freq) * 1000000000ull / freq);
        const uint64_t now = read_cntpct();
        const uint64_t rem = (t->expire_cnt > now) ? (t->expire_cnt - now) : 0;
        its[2] = static_cast<int64_t>(rem / freq);
        its[3] = static_cast<int64_t>((rem % freq) * 1000000000ull / freq);
        frame[0] = 0;
        return;
    }
    case 74 /*signalfd4*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int32_t fd = static_cast<int32_t>(frame[0]);
        const uint64_t mask_va = frame[1];
        const uint32_t flags = static_cast<uint32_t>(frame[3]); // SFD_NONBLOCK=0x800
        uint64_t mask = 0;
        if (mask_va != 0 && ensure_user(me, mask_va, 8))
            mask = *reinterpret_cast<const uint64_t *>(mask_va);
        if (fd >= 0) { // update an existing signalfd's mask
            if (static_cast<uint32_t>(fd) >= kMaxFds || me->fds[fd].kind != FdKind::signalfd) {
                frame[0] = static_cast<uint64_t>(-9); return;
            }
            SignalFd *s = signalfd_at(me->fds[fd].sock_idx);
            if (s == nullptr) { frame[0] = static_cast<uint64_t>(-9); return; }
            s->mask = mask;
            frame[0] = static_cast<uint64_t>(fd);
            return;
        }
        const int sid = signalfd_alloc(mask, (flags & 0x800) != 0);
        if (sid < 0) { frame[0] = static_cast<uint64_t>(-23); return; }
        int newfd = -1;
        for (uint32_t i = 0; i < kMaxFds; ++i)
            if (me->fds[i].kind == FdKind::closed) { newfd = static_cast<int>(i); break; }
        if (newfd < 0) { signalfd_close(sid); frame[0] = static_cast<uint64_t>(-24); return; }
        me->fds[newfd] = Fd{};
        me->fds[newfd].kind = FdKind::signalfd;
        me->fds[newfd].sock_idx = sid;
        frame[0] = static_cast<uint64_t>(newfd);
        return;
    }
    case 26 /*inotify_init1*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint32_t flags = static_cast<uint32_t>(frame[0]); // IN_NONBLOCK=0x800
        const int iid = inotify_alloc((flags & 0x800) != 0);
        if (iid < 0) { frame[0] = static_cast<uint64_t>(-24); return; } // -EMFILE
        int newfd = -1;
        for (uint32_t i = 0; i < kMaxFds; ++i)
            if (me->fds[i].kind == FdKind::closed) { newfd = static_cast<int>(i); break; }
        if (newfd < 0) { inotify_close(iid); frame[0] = static_cast<uint64_t>(-24); return; }
        me->fds[newfd] = Fd{};
        me->fds[newfd].kind = FdKind::inotify;
        me->fds[newfd].sock_idx = iid;
        frame[0] = static_cast<uint64_t>(newfd);
        return;
    }
    case 27 /*inotify_add_watch*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t path_va = frame[1];
        const uint32_t mask = static_cast<uint32_t>(frame[2]);
        if (fd >= kMaxFds || me->fds[fd].kind != FdKind::inotify) { frame[0] = static_cast<uint64_t>(-9); return; }
        if (!ensure_user(me, path_va, 1)) { frame[0] = static_cast<uint64_t>(-14); return; }
        Inotify *in = inotify_at(me->fds[fd].sock_idx);
        if (in == nullptr) { frame[0] = static_cast<uint64_t>(-9); return; }
        char abs[kCwdCap];
        if (!resolve_at(me, kAtFdCwd, reinterpret_cast<const char *>(path_va), abs, kCwdCap)) {
            frame[0] = static_cast<uint64_t>(-2); return; // -ENOENT
        }
        // The watch stores the path in kInotifyPathCap bytes; reject one that
        // would be truncated rather than silently watching the wrong path (a
        // truncated stored path never matches kazakiri_notify's full dirname, so
        // events would be lost). Mirrors Linux's NAME_MAX/-ENAMETOOLONG.
        uint32_t plen = 0; while (abs[plen] != 0) ++plen;
        if (plen >= kInotifyPathCap) { frame[0] = static_cast<uint64_t>(-36); return; } // -ENAMETOOLONG
        // Linux requires the watched path to exist (-ENOENT otherwise).
        LateranEntry st;
        if (!lateran_is_dir(abs) && !lateran_stat(abs, &st)) {
            frame[0] = static_cast<uint64_t>(-2); return;
        }
        const int wd = inotify_add(in, abs, mask);
        frame[0] = (wd >= 0) ? static_cast<uint64_t>(wd) : static_cast<uint64_t>(-28); // -ENOSPC
        return;
    }
    case 28 /*inotify_rm_watch*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const int wd = static_cast<int>(frame[1]);
        if (fd >= kMaxFds || me->fds[fd].kind != FdKind::inotify) { frame[0] = static_cast<uint64_t>(-9); return; }
        Inotify *in = inotify_at(me->fds[fd].sock_idx);
        if (in == nullptr) { frame[0] = static_cast<uint64_t>(-9); return; }
        frame[0] = (inotify_rm(in, wd) == 0) ? 0 : static_cast<uint64_t>(-22); // -EINVAL
        return;
    }
    case 20 /*epoll_create1*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const int eid = epoll_alloc();
        if (eid < 0) { frame[0] = static_cast<uint64_t>(-23); return; }
        int newfd = -1;
        for (uint32_t i = 0; i < kMaxFds; ++i) {
            if (me->fds[i].kind == FdKind::closed) { newfd = static_cast<int>(i); break; }
        }
        if (newfd < 0) { epoll_close(eid); frame[0] = static_cast<uint64_t>(-24); return; }
        me->fds[newfd] = Fd{};
        me->fds[newfd].kind = FdKind::epoll;
        me->fds[newfd].sock_idx = eid;
        frame[0] = static_cast<uint64_t>(newfd);
        return;
    }
    case 21 /*epoll_ctl*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t epfd = frame[0];
        const uint64_t op = frame[1];
        const int32_t target = static_cast<int32_t>(frame[2]);
        const uint64_t ev_va = frame[3];
        if (epfd >= kMaxFds || me->fds[epfd].kind != FdKind::epoll) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        Epoll *ep = epoll_at(me->fds[epfd].sock_idx);
        if (ep == nullptr) { frame[0] = static_cast<uint64_t>(-9); return; }
        if (op == 1 /*EPOLL_CTL_ADD*/ || op == 3 /*EPOLL_CTL_MOD*/) {
            if (!ensure_user(me, ev_va, 12)) { frame[0] = static_cast<uint64_t>(-14); return; }
            const uint8_t *p = reinterpret_cast<const uint8_t *>(ev_va);
            const uint32_t events = *reinterpret_cast<const uint32_t *>(p);
            const uint64_t data = *reinterpret_cast<const uint64_t *>(p + 4);
            // Find existing slot for target (MOD/dup ADD) or first free.
            int slot = -1;
            for (uint32_t i = 0; i < kEpollMaxRegs; ++i) {
                if (ep->regs[i].fd == target) { slot = static_cast<int>(i); break; }
            }
            if (slot < 0) {
                for (uint32_t i = 0; i < kEpollMaxRegs; ++i) {
                    if (ep->regs[i].fd < 0) { slot = static_cast<int>(i); break; }
                }
            }
            if (slot < 0) { frame[0] = static_cast<uint64_t>(-28); return; } // -ENOSPC
            ep->regs[slot].fd = target;
            ep->regs[slot].events = events;
            ep->regs[slot].data = data;
            frame[0] = 0;
            return;
        }
        if (op == 2 /*EPOLL_CTL_DEL*/) {
            for (uint32_t i = 0; i < kEpollMaxRegs; ++i) {
                if (ep->regs[i].fd == target) { ep->regs[i] = EpollReg{}; break; }
            }
            frame[0] = 0;
            return;
        }
        frame[0] = static_cast<uint64_t>(-22);
        return;
    }
    case 22 /*epoll_pwait*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t epfd = frame[0];
        const uint64_t out_va = frame[1];
        const uint64_t maxev = frame[2];
        const int64_t timeout_ms = static_cast<int64_t>(frame[3]); // -1 = infinite
        if (epfd >= kMaxFds || me->fds[epfd].kind != FdKind::epoll) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        Epoll *ep = epoll_at(me->fds[epfd].sock_idx);
        if (ep == nullptr) { frame[0] = static_cast<uint64_t>(-9); return; }
        const int pidx = esper_running_index();
        // epoll_pwait's 5th arg (frame[4]) is a sigmask swapped in for the wait,
        // exactly like ppoll's. Read it ONCE on first entry and cache across
        // park-replays (see the long note on ppoll: re-reading per replay raced
        // SIGCHLD delivery and hung logout).
        uint64_t eff_mask;
        if (me->poll_armed) {
            eff_mask = me->poll_eff_mask;
        } else {
            eff_mask = me->sig_mask;
            if (frame[4] != 0 && ensure_user(me, frame[4], 8))
                eff_mask = *reinterpret_cast<const uint64_t *>(frame[4]);
            me->poll_eff_mask = eff_mask;
        }
        // Cap events written: a huge maxevents would overflow maxev*12 (#3 class).
        const uint64_t evcap = (maxev < kEpollMaxRegs) ? maxev : kEpollMaxRegs;
        for (;;) {
            // Snapshot poll gen BEFORE scanning so a producer that fires in the
            // check-then-park window is caught by poll_gen_changed (lost-wakeup
            // free, mirrors ppoll). timerfd/signalfd are time-/signal-driven and
            // don't bump the gen -- the 100 Hz network_tick EpollWait re-kick is
            // what re-checks them, plus the deadline below bounds the wait.
            const uint32_t gen0 = linux_poll_gen();
            // A signal deliverable under eff_mask interrupts the wait with -EINTR
            // and runs its handler (mirrors ppoll's reaper path).
            {
                const uint64_t deliverable = me->sig_pending & ~eff_mask;
                if (deliverable != 0) {
                    const int sig = __builtin_ctzll(deliverable) + 1;
                    frame[0] = static_cast<uint64_t>(-4); // -EINTR
                    if (linux_deliver_signal(me, sig, frame)) {
                        me->poll_armed = false;
                        return; // handler entered; yields -EINTR via sigreturn
                    }
                    frame[0] = epfd; // delivery failed: restore arg0 for park-replay
                    const uint64_t h = me->sig_handler[sig];
                    if (h == 0 /*SIG_DFL*/ || h == 1 /*SIG_IGN*/)
                        me->sig_pending &= ~(1ULL << (sig - 1));
                }
            }
            // Scan registrations through the single readiness source of truth
            // (linux_fd_revents) so timerfd/signalfd/every fd kind are handled
            // identically to poll() -- the old hand-rolled table here silently
            // omitted timerfd, so an epoll on a timerfd never woke.
            uint64_t count = 0;
            if (out_va != 0 && out_va < 0x8000000000ULL &&
                ensure_user(me, out_va, evcap * 12)) {
                uint8_t *out = reinterpret_cast<uint8_t *>(out_va);
                for (uint32_t i = 0; i < kEpollMaxRegs && count < maxev; ++i) {
                    const EpollReg &rg = ep->regs[i];
                    if (rg.fd < 0 || static_cast<uint32_t>(rg.fd) >= kMaxFds) continue;
                    if (me->fds[rg.fd].kind == FdKind::closed) continue;
                    uint32_t re = linux_fd_revents(me, rg.fd);
                    re &= rg.events | 0x18u; // EPOLLERR|EPOLLHUP reported regardless
                    if (re != 0) {
                        *reinterpret_cast<uint32_t *>(out + count * 12) = re;
                        *reinterpret_cast<uint64_t *>(out + count * 12 + 4) = rg.data;
                        ++count;
                    }
                }
            }
            if (count > 0) { me->poll_armed = false; frame[0] = count; return; }
            if (timeout_ms == 0) { me->poll_armed = false; frame[0] = 0; return; }
            // Arm the timeout once; it must survive park-replays (each wake re-runs
            // this syscall from the top). last_order_ticks() is 100 Hz = 10 ms/tick.
            if (!me->poll_armed) {
                me->poll_armed = true;
                me->poll_deadline = (timeout_ms < 0) ? 0 /*infinite*/
                    : (last_order_ticks() + static_cast<uint64_t>(timeout_ms) / 10 + 1);
            }
            if (me->poll_deadline != 0 && last_order_ticks() >= me->poll_deadline) {
                me->poll_armed = false; frame[0] = 0; return; // timed out
            }
            if (pidx < 0) { me->poll_armed = false; frame[0] = 0; return; }
            // Park only if no producer fired since gen0 and no eff_mask-deliverable
            // signal is pending (re-checked atomically under g_esper_lock). The
            // 100 Hz network_tick wildcard EpollWait wake replays us so a timerfd
            // deadline that passes while parked is noticed on the next tick.
            if (ipc_park_unless_ready(pidx, Esper::IpcWaitKind::EpollWait,
                                      static_cast<int>(gen0), frame,
                                      poll_gen_changed, eff_mask)) {
                return; // parked; svc replays the epoll_pwait on wake
            }
            // gen changed in the park window -> re-scan (loop).
        }
    }
    case 199 /*socketpair*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t domain = frame[0];
        const uint64_t type = frame[1] & 0x7f;
        const uint64_t out_va = frame[3];
        if (domain != 1 /*AF_UNIX*/ || type != 1 /*SOCK_STREAM*/) {
            frame[0] = static_cast<uint64_t>(-93); return;
        }
        if (!ensure_user(me, out_va, 8)) { frame[0] = static_cast<uint64_t>(-14); return; }
        int pair[2] = {-1, -1};
        if (!inc_socketpair(pair)) { frame[0] = static_cast<uint64_t>(-23); return; }
        int fd_a = -1, fd_b = -1;
        for (uint32_t i = 0; i < kMaxFds; ++i) {
            if (me->fds[i].kind != FdKind::closed) continue;
            if (fd_a < 0) { fd_a = static_cast<int>(i); continue; }
            fd_b = static_cast<int>(i); break;
        }
        if (fd_a < 0 || fd_b < 0) {
            inc_close(pair[0]); inc_close(pair[1]);
            frame[0] = static_cast<uint64_t>(-24); return;
        }
        me->fds[fd_a] = Fd{}; me->fds[fd_a].kind = FdKind::unix_sock; me->fds[fd_a].sock_idx = pair[0];
        me->fds[fd_b] = Fd{}; me->fds[fd_b].kind = FdKind::unix_sock; me->fds[fd_b].sock_idx = pair[1];
        int32_t *out = reinterpret_cast<int32_t *>(out_va);
        out[0] = fd_a; out[1] = fd_b;
        frame[0] = 0;
        return;
    }
    case 208 /*setsockopt*/:
        // Accept-and-ignore: SO_REUSEADDR / TCP_NODELAY / SO_KEEPALIVE etc.
        // don't change our (single-bind, no-Nagle) behaviour, so silently
        // succeeding is correct for sshd's setup.
        frame[0] = 0;
        return;
    case 209 /*getsockopt*/: {
        // Must WRITE *optval (callers read it). Common: SO_ERROR (level
        // SOL_SOCKET=1, opt=4) after connect -> report 0. SPECIAL CASE:
        // IP_OPTIONS (level IPPROTO_IP=0, opt=4) -- sshd queries this to detect
        // IP source-routing and DISCONNECTS pre-auth if optlen != 0. A normal
        // TCP connection has no IP options, so report zero length.
        const uint64_t level = frame[1];
        const uint64_t optname = frame[2];
        const uint64_t optval_va = frame[3];
        const uint64_t optlen_va = frame[4];
        if (optlen_va == 0 || !ensure_user(me, optlen_va, 4)) { frame[0] = 0; return; }
        if (level == 0 /*IPPROTO_IP*/ && optname == 4 /*IP_OPTIONS*/) {
            *reinterpret_cast<uint32_t *>(optlen_va) = 0; // no IP options present
            frame[0] = 0;
            return;
        }
        uint32_t cap = *reinterpret_cast<uint32_t *>(optlen_va);
        if (cap > 4) cap = 4;
        if (optval_va != 0 && ensure_user(me, optval_va, cap)) {
            for (uint32_t i = 0; i < cap; ++i)
                reinterpret_cast<uint8_t *>(optval_va)[i] = 0;
        }
        *reinterpret_cast<uint32_t *>(optlen_va) = cap;
        frame[0] = 0;
        return;
    }
    case 210 /*shutdown*/:
        frame[0] = 0;
        return;

    // ---- OpenSSH bring-up: SCM_RIGHTS fd passing + sandbox/chroot stubs -----
    // sendmsg/recvmsg parse the kernel's user_msghdr ABI (msg_iovlen and
    // msg_controllen are 8-byte size_t at 0x18/0x28; cmsg_len is 8-byte at
    // off 0, level/type at 8/12, CMSG_DATA at +16). musl's user structs are
    // binary-compatible on little-endian once their pad words are zeroed,
    // which every sender (OpenSSH's mm_send_fd) does via memset. Flags
    // (MSG_NOSIGNAL/MSG_DONTWAIT) don't change our cooperative behaviour.
    case 211 /*sendmsg*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t msg_va = frame[1];
        if (fd >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const FdKind k = me->fds[fd].kind;
        if (k != FdKind::socket && k != FdKind::unix_sock) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        if (!ensure_user(me, msg_va, 56)) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint8_t *mh = reinterpret_cast<const uint8_t *>(msg_va);
        const uint64_t iov_va  = *reinterpret_cast<const uint64_t *>(mh + 0x10);
        const uint64_t iovlen  = *reinterpret_cast<const uint64_t *>(mh + 0x18);
        const uint64_t ctrl_va = *reinterpret_cast<const uint64_t *>(mh + 0x20);
        const uint64_t ctrllen = *reinterpret_cast<const uint64_t *>(mh + 0x28);

        // Gather the iovec payload into a staging buffer (monitor messages are
        // small; a short write just makes the caller send the rest).
        uint8_t stage[2048];
        uint32_t total = 0;
        if (iov_va != 0 && iovlen > 0 && iovlen <= 1024 && ensure_user(me, iov_va, iovlen * 16)) {
            const uint8_t *iov = reinterpret_cast<const uint8_t *>(iov_va);
            for (uint64_t i = 0; i < iovlen && total < sizeof(stage); ++i) {
                const uint64_t base = *reinterpret_cast<const uint64_t *>(iov + i * 16);
                const uint64_t len  = *reinterpret_cast<const uint64_t *>(iov + i * 16 + 8);
                if (base == 0 || len == 0 || !ensure_user(me, base, len)) continue;
                const uint8_t *src = reinterpret_cast<const uint8_t *>(base);
                for (uint64_t j = 0; j < len && total < sizeof(stage); ++j)
                    stage[total++] = src[j];
            }
        }

        // SCM_RIGHTS control message (AF_UNIX only). Snapshot each named fd,
        // take a backend ref so it survives the sender close()ing it, and queue
        // it on the peer *before* the bytes go out -- cooperative scheduling
        // keeps attach+send atomic, so a receiver that sees the bytes will
        // already find the fds parked.
        if (k == FdKind::unix_sock && ctrl_va != 0 && ctrllen >= 16 &&
            ensure_user(me, ctrl_va, ctrllen)) {
            const uint8_t *cm = reinterpret_cast<const uint8_t *>(ctrl_va);
            const uint64_t cmsg_len  = *reinterpret_cast<const uint64_t *>(cm + 0);
            const int32_t cmsg_level = *reinterpret_cast<const int32_t *>(cm + 8);
            const int32_t cmsg_type  = *reinterpret_cast<const int32_t *>(cm + 12);
            if (cmsg_level == 1 /*SOL_SOCKET*/ && cmsg_type == 1 /*SCM_RIGHTS*/ &&
                cmsg_len >= 16 && cmsg_len <= ctrllen) {
                const uint32_t nfd = static_cast<uint32_t>((cmsg_len - 16) / 4);
                const int32_t *fdarr = reinterpret_cast<const int32_t *>(cm + 16);
                Fd passed[kChannelScmMax];
                uint32_t npass = 0;
                for (uint32_t i = 0; i < nfd && npass < kChannelScmMax; ++i) {
                    const int sfd = fdarr[i];
                    if (sfd < 0 || static_cast<uint32_t>(sfd) >= kMaxFds) continue;
                    if (me->fds[sfd].kind == FdKind::closed) continue;
                    passed[npass] = me->fds[sfd];
                    linux_ref_fd_backend(passed[npass]);
                    ++npass;
                }
                const uint32_t moved = inc_attach_fds(me->fds[fd].sock_idx, passed, npass);
                for (uint32_t i = moved; i < npass; ++i)
                    linux_release_fd_backend(passed[i]); // peer queue full -> undo
            }
        }

        int64_t r = (k == FdKind::unix_sock)
                        ? inc_send(me->fds[fd].sock_idx, stage, total)
                        : antenna_tcp_send(me->fds[fd].sock_idx, stage, total);
        frame[0] = static_cast<uint64_t>(r);
        return;
    }
    case 212 /*recvmsg*/: {
        if (me == nullptr) { frame[0] = static_cast<uint64_t>(-14); return; }
        const uint64_t fd = frame[0];
        const uint64_t msg_va = frame[1];
        if (fd >= kMaxFds) { frame[0] = static_cast<uint64_t>(-9); return; }
        const FdKind k = me->fds[fd].kind;
        if (k != FdKind::socket && k != FdKind::unix_sock) {
            frame[0] = static_cast<uint64_t>(-9); return;
        }
        if (!ensure_user(me, msg_va, 56)) { frame[0] = static_cast<uint64_t>(-14); return; }
        uint8_t *mh = reinterpret_cast<uint8_t *>(msg_va);
        const uint64_t iov_va  = *reinterpret_cast<const uint64_t *>(mh + 0x10);
        const uint64_t iovlen  = *reinterpret_cast<const uint64_t *>(mh + 0x18);
        const uint64_t ctrl_va = *reinterpret_cast<const uint64_t *>(mh + 0x20);
        const uint64_t ctrllen = *reinterpret_cast<const uint64_t *>(mh + 0x28);

        uint64_t cap = 0;
        if (iov_va != 0 && iovlen > 0 && iovlen <= 1024 && ensure_user(me, iov_va, iovlen * 16)) {
            const uint8_t *iov = reinterpret_cast<const uint8_t *>(iov_va);
            for (uint64_t i = 0; i < iovlen; ++i)
                cap += *reinterpret_cast<const uint64_t *>(iov + i * 16 + 8);
        }
        uint8_t stage[2048];
        const uint32_t want = cap > sizeof(stage) ? sizeof(stage)
                                                  : static_cast<uint32_t>(cap);
        int64_t r = 0;
        if (want > 0) {
            // Non-blocking recv first; if the ring is empty, PARK (yield to the
            // scheduler) rather than busy-spinning a blocking recv with IRQs
            // masked. On a uniprocessor the peer that fills the ring (e.g. the
            // OpenSSH privsep monitor<->child socketpair) cannot be scheduled
            // while we spin, and the masked timer's frozen tick makes the
            // blocking recv's deadline never expire -> a hard deadlock that
            // also freezes the 100 Hz NIC-drain tick. Mirrors read()'s path.
            const int sidx = me->fds[fd].sock_idx;
            bool peer_gone = false;
            r = (k == FdKind::unix_sock)
                    ? inc_try_recv(sidx, stage, want, &peer_gone)
                    : antenna_tcp_try_recv(sidx, stage, want, &peer_gone);
            if (r == 0 && !peer_gone) {
                const int eidx = esper_running_index();
                const Esper::IpcWaitKind wk = (k == FdKind::unix_sock)
                    ? Esper::IpcWaitKind::ChannelRecv
                    : Esper::IpcWaitKind::AntennaRecv;
                if (eidx >= 0 && linux_ipc_park(eidx, wk, sidx, frame) >= 0) {
                    return; // svc replays recvmsg when the peer wakes us
                }
            }
        }
        if (r < 0) { frame[0] = static_cast<uint64_t>(r); return; }

        // Scatter received bytes back into the iovec.
        uint32_t off = 0;
        if (r > 0 && iov_va != 0) {
            const uint8_t *iov = reinterpret_cast<const uint8_t *>(iov_va);
            for (uint64_t i = 0; i < iovlen && off < static_cast<uint32_t>(r); ++i) {
                const uint64_t base = *reinterpret_cast<const uint64_t *>(iov + i * 16);
                const uint64_t len  = *reinterpret_cast<const uint64_t *>(iov + i * 16 + 8);
                if (base == 0 || len == 0 || !ensure_user(me, base, len)) continue;
                uint8_t *dst = reinterpret_cast<uint8_t *>(base);
                for (uint64_t j = 0; j < len && off < static_cast<uint32_t>(r); ++j)
                    dst[j] = stage[off++];
            }
        }

        // Deliver in-flight SCM_RIGHTS fds: move them out of the channel, install
        // each into our fd table (ownership transfers, no extra ref), and write
        // the new fd numbers + a SCM_RIGHTS cmsg header back to msg_control.
        uint64_t out_ctrllen = 0;
        if (k == FdKind::unix_sock && ctrl_va != 0 && ctrllen >= 16 &&
            ensure_user(me, ctrl_va, ctrllen)) {
            uint32_t room = static_cast<uint32_t>((ctrllen - 16) / 4);
            if (room > kChannelScmMax) room = kChannelScmMax;
            Fd got[kChannelScmMax];
            const uint32_t ngot = inc_take_fds(me->fds[fd].sock_idx, got, room);
            if (ngot > 0) {
                uint8_t *cm = reinterpret_cast<uint8_t *>(ctrl_va);
                int32_t *fdarr = reinterpret_cast<int32_t *>(cm + 16);
                uint32_t installed = 0;
                for (uint32_t i = 0; i < ngot; ++i) {
                    const int nfd = linux_install_fd(me, got[i]);
                    if (nfd < 0) { linux_release_fd_backend(got[i]); continue; }
                    fdarr[installed++] = nfd;
                }
                if (installed > 0) {
                    const uint64_t clen = 16 + static_cast<uint64_t>(installed) * 4;
                    *reinterpret_cast<uint64_t *>(cm + 0) = clen;  // cmsg_len = CMSG_LEN
                    *reinterpret_cast<int32_t *>(cm + 8)  = 1;     // SOL_SOCKET
                    *reinterpret_cast<int32_t *>(cm + 12) = 1;     // SCM_RIGHTS
                    // msg_controllen must be CMSG_SPACE (header + 8-aligned data),
                    // NOT CMSG_LEN. OpenSSH mm_receive_fd checks it == CMSG_SPACE
                    // exactly; returning CMSG_LEN (20 for 1 fd) made it reject the
                    // pty fd with "receive fds failed". CMSG_SPACE(n*4)=16+align8(n*4).
                    out_ctrllen = 16 + ((static_cast<uint64_t>(installed) * 4 + 7) & ~uint64_t(7));
                }
            }
        }
        *reinterpret_cast<uint64_t *>(mh + 0x28) = out_ctrllen; // msg_controllen
        *reinterpret_cast<uint32_t *>(mh + 0x30) = 0;           // msg_flags
        frame[0] = static_cast<uint64_t>(r);
        return;
    }
    case 167 /*prctl*/: {
        // No capabilities/dumpable/seccomp enforcement, but sshd sets these at
        // startup and treats failure as fatal. Honour PR_SET_NAME (so `ps`
        // reflects it); accept everything else silently. PR_SET_SECCOMP(22)
        // would arrive here too -- we have no seccomp, so OpenSSH must be built
        // with the rlimit sandbox; accepting keeps the rlimit path's prctls
        // (DUMPABLE/KEEPCAPS/NO_NEW_PRIVS) from aborting it.
        if (frame[0] == 15 /*PR_SET_NAME*/ && me != nullptr) {
            const uint64_t va = frame[1];
            // PR_SET_NAME's buffer is TASK_COMM_LEN (16) bytes; we validate and
            // read at most that many. me->name is wider (24), so the loop bound
            // must be the VALIDATED 16 -- not sizeof(me->name)-1 (=23), which would
            // read up to 7 bytes past the ensure_user'd region for a name with no
            // NUL in the first 16 bytes (EL1 fault / same-process info leak).
            constexpr uint32_t kTaskCommLen = 16;
            if (ensure_user(me, va, kTaskCommLen)) {
                const char *src = reinterpret_cast<const char *>(va);
                uint32_t i = 0;
                for (; i < kTaskCommLen - 1 && i < sizeof(me->name) - 1 && src[i]; ++i)
                    me->name[i] = src[i];
                me->name[i] = 0;
            }
        }
        frame[0] = 0;
        return;
    }
    case 51 /*chroot*/:
        // Index doesn't enforce filesystem containment yet; sshd's privsep
        // child chroots to /var/empty and aborts on failure, so accept.
        frame[0] = 0;
        return;

    default:
        // During bring-up, announce anything we haven't implemented so it's
        // obvious which syscall musl is blocked on. Returns -ENOSYS.
        district::write("[Index] ENOSYS syscall ");
        district::dec(nr);
        district::write("\n");
        frame[0] = static_cast<uint64_t>(kEnosys);
        return;
    }
}

} // namespace index
