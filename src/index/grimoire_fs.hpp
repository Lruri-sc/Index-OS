#pragma once

#include <stdint.h>

namespace index {

// GrimoireFS is the first shelf of the Bookshelf: a tiny read-only filesystem
// whose "files" are grimoires baked into the kernel image. Index memorises
// 103,000 grimoires; this is a humble start with a handful, enough to back
// `ls` and `cat` until a real on-disk format arrives.
struct Grimoire {
    const char *name;
    const char *text; // null-terminated contents
};

uint32_t grimoire_fs_count();
const Grimoire *grimoire_fs_at(uint32_t index);
const Grimoire *grimoire_fs_find(const char *name);

} // namespace index
