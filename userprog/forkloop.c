/* forkloop.c: fork + write-to-memory (trigger CoW) + exit, in a loop. Each
 * child dirties a page (forcing a copy-on-write) then exits; the parent reaps
 * it. If physical pages are reclaimed on exit, the free-page count stays flat
 * across many iterations instead of leaking. Run it, then check `tree` in the
 * kernel shell -- the available page count should return to its baseline. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    const int N = 20;
    char *buf = malloc(8192);
    for (int i = 0; i < N; i++) {
        pid_t c = fork();
        if (c == 0) {
            // Child: dirty the inherited buffer (copy-on-write) then exit.
            for (int j = 0; j < 8192; j++) buf[j] = (char)(i + j);
            _exit(i & 0x7f);
        }
        int status = 0;
        waitpid(c, &status, 0);
    }
    free(buf);
    printf("forkloop: completed %d fork/wait cycles\n", N);
    return 0;
}
