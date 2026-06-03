/* forktest.c: classic Linux fork()/waitpid(). The child prints and exits with
 * a known code; the parent waits, reaps it, and reports the decoded status.
 * Exercises the kernel's clone(no CLONE_VM) address-space copy + wait4. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    printf("parent: before fork (pid=%d)\n", (int)getpid());
    pid_t c = fork();
    if (c == 0) {
        printf("  child: running (pid=%d), exiting 42\n", (int)getpid());
        _exit(42);
    }
    int status = 0;
    pid_t r = waitpid(c, &status, 0);
    printf("parent: reaped pid=%d, WIFEXITED=%d code=%d\n",
           (int)r, WIFEXITED(status), WEXITSTATUS(status));
    return 0;
}
