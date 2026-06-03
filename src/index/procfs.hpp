#pragma once

#include <stdint.h>

#include "index/lateran.hpp" // LateranEntry

namespace index {

// Read-only /proc filesystem. Lateran consults this BEFORE the on-disk
// backend (ext2/FAT) on any path that starts with "/proc", so programs that
// probe Linux runtime info (cpuinfo, meminfo, self/maps, ...) see the same
// shape they would on a real kernel. Content is synthesised on demand --
// there is no actual /proc directory anywhere on disk.

bool procfs_owns_path(const char *path); // true iff path starts with /proc

int64_t procfs_read_file(const char *path, char *buf, uint32_t cap);
bool procfs_is_dir(const char *path);
uint32_t procfs_list_dir(const char *path, LateranEntry *out, uint32_t max);
int64_t procfs_file_size(const char *path);

} // namespace index
