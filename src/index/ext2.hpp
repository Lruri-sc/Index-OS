#pragma once

#include <stdint.h>

#include "index/lateran.hpp" // LateranEntry (shared dir-entry shape)

namespace index {

// ext2: a real Unix filesystem on the Underline disk. Unlike the FAT16 backend
// (8.3 names, no ownership), ext2 stores long filenames, a directory tree, and
// real inode metadata (mode/uid/gid) -- so genuine paths like
// /lib/ld-musl-aarch64.so.1 work and the future multi-user module's
// permissions persist. Lateran detects the on-disk format and routes to this
// backend or the FAT one; these functions mirror the public lateran_* API.
//
// This file is the read side (mount + resolve + read + list); the write side
// (alloc + write + mkdir + unlink) lives alongside in ext2.cpp.

bool ext2_mount();                  // true if the disk holds an ext2 volume
bool ext2_mounted();

int64_t ext2_read_file(const char *path, char *buf, uint32_t cap);
int64_t ext2_pread(const char *path, uint64_t offset, char *buf, uint32_t len); // demand-paging read
uint32_t ext2_list_dir(const char *path, LateranEntry *out, uint32_t max);
bool ext2_is_dir(const char *path);

// Stat a path (no symlink follow-mode toggle yet -- always follows). Fills
// out's mode/size/uid/gid/atime/mtime/ctime. Returns false if the path
// doesn't exist.
bool ext2_stat(const char *path, LateranEntry *out);

bool ext2_rename(const char *old_path, const char *new_path);
bool ext2_truncate(const char *path, uint64_t new_size);
bool ext2_chmod(const char *path, uint32_t new_mode);
bool ext2_chown(const char *path, uint32_t uid, uint32_t gid);
bool ext2_link(const char *target, const char *link_path);
bool ext2_utime(const char *path, int64_t atime, int64_t mtime);

// Whole-filesystem statistics for statfs(). All counts are in `block_size`
// units except inodes. Returns false if not mounted.
struct Ext2FsStats {
    uint32_t block_size = 0;
    uint64_t total_blocks = 0;
    uint64_t free_blocks = 0;
    uint64_t total_inodes = 0;
    uint64_t free_inodes = 0;
    uint32_t namelen_max = 255;
};
bool ext2_fs_stats(Ext2FsStats *out);

// Write side.
int64_t ext2_write_file(const char *path, const char *buf, uint32_t len);
bool ext2_unlink(const char *path);
bool ext2_mkdir(const char *path);

// Read a symbolic link's target (does NOT follow the link itself). Returns
// the length of the target written into `out`, NUL-terminated, or -1 if
// `path` is not a symlink, missing, or the target overflows `cap`.
int32_t ext2_readlink(const char *path, char *out, uint32_t cap);

// Create a symbolic link at `path` whose target string is `target`. Returns
// false on failure (parent not present, link exists, target too long, no
// free inode/block, ...).
bool ext2_symlink(const char *target, const char *path);

} // namespace index
