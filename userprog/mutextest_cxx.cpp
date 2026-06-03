// Isolate: std::thread spawning POSIX pthread_mutex (skip std::mutex).
// If this works, std::mutex is broken. If this hangs, std::thread is broken.
#include <thread>
#include <atomic>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static int counter = 0;

static void puts_raw(const char *s) { write(1, s, strlen(s)); }

static void put_dec(int n) {
    char buf[16]; int p = 0;
    if (n == 0) { write(1, "0", 1); return; }
    while (n && p < (int)sizeof(buf)) { buf[p++] = '0' + (n % 10); n /= 10; }
    while (p--) write(1, &buf[p], 1);
}

int main() {
    puts_raw("[mt_cxx start]\n");
    std::thread t1([]{
        for (int i = 0; i < 500; ++i) {
            pthread_mutex_lock(&m);
            ++counter;
            pthread_mutex_unlock(&m);
        }
    });
    puts_raw("spawned t1\n");
    std::thread t2([]{
        for (int i = 0; i < 500; ++i) {
            pthread_mutex_lock(&m);
            ++counter;
            pthread_mutex_unlock(&m);
        }
    });
    puts_raw("spawned t2\n");
    t1.join();
    puts_raw("joined t1\n");
    t2.join();
    puts_raw("joined t2 counter="); put_dec(counter); puts_raw("\n");
    return 0;
}
