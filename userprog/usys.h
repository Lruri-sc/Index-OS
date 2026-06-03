#pragma once
// User-side system call wrappers for arm-Index EL0 programs.
// ABI: svc #0, x8 = syscall number, x0..x2 = args, x0 = return.
// Freestanding + PIE: only svc, inline helpers, and PC-relative string literals
// (no libc, no global mutable data -> no dynamic relocations for the loader).

namespace {

constexpr long SYS_putc = 1;
constexpr long SYS_getpid = 2;
constexpr long SYS_exit = 3;
constexpr long SYS_yield = 4;
constexpr long SYS_fork = 5;
constexpr long SYS_exec = 6;
constexpr long SYS_wait = 7;
constexpr long SYS_write = 8;
constexpr long SYS_read = 9;
constexpr long SYS_open = 10;
constexpr long SYS_close = 11;
constexpr long SYS_pipe = 12;
constexpr long SYS_dup = 13;
constexpr long SYS_dup2 = 14;
constexpr long SYS_kill = 15;

// Signal numbers handled by Fortis931. MVP terminates the target for any of
// these (no handler registration yet); SIGKILL keeps its conventional meaning
// of "unblockable terminate" and SIGTERM is the default Komoe sends.
constexpr long SIGINT = 2;
constexpr long SIGKILL = 9;
constexpr long SIGTERM = 15;

inline long syscall0(long n) {
    register long x0 asm("x0");
    register long x8 asm("x8") = n;
    asm volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

inline long syscall1(long n, long a0) {
    register long x0 asm("x0") = a0;
    register long x8 asm("x8") = n;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

inline long syscall2(long n, long a0, long a1) {
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x8 asm("x8") = n;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

inline long syscall3(long n, long a0, long a1, long a2) {
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x8 asm("x8") = n;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

inline void sys_putc(char c) { syscall1(SYS_putc, static_cast<long>(static_cast<unsigned char>(c))); }
inline long sys_getpid() { return syscall0(SYS_getpid); }
inline void sys_yield() { syscall0(SYS_yield); }
[[noreturn]] inline void sys_exit(long code) {
    syscall1(SYS_exit, code);
    for (;;) {
    }
}
inline long sys_fork() { return syscall0(SYS_fork); }
inline long sys_exec(const char *path) { return syscall1(SYS_exec, reinterpret_cast<long>(path)); }
inline long sys_wait(long *status) { return syscall1(SYS_wait, reinterpret_cast<long>(status)); }
inline long sys_write(long fd, const char *buf, long len) {
    return syscall3(SYS_write, fd, reinterpret_cast<long>(buf), len);
}
inline long sys_read(long fd, char *buf, long len) {
    return syscall3(SYS_read, fd, reinterpret_cast<long>(buf), len);
}
inline long sys_open(const char *path) { return syscall1(SYS_open, reinterpret_cast<long>(path)); }
inline long sys_close(long fd) { return syscall1(SYS_close, fd); }
inline long sys_pipe(int fds[2]) { return syscall1(SYS_pipe, reinterpret_cast<long>(fds)); }
inline long sys_dup(long fd) { return syscall1(SYS_dup, fd); }
inline long sys_dup2(long oldfd, long newfd) { return syscall2(SYS_dup2, oldfd, newfd); }
inline long sys_kill(long pid, long sig) { return syscall2(SYS_kill, pid, sig); }

// --- tiny freestanding string helpers (no libc) ---
inline unsigned long ustrlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) {
        ++n;
    }
    return n;
}

inline void uputs(const char *s) { sys_write(1, s, static_cast<long>(ustrlen(s))); }

inline bool ustreq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

inline bool ustarts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return false;
        }
    }
    return true;
}

inline void uputdec(long v) {
    if (v < 0) {
        sys_putc('-');
        v = -v;
    }
    if (v == 0) {
        sys_putc('0');
        return;
    }
    char buf[24];
    int i = 0;
    while (v > 0) {
        buf[i++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        sys_putc(buf[--i]);
    }
}

} // namespace
