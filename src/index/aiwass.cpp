#include "index/aiwass.hpp"

#include "index/dark_matter.hpp"
#include "index/imaginary_number_district.hpp" // [WD] aiwass_report

namespace index {

namespace {

Aiwass g_pipes[kMaxAiwass];

bool valid(int idx) {
    return idx >= 0 && static_cast<uint32_t>(idx) < kMaxAiwass && g_pipes[idx].in_use;
}

void maybe_free(int idx) {
    Aiwass &p = g_pipes[idx];
    if (p.read_refs == 0 && p.write_refs == 0) {
        if (p.buf != nullptr) {
            dark_matter_free(p.buf);
        }
        p = Aiwass{};
    }
}

} // namespace

// [WD] Dump every live pipe's fill level + open ref counts, for the SMP hang
// watchdog (privsep log-pipe deadlock diagnosis).
void aiwass_report() {
    namespace district = imaginary_number_district;
    district::writeln("Aiwass pipes:");
    for (uint32_t i = 0; i < kMaxAiwass; ++i) {
        const Aiwass &p = g_pipes[i];
        if (!p.in_use) continue;
        district::write("  ["); district::dec(i); district::write("] avail=");
        district::dec(p.avail);
        district::write(" rrefs="); district::dec(p.read_refs);
        district::write(" wrefs="); district::dec(p.write_refs);
        district::writeln("");
    }
}

int aiwass_create() {
    for (uint32_t i = 0; i < kMaxAiwass; ++i) {
        if (!g_pipes[i].in_use) {
            auto *buf = static_cast<uint8_t *>(dark_matter_alloc(kAiwassBufSize));
            if (buf == nullptr) {
                return -1;
            }
            g_pipes[i] = Aiwass{};
            g_pipes[i].in_use = true;
            g_pipes[i].buf = buf;
            g_pipes[i].read_refs = 1;
            g_pipes[i].write_refs = 1;
            return static_cast<int>(i);
        }
    }
    return -1;
}

Aiwass *aiwass_at(int idx) {
    return valid(idx) ? &g_pipes[idx] : nullptr;
}

void aiwass_inc_read(int idx) {
    if (valid(idx)) {
        ++g_pipes[idx].read_refs;
    }
}

void aiwass_inc_write(int idx) {
    if (valid(idx)) {
        ++g_pipes[idx].write_refs;
    }
}

void aiwass_close_read(int idx) {
    if (valid(idx) && g_pipes[idx].read_refs > 0) {
        --g_pipes[idx].read_refs;
        maybe_free(idx);
    }
}

void aiwass_close_write(int idx) {
    if (valid(idx) && g_pipes[idx].write_refs > 0) {
        --g_pipes[idx].write_refs;
        maybe_free(idx);
    }
}

// `avail` is the one ring counter both the writer (one core) and the reader
// (another core) mutate. A plain ++/-- is a cross-core non-atomic RMW: a lost
// update desyncs the ring so the reader sees a truncated/garbled message --
// which deadlocked the OpenSSH privsep monitor (it poll()ed the log pipe
// readable, then blocked in atomicio reading a length whose body the corrupted
// counter hid). Make it a lock-free SPSC counter (matching the Antenna rx ring)
// rather than a lock: aiwass_readable/writable are invoked from park_on_pipe
// while g_esper_lock is held, and linux_pipe_write wakes under g_esper_lock, so
// a pipe lock would invert against g_esper_lock (ABBA). head (reader-owned) and
// tail (writer-owned) need no atomics; only this shared counter does.
int64_t aiwass_read(int idx, char *dst, uint64_t len) {
    if (!valid(idx) || dst == nullptr) {
        return kAiwassBrokenPipe;
    }
    Aiwass &p = g_pipes[idx];
    uint32_t cur = __atomic_load_n(&p.avail, __ATOMIC_ACQUIRE); // see published bytes
    if (cur == 0) {
        return p.write_refs > 0 ? kAiwassWouldBlock : 0;
    }
    uint64_t n = 0;
    while (n < len && cur > 0) {
        dst[n++] = static_cast<char>(p.buf[p.head]);
        p.head = (p.head + 1) % kAiwassBufSize;
        --cur;
    }
    __atomic_sub_fetch(&p.avail, static_cast<uint32_t>(n), __ATOMIC_ACQ_REL);
    return static_cast<int64_t>(n);
}

// Poll readiness (non-consuming). Readable when data is present OR all writers
// are gone (read then sees EOF). Writable when buffer has space OR all readers
// gone (write then sees EPIPE). Real poll() needs this so a reader polling an
// empty pipe doesn't spin -- which was deadlocking sshd's monitor.
bool aiwass_readable(int idx) {
    if (!valid(idx)) return true;
    const Aiwass &p = g_pipes[idx];
    return __atomic_load_n(&p.avail, __ATOMIC_ACQUIRE) > 0 || p.write_refs == 0;
}
bool aiwass_writable(int idx) {
    if (!valid(idx)) return true;
    const Aiwass &p = g_pipes[idx];
    return __atomic_load_n(&p.avail, __ATOMIC_ACQUIRE) < kAiwassBufSize ||
           p.read_refs == 0;
}

int64_t aiwass_write(int idx, const char *src, uint64_t len) {
    if (!valid(idx) || src == nullptr) {
        return kAiwassBrokenPipe;
    }
    Aiwass &p = g_pipes[idx];
    if (p.read_refs == 0) {
        return kAiwassBrokenPipe;
    }
    uint32_t cur = __atomic_load_n(&p.avail, __ATOMIC_ACQUIRE);
    if (cur == kAiwassBufSize) {
        return kAiwassWouldBlock;
    }
    // PIPE_BUF atomicity: a write that fits within the buffer must land all-or-
    // nothing so a reader never sees a torn message. Block (wait for room for
    // the whole message) rather than write a prefix. With the 64 KiB buffer this
    // effectively never blocks for OpenSSH's small framed messages.
    if (len <= kAiwassPipeBuf && kAiwassBufSize - cur < len) {
        return kAiwassWouldBlock;
    }
    uint64_t n = 0;
    while (n < len && cur < kAiwassBufSize) {
        p.buf[p.tail] = static_cast<uint8_t>(src[n++]);
        p.tail = (p.tail + 1) % kAiwassBufSize;
        ++cur;
    }
    __atomic_add_fetch(&p.avail, static_cast<uint32_t>(n), __ATOMIC_RELEASE); // publish buf
    return static_cast<int64_t>(n);
}

} // namespace index
