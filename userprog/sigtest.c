/* sigtest.c: install a SIGUSR1 handler with musl, raise the signal, and prove
 * the handler ran and control returned to main afterward. Exercises the
 * kernel's rt_sigaction + rt_sigframe delivery + rt_sigreturn path. */

#include <stdio.h>
#include <signal.h>

static volatile int caught = 0;

static void on_usr1(int sig) {
    caught = sig;
    printf("  handler: caught signal %d\n", sig);
}

int main(void) {
    signal(SIGUSR1, on_usr1);
    printf("before raise, caught=%d\n", caught);
    raise(SIGUSR1);
    printf("after raise, caught=%d\n", caught);
    return caught == SIGUSR1 ? 0 : 1;
}
