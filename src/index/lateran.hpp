#pragma once

#include <stdint.h>

namespace index {

// Lateran: the Roman Catholic Church's grand archive in the series. Here it is a
// read-only FAT16 filesystem living on the Underline (virtio-blk) disk -- real
// files in a real on-disk format, in contrast to GrimoireFS (baked into the
// image) and Bookshelf (in the heap).
struct LateranEntry {
    char name[64]; // file name, NUL-terminated (FAT 8.3 or ext2 long name)
    uint32_t size;
    uint16_t first_cluster;
    bool is_dir = false;
    // Unix metadata. FAT's on-disk format can't store these, so they are
    // synthesised with sane defaults (owner uid 0, 0755 dirs / 0644 files)
    // -- the in-kernel representation carries them so stat()/the future
    // multi-user (Phase H) module slot in without changing the interface.
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t mode = 0;  // filled in by lateran_list (S_IFREG|0644 etc.)
    uint32_t atime = 0; // seconds since Unix epoch (0 if unknown)
    uint32_t mtime = 0;
    uint32_t ctime = 0;
    uint16_t nlink = 1; // hard-link count
    // ext2 inode number (0 if FAT16 or not yet filled). Needed for fstat's
    // st_ino field — musl ld's dlopen dedup keys on (st_dev, st_ino), and
    // collapsing every distinct file to ino=0 makes it skip mmap on second
    // and later opens (libstdc++ etc.).
    uint32_t ino = 0;
};

// Mount the disk archive, detecting its format (ext2 preferred, else FAT16).
bool lateran_mount();
bool lateran_mounted();

// "ext2", "FAT16", or "none" -- for the boot banner / ls labels.
const char *lateran_format();

// Fill `out` with up to `max` root-directory file entries; returns the count.
uint32_t lateran_list(LateranEntry *out, uint32_t max);

// Read file `name` (8.3, case-insensitive) into `buf` (up to `cap` bytes).
// Returns bytes read, or -1 if not found / not mounted.
int64_t lateran_read_file(const char *name, char *buf, uint32_t cap);

// --- write support (#7) ---------------------------------------------------

// Create or overwrite root-directory file `name` with `len` bytes from `buf`
// (allocating/freeing FAT clusters and updating the directory entry). Returns
// the bytes written, or -1 on error (disk full, root full, not mounted). The
// data persists to the disk image.
int64_t lateran_write_file(const char *name, const char *buf, uint32_t len);

// Delete file `name` (path with optional subdirectories): free its cluster
// chain and mark the directory entry deleted. Returns true if removed.
bool lateran_unlink(const char *name);

// Create directory `path` (its parent must exist). Allocates a cluster, writes
// "." and ".." entries, and adds a directory entry in the parent. Returns
// false if the parent is missing, the name already exists, or the disk is full.
bool lateran_mkdir(const char *path);

// True if `path` (empty/"/" = root) names an existing directory.
bool lateran_is_dir(const char *path);

// List the entries of directory `path` (empty/"/" = root) into `out` (up to
// `max`). Returns the count. Includes subdirectories (is_dir set).
uint32_t lateran_list_dir(const char *path, LateranEntry *out, uint32_t max);

// Stat a path: fill `out` with mode/size/uid/gid/atime/mtime/ctime. Returns
// false if the path doesn't exist. Follows symlinks. Synthesises sensible
// defaults for the FAT backend.
bool lateran_stat(const char *path, LateranEntry *out);

// Read the target of a symbolic link at `path`. Returns the length of the
// NUL-terminated target written to `out`, or -1 if the path isn't a symlink,
// doesn't exist, or doesn't fit in `cap`. Only ext2 supports symlinks; the
// FAT backend always returns -1.
int32_t lateran_readlink(const char *path, char *out, uint32_t cap);

// Create a symbolic link at `path` whose target string is `target`. Returns
// false on failure (parent not present, link exists, FS doesn't support
// symlinks). FAT backend always returns false.
bool lateran_symlink(const char *target, const char *path);

// Atomic-ish rename (POSIX rename semantics): if `new_path` already exists it
// is replaced; if `old_path` doesn't exist returns false.
bool lateran_rename(const char *old_path, const char *new_path);

// Truncate `path` to exactly `new_size` bytes (free trailing blocks if
// shorter; extend with implicit zero fill if longer). ext2-only.
bool lateran_truncate(const char *path, uint64_t new_size);

// Change permission/owner of `path`. uid/gid == ~0 leaves that field intact
// (matches Linux semantics for chown's "no change" sentinel).
bool lateran_chmod(const char *path, uint32_t new_mode);
bool lateran_chown(const char *path, uint32_t uid, uint32_t gid);

// Create a hard link `link_path` pointing at the same inode as `target`.
// ext2-only (FAT has no inode concept).
bool lateran_link(const char *target, const char *link_path);

// Set atime/mtime on `path`. -1 means "do not change that field".
bool lateran_utime(const char *path, int64_t atime, int64_t mtime);

} // namespace index
