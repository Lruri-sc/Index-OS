// std::thread + std::mutex (everything libc++). Expected to hang.
#include <mutex>
#include <thread>
#include <unistd.h>
#include <string.h>

static std::mutex sm;
static int counter = 0;

static void puts_raw(const char *s) { write(1, s, strlen(s)); }

static void put_dec(int n) {
    char buf[16]; int p = 0;
    if (n == 0) { write(1, "0", 1); return; }
    while (n && p < (int)sizeof(buf)) { buf[p++] = '0' + (n % 10); n /= 10; }
    while (p--) write(1, &buf[p], 1);
}

#include <atomic>
int main() {
    puts_raw("[stdmutex] start\n");

    // Warm with atomic-only threads (test_thread).
    {
        std::atomic<int> n{0};
        std::thread w1([&]{ for (int i = 0; i < 1000; ++i) n.fetch_add(1); });
        std::thread w2([&]{ for (int i = 0; i < 1000; ++i) n.fetch_add(1); });
        w1.join();
        w2.join();
        puts_raw("warm n="); put_dec(n.load()); puts_raw("\n");
    }

    // Right after warm: ONE thread + raw pthread_mutex (NOT std::mutex), same
    // contention pattern. If this works, std::mutex is the differentiator.
    {
        puts_raw("after-warm pthread_mutex\n");
        pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
        int c = 0;
        pthread_mutex_lock(&pm);
        std::thread t([&]{
            puts_raw("t1 trying\n");
            pthread_mutex_lock(&pm);
            ++c;
            pthread_mutex_unlock(&pm);
            puts_raw("t1 done\n");
        });
        pthread_mutex_unlock(&pm);
        t.join();
        puts_raw("pthread_mutex c="); put_dec(c); puts_raw("\n");
    }

    // Same but with std::mutex.
    {
        puts_raw("after-warm std::mutex\n");
        std::mutex m;
        int c = 0;
        m.lock();
        std::thread t([&]{
            puts_raw("t2 trying\n");
            m.lock();
            ++c;
            m.unlock();
            puts_raw("t2 done\n");
        });
        m.unlock();
        t.join();
        puts_raw("std::mutex c="); put_dec(c); puts_raw("\n");
    }
    return 0;
}
