// Minimal C pthread mutex test -- bypasses libc++ entirely. If even this
// hangs, the bug is in musl/Index. If this works, the bug is in libc++.
// Uses raw write() (not printf) to avoid pulling in musl's stdio init,
// which in zig 0.16 references statx (a symbol Alpine 3.18 musl 1.2.4
// hasn't yet exported).
#include <pthread.h>
#include <unistd.h>
#include <string.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static int counter = 0;

static void puts_raw(const char *s) { write(1, s, strlen(s)); }

static void put_dec(int n) {
    char buf[16]; int p = 0;
    if (n < 0) { write(1, "-", 1); n = -n; }
    if (n == 0) { write(1, "0", 1); return; }
    while (n && p < (int)sizeof(buf)) { buf[p++] = '0' + (n % 10); n /= 10; }
    while (p--) write(1, &buf[p], 1);
}

static void *worker(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < 500; ++i) {
        pthread_mutex_lock(&m);
        ++counter;
        pthread_mutex_unlock(&m);
    }
    puts_raw("thread "); put_dec(id); puts_raw(" done\n");
    return NULL;
}

int main(void) {
    pthread_t t1, t2;
    puts_raw("[mutextest start]\n");
    pthread_create(&t1, NULL, worker, (void *)1);
    puts_raw("spawned t1\n");
    pthread_create(&t2, NULL, worker, (void *)2);
    puts_raw("spawned t2\n");
    pthread_join(t1, NULL);
    puts_raw("joined t1\n");
    pthread_join(t2, NULL);
    puts_raw("joined t2\n");
    puts_raw("[mutextest end] counter="); put_dec(counter);
    puts_raw(" (expected 1000)\n");
    return counter == 1000 ? 0 : 1;
}
