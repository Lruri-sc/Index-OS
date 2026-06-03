// init: a standalone EL0 user program, compiled to a position-independent ELF
// and loaded from the Lateran (FAT) disk. Loops a few times printing its pid
// and an iteration counter, yielding the CPU between iterations -- so when two
// copies run via `coexec`, their output interleaves, demonstrating cooperative
// concurrent processes. Now uses AcademyCity (ac_printf) instead of hand-rolled
// putc loops, demonstrating the user-side library payoff.

#include "academy_city.h"

extern "C" [[noreturn]] void _start() {
    const long pid = sys_getpid();
    for (long i = 0; i < 3; ++i) {
        ac_printf("pid=%d i=%d\n", pid, i);
        sys_yield(); // let another Esper run
    }
    sys_exit(0);
}
