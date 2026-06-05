// Test the ptrace "Mental Out" tracer-death cleanup: if a tracer exits while
// its tracee is in a ptrace-stop, the kernel must detach + resume the tracee so
// it isn't orphaned (stuck parked forever). 3 processes: main -> tracer T ->
// tracee C. T waits for C's SIGSTOP stop, then exits WITHOUT PTRACE_CONT. The
// fix resumes C, which then prints ORPHAN_RESUMED. Built static-musl, baked as
// /bin/ptraceorphan. Harness greps for ORPHAN_RESUMED.
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>

int main(void) {
    pid_t t = fork();
    if (t == 0) {
        // Tracer.
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0); // tracer = our parent (this T)
            raise(SIGSTOP);                  // ptrace-stop; tracer never CONTs us
            // Reaching here means the kernel resumed us after the tracer died.
            printf("ORPHAN_RESUMED\n");
            fflush(stdout);
            _exit(0);
        }
        int st;
        waitpid(c, &st, 0);  // observe C's SIGSTOP ptrace-stop
        _exit(0);            // tracer dies while C is still stopped -> C orphaned
    }
    int st;
    waitpid(t, &st, 0);      // reap the tracer
    printf("TRACER_EXITED\n");
    fflush(stdout);
    // Spin a bit so the now-orphaned-and-resumed tracee gets to run + print.
    for (volatile long i = 0; i < 80000000L; ++i) { }
    printf("ORPHAN_DONE\n");
    return 0;
}
