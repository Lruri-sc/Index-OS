#pragma once

#include <stdint.h>

namespace index {

// SisterRelay: a Pty pair (master + slave) in kernel memory.
//
// In the Sisters Network the clones forward signals brain-to-brain without
// a physical wire; here the same metaphor names the in-kernel pty layer
// that ferries bytes between a controlling process (master end) and a
// program running on what looks to it like a real tty (slave end).  Used
// by terminal multiplexers, ssh-style remote shells, and anything wanting
// an extra TTY-like channel.

constexpr uint32_t kMaxRelays = 8;
constexpr uint32_t kRelayRingBytes = 4096;
constexpr uint32_t kRelayTermiosBytes = 64;

struct SisterRelay {
    bool in_use = false;
    uint16_t master_refs = 0;
    uint16_t slave_refs = 0;
    bool slave_locked = true; // POSIX: openpt+unlockpt before open slave
    bool slave_ever_opened = false; // master_read only signals EOF if a slave
                                    // was actually opened and then fully closed

    // master -> slave: slave reads from here
    uint8_t m2s[kRelayRingBytes] = {};
    uint32_t m2s_head = 0, m2s_tail = 0, m2s_avail = 0;
    // slave -> master: master reads from here
    uint8_t s2m[kRelayRingBytes] = {};
    uint32_t s2m_head = 0, s2m_tail = 0, s2m_avail = 0;
    // [WD] cumulative byte counters for the pty-output-forwarding diagnosis:
    // s2m_w = total bytes the shell wrote (slave_write); s2m_r = total bytes the
    // master side (sshd) actually read out. s2m_w>0 && s2m_r==0 == shell
    // produced output but sshd never read it (forwarding lost-wakeup).
    uint64_t s2m_w = 0, s2m_r = 0;

    // Slave-side TTY state.  44 bytes match the termios blob we hand to
    // TCGETS/TCSETS; the extra room is padding for future fields.
    uint8_t termios[kRelayTermiosBytes] = {};
    uint16_t rows = 24, cols = 80;
    int fg_pgid = -1;
    int sid = -1;
};

int sr_alloc();
int sr_open_slave(int idx);
SisterRelay *sr_at(int idx);

int64_t sr_master_write(int idx, const uint8_t *buf, uint32_t len);
int64_t sr_slave_write(int idx, const uint8_t *buf, uint32_t len);
int64_t sr_master_try_read(int idx, uint8_t *buf, uint32_t cap, bool *peer_gone);
int64_t sr_slave_try_read(int idx, uint8_t *buf, uint32_t cap, bool *peer_gone);

// poll/epoll readiness masks (POLLIN 0x1 / POLLOUT 0x4 / POLLHUP 0x10).
uint32_t sr_poll_master(int idx);
uint32_t sr_poll_slave(int idx);

void sr_close_master(int idx);
void sr_close_slave(int idx);
void sr_master_inc_ref(int idx);
void sr_slave_inc_ref(int idx);

bool sr_unlock(int idx);

// [WD] Dump every in-use relay's ring fill + refs. s2m_avail answers the key
// question for the pty hang: did the shell actually write its prompt (s2m>0,
// so the master side / sshd lost the wakeup) or not (s2m==0, shell-side hang)?
void sr_report();

} // namespace index
