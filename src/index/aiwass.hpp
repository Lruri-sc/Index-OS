#pragma once

#include <stdint.h>

namespace index {

// Aiwass: Aleister Crowley's holy guardian -- his single private conduit, the
// one voice he could send messages through. Here it is the kernel pipe: a
// one-way byte stream connecting two Espers, with a fixed-size ring buffer
// refcounted on each end so the storage lives exactly as long as someone has
// a file descriptor to it. Drop the last read end and writers see broken pipe;
// drop the last write end and readers see EOF.

constexpr uint32_t kMaxAiwass = 16;
// 64 KiB capacity == Linux's default pipe size (16x the old 4 KiB). A 4 KiB ring
// filled up under OpenSSH's DEBUG2 log traffic and forced message splits; a
// split + the non-atomic counter let the privsep monitor read a torn log message
// and block forever in atomicio -> SMP deadlock. Matching Linux's size keeps the
// monitor-drained log pipe from ever filling in practice.
constexpr uint32_t kAiwassBufSize = 65536;
// POSIX PIPE_BUF: writes up to this size are atomic (all-or-nothing), so a reader
// never observes a torn message. OpenSSH's framed log/monitor messages are small.
constexpr uint32_t kAiwassPipeBuf = 4096;

struct Aiwass {
    bool in_use = false;
    uint8_t *buf = nullptr;   // ring buffer (kAiwassBufSize bytes, on DarkMatter heap)
    uint32_t head = 0;        // next byte to read
    uint32_t tail = 0;        // next slot to write
    uint32_t avail = 0;       // bytes currently in buffer, in [0, kAiwassBufSize]
    uint32_t read_refs = 0;   // open read ends
    uint32_t write_refs = 0;  // open write ends
};

// Sentinels used by aiwass_read / aiwass_write to tell the syscall dispatcher
// "I cannot make progress right now, please block the caller" vs. "the other
// side is gone, return EOF/EPIPE to user space".
constexpr int64_t kAiwassWouldBlock = -2;
constexpr int64_t kAiwassBrokenPipe = -1;

// Allocate a pipe + its ring buffer. Both sides start with one ref (the
// caller's), so a freshly-created pipe is alive until the caller closes both
// fds. Returns the pipe index (>=0), or -1 on no free slot / no heap.
int aiwass_create();

Aiwass *aiwass_at(int idx);

// Bump a side's ref count (used by dup/dup2/fork to share an fd).
void aiwass_inc_read(int idx);
void aiwass_inc_write(int idx);

// Drop a ref; when both sides hit 0, free the buffer and recycle the slot.
void aiwass_close_read(int idx);
void aiwass_close_write(int idx);

// Move up to `len` bytes between user buf and pipe buf. Returns the number of
// bytes moved, or one of the sentinels above.
//
//   aiwass_read:  0 = EOF (buffer empty, write_refs == 0)
//                 kAiwassWouldBlock = empty but writers still alive
//   aiwass_write: kAiwassBrokenPipe = no readers left
//                 kAiwassWouldBlock = full but readers still alive
int64_t aiwass_read(int idx, char *dst, uint64_t len);
int64_t aiwass_write(int idx, const char *src, uint64_t len);

// Non-consuming poll readiness.
bool aiwass_readable(int idx);
bool aiwass_writable(int idx);

// [WD] Dump live pipe fill/refcounts for the SMP hang watchdog.
void aiwass_report();

} // namespace index
