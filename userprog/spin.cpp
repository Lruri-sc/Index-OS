// spin: a CPU-bound EL0 program that NEVER yields. It prints its pid a few
// times with a busy-compute delay in between and then exits. Run two copies
// with `coexec SPIN.ELF SPIN.ELF`: if their digits interleave even though
// neither yields, the timer interrupt is preempting them -- preemptive
// multitasking. PIE, no relocations (only svc + immediates).

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

extern "C" [[noreturn]] void _start() {
    const long pid = sys_getpid();
    for (long i = 0; i < 6; ++i) {
        // Busy-compute long enough to span several timer ticks, never yielding.
        for (volatile long d = 0; d < 60000000; d = d + 1) {
        }
        sys_putc(static_cast<char>('0' + (pid % 10)));
    }
    sys_putc('\n');
    sys_exit(0);
}
