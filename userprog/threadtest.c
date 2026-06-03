/* threadtest.c: create a POSIX thread, have it compute a value, join it, and
 * print the result. Exercises clone(CLONE_VM|CLONE_SETTLS|...) + futex
 * (pthread mutex/join) + set_tid_address/clear_child_tid + per-thread TLS. */

#include <stdio.h>
#include <pthread.h>

static void *worker(void *arg) {
    long n = (long)arg;
    long sum = 0;
    for (long i = 1; i <= n; i++) sum += i;
    printf("  worker thread: sum(1..%ld) = %ld\n", n, sum);
    return (void *)sum;
}

int main(void) {
    pthread_t t;
    printf("main: creating thread\n");
    pthread_create(&t, NULL, worker, (void *)100);
    void *ret = NULL;
    pthread_join(t, &ret);
    printf("main: joined, thread returned %ld\n", (long)ret);
    return 0;
}
