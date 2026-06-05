#include "index/ext2.hpp"

#include "drivers/underline.hpp"
#include "index/idol_theory.hpp"

namespace index {

namespace {

// On-disk little-endian field readers.
uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(uint8_t *p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void wr32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}

constexpr uint32_t kSector = 512;
constexpr uint32_t kMaxBlock = 4096; // largest block size we support

struct Super {
    bool mounted = false;
    uint32_t block_size = 0;
    uint32_t inode_size = 0;
    uint32_t inodes_per_group = 0;
    uint32_t blocks_per_group = 0;
    uint32_t first_data_block = 0;
    uint32_t inode_count = 0;
    uint32_t block_count = 0;
    uint32_t group_count = 0;
    uint32_t gd_block = 0; // block where the group-descriptor table starts
};

Super g_sb;
alignas(16) uint8_t g_blk[kMaxBlock];   // general block scratch (data / dir)
alignas(16) uint8_t g_ind[kMaxBlock];   // single-indirect block scratch
alignas(16) uint8_t g_ind2[kMaxBlock];  // double-indirect mid scratch
alignas(16) uint8_t g_inode[256];       // one inode

// [BLKCACHE] Direct-mapped buffer cache. ext2 had no block cache, so every
// read_block hit the virtio-blk spin-poll (underline_read) -- class loading and
// repeated inode/indirect/dir-block reads re-fetched the same blocks thousands
// of times. A 2 MB direct-mapped cache turns the repeats into a memcpy. Write-
// through (write_block updates the line) keeps it coherent with the disk. Safe
// under the FsGuard that serializes all Lateran/ext2 access -- the same single-
// threading the g_blk/g_ind scratch buffers above already rely on.
constexpr uint32_t kBlkCacheN = 512; // 512 lines * 4 KB = 2 MB
struct BlkCacheLine { uint32_t block; bool valid; };
BlkCacheLine g_bc_meta[kBlkCacheN];
alignas(16) uint8_t g_bc_data[kBlkCacheN][kMaxBlock];
void bc_copy(uint8_t *d, const uint8_t *s, uint32_t n) { for (uint32_t i = 0; i < n; ++i) d[i] = s[i]; }

// Current wall-clock seconds for inode timestamps. ext2 stores i_atime/i_ctime/
// i_mtime as u32 seconds since the Unix epoch at offsets 8/12/16.
uint32_t now_epoch_u32() {
    const uint64_t s = idol_theory_epoch_seconds();
    return s > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(s);
}

void stamp_inode_all(uint32_t t) {
    wr32(&g_inode[8], t);  // i_atime
    wr32(&g_inode[12], t); // i_ctime
    wr32(&g_inode[16], t); // i_mtime
}

const drivers::Underline &disk() { return drivers::underline_status(); }

// Read a whole filesystem block into `dst`.
bool read_block(uint32_t block, uint8_t *dst) {
    const uint32_t bs = g_sb.block_size;
    const uint32_t idx = block % kBlkCacheN;
    if (g_bc_meta[idx].valid && g_bc_meta[idx].block == block) {
        bc_copy(dst, g_bc_data[idx], bs); // hit: skip the virtio-blk spin-poll
        return true;
    }
    const uint32_t spb = bs / kSector;
    for (uint32_t s = 0; s < spb; ++s) {
        if (!drivers::underline_read(disk(), static_cast<uint64_t>(block) * spb + s, dst + s * kSector)) {
            return false;
        }
    }
    bc_copy(g_bc_data[idx], dst, bs); // fill the line
    g_bc_meta[idx].block = block;
    g_bc_meta[idx].valid = true;
    return true;
}

bool write_block(uint32_t block, const uint8_t *src) {
    const uint32_t spb = g_sb.block_size / kSector;
    for (uint32_t s = 0; s < spb; ++s) {
        if (!drivers::underline_write(disk(), static_cast<uint64_t>(block) * spb + s, src + s * kSector)) {
            return false;
        }
    }
    const uint32_t idx = block % kBlkCacheN; // write-through: keep the line coherent
    bc_copy(g_bc_data[idx], src, g_sb.block_size);
    g_bc_meta[idx].block = block;
    g_bc_meta[idx].valid = true;
    return true;
}

// Read the group descriptor for group `g` into the 32-byte `out`.
bool read_gd(uint32_t g, uint8_t *out) {
    const uint64_t byte = static_cast<uint64_t>(g_sb.gd_block) * g_sb.block_size + g * 32;
    const uint32_t blk = static_cast<uint32_t>(byte / g_sb.block_size);
    const uint32_t off = static_cast<uint32_t>(byte % g_sb.block_size);
    if (!read_block(blk, g_blk)) return false;
    for (uint32_t i = 0; i < 32; ++i) out[i] = g_blk[off + i];
    return true;
}

// Read inode `ino` (1-based) into g_inode. Returns false on error.
bool read_inode(uint32_t ino) {
    if (ino == 0) return false;
    const uint32_t grp = (ino - 1) / g_sb.inodes_per_group;
    const uint32_t idx = (ino - 1) % g_sb.inodes_per_group;
    uint8_t gd[32];
    if (!read_gd(grp, gd)) return false;
    const uint32_t inode_table = rd32(&gd[8]);
    const uint64_t byte = static_cast<uint64_t>(inode_table) * g_sb.block_size +
                          static_cast<uint64_t>(idx) * g_sb.inode_size;
    const uint32_t blk = static_cast<uint32_t>(byte / g_sb.block_size);
    const uint32_t off = static_cast<uint32_t>(byte % g_sb.block_size);
    if (!read_block(blk, g_blk)) return false;
    const uint32_t n = g_sb.inode_size < sizeof(g_inode) ? g_sb.inode_size : sizeof(g_inode);
    for (uint32_t i = 0; i < n; ++i) g_inode[i] = g_blk[off + i];
    return true;
}

// Map a file's logical block number to its physical block (0 = hole/sparse).
// Supports direct, single-indirect and double-indirect (enough for multi-MB
// files at any supported block size). The current inode is in g_inode.
uint32_t map_block(uint32_t lbn) {
    const uint8_t *iblock = &g_inode[40];
    const uint32_t per = g_sb.block_size / 4; // u32 entries per indirect block
    if (lbn < 12) {
        return rd32(&iblock[lbn * 4]);
    }
    lbn -= 12;
    if (lbn < per) {
        const uint32_t ind = rd32(&iblock[12 * 4]);
        if (ind == 0 || !read_block(ind, g_ind)) return 0;
        return rd32(&g_ind[lbn * 4]);
    }
    lbn -= per;
    if (lbn < per * per) {
        const uint32_t dind = rd32(&iblock[13 * 4]);
        if (dind == 0 || !read_block(dind, g_ind2)) return 0;
        const uint32_t mid = rd32(&g_ind2[(lbn / per) * 4]);
        if (mid == 0 || !read_block(mid, g_ind)) return 0;
        return rd32(&g_ind[(lbn % per) * 4]);
    }
    return 0; // triple-indirect not supported
}

uint16_t inode_mode() { return rd16(&g_inode[0]); }
uint32_t inode_size() { return rd32(&g_inode[4]); }
bool inode_is_dir() { return (inode_mode() & 0xF000) == 0x4000; }
bool inode_is_symlink() { return (inode_mode() & 0xF000) == 0xA000; }

// Read a symlink's target string from the current g_inode into `out` (NUL-
// terminated). Returns the length, or -1 if this inode isn't a symlink or the
// target is too large for `cap`. Fast symlinks (size <= 60) store the target
// inline in i_block; slow ones in regular data blocks.
int32_t inode_read_symlink(char *out, uint32_t cap) {
    if (!inode_is_symlink()) return -1;
    const uint32_t size = inode_size();
    if (size == 0 || size + 1 > cap) return -1;
    if (size <= 60) {
        for (uint32_t i = 0; i < size; ++i) out[i] = static_cast<char>(g_inode[40 + i]);
        out[size] = 0;
        return static_cast<int32_t>(size);
    }
    const uint32_t pb = map_block(0);
    if (pb == 0 || !read_block(pb, g_blk)) return -1;
    const uint32_t lim = size < g_sb.block_size ? size : g_sb.block_size;
    for (uint32_t i = 0; i < lim; ++i) out[i] = static_cast<char>(g_blk[i]);
    out[lim] = 0;
    return static_cast<int32_t>(lim);
}

// Find `name` in the directory inode currently in g_inode; return its inode
// number, or 0 if not present. ext2 file_type in the dirent is optional
// (genext2fs omits it), so callers must read the target inode for the type.
uint32_t dir_lookup(const char *name) {
    const uint32_t size = inode_size();
    const uint32_t bs = g_sb.block_size;
    for (uint32_t lbn = 0; lbn * bs < size; ++lbn) {
        const uint32_t pb = map_block(lbn);
        if (pb == 0 || !read_block(pb, g_blk)) continue;
        uint32_t off = 0;
        while (off + 8 <= bs) {
            const uint32_t e_ino = rd32(&g_blk[off]);
            const uint16_t rec = rd16(&g_blk[off + 4]);
            const uint8_t nlen = g_blk[off + 6];
            if (rec == 0) break;
            // Validate the dirent against the block bound before reading its
            // name: a corrupt/malicious ext2 image can set name_len so that
            // off+8+nlen runs past the block scratch g_blk -> over-read of
            // adjacent kernel memory. Mirrors Linux ext2_check_dir_entry
            // (name_len + 8 <= rec_len, entry within the block). Skip the whole
            // block on a malformed entry rather than trusting it.
            if (static_cast<uint32_t>(off) + 8u + nlen > bs ||
                rec < 8u + nlen) {
                break;
            }
            if (e_ino != 0) {
                bool eq = true;
                for (uint32_t i = 0; i < nlen && eq; ++i) {
                    if (g_blk[off + 8 + i] != static_cast<uint8_t>(name[i])) eq = false;
                }
                if (eq && name[nlen] == 0) {
                    return e_ino;
                }
            }
            off += rec;
        }
    }
    return 0;
}

// Resolve an absolute (or root-relative) path to its inode number, starting
// from `start_ino`. Leaves g_inode holding the resolved inode on success.
// Follows up to ~8 symlinks (anti-loop). If `nofollow_last` is true the final
// component is returned as-is even if it's a symlink (used by readlink).
uint32_t resolve_from(uint32_t start_ino, const char *path, uint32_t depth,
                      bool nofollow_last) {
    if (depth > 8) return 0;
    uint32_t parent = start_ino;
    if (!read_inode(parent)) return 0;
    const char *p = path;
    while (*p == '/') {
        ++p;
        parent = 2; // absolute restart at root
    }
    if (!read_inode(parent)) return 0;
    while (*p) {
        char comp[64];
        uint32_t n = 0;
        while (*p && *p != '/' && n + 1 < sizeof(comp)) comp[n++] = *p++;
        comp[n] = 0;
        const char *rest = p;
        while (*rest == '/') ++rest;
        if (!inode_is_dir()) return 0; // intermediate is not a directory
        const uint32_t next = dir_lookup(comp);
        if (next == 0) return 0;
        if (!read_inode(next)) return 0;
        const bool last = (*rest == 0);

        if (inode_is_symlink() && !(last && nofollow_last)) {
            char target[256];
            const int32_t tlen = inode_read_symlink(target, sizeof(target));
            if (tlen <= 0) return 0;
            // Rebuild the path: target + "/" + rest (if any).
            char newp[512];
            uint32_t np = 0;
            for (int32_t k = 0; k < tlen && np + 1 < sizeof(newp); ++k) newp[np++] = target[k];
            if (*rest) {
                if (np > 0 && newp[np - 1] != '/' && np + 1 < sizeof(newp)) newp[np++] = '/';
                while (*rest && np + 1 < sizeof(newp)) newp[np++] = *rest++;
            }
            newp[np] = 0;
            // Absolute target restarts from root; relative continues from the
            // directory the symlink lived in (i.e. `parent`).
            const uint32_t restart = (target[0] == '/') ? 2 : parent;
            return resolve_from(restart, newp, depth + 1, nofollow_last);
        }

        parent = next;
        p = rest;
    }
    return parent;
}

// Public-style entry: absolute paths only (the wrapper expectation).
uint32_t resolve(const char *path) {
    return resolve_from(2, path, 0, false);
}

} // namespace

bool ext2_mount() {
    g_sb.mounted = false;
    if (!disk().present) return false;
    // The superblock lives at byte offset 1024 (sectors 2..3).
    uint8_t sb[1024];
    if (!drivers::underline_read(disk(), 2, sb) || !drivers::underline_read(disk(), 3, sb + 512)) {
        return false;
    }
    if (rd16(&sb[56]) != 0xEF53) {
        return false; // not ext2
    }
    Super s;
    s.inode_count = rd32(&sb[0]);
    s.block_count = rd32(&sb[4]);
    s.first_data_block = rd32(&sb[20]);
    s.block_size = 1024u << rd32(&sb[24]);
    s.blocks_per_group = rd32(&sb[32]);
    s.inodes_per_group = rd32(&sb[40]);
    s.inode_size = rd16(&sb[88]);
    if (s.inode_size == 0) s.inode_size = 128; // rev 0 default
    if (s.block_size == 0 || s.block_size > kMaxBlock || s.blocks_per_group == 0 ||
        s.inodes_per_group == 0) {
        return false;
    }
    s.group_count = (s.block_count - s.first_data_block + s.blocks_per_group - 1) / s.blocks_per_group;
    s.gd_block = s.first_data_block + 1; // GD table follows the superblock block
    s.mounted = true;
    g_sb = s;
    return true;
}

bool ext2_mounted() { return g_sb.mounted; }

int64_t ext2_read_file(const char *path, char *buf, uint32_t cap) {
    if (!g_sb.mounted) return -1;
    const uint32_t ino = resolve(path);
    if (ino == 0) return -1;
    if (inode_is_dir()) return -1; // not a regular file
    const uint32_t size = inode_size();
    const uint32_t want = size < cap ? size : cap;
    const uint32_t bs = g_sb.block_size;
    uint32_t done = 0;
    for (uint32_t lbn = 0; done < want; ++lbn) {
        const uint32_t pb = map_block(lbn);
        uint32_t n = want - done;
        if (n > bs) n = bs;
        if (pb == 0) {
            for (uint32_t i = 0; i < n; ++i) buf[done + i] = 0; // sparse hole
        } else {
            if (!read_block(pb, g_blk)) break;
            for (uint32_t i = 0; i < n; ++i) buf[done + i] = static_cast<char>(g_blk[i]);
        }
        done += n;
    }
    return static_cast<int64_t>(done);
}

// Like ext2_read_file but reads `len` bytes starting at byte `offset` -- the
// demand-paging primitive (file-backed mmap faults one page at a time instead
// of slurping the whole file into the kernel heap). Seeks to offset/block_size,
// honors the in-block offset, never crosses a block boundary per iteration, and
// clamps to the file size. Returns bytes read (0 at/past EOF), -1 on error.
int64_t ext2_pread(const char *path, uint64_t offset, char *buf, uint32_t len) {
    if (!g_sb.mounted) return -1;
    const uint32_t ino = resolve(path);
    if (ino == 0) return -1;
    if (inode_is_dir()) return -1; // not a regular file
    const uint32_t size = inode_size();
    if (offset >= size) return 0; // at or past EOF
    const uint64_t avail = size - offset;
    uint32_t want = (len < avail) ? len : static_cast<uint32_t>(avail);
    const uint32_t bs = g_sb.block_size;
    uint32_t done = 0;
    while (done < want) {
        const uint64_t cur = offset + done;
        const uint32_t lbn = static_cast<uint32_t>(cur / bs);
        const uint32_t boff = static_cast<uint32_t>(cur % bs); // byte offset within the block
        const uint32_t pb = map_block(lbn);
        uint32_t n = want - done;
        if (n > bs - boff) n = bs - boff; // stay within this block per iteration
        if (pb == 0) {
            for (uint32_t i = 0; i < n; ++i) buf[done + i] = 0; // sparse hole
        } else {
            if (!read_block(pb, g_blk)) break;
            for (uint32_t i = 0; i < n; ++i) buf[done + i] = static_cast<char>(g_blk[boff + i]);
        }
        done += n;
    }
    return static_cast<int64_t>(done);
}

bool ext2_is_dir(const char *path) {
    if (!g_sb.mounted) return false;
    if (path == nullptr || path[0] == 0) return true;
    const char *p = path;
    while (*p == '/') ++p;
    if (*p == 0) return true; // root
    if (resolve(path) == 0) return false;
    return inode_is_dir();
}

int32_t ext2_readlink(const char *path, char *out, uint32_t cap) {
    if (!g_sb.mounted || path == nullptr || out == nullptr || cap == 0) return -1;
    if (resolve_from(2, path, 0, /*nofollow_last=*/true) == 0) return -1;
    if (!inode_is_symlink()) return -1;
    return inode_read_symlink(out, cap);
}

uint32_t ext2_list_dir(const char *path, LateranEntry *out, uint32_t max) {
    if (!g_sb.mounted) return 0;
    // Resolve the directory (empty/"/" = root inode 2). After this g_inode holds
    // the directory inode -- pass 1 must not call read_inode so it stays valid.
    const char *p = (path == nullptr) ? "" : path;
    while (*p == '/') ++p;
    if (*p == 0) {
        if (!read_inode(2)) return 0;
    } else if (resolve(path) == 0) {
        return 0;
    }
    if (!inode_is_dir()) return 0;

    const uint32_t size = inode_size();
    const uint32_t bs = g_sb.block_size;
    // Pass 1: collect (inode, name) for every entry without reading child
    // inodes (which would clobber g_inode/g_blk). Stash the inode number in
    // first_cluster temporarily.
    uint32_t count = 0;
    for (uint32_t lbn = 0; lbn * bs < size && count < max; ++lbn) {
        const uint32_t pb = map_block(lbn);
        if (pb == 0 || !read_block(pb, g_blk)) continue;
        uint32_t off = 0;
        while (off + 8 <= bs && count < max) {
            const uint32_t e_ino = rd32(&g_blk[off]);
            const uint16_t rec = rd16(&g_blk[off + 4]);
            const uint8_t nlen = g_blk[off + 6];
            if (rec == 0) break;
            if (e_ino != 0 && nlen > 0) {
                uint32_t cn = 0;
                for (; cn < nlen && cn + 1 < sizeof(out[count].name); ++cn) {
                    out[count].name[cn] = static_cast<char>(g_blk[off + 8 + cn]);
                }
                out[count].name[cn] = 0;
                out[count].first_cluster = static_cast<uint16_t>(e_ino); // temp
                out[count].size = e_ino; // stash full inode no. in size temporarily
                ++count;
            }
            off += rec;
        }
    }
    // Pass 2: fill real metadata from each child inode.
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e_ino = out[i].size; // recover stashed inode number
        if (read_inode(e_ino)) {
            out[i].is_dir = inode_is_dir();
            out[i].size = inode_size();
            out[i].uid = rd16(&g_inode[2]);
            out[i].gid = rd16(&g_inode[24]);
            out[i].mode = inode_mode();
            out[i].atime = rd32(&g_inode[8]);
            out[i].ctime = rd32(&g_inode[12]);
            out[i].mtime = rd32(&g_inode[16]);
        } else {
            out[i].is_dir = false;
            out[i].size = 0;
            out[i].uid = 0;
            out[i].gid = 0;
            out[i].mode = 0100644;
            out[i].atime = out[i].mtime = out[i].ctime = 0;
        }
        out[i].first_cluster = 0;
    }
    return count;
}

bool ext2_fs_stats(Ext2FsStats *out) {
    if (!g_sb.mounted || out == nullptr) return false;
    // Superblock at byte 1024 holds the live free counters at offsets 12/16.
    uint8_t sb[1024];
    if (!drivers::underline_read(disk(), 2, sb) ||
        !drivers::underline_read(disk(), 3, sb + 512)) {
        return false;
    }
    out->block_size   = g_sb.block_size;
    out->total_blocks = g_sb.block_count;
    out->free_blocks  = rd32(&sb[12]);
    out->total_inodes = g_sb.inode_count;
    out->free_inodes  = rd32(&sb[16]);
    out->namelen_max  = 255;
    return true;
}

bool ext2_stat(const char *path, LateranEntry *out) {
    if (!g_sb.mounted || path == nullptr || out == nullptr) return false;
    const uint32_t ino = resolve(path);
    if (ino == 0) return false;
    out->is_dir = inode_is_dir();
    out->size = inode_size();
    // Index is single-user: uid 0 (crowley) owns the entire rootfs. The uid/gid
    // genext2fs baked into the image are the host builder's (e.g. 501:20) and are
    // meaningless here -- worse, they trip software like sshd that checks
    // st_uid==0 on its privsep dir. Report root ownership.
    out->uid = 0;
    out->gid = 0;
    out->mode = inode_mode();
    out->atime = rd32(&g_inode[8]);
    out->ctime = rd32(&g_inode[12]);
    out->mtime = rd32(&g_inode[16]);
    out->nlink = rd16(&g_inode[26]);
    out->first_cluster = 0;
    out->name[0] = 0;
    out->ino = ino;
    return true;
}

// --- write side -----------------------------------------------------------

namespace {

uint32_t roundup4(uint32_t x) { return (x + 3u) & ~3u; }

// Adjust the superblock's free-block / free-inode counters (at byte 1024).
void sb_adjust(int32_t dblocks, int32_t dinodes) {
    uint8_t sb[1024];
    if (!drivers::underline_read(disk(), 2, sb) || !drivers::underline_read(disk(), 3, sb + 512)) {
        return;
    }
    wr32(&sb[12], rd32(&sb[12]) + static_cast<uint32_t>(dblocks));
    wr32(&sb[16], rd32(&sb[16]) + static_cast<uint32_t>(dinodes));
    drivers::underline_write(disk(), 2, sb);
    drivers::underline_write(disk(), 3, sb + 512);
}

// Write a 32-byte group descriptor back.
bool write_gd(uint32_t g, const uint8_t *gd) {
    const uint64_t byte = static_cast<uint64_t>(g_sb.gd_block) * g_sb.block_size + g * 32;
    const uint32_t blk = static_cast<uint32_t>(byte / g_sb.block_size);
    const uint32_t off = static_cast<uint32_t>(byte % g_sb.block_size);
    if (!read_block(blk, g_ind)) return false; // use g_ind (g_blk may hold data)
    for (uint32_t i = 0; i < 32; ++i) g_ind[off + i] = gd[i];
    return write_block(blk, g_ind);
}

// Write g_inode back to inode `ino`'s on-disk slot.
bool write_inode(uint32_t ino) {
    const uint32_t grp = (ino - 1) / g_sb.inodes_per_group;
    const uint32_t idx = (ino - 1) % g_sb.inodes_per_group;
    uint8_t gd[32];
    if (!read_gd(grp, gd)) return false;
    const uint32_t inode_table = rd32(&gd[8]);
    const uint64_t byte = static_cast<uint64_t>(inode_table) * g_sb.block_size +
                          static_cast<uint64_t>(idx) * g_sb.inode_size;
    const uint32_t blk = static_cast<uint32_t>(byte / g_sb.block_size);
    const uint32_t off = static_cast<uint32_t>(byte % g_sb.block_size);
    if (!read_block(blk, g_ind)) return false;
    for (uint32_t i = 0; i < g_sb.inode_size; ++i) g_ind[off + i] = g_inode[i];
    return write_block(blk, g_ind);
}

// Allocate a free block (data) from the bitmaps. Returns its block number or 0.
uint32_t alloc_block() {
    for (uint32_t g = 0; g < g_sb.group_count; ++g) {
        uint8_t gd[32];
        if (!read_gd(g, gd)) return 0;
        if (rd16(&gd[12]) == 0) continue; // no free blocks in this group
        const uint32_t bbmp = rd32(&gd[0]);
        if (!read_block(bbmp, g_ind)) return 0;
        for (uint32_t i = 0; i < g_sb.blocks_per_group; ++i) {
            const uint32_t bno = g_sb.first_data_block + g * g_sb.blocks_per_group + i;
            if (bno >= g_sb.block_count) break;
            if (!(g_ind[i / 8] & (1u << (i % 8)))) {
                g_ind[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
                if (!write_block(bbmp, g_ind)) return 0;
                wr16(&gd[12], static_cast<uint16_t>(rd16(&gd[12]) - 1));
                write_gd(g, gd);
                sb_adjust(-1, 0);
                // Zero the freshly allocated block.
                for (uint32_t b = 0; b < g_sb.block_size; ++b) g_blk[b] = 0;
                write_block(bno, g_blk);
                return bno;
            }
        }
    }
    return 0;
}

void free_block(uint32_t bno) {
    if (bno < g_sb.first_data_block) return;
    const uint32_t rel = bno - g_sb.first_data_block;
    const uint32_t g = rel / g_sb.blocks_per_group;
    const uint32_t i = rel % g_sb.blocks_per_group;
    uint8_t gd[32];
    if (!read_gd(g, gd)) return;
    const uint32_t bbmp = rd32(&gd[0]);
    if (!read_block(bbmp, g_ind)) return;
    if (g_ind[i / 8] & (1u << (i % 8))) {
        g_ind[i / 8] &= static_cast<uint8_t>(~(1u << (i % 8)));
        write_block(bbmp, g_ind);
        wr16(&gd[12], static_cast<uint16_t>(rd16(&gd[12]) + 1));
        write_gd(g, gd);
        sb_adjust(1, 0);
    }
}

// Allocate a free inode. `is_dir` updates the group's used-dirs count. Returns
// the inode number or 0.
uint32_t alloc_inode(bool is_dir) {
    for (uint32_t g = 0; g < g_sb.group_count; ++g) {
        uint8_t gd[32];
        if (!read_gd(g, gd)) return 0;
        if (rd16(&gd[14]) == 0) continue; // no free inodes
        const uint32_t ibmp = rd32(&gd[4]);
        if (!read_block(ibmp, g_ind)) return 0;
        for (uint32_t i = 0; i < g_sb.inodes_per_group; ++i) {
            if (!(g_ind[i / 8] & (1u << (i % 8)))) {
                g_ind[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
                if (!write_block(ibmp, g_ind)) return 0;
                wr16(&gd[14], static_cast<uint16_t>(rd16(&gd[14]) - 1));
                if (is_dir) wr16(&gd[16], static_cast<uint16_t>(rd16(&gd[16]) + 1));
                write_gd(g, gd);
                sb_adjust(0, -1);
                return g * g_sb.inodes_per_group + i + 1;
            }
        }
    }
    return 0;
}

void free_inode(uint32_t ino, bool was_dir) {
    const uint32_t g = (ino - 1) / g_sb.inodes_per_group;
    const uint32_t i = (ino - 1) % g_sb.inodes_per_group;
    uint8_t gd[32];
    if (!read_gd(g, gd)) return;
    const uint32_t ibmp = rd32(&gd[4]);
    if (!read_block(ibmp, g_ind)) return;
    if (g_ind[i / 8] & (1u << (i % 8))) {
        g_ind[i / 8] &= static_cast<uint8_t>(~(1u << (i % 8)));
        write_block(ibmp, g_ind);
        wr16(&gd[14], static_cast<uint16_t>(rd16(&gd[14]) + 1));
        if (was_dir && rd16(&gd[16]) > 0) wr16(&gd[16], static_cast<uint16_t>(rd16(&gd[16]) - 1));
        write_gd(g, gd);
        sb_adjust(0, 1);
    }
}

// Resolve a path to (parent dir inode, leaf). Leaves g_inode = parent inode.
bool resolve_parent(const char *path, uint32_t *parent_ino, char *leaf, uint32_t cap) {
    uint32_t ino = 2;
    if (!read_inode(ino)) return false;
    const char *p = path;
    while (*p == '/') ++p;
    if (*p == 0) return false; // no leaf (it's the root)
    for (;;) {
        char comp[64];
        uint32_t n = 0;
        while (*p && *p != '/' && n + 1 < sizeof(comp)) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') ++p;
        if (*p == 0) {
            uint32_t i = 0;
            for (; comp[i] && i + 1 < cap; ++i) leaf[i] = comp[i];
            leaf[i] = 0;
            *parent_ino = ino;
            read_inode(ino); // ensure g_inode = parent
            return leaf[0] != 0;
        }
        if (!inode_is_dir()) return false;
        const uint32_t next = dir_lookup(comp);
        if (next == 0 || !read_inode(next)) return false;
        ino = next;
    }
}

// Insert a directory entry (name -> ino, type) into directory `dir_ino`. Splits
// an existing entry's slack, or appends a new directory block if needed.
bool add_dirent(uint32_t dir_ino, const char *name, uint32_t ino, uint8_t type) {
    uint32_t nlen = 0;
    while (name[nlen]) ++nlen;
    const uint32_t need = roundup4(8 + nlen);
    const uint32_t bs = g_sb.block_size;
    if (!read_inode(dir_ino)) return false;
    const uint32_t dsize = inode_size();
    for (uint32_t lbn = 0; lbn * bs < dsize; ++lbn) {
        const uint32_t pb = map_block(lbn);
        if (pb == 0 || !read_block(pb, g_blk)) continue;
        uint32_t off = 0;
        while (off + 8 <= bs) {
            const uint32_t e_ino = rd32(&g_blk[off]);
            const uint16_t rec = rd16(&g_blk[off + 4]);
            const uint8_t en = g_blk[off + 6];
            if (rec == 0) break;
            const uint32_t used = (e_ino == 0) ? 0 : roundup4(8 + en);
            if (rec - used >= need) {
                // Split: shrink the existing entry, place the new one in the slack.
                uint32_t noff = off;
                uint16_t nrec = rec;
                if (e_ino != 0) {
                    wr16(&g_blk[off + 4], static_cast<uint16_t>(used));
                    noff = off + used;
                    nrec = static_cast<uint16_t>(rec - used);
                }
                wr32(&g_blk[noff], ino);
                wr16(&g_blk[noff + 4], nrec);
                g_blk[noff + 6] = static_cast<uint8_t>(nlen);
                g_blk[noff + 7] = type;
                for (uint32_t i = 0; i < nlen; ++i) g_blk[noff + 8 + i] = static_cast<uint8_t>(name[i]);
                return write_block(pb, g_blk);
            }
            off += rec;
        }
    }
    // No slack: append a fresh directory block.
    const uint32_t nb = alloc_block();
    if (nb == 0) return false;
    for (uint32_t i = 0; i < bs; ++i) g_blk[i] = 0;
    wr32(&g_blk[0], ino);
    wr16(&g_blk[4], static_cast<uint16_t>(bs));
    g_blk[6] = static_cast<uint8_t>(nlen);
    g_blk[7] = type;
    for (uint32_t i = 0; i < nlen; ++i) g_blk[8 + i] = static_cast<uint8_t>(name[i]);
    if (!write_block(nb, g_blk)) return false;
    // Link the new block into the dir inode (direct slot) and grow its size.
    if (!read_inode(dir_ino)) return false;
    const uint32_t lbn = dsize / bs;
    if (lbn >= 12) return false; // would need indirect dir blocks (unsupported)
    wr32(&g_inode[40 + lbn * 4], nb);
    wr32(&g_inode[4], dsize + bs);
    const uint32_t iblocks = rd32(&g_inode[28]) + bs / kSector;
    wr32(&g_inode[28], iblocks);
    return write_inode(dir_ino);
}

// Free all data blocks of the inode currently in g_inode (direct + single
// indirect). Used by truncate / unlink.
void free_inode_blocks() {
    const uint32_t per = g_sb.block_size / 4;
    for (uint32_t i = 0; i < 12; ++i) {
        const uint32_t b = rd32(&g_inode[40 + i * 4]);
        if (b) free_block(b);
    }
    const uint32_t ind = rd32(&g_inode[40 + 12 * 4]);
    if (ind) {
        if (read_block(ind, g_ind2)) {
            for (uint32_t i = 0; i < per; ++i) {
                const uint32_t b = rd32(&g_ind2[i * 4]);
                if (b) free_block(b);
            }
        }
        free_block(ind);
    }
}

} // namespace

int64_t ext2_write_file(const char *path, const char *buf, uint32_t len) {
    if (!g_sb.mounted) return -1;
    uint32_t parent_ino = 0;
    char leaf[64];
    if (!resolve_parent(path, &parent_ino, leaf, sizeof(leaf))) return -1;
    if (!inode_is_dir()) return -1; // parent isn't a directory

    // Existing file? Truncate it (free old blocks) and reuse the inode.
    const uint32_t existing = dir_lookup(leaf);
    uint32_t ino = existing;
    bool is_new = false;
    if (existing != 0) {
        if (!read_inode(existing)) return -1;
        if (inode_is_dir()) return -1; // refuse to overwrite a directory
        free_inode_blocks();
    } else {
        ino = alloc_inode(false);
        if (ino == 0) return -1;
        is_new = true;
    }

    // Build the inode and allocate/write the data blocks (direct + 1 indirect).
    for (uint32_t i = 0; i < g_sb.inode_size; ++i) g_inode[i] = 0;
    wr16(&g_inode[0], 0x8000 | 0644); // S_IFREG | rw-r--r--
    wr16(&g_inode[26], 1);            // links_count
    stamp_inode_all(now_epoch_u32()); // atime/ctime/mtime
    const uint32_t bs = g_sb.block_size;
    const uint32_t per = bs / 4;
    uint32_t indirect = 0;
    uint32_t blocks_used = 0;
    uint32_t written = 0;
    for (uint32_t lbn = 0; written < len; ++lbn) {
        const uint32_t db = alloc_block();
        if (db == 0) { /* disk full: best-effort, stop */ break; }
        ++blocks_used;
        uint32_t n = len - written;
        if (n > bs) n = bs;
        for (uint32_t i = 0; i < bs; ++i) g_blk[i] = i < n ? static_cast<uint8_t>(buf[written + i]) : 0;
        if (!write_block(db, g_blk)) break;
        if (lbn < 12) {
            wr32(&g_inode[40 + lbn * 4], db);
        } else if (lbn < 12 + per) {
            if (indirect == 0) {
                indirect = alloc_block();
                if (indirect == 0) break;
                ++blocks_used;
                wr32(&g_inode[40 + 12 * 4], indirect);
            }
            if (!read_block(indirect, g_ind)) break;
            wr32(&g_ind[(lbn - 12) * 4], db);
            write_block(indirect, g_ind);
        } else {
            break; // beyond single-indirect (unsupported for writes)
        }
        written += n;
    }
    wr32(&g_inode[4], written);                      // i_size
    wr32(&g_inode[28], blocks_used * (bs / kSector)); // i_blocks (512-byte units)
    if (!write_inode(ino)) return -1;

    if (is_new && !add_dirent(parent_ino, leaf, ino, 1 /*EXT2_FT_REG_FILE*/)) {
        return -1;
    }
    return static_cast<int64_t>(written);
}

bool ext2_symlink(const char *target, const char *path) {
    if (!g_sb.mounted || target == nullptr || path == nullptr) return false;
    uint32_t tlen = 0;
    while (target[tlen]) ++tlen;
    if (tlen == 0 || tlen > 255) return false; // ext2 symlink upper bound is 255 anyway
    uint32_t parent_ino = 0;
    char leaf[64];
    if (!resolve_parent(path, &parent_ino, leaf, sizeof(leaf))) return false;
    if (!inode_is_dir()) return false;
    if (dir_lookup(leaf) != 0) return false; // already exists

    const uint32_t ino = alloc_inode(false);
    if (ino == 0) return false;

    const uint32_t bs = g_sb.block_size;
    uint32_t blk = 0;
    if (tlen > 60) {
        blk = alloc_block();
        if (blk == 0) { free_inode(ino, false); return false; }
    }

    for (uint32_t i = 0; i < g_sb.inode_size; ++i) g_inode[i] = 0;
    wr16(&g_inode[0], 0xA000 | 0777); // S_IFLNK, mode rwxrwxrwx (the kernel ignores symlink mode bits)
    wr16(&g_inode[26], 1);            // links_count
    stamp_inode_all(now_epoch_u32());
    wr32(&g_inode[4], tlen);          // i_size = target length
    if (tlen <= 60) {
        // Fast symlink: inline in i_block (offset 40, 15 * 4 = 60 bytes).
        for (uint32_t i = 0; i < tlen; ++i) g_inode[40 + i] = static_cast<uint8_t>(target[i]);
        wr32(&g_inode[28], 0); // no data blocks
    } else {
        for (uint32_t i = 0; i < bs; ++i) g_blk[i] = (i < tlen) ? static_cast<uint8_t>(target[i]) : 0;
        if (!write_block(blk, g_blk)) {
            free_block(blk); free_inode(ino, false);
            return false;
        }
        wr32(&g_inode[40], blk);
        wr32(&g_inode[28], bs / kSector);
    }
    if (!write_inode(ino)) {
        if (blk) free_block(blk);
        free_inode(ino, false);
        return false;
    }
    if (!add_dirent(parent_ino, leaf, ino, 7 /*EXT2_FT_SYMLINK*/)) {
        if (blk) free_block(blk);
        free_inode(ino, false);
        return false;
    }
    return true;
}

bool ext2_mkdir(const char *path) {
    if (!g_sb.mounted) return false;
    uint32_t parent_ino = 0;
    char leaf[64];
    if (!resolve_parent(path, &parent_ino, leaf, sizeof(leaf))) return false;
    if (!inode_is_dir()) return false;
    if (dir_lookup(leaf) != 0) return false; // already exists

    const uint32_t ino = alloc_inode(true);
    if (ino == 0) return false;
    const uint32_t db = alloc_block();
    if (db == 0) { free_inode(ino, true); return false; }
    const uint32_t bs = g_sb.block_size;

    // Directory data block: "." (-> self) then ".." (-> parent), the second
    // entry's rec_len filling the rest of the block.
    for (uint32_t i = 0; i < bs; ++i) g_blk[i] = 0;
    wr32(&g_blk[0], ino); wr16(&g_blk[4], 12); g_blk[6] = 1; g_blk[7] = 2; g_blk[8] = '.';
    wr32(&g_blk[12], parent_ino); wr16(&g_blk[16], static_cast<uint16_t>(bs - 12));
    g_blk[18] = 2; g_blk[19] = 2; g_blk[20] = '.'; g_blk[21] = '.';
    if (!write_block(db, g_blk)) return false;

    // The directory inode: mode 0755, size = one block, links 2 (self + "."),
    // one direct block.
    for (uint32_t i = 0; i < g_sb.inode_size; ++i) g_inode[i] = 0;
    wr16(&g_inode[0], 0x4000 | 0755); // S_IFDIR
    wr16(&g_inode[26], 2);            // links: the dir's own entry + "."
    stamp_inode_all(now_epoch_u32());
    wr32(&g_inode[4], bs);
    wr32(&g_inode[28], bs / kSector);
    wr32(&g_inode[40], db);
    if (!write_inode(ino)) return false;

    // Bump the parent's link count (its new ".." back-reference).
    if (read_inode(parent_ino)) {
        wr16(&g_inode[26], static_cast<uint16_t>(rd16(&g_inode[26]) + 1));
        write_inode(parent_ino);
    }
    return add_dirent(parent_ino, leaf, ino, 2 /*EXT2_FT_DIR*/);
}

namespace {

// Strip the directory entry `leaf` from `dir_ino`. Folds the freed space into
// the previous entry's rec_len (or clears the inode field if it was first).
// Returns true if the entry was found and removed.
bool remove_dirent(uint32_t dir_ino, const char *leaf) {
    if (!read_inode(dir_ino)) return false;
    const uint32_t bs = g_sb.block_size;
    const uint32_t dsize = inode_size();
    uint32_t leaf_len = 0;
    while (leaf[leaf_len]) ++leaf_len;
    for (uint32_t lbn = 0; lbn * bs < dsize; ++lbn) {
        const uint32_t pb = map_block(lbn);
        if (pb == 0 || !read_block(pb, g_blk)) continue;
        uint32_t off = 0, prev = 0xFFFFFFFF;
        while (off + 8 <= bs) {
            const uint32_t e_ino = rd32(&g_blk[off]);
            const uint16_t rec = rd16(&g_blk[off + 4]);
            const uint8_t en = g_blk[off + 6];
            if (rec == 0) break;
            if (e_ino != 0 && en == leaf_len) {
                bool eq = true;
                for (uint32_t i = 0; i < en && eq; ++i) {
                    if (g_blk[off + 8 + i] != static_cast<uint8_t>(leaf[i])) eq = false;
                }
                if (eq) {
                    if (prev != 0xFFFFFFFF) {
                        wr16(&g_blk[prev + 4], static_cast<uint16_t>(rd16(&g_blk[prev + 4]) + rec));
                    } else {
                        wr32(&g_blk[off], 0); // first in block: zero ino
                    }
                    write_block(pb, g_blk);
                    return true;
                }
            }
            prev = off;
            off += rec;
        }
    }
    return false;
}

// Free blocks at logical positions [start_lbn, end_lbn) -- the i_block array's
// direct + indirect entries get cleared as we walk. Used by truncate and the
// post-unlink reclaim path. Does NOT touch i_size or i_blocks; the caller
// updates those.
void free_blocks_range(uint32_t start_lbn, uint32_t end_lbn) {
    const uint32_t bs = g_sb.block_size;
    const uint32_t per = bs / 4;
    for (uint32_t lbn = start_lbn; lbn < end_lbn; ++lbn) {
        if (lbn < 12) {
            const uint32_t b = rd32(&g_inode[40 + lbn * 4]);
            if (b) { free_block(b); wr32(&g_inode[40 + lbn * 4], 0); }
        } else if (lbn < 12 + per) {
            const uint32_t ind = rd32(&g_inode[40 + 12 * 4]);
            if (ind && read_block(ind, g_ind)) {
                const uint32_t k = lbn - 12;
                const uint32_t b = rd32(&g_ind[k * 4]);
                if (b) { free_block(b); wr32(&g_ind[k * 4], 0); }
                write_block(ind, g_ind);
            }
        }
    }
    // If we cleared the whole indirect block, drop it too.
    if (start_lbn <= 12 && end_lbn >= 12 + per) {
        const uint32_t ind = rd32(&g_inode[40 + 12 * 4]);
        if (ind) { free_block(ind); wr32(&g_inode[40 + 12 * 4], 0); }
    }
}

} // namespace

bool ext2_unlink(const char *path) {
    if (!g_sb.mounted) return false;
    uint32_t parent_ino = 0;
    char leaf[64];
    if (!resolve_parent(path, &parent_ino, leaf, sizeof(leaf))) return false;
    const uint32_t ino = dir_lookup(leaf);
    if (ino == 0) return false;
    if (!remove_dirent(parent_ino, leaf)) return false;

    // Drop a link; free the inode + its data when the count hits zero.
    if (read_inode(ino)) {
        const bool was_dir = inode_is_dir();
        uint16_t links = rd16(&g_inode[26]);
        if (links > 0) --links;
        wr16(&g_inode[26], links);
        write_inode(ino);
        if (links == 0) {
            free_inode_blocks();
            free_inode(ino, was_dir);
        }
    }
    return true;
}

bool ext2_rename(const char *old_path, const char *new_path) {
    if (!g_sb.mounted) return false;
    // Resolve both parents + leaves.
    uint32_t old_parent = 0, new_parent = 0;
    char old_leaf[64], new_leaf[64];
    if (!resolve_parent(old_path, &old_parent, old_leaf, sizeof(old_leaf))) return false;
    if (!resolve_parent(new_path, &new_parent, new_leaf, sizeof(new_leaf))) return false;
    if (!read_inode(old_parent)) return false;
    const uint32_t ino = dir_lookup(old_leaf);
    if (ino == 0) return false;
    if (!read_inode(ino)) return false;
    const bool is_dir = inode_is_dir();

    // If the new name already exists, unlink it first (POSIX semantics:
    // rename atomically replaces). For directories that's an error if
    // non-empty -- we don't enforce that yet; simplest is unlink it.
    if (!read_inode(new_parent)) return false;
    const uint32_t existing = dir_lookup(new_leaf);
    if (existing != 0) {
        if (existing == ino) {
            // Same file -- nothing to do (e.g., rename("a", "a")).
            return true;
        }
        // Remove the existing entry from new_parent.
        if (!remove_dirent(new_parent, new_leaf)) return false;
        if (read_inode(existing)) {
            const bool ex_dir = inode_is_dir();
            uint16_t links = rd16(&g_inode[26]);
            if (links > 0) --links;
            wr16(&g_inode[26], links);
            write_inode(existing);
            if (links == 0) {
                free_inode_blocks();
                free_inode(existing, ex_dir);
            }
        }
    }

    // Remove the source entry, then add the destination entry.
    if (!remove_dirent(old_parent, old_leaf)) return false;
    if (!add_dirent(new_parent, new_leaf, ino, is_dir ? 2 : 1)) return false;
    return true;
}

bool ext2_truncate(const char *path, uint64_t new_size) {
    if (!g_sb.mounted) return false;
    const uint32_t ino = resolve(path);
    if (ino == 0) return false;
    if (inode_is_dir()) return false;
    const uint32_t bs = g_sb.block_size;
    const uint32_t old_size = inode_size();
    const uint32_t new_blocks = static_cast<uint32_t>((new_size + bs - 1) / bs);
    const uint32_t old_blocks = (old_size + bs - 1) / bs;
    if (new_blocks < old_blocks) {
        free_blocks_range(new_blocks, old_blocks);
    }
    wr32(&g_inode[4], static_cast<uint32_t>(new_size));
    wr32(&g_inode[28], new_blocks * (bs / kSector));
    wr32(&g_inode[16], now_epoch_u32()); // mtime
    wr32(&g_inode[12], now_epoch_u32()); // ctime
    return write_inode(ino);
}

bool ext2_chmod(const char *path, uint32_t new_mode) {
    if (!g_sb.mounted) return false;
    const uint32_t ino = resolve(path);
    if (ino == 0) return false;
    // Preserve the file-type bits (top 4 of the 16-bit mode), replace the
    // permission bits.
    const uint16_t cur = rd16(&g_inode[0]);
    const uint16_t out = static_cast<uint16_t>((cur & 0xF000) | (new_mode & 0x0FFF));
    wr16(&g_inode[0], out);
    wr32(&g_inode[12], now_epoch_u32()); // ctime tracks metadata changes
    return write_inode(ino);
}

bool ext2_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (!g_sb.mounted) return false;
    const uint32_t ino = resolve(path);
    if (ino == 0) return false;
    if (uid != 0xffffffffu) wr16(&g_inode[2], static_cast<uint16_t>(uid));
    if (gid != 0xffffffffu) wr16(&g_inode[24], static_cast<uint16_t>(gid));
    wr32(&g_inode[12], now_epoch_u32());
    return write_inode(ino);
}

bool ext2_link(const char *target, const char *link_path) {
    if (!g_sb.mounted) return false;
    const uint32_t ino = resolve(target);
    if (ino == 0) return false;
    if (inode_is_dir()) return false; // POSIX forbids hard-linking directories
    // Bump links_count first so a partial failure doesn't drop the file.
    uint16_t links = rd16(&g_inode[26]);
    wr16(&g_inode[26], static_cast<uint16_t>(links + 1));
    wr32(&g_inode[12], now_epoch_u32());
    write_inode(ino);

    uint32_t parent = 0;
    char leaf[64];
    if (!resolve_parent(link_path, &parent, leaf, sizeof(leaf))) return false;
    if (!inode_is_dir()) return false;
    if (dir_lookup(leaf) != 0) return false; // already exists
    return add_dirent(parent, leaf, ino, 1 /*EXT2_FT_REG_FILE*/);
}

bool ext2_utime(const char *path, int64_t atime, int64_t mtime) {
    if (!g_sb.mounted) return false;
    const uint32_t ino = resolve(path);
    if (ino == 0) return false;
    if (atime >= 0) wr32(&g_inode[8], static_cast<uint32_t>(atime));
    if (mtime >= 0) wr32(&g_inode[16], static_cast<uint32_t>(mtime));
    wr32(&g_inode[12], now_epoch_u32()); // ctime always touches
    return write_inode(ino);
}

} // namespace index
