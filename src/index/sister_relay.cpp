#include "index/sister_relay.hpp"

#include "index/esper.hpp"
#include "index/imaginary_number_district.hpp" // [WD] sr_report
#include "index/usermode.hpp" // linux_ipc_wake

namespace index {

namespace {

SisterRelay g_relays[kMaxRelays];

bool valid(int idx) {
    return idx >= 0 && static_cast<uint32_t>(idx) < kMaxRelays && g_relays[idx].in_use;
}

// Each pty ring's `avail` is mutated by BOTH ends on different cores (master
// writes m2s while slave reads it; slave writes s2m while master reads it). A
// plain ++/-- is a torn cross-core RMW -> lost byte counts -> the interactive
// SSH shell loses pty data and hangs (command mode, which allocates no pty, was
// already 8/8 on -smp 8). Lock-free SPSC counter, matching the Antenna/Aiwass/
// Channel rings: the writer's add carries RELEASE (publishing the ring byte),
// readers load with ACQUIRE (in the callers' avail checks). head/tail are
// single-owner.
void put(uint8_t *ring, uint32_t &head, uint32_t &tail, uint32_t &avail,
         uint8_t b) {
    ring[tail] = b;
    tail = (tail + 1) % kRelayRingBytes;
    __atomic_add_fetch(&avail, 1, __ATOMIC_RELEASE);
    (void)head;
}

uint8_t get(uint8_t *ring, uint32_t &head, uint32_t &tail, uint32_t &avail) {
    uint8_t b = ring[head];
    head = (head + 1) % kRelayRingBytes;
    __atomic_sub_fetch(&avail, 1, __ATOMIC_ACQ_REL);
    (void)tail;
    return b;
}

// ACQUIRE-load of a ring's avail (pairs with put's RELEASE add to see the bytes).
inline uint32_t avail_load(const uint32_t &avail) {
    return __atomic_load_n(&avail, __ATOMIC_ACQUIRE);
}

// master_refs/slave_refs are read-modify-written from different cores: a fork
// inherits the pty fd and bumps the ref on the forking core (the clone fd-copy
// loop in usermode.cpp) while a concurrent close() drops it on another, and
// SCM_RIGHTS fd passing refs on the sender's core / transfers on the receiver's.
// A plain ++/-- is a torn cross-core RMW. A lost decrement pins the relay (fd
// leak); a lost increment drops the count to 0 while a process still holds the
// slave -- then sr_master_try_read's `slave_refs == 0` EOF test fires
// spuriously and a live interactive SSH session is torn down ("read failed
// rfd9 ... Broken pipe" -> channel drain -> session collapse). Linux keeps
// tty->count / file->f_count atomic for exactly this. Atomic add + CAS
// dec-if-positive (refcount_t style); ACQUIRE load so readers see a coherent
// count.
inline uint16_t ref_inc(uint16_t &v) {
    return __atomic_add_fetch(&v, 1, __ATOMIC_ACQ_REL);
}
inline uint16_t ref_dec(uint16_t &v) {
    uint16_t cur = __atomic_load_n(&v, __ATOMIC_RELAXED);
    while (cur > 0) {
        if (__atomic_compare_exchange_n(&v, &cur, static_cast<uint16_t>(cur - 1),
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return static_cast<uint16_t>(cur - 1);
        // CAS failed: `cur` now holds the latest value -- retry.
    }
    return 0;
}
inline uint16_t ref_load(const uint16_t &v) {
    return __atomic_load_n(&v, __ATOMIC_ACQUIRE);
}

} // namespace

int sr_alloc() {
    for (uint32_t i = 0; i < kMaxRelays; ++i) {
        if (!g_relays[i].in_use) {
            g_relays[i] = SisterRelay{};
            g_relays[i].in_use = true;
            g_relays[i].master_refs = 1;
            g_relays[i].slave_refs = 0;
            g_relays[i].slave_locked = true;
            // Default termios (matches the console's default): ICRNL|IXON,
            // OPOST|ONLCR, CS8|CREAD|B38400, ISIG|ICANON|ECHO|ECHOK|ECHOE.
            // Layout follows linux_abi's termios blob: 4*u32 then c_line + c_cc[].
            *reinterpret_cast<uint32_t *>(g_relays[i].termios + 0)  = 0x0500;
            *reinterpret_cast<uint32_t *>(g_relays[i].termios + 4)  = 0x0005;
            *reinterpret_cast<uint32_t *>(g_relays[i].termios + 8)  = 0x00bf;
            *reinterpret_cast<uint32_t *>(g_relays[i].termios + 12) = 0x800b;
            g_relays[i].termios[17 + 0] = 3;    // VINTR Ctrl-C
            g_relays[i].termios[17 + 2] = 0x7f; // VERASE
            g_relays[i].termios[17 + 4] = 4;    // VEOF
            g_relays[i].termios[17 + 6] = 1;    // VMIN
            return static_cast<int>(i);
        }
    }
    return -1;
}

int sr_open_slave(int idx) {
    if (!valid(idx) || g_relays[idx].slave_locked) return -1;
    ref_inc(g_relays[idx].slave_refs);
    g_relays[idx].slave_ever_opened = true;
    return idx;
}

SisterRelay *sr_at(int idx) { return valid(idx) ? &g_relays[idx] : nullptr; }

bool sr_unlock(int idx) {
    if (!valid(idx)) return false;
    g_relays[idx].slave_locked = false;
    return true;
}

int64_t sr_master_write(int idx, const uint8_t *buf, uint32_t len) {
    if (!valid(idx)) return -9; // -EBADF
    SisterRelay &r = g_relays[idx];
    const uint32_t free = kRelayRingBytes - avail_load(r.m2s_avail);
    uint32_t n = len > free ? free : len;
    for (uint32_t i = 0; i < n; ++i) put(r.m2s, r.m2s_head, r.m2s_tail, r.m2s_avail, buf[i]);
    if (n > 0) linux_ipc_wake(Esper::IpcWaitKind::PtySlaveRead, idx);
    return n;
}

int64_t sr_slave_write(int idx, const uint8_t *buf, uint32_t len) {
    if (!valid(idx)) return -9;
    SisterRelay &r = g_relays[idx];
    // Output post-processing (termios c_oflag OPOST|ONLCR): the slave end (the
    // shell) writes bare '\n'; the tty line discipline expands it to "\r\n" on
    // the way to the master so a raw client terminal returns to column 0.
    // Without this, multi-line output (e.g. `ls`) staircases down the screen.
    // Mirrors Linux n_tty do_output_char(). c_oflag is the 2nd u32 of termios.
    const uint32_t oflag = *reinterpret_cast<const uint32_t *>(r.termios + 4);
    const bool onlcr = (oflag & 0x1u /*OPOST*/) != 0 && (oflag & 0x4u /*ONLCR*/) != 0;
    uint32_t free = kRelayRingBytes - avail_load(r.s2m_avail);
    uint32_t consumed = 0; // input bytes accepted (the write() return value)
    uint32_t produced = 0; // ring bytes emitted (incl. inserted CR; keeps the
                           // s2m_w==s2m_r "fully drained" watchdog invariant)
    for (uint32_t i = 0; i < len; ++i) {
        const uint8_t c = buf[i];
        if (onlcr && c == '\n') {
            if (free < 2) break; // emit CR+LF atomically or not at all
            put(r.s2m, r.s2m_head, r.s2m_tail, r.s2m_avail, '\r');
            put(r.s2m, r.s2m_head, r.s2m_tail, r.s2m_avail, '\n');
            free -= 2; produced += 2;
        } else {
            if (free < 1) break;
            put(r.s2m, r.s2m_head, r.s2m_tail, r.s2m_avail, c);
            free -= 1; produced += 1;
        }
        ++consumed;
    }
    r.s2m_w += produced; // [WD] total shell output written (ring bytes)
    if (produced > 0) linux_ipc_wake(Esper::IpcWaitKind::PtyMasterRead, idx);
    return consumed;
}

int64_t sr_master_try_read(int idx, uint8_t *buf, uint32_t cap, bool *peer_gone) {
    if (!valid(idx)) return -9;
    SisterRelay &r = g_relays[idx];
    // EOF only after a slave was actually opened *and* then fully closed;
    // before the first open we keep blocking so the slave still has a chance
    // to wire up (parent reading immediately after fork+exec).
    const uint32_t av = avail_load(r.s2m_avail);
    if (peer_gone) *peer_gone = (r.slave_ever_opened && ref_load(r.slave_refs) == 0 &&
                                  av == 0);
    if (av == 0) return 0;
    uint32_t n = cap > av ? av : cap;
    for (uint32_t i = 0; i < n; ++i) buf[i] = get(r.s2m, r.s2m_head, r.s2m_tail, r.s2m_avail);
    r.s2m_r += n; // [WD] total shell output the master side (sshd) read out
    return static_cast<int64_t>(n);
}

int64_t sr_slave_try_read(int idx, uint8_t *buf, uint32_t cap, bool *peer_gone) {
    if (!valid(idx)) return -9;
    SisterRelay &r = g_relays[idx];
    const uint32_t av = avail_load(r.m2s_avail);
    if (peer_gone) *peer_gone = (ref_load(r.master_refs) == 0 && av == 0);
    if (av == 0) return 0;
    uint32_t n = cap > av ? av : cap;
    for (uint32_t i = 0; i < n; ++i) buf[i] = get(r.m2s, r.m2s_head, r.m2s_tail, r.m2s_avail);
    return static_cast<int64_t>(n);
}

// poll/epoll readiness for the master end (what sshd's pty relay polls). The
// old stub returned "always readable+writable", which made non-blocking sshd
// spin-read an empty master (and Index parked it) so the channel relay never
// got to forward the client's input. Real readiness mirrors antenna_poll.
uint32_t sr_poll_master(int idx) {
    SisterRelay *r = sr_at(idx);
    if (r == nullptr) return 0x20; // POLLNVAL
    uint32_t re = 0x4; // POLLOUT: master->slave ring has room (never full in practice)
    if (avail_load(r->s2m_avail) > 0) re |= 0x1;    // POLLIN: slave wrote output
    if (r->slave_ever_opened && ref_load(r->slave_refs) == 0) // slave hung up -> read() sees EOF
        re |= 0x1 | 0x10;                           // POLLIN | POLLHUP
    return re;
}
uint32_t sr_poll_slave(int idx) {
    SisterRelay *r = sr_at(idx);
    if (r == nullptr) return 0x20;
    uint32_t re = 0x4; // POLLOUT
    if (avail_load(r->m2s_avail) > 0) re |= 0x1; // POLLIN: master wrote input
    if (ref_load(r->master_refs) == 0) re |= 0x1 | 0x10;  // master closed -> EOF
    return re;
}

void sr_close_master(int idx) {
    if (!valid(idx)) return;
    SisterRelay &r = g_relays[idx];
    if (ref_dec(r.master_refs) == 0) {
        // Wake any slave reader so it sees EOF.
        linux_ipc_wake(Esper::IpcWaitKind::PtySlaveRead, idx);
        if (ref_load(r.slave_refs) == 0) r = SisterRelay{};
    }
}

void sr_close_slave(int idx) {
    if (!valid(idx)) return;
    SisterRelay &r = g_relays[idx];
    if (ref_dec(r.slave_refs) == 0) {
        linux_ipc_wake(Esper::IpcWaitKind::PtyMasterRead, idx);
        if (ref_load(r.master_refs) == 0) r = SisterRelay{};
    }
}

void sr_master_inc_ref(int idx) { if (valid(idx)) ref_inc(g_relays[idx].master_refs); }
void sr_slave_inc_ref(int idx)  { if (valid(idx)) ref_inc(g_relays[idx].slave_refs); }

void sr_report() {
    namespace district = imaginary_number_district;
    district::writeln("[WD] SisterRelays (pty):");
    bool any = false;
    for (uint32_t i = 0; i < kMaxRelays; ++i) {
        const SisterRelay &r = g_relays[i];
        if (!r.in_use) continue;
        any = true;
        district::write("[WD]   [");
        district::dec(i);
        district::write("] m2s="); district::dec(avail_load(r.m2s_avail)); // client->shell input
        district::write(" s2m="); district::dec(avail_load(r.s2m_avail));  // shell->client output (prompt!)
        district::write(" mref="); district::dec(static_cast<uint64_t>(ref_load(r.master_refs)));
        district::write(" sref="); district::dec(static_cast<uint64_t>(ref_load(r.slave_refs)));
        district::write(" sopened="); district::dec(r.slave_ever_opened ? 1u : 0u);
        district::write(" s2m_w="); district::dec(r.s2m_w);   // shell wrote
        district::write(" s2m_r="); district::dec(r.s2m_r);   // sshd read
        district::write(" fgpgid="); district::dec(static_cast<uint64_t>(static_cast<uint32_t>(r.fg_pgid)));
        district::write(" sid="); district::dec(static_cast<uint64_t>(static_cast<uint32_t>(r.sid)));
        district::write("\n");
    }
    if (!any) district::writeln("[WD]   (none)");
}

} // namespace index
