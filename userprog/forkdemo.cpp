// forkdemo: a standalone EL0 program (loaded from the Lateran disk) that proves
// fork + wait + exit work. The parent forks; the child prints its pid and exits
// with code 7; the parent waits, reaps the child, and prints the pid + code it
// got back. Deterministic output, easy to check in a scripted run.

#include "usys.h"

extern "C" [[noreturn]] void _start() {
    uputs("forkdemo: before fork\n");

    const long pid = sys_fork();
    if (pid == 0) {
        // Child.
        uputs("  child : pid=");
        uputdec(sys_getpid());
        uputs(", exiting with code 7\n");
        sys_exit(7);
    }

    // Parent.
    uputs("  parent: forked child pid=");
    uputdec(pid);
    uputs("\n");

    long status = -1;
    const long reaped = sys_wait(&status);
    uputs("  parent: reaped pid=");
    uputdec(reaped);
    uputs(" code=");
    uputdec(status);
    uputs("\n");

    sys_exit(0);
}
