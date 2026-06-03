#pragma once

#include <stdint.h>

#include "index/lateran.hpp"

// StiylMagnus -- the Necessarius mage who specialises in carrying word and
// fire between the Church and the outside world. Here he plays the same role
// for files: a virtio-9p (9P2000.L) client that bridges a host directory into
// Index's filesystem, so the host can edit code that the guest sees live
// without rebuilding the ext2 image. Discovered on the same virtio-mmio bus
// as Underline / MisakaMail.
//
// 9P paths visible to the guest are prefixed with "/host" -- e.g. host file
// `share/foo.txt` opens as guest `/host/foo.txt`. This avoids needing a real
// mount table; Lateran routes any path starting with "/host" to here.

namespace index::drivers {

struct StiylMagnus {
    bool present = false;
    uint64_t base = 0;          // virtio-mmio register base if mmio transport
    uint64_t notify_addr = 0;   // where to write the queue-notify token
    uint32_t msize = 0;         // negotiated max 9P message size
    uint32_t root_fid = 0;      // fid for the mount root after TATTACH (0 = not attached)
};

// Scan virtio-mmio for a 9p device (device id 9), bring it up, then negotiate
// 9P2000.L and TATTACH the root. Returns the global instance.
StiylMagnus stiyl_magnus_probe();
const StiylMagnus &stiyl_magnus_status();

// File system surface used by Lateran when a path starts with "/host". Each
// strips the "/host" prefix before calling here (so "/host/foo" -> "foo",
// "/host" -> "" = root).
int64_t stiyl_read_file(const char *path, char *buf, uint32_t cap);
bool stiyl_stat(const char *path, LateranEntry *out);
bool stiyl_is_dir(const char *path);
uint32_t stiyl_list_dir(const char *path, LateranEntry *out, uint32_t max);

// Write side (Tlcreate / Twrite / Tremove). atomic create-or-truncate.
// Returns bytes written, or -1 on failure.
int64_t stiyl_write_file(const char *path, const char *buf, uint32_t len);
bool stiyl_unlink(const char *path);
bool stiyl_truncate(const char *path, uint64_t new_size);

} // namespace index::drivers
