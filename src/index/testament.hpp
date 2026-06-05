#pragma once

#include <stdint.h>

namespace index {

struct LateranEntry; // index/lateran.hpp

// Testament: the series' memory-implant / learning device. Here it is the
// in-memory (RAM-backed) filesystem behind tmpfs mounts -- /tmp, /dev/shm, and
// any `mount -t tmpfs`. Contents live on the DarkMatter heap and are volatile
// (gone on reboot), exactly like Linux tmpfs. Lateran routes paths under an
// active tmpfs mount point here (tmpfs_owns_path) ahead of the on-disk ext2
// backend. Mirrors the Lateran VFS op set so the syscall layer is unchanged.

// Mount/unmount a tmpfs at `point` (absolute, e.g. "/tmp"). mount creates a
// fresh empty root dir node; umount marks the mount inactive (nodes are left
// for any open fds, reclaimed lazily). Returns false on table-full / bad args.
bool testament_mount(const char *point);
bool testament_umount(const char *point);

// True iff `path` falls under an active tmpfs mount point (so Lateran should
// dispatch the op here instead of to ext2). Longest-prefix match.
bool tmpfs_owns_path(const char *path);

// VFS ops (subset Lateran routes). All paths are absolute + already normalized
// by resolve_path. Return values match the Lateran equivalents.
bool     tmpfs_stat(const char *path, LateranEntry *out);
bool     tmpfs_is_dir(const char *path);
int64_t  tmpfs_read_file(const char *path, char *buf, uint32_t cap);
int64_t  tmpfs_pread(const char *path, uint64_t offset, char *buf, uint32_t len); // demand-paging read
int64_t  tmpfs_write_file(const char *path, const char *buf, uint32_t len); // create/overwrite
int64_t  tmpfs_append_file(const char *path, const char *buf, uint32_t len, uint64_t off); // pwrite
bool     tmpfs_create(const char *path);          // empty regular file (O_CREAT)
bool     tmpfs_unlink(const char *path);
bool     tmpfs_mkdir(const char *path);
bool     tmpfs_rmdir(const char *path);
bool     tmpfs_truncate(const char *path, uint64_t new_size);
bool     tmpfs_rename(const char *old_path, const char *new_path);
bool     tmpfs_symlink(const char *target, const char *path);
int32_t  tmpfs_readlink(const char *path, char *out, uint32_t cap);
bool     tmpfs_chmod(const char *path, uint32_t mode);
bool     tmpfs_chown(const char *path, uint32_t uid, uint32_t gid);
bool     tmpfs_utime(const char *path, int64_t atime, int64_t mtime);
uint32_t tmpfs_list_dir(const char *path, LateranEntry *out, uint32_t max);

// Append the active mounts to a /proc/mounts buffer (used by procfs gen_mounts).
// Returns the new offset.
uint32_t tmpfs_proc_mounts(char *buf, uint32_t cap, uint32_t off);

} // namespace index
