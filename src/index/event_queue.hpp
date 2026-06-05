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
constexpr uint32_t kMaxTimerFds = 16;
constexpr uint32_t kMaxSignalFds = 16;

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

// timerfd: an fd that becomes readable when a CNTPCT deadline passes; read()
// returns the u64 expiration count. Poll/epoll re-check it via linux_fd_revents
// (the existing 100 Hz poll-wake covers blocking waiters), so no separate timer
// IRQ is needed. expire_cnt==0 means disarmed.
struct TimerFd {
    bool in_use = false;
    uint16_t refs = 0;
    bool nonblock = false;
    uint64_t expire_cnt = 0;    // CNTPCT value at next expiry (0 = disarmed)
    uint64_t interval_cnt = 0;  // periodic reload in CNTPCT ticks (0 = one-shot)
};
int timerfd_alloc(bool nonblock);
TimerFd *timerfd_at(int idx);
void timerfd_close(int idx);
void timerfd_inc_ref(int idx);

// signalfd: an fd that reports the owning process's pending signals selected by
// `mask`. read() returns a signalfd_siginfo and consumes the pending bit; poll
// reports readable while a masked signal is pending. The owning Esper's
// sig_pending is the source of truth (passed in by linux_abi at read/poll time).
struct SignalFd {
    bool in_use = false;
    uint16_t refs = 0;
    bool nonblock = false;
    uint64_t mask = 0;          // signals (bit n-1 = signal n) this fd reports
};
int signalfd_alloc(uint64_t mask, bool nonblock);
SignalFd *signalfd_at(int idx);
void signalfd_close(int idx);
void signalfd_inc_ref(int idx);

// inotify "Kazakiri" -- an fd that reports filesystem mutations. Each instance
// holds a set of watches (a watched absolute path + an IN_* mask) and a ring of
// pending events. The producer side is kazakiri_notify() in linux_abi, called
// from the VFS mutation syscalls (create/unlink/mkdir/rename/write); it scans
// every instance's watches, enqueues matching events, and wakes pollers. read()
// drains the ring as Linux `struct inotify_event { int wd; u32 mask, cookie,
// len; char name[len]; }`. Directory watches report children by basename in
// `name`; a watch on the mutated path itself reports a name-less event.
constexpr uint32_t kMaxInotify     = 8;   // instances (inotify_init1 fds)
constexpr uint32_t kInotifyWatches = 8;   // watches per instance
constexpr uint32_t kInotifyEvents  = 32;  // event-ring depth per instance
constexpr uint32_t kInotifyNameCap = 64;  // basename storage (incl. NUL)
constexpr uint32_t kInotifyPathCap = 128; // watched-path storage (incl. NUL)
struct InotifyWatch {
    bool in_use = false;
    int wd = 0;                       // watch descriptor (>=1, unique per instance)
    uint32_t mask = 0;                // IN_* events of interest
    char path[kInotifyPathCap] = {};  // absolute watched path (dir or file)
};
struct InotifyEvent {
    int wd = 0;
    uint32_t mask = 0;
    uint32_t cookie = 0;
    char name[kInotifyNameCap] = {};  // basename ("" = name-less self event)
};
struct Inotify {
    bool in_use = false;
    uint16_t refs = 0;
    bool nonblock = false;
    bool overflow = false;            // a queued event was dropped (ring full)
    int next_wd = 1;
    InotifyWatch watches[kInotifyWatches];
    InotifyEvent ring[kInotifyEvents];
    uint16_t head = 0;                // next slot to read
    uint16_t count = 0;              // events queued
};
int inotify_alloc(bool nonblock);
Inotify *inotify_at(int idx);
void inotify_close(int idx);
void inotify_inc_ref(int idx);
// add_watch: returns the wd (reusing an existing watch on the same path, like
// Linux) or -1 if the table is full. rm_watch: 0 on success, -1 if no such wd.
int inotify_add(Inotify *in, const char *path, uint32_t mask);
int inotify_rm(Inotify *in, int wd);
// Enqueue an event (drops + latches overflow if the ring is full).
void inotify_enqueue(Inotify *in, int wd, uint32_t mask, uint32_t cookie, const char *name);
// Drain one event into out[0..cap); returns bytes written, 0 if cap too small
// for the head event, or -11 (-EAGAIN) when the ring is empty.
int64_t inotify_read_one(Inotify *in, uint8_t *out, uint64_t cap);
bool inotify_has_events(const Inotify *in);
// O(1)-ish fast path for the producer: true only if some instance has a live
// watch. Mirrors Linux fsnotify's "no marks -> bail before doing any work" check
// so the VFS mutation hot path pays ~nothing when nobody is watching.
bool inotify_any_watches();

} // namespace index
