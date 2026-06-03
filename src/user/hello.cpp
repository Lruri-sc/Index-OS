// A tiny user-space program that runs at EL0. It may touch nothing in the
// kernel directly -- its only channel is the `svc` instruction (system calls).
// It is linked into the .user_text section, which Teleport maps EL0-executable;
// the rest of the kernel stays EL0-inaccessible, so this code is genuinely
// unprivileged. To avoid needing EL0-readable .rodata, all output goes through
// SYS_putc with character immediates (no string literals, no data section).

namespace {

constexpr long kSysPutc = 1;
constexpr long kSysGetpid = 2;
constexpr long kSysExit = 3;

void sys_putc(char c) {
    register long x0 asm("x0") = static_cast<long>(c);
    register long x8 asm("x8") = kSysPutc;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
}

long sys_getpid() {
    register long x0 asm("x0");
    register long x8 asm("x8") = kSysGetpid;
    asm volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

[[noreturn]] void sys_exit(long code) {
    register long x0 asm("x0") = code;
    register long x8 asm("x8") = kSysExit;
    asm volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
    for (;;) {
    }
}

} // namespace

extern "C" void user_entry() {
    sys_putc('H');
    sys_putc('e');
    sys_putc('l');
    sys_putc('l');
    sys_putc('o');
    sys_putc(' ');
    sys_putc('f');
    sys_putc('r');
    sys_putc('o');
    sys_putc('m');
    sys_putc(' ');
    sys_putc('E');
    sys_putc('L');
    sys_putc('0');
    sys_putc('!');
    sys_putc('\n');

    const long pid = sys_getpid();
    sys_putc('p');
    sys_putc('i');
    sys_putc('d');
    sys_putc('=');
    sys_putc(static_cast<char>('0' + (pid % 10)));
    sys_putc('\n');

    sys_exit(0);
}

// A second entry that deliberately violates the privilege boundary: it writes
// to a kernel-only address. EL0 has no access, so this faults -- the kernel
// should report it and reclaim control rather than die.
extern "C" void user_fault_entry() {
    volatile long *kernel_only = reinterpret_cast<volatile long *>(0xFFFFFF8040100000ULL);
    *kernel_only = 0; // permission fault at EL0
    sys_exit(0);
}
