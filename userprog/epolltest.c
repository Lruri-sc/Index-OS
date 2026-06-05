// Functional test for the new timerfd + signalfd + epoll integration.
// Built static-musl (see Makefile), baked into the rootfs as /bin/epolltest.
// Prints TIMERFD_OK / SIGFD_OK on success so a boot harness can grep for them.
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    // --- timerfd: a 120ms one-shot, waited on via epoll -------------------
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0) { printf("TIMERFD_FAIL create\n"); return 1; }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 120 * 1000 * 1000; // 120ms
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) { printf("TIMERFD_FAIL settime\n"); return 1; }

    int ep = epoll_create1(0);
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);

    struct epoll_event out[4];
    int n = epoll_wait(ep, out, 4, 2000); // up to 2s
    if (n >= 1) {
        uint64_t expirations = 0;
        ssize_t r = read(tfd, &expirations, sizeof(expirations));
        if (r == (ssize_t)sizeof(expirations) && expirations >= 1)
            printf("TIMERFD_OK exp=%llu\n", (unsigned long long)expirations);
        else
            printf("TIMERFD_FAIL read r=%ld exp=%llu\n", (long)r, (unsigned long long)expirations);
    } else {
        printf("TIMERFD_FAIL epoll n=%d\n", n);
    }

    // --- signalfd: block SIGUSR1, raise it, read it back through the fd ----
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL); // so it stays pending for the fd
    int sfd = signalfd(-1, &mask, 0);
    if (sfd < 0) { printf("SIGFD_FAIL create\n"); return 1; }
    raise(SIGUSR1); // now pending + blocked -> readable on sfd
    struct signalfd_siginfo si;
    memset(&si, 0, sizeof(si));
    ssize_t sr = read(sfd, &si, sizeof(si));
    if (sr == (ssize_t)sizeof(si) && si.ssi_signo == (uint32_t)SIGUSR1)
        printf("SIGFD_OK signo=%u\n", si.ssi_signo);
    else
        printf("SIGFD_FAIL sr=%ld signo=%u\n", (long)sr, si.ssi_signo);

    return 0;
}
