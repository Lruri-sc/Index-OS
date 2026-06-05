#include "index/event_queue.hpp"

namespace index {

namespace {

EventFd g_eventfds[kMaxEventFds];
Epoll g_epolls[kMaxEpolls];
TimerFd g_timerfds[kMaxTimerFds];
SignalFd g_signalfds[kMaxSignalFds];
Inotify g_inotifys[kMaxInotify];

// Local freestanding string helpers (no libc here).
bool ieq(const char *a, const char *b) {
    while (*a != 0 && *a == *b) { ++a; ++b; }
    return *a == *b;
}
void icopy(char *d, const char *s, uint32_t cap) {
    uint32_t i = 0;
    for (; i + 1 < cap && s[i] != 0; ++i) d[i] = s[i];
    d[i] = 0;
}
// Byte-wise little-endian u32 store: the kernel is built -mstrict-align and the
// user-supplied inotify buffer is not guaranteed 4-aligned, so a plain 32-bit
// store could take an alignment fault.
void put_u32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

} // namespace

int eventfd_alloc(uint32_t initval, bool semaphore, bool nonblock) {
    for (uint32_t i = 0; i < kMaxEventFds; ++i) {
        if (!g_eventfds[i].in_use) {
            g_eventfds[i] = EventFd{};
            g_eventfds[i].in_use = true;
            g_eventfds[i].refs = 1;
            g_eventfds[i].counter = initval;
            g_eventfds[i].semaphore = semaphore;
            g_eventfds[i].nonblock = nonblock;
            return static_cast<int>(i);
        }
    }
    return -1;
}

EventFd *eventfd_at(int idx) {
    if (idx < 0 || static_cast<uint32_t>(idx) >= kMaxEventFds) return nullptr;
    return g_eventfds[idx].in_use ? &g_eventfds[idx] : nullptr;
}

void eventfd_close(int idx) {
    EventFd *e = eventfd_at(idx);
    if (e == nullptr) return;
    if (e->refs > 0) --e->refs;
    if (e->refs == 0) *e = EventFd{};
}

void eventfd_inc_ref(int idx) {
    EventFd *e = eventfd_at(idx);
    if (e != nullptr) ++e->refs;
}

int epoll_alloc() {
    for (uint32_t i = 0; i < kMaxEpolls; ++i) {
        if (!g_epolls[i].in_use) {
            g_epolls[i] = Epoll{};
            g_epolls[i].in_use = true;
            g_epolls[i].refs = 1;
            for (auto &r : g_epolls[i].regs) r = EpollReg{};
            return static_cast<int>(i);
        }
    }
    return -1;
}

Epoll *epoll_at(int idx) {
    if (idx < 0 || static_cast<uint32_t>(idx) >= kMaxEpolls) return nullptr;
    return g_epolls[idx].in_use ? &g_epolls[idx] : nullptr;
}

void epoll_close(int idx) {
    Epoll *e = epoll_at(idx);
    if (e == nullptr) return;
    if (e->refs > 0) --e->refs;
    if (e->refs == 0) *e = Epoll{};
}

void epoll_inc_ref(int idx) {
    Epoll *e = epoll_at(idx);
    if (e != nullptr) ++e->refs;
}

// --- timerfd ---------------------------------------------------------------
int timerfd_alloc(bool nonblock) {
    for (uint32_t i = 0; i < kMaxTimerFds; ++i) {
        if (!g_timerfds[i].in_use) {
            g_timerfds[i] = TimerFd{};
            g_timerfds[i].in_use = true;
            g_timerfds[i].refs = 1;
            g_timerfds[i].nonblock = nonblock;
            return static_cast<int>(i);
        }
    }
    return -1;
}

TimerFd *timerfd_at(int idx) {
    if (idx < 0 || static_cast<uint32_t>(idx) >= kMaxTimerFds) return nullptr;
    return g_timerfds[idx].in_use ? &g_timerfds[idx] : nullptr;
}

void timerfd_close(int idx) {
    TimerFd *t = timerfd_at(idx);
    if (t == nullptr) return;
    if (t->refs > 0) --t->refs;
    if (t->refs == 0) *t = TimerFd{};
}

void timerfd_inc_ref(int idx) {
    TimerFd *t = timerfd_at(idx);
    if (t != nullptr) ++t->refs;
}

// --- signalfd --------------------------------------------------------------
int signalfd_alloc(uint64_t mask, bool nonblock) {
    for (uint32_t i = 0; i < kMaxSignalFds; ++i) {
        if (!g_signalfds[i].in_use) {
            g_signalfds[i] = SignalFd{};
            g_signalfds[i].in_use = true;
            g_signalfds[i].refs = 1;
            g_signalfds[i].mask = mask;
            g_signalfds[i].nonblock = nonblock;
            return static_cast<int>(i);
        }
    }
    return -1;
}

SignalFd *signalfd_at(int idx) {
    if (idx < 0 || static_cast<uint32_t>(idx) >= kMaxSignalFds) return nullptr;
    return g_signalfds[idx].in_use ? &g_signalfds[idx] : nullptr;
}

void signalfd_close(int idx) {
    SignalFd *s = signalfd_at(idx);
    if (s == nullptr) return;
    if (s->refs > 0) --s->refs;
    if (s->refs == 0) *s = SignalFd{};
}

void signalfd_inc_ref(int idx) {
    SignalFd *s = signalfd_at(idx);
    if (s != nullptr) ++s->refs;
}

// --- inotify (Kazakiri) ----------------------------------------------------
int inotify_alloc(bool nonblock) {
    for (uint32_t i = 0; i < kMaxInotify; ++i) {
        if (!g_inotifys[i].in_use) {
            g_inotifys[i] = Inotify{};
            g_inotifys[i].in_use = true;
            g_inotifys[i].refs = 1;
            g_inotifys[i].nonblock = nonblock;
            g_inotifys[i].next_wd = 1;
            return static_cast<int>(i);
        }
    }
    return -1;
}

Inotify *inotify_at(int idx) {
    if (idx < 0 || static_cast<uint32_t>(idx) >= kMaxInotify) return nullptr;
    return g_inotifys[idx].in_use ? &g_inotifys[idx] : nullptr;
}

void inotify_close(int idx) {
    Inotify *in = inotify_at(idx);
    if (in == nullptr) return;
    if (in->refs > 0) --in->refs;
    if (in->refs == 0) *in = Inotify{};
}

void inotify_inc_ref(int idx) {
    Inotify *in = inotify_at(idx);
    if (in != nullptr) ++in->refs;
}

int inotify_add(Inotify *in, const char *path, uint32_t mask) {
    if (in == nullptr || path == nullptr) return -1;
    // Re-adding an existing watch updates its mask + returns the same wd (Linux).
    for (uint32_t i = 0; i < kInotifyWatches; ++i)
        if (in->watches[i].in_use && ieq(in->watches[i].path, path)) {
            in->watches[i].mask = mask;
            return in->watches[i].wd;
        }
    for (uint32_t i = 0; i < kInotifyWatches; ++i)
        if (!in->watches[i].in_use) {
            in->watches[i] = InotifyWatch{};
            in->watches[i].in_use = true;
            in->watches[i].wd = in->next_wd++;
            in->watches[i].mask = mask;
            icopy(in->watches[i].path, path, kInotifyPathCap);
            return in->watches[i].wd;
        }
    return -1; // table full (-ENOSPC)
}

int inotify_rm(Inotify *in, int wd) {
    if (in == nullptr) return -1;
    for (uint32_t i = 0; i < kInotifyWatches; ++i)
        if (in->watches[i].in_use && in->watches[i].wd == wd) {
            in->watches[i] = InotifyWatch{};
            return 0;
        }
    return -1; // -EINVAL (no such wd)
}

void inotify_enqueue(Inotify *in, int wd, uint32_t mask, uint32_t cookie,
                     const char *name) {
    if (in == nullptr) return;
    if (in->count >= kInotifyEvents) { in->overflow = true; return; }
    const uint16_t slot = static_cast<uint16_t>((in->head + in->count) % kInotifyEvents);
    InotifyEvent &e = in->ring[slot];
    e = InotifyEvent{};
    e.wd = wd;
    e.mask = mask;
    e.cookie = cookie;
    if (name != nullptr) icopy(e.name, name, kInotifyNameCap);
    ++in->count;
}

int64_t inotify_read_one(Inotify *in, uint8_t *out, uint64_t cap) {
    if (in == nullptr) return -9;
    if (in->count == 0) return -11; // -EAGAIN: ring empty
    const InotifyEvent &e = in->ring[in->head];
    uint32_t nlen = 0;
    while (nlen < kInotifyNameCap && e.name[nlen] != 0) ++nlen;
    // Linux pads the name field with NULs so the next event stays aligned: len
    // is 0 for a name-less event, else strlen+1 rounded up to a multiple of 4.
    const uint32_t len = (nlen == 0) ? 0u : ((nlen + 1u + 3u) & ~3u);
    const uint64_t total = 16ull + len;
    if (cap < total) return -22; // -EINVAL: caller's buffer too small for this event
    put_u32(out + 0, static_cast<uint32_t>(e.wd));
    put_u32(out + 4, e.mask);
    put_u32(out + 8, e.cookie);
    put_u32(out + 12, len);
    for (uint32_t i = 0; i < len; ++i)
        out[16 + i] = (i < nlen) ? static_cast<uint8_t>(e.name[i]) : 0;
    in->head = static_cast<uint16_t>((in->head + 1) % kInotifyEvents);
    --in->count;
    return static_cast<int64_t>(total);
}

bool inotify_has_events(const Inotify *in) {
    return in != nullptr && in->count > 0;
}

bool inotify_any_watches() {
    for (uint32_t i = 0; i < kMaxInotify; ++i) {
        if (!g_inotifys[i].in_use) continue;
        for (uint32_t w = 0; w < kInotifyWatches; ++w)
            if (g_inotifys[i].watches[w].in_use) return true;
    }
    return false;
}

} // namespace index
