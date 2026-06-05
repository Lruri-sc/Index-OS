// Functional test for PTRACE_SYSCALL ("Mental Out" syscall-stops) + ATTACH path.
// A parent traces a child, steps it by syscall (entry+exit stops) with
// PTRACE_O_TRACESYSGOOD, and confirms the syscall-stops arrive. Built static-musl,
// baked as /bin/ptsyscalltest. Prints PTSYS_OK on success.
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>

int main(void) {
    pid_t child = fork();
    if (child == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);     // initial stop
        getpid();           // an observable syscall -> entry + exit stop
        getpid();
        _exit(7);
    }

    int status;
    if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status) ||
        WSTOPSIG(status) != SIGSTOP) {
        printf("PTSYS_FAIL stop status=0x%x\n", status);
        return 1;
    }
    // TRACESYSGOOD: syscall-stops report SIGTRAP|0x80 so we can tell them apart.
    ptrace(PTRACE_SETOPTIONS, child, 0, (void *)1 /*PTRACE_O_TRACESYSGOOD*/);

    int sysstops = 0;
    for (int i = 0; i < 12; ++i) {
        if (ptrace(PTRACE_SYSCALL, child, 0, 0) != 0) break;
        if (waitpid(child, &status, 0) != child) break;
        if (WIFEXITED(status)) {
            printf("PTSYS child exited code=%d after %d syscall-stops\n",
                   WEXITSTATUS(status), sysstops);
            break;
        }
        if (WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80)) {
            ++sysstops; // a genuine syscall-stop (entry or exit)
        }
    }

    if (sysstops >= 2) printf("PTSYS_OK stops=%d\n", sysstops);
    else printf("PTSYS_FAIL stops=%d\n", sysstops);
    return 0;
}
