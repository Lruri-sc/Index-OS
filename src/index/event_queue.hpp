#pragma once

#include <stdint.h>

namespace index {

// eventfd / epoll backing tables. Single-file home for the small fixed-size
// arrays so linux_abi.cpp doesn't grow a separate compilation unit just to
// hold counters. Both modules cooperate with the park/wake mechanism: an
// eventfd write wakes EventfdRead waiters, and an eventfd write (or any
// other tracked fd becoming ready) wakes EpollWait waiters whose set
// includes that fd.

constexpr uint32_t kMaxEventFds = 16;
constexpr uint32_t kMaxEpolls = 8;
constexpr uint32_t kEpollMaxRegs = 16;

struct EventFd {
    bool in_use = false;
    uint16_t refs = 0;
    uint64_t counter = 0;
    bool semaphore = false; // EFD_SEMAPHORE: read consumes 1, not the whole count
    bool nonblock = false;  // EFD_NONBLOCK
};

struct EpollReg {
    int fd = -1;        // monitored Esper fd
    uint32_t events = 0; // EPOLLIN / EPOLLOUT / ...
    uint64_t data = 0;   // opaque cookie returned to user
};

struct Epoll {
    bool in_use = false;
    uint16_t refs = 0;
    EpollReg regs[kEpollMaxRegs] = {};
};

// Table accessors. -1 / nullptr on bad idx.
int eventfd_alloc(uint32_t initval, bool semaphore, bool nonblock);
EventFd *eventfd_at(int idx);
void eventfd_close(int idx);
void eventfd_inc_ref(int idx);

int epoll_alloc();
Epoll *epoll_at(int idx);
void epoll_close(int idx);
void epoll_inc_ref(int idx);

} // namespace index
