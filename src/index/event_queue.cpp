#include "index/event_queue.hpp"

namespace index {

namespace {

EventFd g_eventfds[kMaxEventFds];
Epoll g_epolls[kMaxEpolls];

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

} // namespace index
