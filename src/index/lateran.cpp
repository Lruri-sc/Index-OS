#include "index/lateran.hpp"

#include "arch/aarch64/cpu.hpp" // arch::this_cpu_id for the re-entrant FsGuard
#include "drivers/stiyl_magnus.hpp"
#include "drivers/underline.hpp"
#include "index/anti_skill.hpp" // serialise the shared ext2/lateran block scratch
#include "index/ext2.hpp"
#include "index/procfs.hpp"
#include "index/testament.hpp" // tmpfs (in-memory) mounts: /tmp, /dev/shm

namespace {
// "/host" / "/host/foo" -> "/foo" / "" routed to StiylMagnus (virtio-9p).
// Returns nullptr if not a /host path.
const char *strip_host_prefix(const char *path) {
    if (path == nullptr || path[0] != '/') return nullptr;
    if (path[1] != 'h' || path[2] != 'o' || path[3] != 's' || path[4] != 't') {
        return nullptr;
    }
    if (path[5] == 0) return "";       // "/host"
    if (path[5] == '/') return path + 5; // "/host/foo" -> "/foo"
    return nullptr;
}
} // namespace

namespace index {

namespace {

// Lateran is the disk archive; it now detects the on-disk format at mount and
// routes the public API to either the ext2 backend (real Unix paths +
// metadata) or the legacy FAT16 backend below.
enum class Backend { none, fat, ext2 };
Backend g_backend = Backend::none;

constexpr uint32_t kSector = 512;

struct Layout {
    bool mounted = false;
    uint16_t bytes_per_sector = 0;
    uint8_t sectors_per_cluster = 0;
    uint16_t reserved = 0;
    uint8_t num_fats = 0;
    uint16_t root_entries = 0;
    uint16_t fat_size = 0;
    uint32_t fat_start = 0;
    uint32_t root_start = 0;
    uint32_t root_sectors = 0;
    uint32_t data_start = 0;
};

Layout g_fs;
alignas(16) uint8_t g_sector[kSector]; // scratch for one sector

uint16_t rd16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool read_sector(uint32_t sector) {
    return drivers::underline_read(drivers::underline_status(), sector, g_sector);
}

bool write_sector(uint32_t sector) {
    return drivers::underline_write(drivers::underline_status(), sector, g_sector);
}

void wr16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void wr32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

char up(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (up(*a) != up(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

// Convert a raw 11-byte 8.3 directory name into "NAME.EXT".
void format_83(const uint8_t *raw, char out[13]) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 8 && raw[i] != ' '; ++i) {
        out[n++] = static_cast<char>(raw[i]);
    }
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (uint32_t i = 8; i < 11 && raw[i] != ' '; ++i) {
            out[n++] = static_cast<char>(raw[i]);
        }
    }
    out[n] = 0;
}

// Next cluster in the chain, or >= 0xFFF8 for end-of-chain.
uint16_t fat_next(uint16_t cluster) {
    const uint32_t byte = static_cast<uint32_t>(cluster) * 2;
    const uint32_t sector = g_fs.fat_start + byte / kSector;
    if (!read_sector(sector)) {
        return 0xFFFF;
    }
    return rd16(&g_sector[byte % kSector]);
}

// --- write primitives (#7b) -----------------------------------------------

// Set FAT[cluster] = value in every FAT copy (FAT16 keeps num_fats mirrors).
bool fat_set(uint16_t cluster, uint16_t value) {
    const uint32_t byte = static_cast<uint32_t>(cluster) * 2;
    const uint32_t off = byte % kSector;
    for (uint8_t f = 0; f < g_fs.num_fats; ++f) {
        const uint32_t sector = g_fs.fat_start + static_cast<uint32_t>(f) * g_fs.fat_size + byte / kSector;
        if (!read_sector(sector)) return false;
        wr16(&g_sector[off], value);
        if (!write_sector(sector)) return false;
    }
    return true;
}

// Number of data clusters on the volume (cluster numbers run 2..total+1).
uint32_t total_clusters() {
    const uint64_t cap = drivers::underline_status().capacity_sectors;
    if (cap <= g_fs.data_start) return 0;
    return static_cast<uint32_t>((cap - g_fs.data_start) / g_fs.sectors_per_cluster);
}

uint32_t cluster_first_sector(uint16_t cluster) {
    return g_fs.data_start + (static_cast<uint32_t>(cluster) - 2) * g_fs.sectors_per_cluster;
}

// Find a free cluster, mark it end-of-chain, zero its data sectors, return it.
// Returns 0 if the volume is full.
uint16_t alloc_cluster() {
    const uint32_t total = total_clusters();
    for (uint32_t c = 2; c < total + 2 && c < 0xFFF8; ++c) {
        if (fat_next(static_cast<uint16_t>(c)) == 0) {
            if (!fat_set(static_cast<uint16_t>(c), 0xFFFF)) return 0;
            for (uint32_t i = 0; i < kSector; ++i) g_sector[i] = 0;
            for (uint8_t s = 0; s < g_fs.sectors_per_cluster; ++s) {
                write_sector(cluster_first_sector(static_cast<uint16_t>(c)) + s);
            }
            return static_cast<uint16_t>(c);
        }
    }
    return 0;
}

// Free a whole cluster chain (set each FAT entry back to 0).
void free_chain(uint16_t cluster) {
    uint32_t guard = 0;
    while (cluster >= 2 && cluster < 0xFFF8 && guard++ < 100000) {
        const uint16_t next = fat_next(cluster);
        fat_set(cluster, 0);
        cluster = next;
    }
}

// Convert "name.ext" into a raw 11-byte space-padded uppercase 8.3 field.
void to_83(const char *name, uint8_t *raw) {
    for (uint32_t i = 0; i < 11; ++i) raw[i] = ' ';
    uint32_t i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) raw[j++] = static_cast<uint8_t>(up(name[i++]));
    while (name[i] && name[i] != '.') ++i;
    if (name[i] == '.') {
        ++i;
        uint32_t k = 8;
        while (name[i] && k < 11) raw[k++] = static_cast<uint8_t>(up(name[i++]));
    }
}

// A directory is either the fixed-location root, or a subdirectory that lives
// in a cluster chain (start cluster). Generalising the directory operations
// over this lets the same code serve both.
struct DirLoc {
    bool is_root = true;
    uint16_t cluster = 0; // start cluster for subdirectories
};

// Return the Nth 512-byte sector of a directory, or false if past its end.
// For the root that's a fixed run of sectors; for a subdirectory it walks the
// cluster chain. (fat_next reloads g_sector, so callers must read the returned
// sector afterwards -- which they do.)
bool dir_nth_sector(const DirLoc &d, uint32_t n, uint32_t *out) {
    if (d.is_root) {
        if (n >= g_fs.root_sectors) return false;
        *out = g_fs.root_start + n;
        return true;
    }
    const uint32_t spc = g_fs.sectors_per_cluster;
    uint16_t c = d.cluster;
    for (uint32_t i = 0; i < n / spc; ++i) {
        c = fat_next(c);
        if (c < 2 || c >= 0xFFF8) return false;
    }
    if (c < 2 || c >= 0xFFF8) return false;
    *out = cluster_first_sector(c) + (n % spc);
    return true;
}

// Find entry `name` in directory `d`; report the sector + byte offset.
bool dir_find(const DirLoc &d, const char *name, uint32_t *out_sector, uint32_t *out_off) {
    for (uint32_t n = 0;; ++n) {
        uint32_t sec = 0;
        if (!dir_nth_sector(d, n, &sec)) return false;
        if (!read_sector(sec)) return false;
        for (uint32_t e = 0; e < kSector / 32; ++e) {
            const uint8_t *ent = &g_sector[e * 32];
            if (ent[0] == 0x00) return false; // end of directory
            if (ent[0] == 0xE5) continue;
            const uint8_t attr = ent[11];
            if (attr == 0x0F || (attr & 0x08)) continue; // LFN / volume label
            char disp[13];
            format_83(ent, disp);
            if (ci_equal(disp, name)) {
                *out_sector = sec;
                *out_off = e * 32;
                return true;
            }
        }
    }
}

// Find a free (0x00 or 0xE5) slot in directory `d`. For a subdirectory that is
// full, extend it by one cluster and use that cluster's first slot. The root
// directory has a fixed size and cannot grow.
bool dir_free_slot(const DirLoc &d, uint32_t *out_sector, uint32_t *out_off) {
    uint16_t last_cluster = d.cluster;
    for (uint32_t n = 0;; ++n) {
        uint32_t sec = 0;
        if (!dir_nth_sector(d, n, &sec)) break;
        if (!read_sector(sec)) return false;
        for (uint32_t e = 0; e < kSector / 32; ++e) {
            const uint8_t *ent = &g_sector[e * 32];
            if (ent[0] == 0x00 || ent[0] == 0xE5) {
                *out_sector = sec;
                *out_off = e * 32;
                return true;
            }
        }
        if (!d.is_root) {
            // remember the last cluster so we can extend after the scan
            const uint32_t spc = g_fs.sectors_per_cluster;
            if ((n + 1) % spc == 0) last_cluster = fat_next(last_cluster);
        }
    }
    if (d.is_root) {
        return false; // root full, cannot grow
    }
    // Extend the subdirectory: link a fresh cluster onto the chain.
    uint16_t tail = d.cluster;
    uint32_t guard = 0;
    while (fat_next(tail) < 0xFFF8 && fat_next(tail) >= 2 && guard++ < 100000) {
        tail = fat_next(tail);
    }
    const uint16_t nc = alloc_cluster();
    if (nc == 0) return false;
    fat_set(tail, nc);
    *out_sector = cluster_first_sector(nc);
    *out_off = 0;
    return true;
}

// Resolve a path to its parent directory + leaf name. "a/b/c.txt" yields the
// directory for "a/b" and leaf "c.txt"; "file.txt" yields root + "file.txt".
// Leading slashes are ignored (no real cwd; all paths are from root). Returns
// false if an intermediate component is missing or not a directory.
bool resolve_parent(const char *path, DirLoc *parent, char *leaf, uint32_t leafcap) {
    DirLoc cur;
    const char *p = path;
    while (*p == '/') ++p;
    for (;;) {
        char comp[16];
        uint32_t ci = 0;
        while (*p && *p != '/' && ci + 1 < sizeof(comp)) comp[ci++] = *p++;
        comp[ci] = 0;
        while (*p == '/') ++p;
        if (*p == 0) {
            uint32_t i = 0;
            for (; comp[i] && i + 1 < leafcap; ++i) leaf[i] = comp[i];
            leaf[i] = 0;
            *parent = cur;
            return leaf[0] != 0;
        }
        // Intermediate component: must be an existing subdirectory.
        uint32_t dsec = 0, doff = 0;
        if (!dir_find(cur, comp, &dsec, &doff)) return false;
        if (!read_sector(dsec)) return false;
        const uint8_t *ent = &g_sector[doff];
        if (!(ent[11] & 0x10)) return false; // not a directory
        cur.is_root = false;
        cur.cluster = rd16(&ent[26]);
    }
}

} // namespace

namespace {

// Whole-layer serialization. ext2 (g_blk/g_ind/g_ind2), the FAT path
// (g_sector), AND the virtio-blk request ring inside underline_read are all
// SHARED file-scope state, so two EL0 FS syscalls on different cores would
// shred each other's block scratch / device ring mid-operation -- the SMP
// "No such file" / corrupted-read race that made init's sshd respawn and
// per-connection sshd-session exec fail intermittently. One spinlock around
// every public Lateran entry makes each whole operation atomic w.r.t. the
// others. Re-entrant on the same CPU (tracked via g_lateran_owner) so
// lateran_stat() can call lateran_is_dir()/_read_file() without self-deadlock.
//
// Deliberately NOT irqsave: the lock can be held for a while (underline_read
// polls up to ~2e8 spins per block), and masking IRQs that long starves the
// 100 Hz network_tick that drains TCP RX -- which stalled SSH KEX on -smp >1.
// Leaving IRQs enabled is safe here: esper_preempt never preempts EL1 (the FS
// syscall holding this lock can't be switched out mid-section), and no IRQ
// handler touches the FS, so there is no same-core re-entrant or self-deadlock
// path. underline_read polls memory (DMA-updated used ring), so it completes
// fine with IRQs enabled.
AntiSkill g_lateran_lock;
volatile int32_t g_lateran_owner = -1; // CPU currently inside Lateran, -1 = none

struct FsGuard {
    bool owns;
    FsGuard() {
        const int32_t cpu = static_cast<int32_t>(arch::this_cpu_id());
        if (g_lateran_owner == cpu) { // already inside on this CPU -> re-entrant
            owns = false;
            return;
        }
        anti_skill_lock(g_lateran_lock);
        g_lateran_owner = cpu;
        owns = true;
    }
    ~FsGuard() {
        if (!owns) return;
        g_lateran_owner = -1;
        anti_skill_unlock(g_lateran_lock);
    }
    FsGuard(const FsGuard &) = delete;
    FsGuard &operator=(const FsGuard &) = delete;
};

} // namespace

bool lateran_mount() {
    g_backend = Backend::none;
    g_fs.mounted = false;
    if (!drivers::underline_status().present) {
        return false;
    }
    // Prefer ext2 (real Unix filesystem); fall back to FAT16.
    if (ext2_mount()) {
        g_backend = Backend::ext2;
        return true;
    }
    if (!read_sector(0)) {
        return false;
    }
    if (g_sector[510] != 0x55 || g_sector[511] != 0xAA) {
        return false; // no boot signature
    }

    Layout fs;
    fs.bytes_per_sector = rd16(&g_sector[11]);
    fs.sectors_per_cluster = g_sector[13];
    fs.reserved = rd16(&g_sector[14]);
    fs.num_fats = g_sector[16];
    fs.root_entries = rd16(&g_sector[17]);
    fs.fat_size = rd16(&g_sector[22]);

    if (fs.bytes_per_sector != kSector || fs.sectors_per_cluster == 0 ||
        fs.num_fats == 0 || fs.fat_size == 0 || fs.root_entries == 0) {
        return false;
    }

    fs.fat_start = fs.reserved;
    fs.root_start = fs.reserved + static_cast<uint32_t>(fs.num_fats) * fs.fat_size;
    fs.root_sectors = (static_cast<uint32_t>(fs.root_entries) * 32 + kSector - 1) / kSector;
    fs.data_start = fs.root_start + fs.root_sectors;
    fs.mounted = true;

    g_fs = fs;
    g_backend = Backend::fat;
    return true;
}

bool lateran_mounted() {
    return g_backend != Backend::none;
}

const char *lateran_format() {
    switch (g_backend) {
    case Backend::ext2: return "ext2";
    case Backend::fat: return "FAT16";
    default: return "none";
    }
}

uint32_t lateran_list(LateranEntry *out, uint32_t max) {
    FsGuard _g;
    if (g_backend == Backend::ext2) {
        return ext2_list_dir("/", out, max);
    }
    if (!g_fs.mounted) {
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t s = 0; s < g_fs.root_sectors && count < max; ++s) {
        if (!read_sector(g_fs.root_start + s)) {
            break;
        }
        for (uint32_t e = 0; e < kSector / 32 && count < max; ++e) {
            const uint8_t *ent = &g_sector[e * 32];
            if (ent[0] == 0x00) {
                return count; // end of directory
            }
            if (ent[0] == 0xE5) {
                continue; // deleted
            }
            const uint8_t attr = ent[11];
            if (attr == 0x0F || (attr & 0x08) || (attr & 0x10)) {
                continue; // long-name, volume label, or subdirectory
            }
            format_83(ent, out[count].name);
            out[count].first_cluster = rd16(&ent[26]);
            out[count].size = rd32(&ent[28]);
            out[count].is_dir = (attr & 0x10) != 0;
            out[count].uid = 0;
            out[count].gid = 0;
            // Synthesise a Unix mode: dir 0755 / file 0644, read-only attr (0x01)
            // strips the write bits. S_IFDIR=040000, S_IFREG=0100000.
            {
                uint32_t base = out[count].is_dir ? (0040000u | 0755u) : (0100000u | 0644u);
                if (attr & 0x01) base &= ~0222u; // FAT read-only -> no write bits
                out[count].mode = base;
            }
            ++count;
        }
    }
    return count;
}

int64_t lateran_read_file(const char *name, char *buf, uint32_t cap) {
    FsGuard _g;
    if (tmpfs_owns_path(name)) return tmpfs_read_file(name, buf, cap);
    if (const char *sub = strip_host_prefix(name)) {
        return drivers::stiyl_read_file(sub, buf, cap);
    }
    if (procfs_owns_path(name)) {
        return procfs_read_file(name, buf, cap);
    }
    if (g_backend == Backend::ext2) {
        return ext2_read_file(name, buf, cap);
    }
    if (!g_fs.mounted) {
        return -1;
    }

    // Resolve the path to (parent directory, leaf) and locate the file's entry.
    DirLoc parent;
    char leaf[16];
    if (!resolve_parent(name, &parent, leaf, sizeof(leaf))) {
        return -1;
    }
    uint32_t dsec = 0, doff = 0;
    if (!dir_find(parent, leaf, &dsec, &doff)) {
        return -1;
    }
    if (!read_sector(dsec)) return -1;
    const uint8_t *fe = &g_sector[doff];
    if (fe[11] & 0x10) return -1; // it's a directory, not a file
    uint16_t cluster = rd16(&fe[26]);
    uint32_t size = rd32(&fe[28]);
    const bool found = true;
    if (!found) {
        return -1;
    }

    uint32_t remaining = size < cap ? size : cap;
    uint32_t written = 0;
    uint32_t guard = 0;
    while (cluster >= 2 && cluster < 0xFFF8 && remaining > 0 && guard < 100000) {
        ++guard;
        for (uint32_t s = 0; s < g_fs.sectors_per_cluster && remaining > 0; ++s) {
            const uint32_t sector =
                g_fs.data_start + (static_cast<uint32_t>(cluster) - 2) * g_fs.sectors_per_cluster + s;
            if (!read_sector(sector)) {
                return static_cast<int64_t>(written);
            }
            uint32_t n = remaining < kSector ? remaining : kSector;
            for (uint32_t i = 0; i < n; ++i) {
                buf[written + i] = static_cast<char>(g_sector[i]);
            }
            written += n;
            remaining -= n;
        }
        cluster = fat_next(cluster);
    }
    return static_cast<int64_t>(written);
}

// Demand-paging read primitive (file-backed mmap fault path). Routes like
// lateran_read_file but seeks to `offset`. ext2 + tmpfs support it; host-9p /
// FAT return -1 (those mmaps still slurp the whole file via lateran_read_file).
int64_t lateran_pread(const char *name, uint64_t offset, char *buf, uint32_t len) {
    FsGuard _g;
    if (tmpfs_owns_path(name)) return tmpfs_pread(name, offset, buf, len);
    if (procfs_owns_path(name)) return -1; // synthesized, not mmap-backed
    if (g_backend == Backend::ext2) return ext2_pread(name, offset, buf, len);
    return -1;
}

int64_t lateran_pwrite(const char *name, uint64_t off, const char *buf, uint32_t len) {
    FsGuard _g;
    // Incremental (offset-based) write so the caller never falls back to the
    // whole-file read-modify-write, which is capped at kElfReadCap (4 MiB) and
    // would clamp larger writes. tmpfs routes to its own offset pwrite (Testament
    // grows the node on demand) -- needed because apt's pkgcache.bin /
    // srcpkgcache.bin (~5-6 MiB for a real suite) live on the tmpfs idxapt mounts
    // over /var/cache/apt; without this apt died "write, still have N to write but
    // couldn't" / "IO Error saving source cache". 9p and FAT have no incremental
    // path -> -2 (whole-file fallback; those writes are small / in-memory).
    if (tmpfs_owns_path(name)) {
        return tmpfs_append_file(name, buf, len, off);
    }
    if (g_backend == Backend::ext2 && strip_host_prefix(name) == nullptr) {
        return ext2_pwrite(name, off, buf, len);
    }
    return -2;
}

int64_t lateran_write_file(const char *name, const char *buf, uint32_t len) {
    FsGuard _g;
    if (tmpfs_owns_path(name)) return tmpfs_write_file(name, buf, len);
    if (const char *sub = strip_host_prefix(name)) {
        return drivers::stiyl_write_file(sub, buf, len);
    }
    if (g_backend == Backend::ext2) {
        return ext2_write_file(name, buf, len);
    }
    if (!g_fs.mounted) {
        return -1;
    }
    DirLoc parent;
    char leaf[16];
    if (!resolve_parent(name, &parent, leaf, sizeof(leaf))) {
        return -1; // a path component is missing / not a directory
    }
    // Find the existing entry (free its old chain) or claim a fresh slot.
    uint32_t dsec = 0, doff = 0;
    if (dir_find(parent, leaf, &dsec, &doff)) {
        if (!read_sector(dsec)) return -1;
        free_chain(rd16(&g_sector[doff + 26]));
    } else if (!dir_free_slot(parent, &dsec, &doff)) {
        return -1; // directory full
    }

    // Allocate a cluster chain and write the data into it.
    uint16_t first = 0, prev = 0;
    uint32_t written = 0;
    while (written < len) {
        const uint16_t c = alloc_cluster();
        if (c == 0) {
            if (first != 0) free_chain(first); // disk full: roll back
            return -1;
        }
        if (first == 0) {
            first = c;
        } else {
            fat_set(prev, c); // link previous cluster to this one
        }
        prev = c;
        for (uint8_t s = 0; s < g_fs.sectors_per_cluster && written < len; ++s) {
            uint32_t n = len - written;
            if (n > kSector) n = kSector;
            for (uint32_t i = 0; i < kSector; ++i) {
                g_sector[i] = i < n ? static_cast<uint8_t>(buf[written + i]) : 0;
            }
            if (!write_sector(cluster_first_sector(c) + s)) return -1;
            written += n;
        }
    }

    // Write the directory entry (8.3 leaf name, archive attr, first cluster, size).
    if (!read_sector(dsec)) return -1;
    uint8_t *ent = &g_sector[doff];
    to_83(leaf, ent);
    ent[11] = 0x20; // ATTR_ARCHIVE
    for (uint32_t i = 12; i < 26; ++i) ent[i] = 0; // clear reserved + time/date
    wr16(&ent[26], first);
    wr32(&ent[28], len);
    if (!write_sector(dsec)) return -1;
    return static_cast<int64_t>(len);
}

bool lateran_unlink(const char *name) {
    FsGuard _g;
    // unlinkat ignores AT_REMOVEDIR, so this removes both files and empty dirs.
    if (tmpfs_owns_path(name)) return tmpfs_is_dir(name) ? tmpfs_rmdir(name) : tmpfs_unlink(name);
    if (const char *sub = strip_host_prefix(name)) {
        return drivers::stiyl_unlink(sub);
    }
    if (g_backend == Backend::ext2) {
        return ext2_unlink(name);
    }
    if (!g_fs.mounted) {
        return false;
    }
    DirLoc parent;
    char leaf[16];
    if (!resolve_parent(name, &parent, leaf, sizeof(leaf))) {
        return false;
    }
    uint32_t dsec = 0, doff = 0;
    if (!dir_find(parent, leaf, &dsec, &doff)) {
        return false;
    }
    if (!read_sector(dsec)) return false;
    const uint16_t first = rd16(&g_sector[doff + 26]);
    g_sector[doff] = 0xE5; // mark entry deleted
    if (!write_sector(dsec)) return false;
    free_chain(first);
    return true;
}

bool lateran_mkdir(const char *path) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_mkdir(path);
    if (g_backend == Backend::ext2) {
        return ext2_mkdir(path);
    }
    if (!g_fs.mounted) {
        return false;
    }
    DirLoc parent;
    char leaf[16];
    if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) {
        return false;
    }
    uint32_t dsec = 0, doff = 0;
    if (dir_find(parent, leaf, &dsec, &doff)) {
        return false; // already exists
    }
    // Allocate the new directory's first cluster and lay down "." and "..".
    const uint16_t nc = alloc_cluster();
    if (nc == 0) return false;
    for (uint32_t i = 0; i < kSector; ++i) g_sector[i] = 0;
    // "." -> this directory; ".." -> parent (cluster 0 if parent is root).
    uint8_t *dot = &g_sector[0];
    for (uint32_t i = 0; i < 11; ++i) dot[i] = ' ';
    dot[0] = '.';
    dot[11] = 0x10; // ATTR_DIRECTORY
    wr16(&dot[26], nc);
    uint8_t *dotdot = &g_sector[32];
    for (uint32_t i = 0; i < 11; ++i) dotdot[i] = ' ';
    dotdot[0] = '.';
    dotdot[1] = '.';
    dotdot[11] = 0x10;
    wr16(&dotdot[26], parent.is_root ? 0 : parent.cluster);
    if (!write_sector(cluster_first_sector(nc))) return false;

    // Create the directory entry in the parent.
    if (!dir_free_slot(parent, &dsec, &doff)) {
        free_chain(nc);
        return false;
    }
    if (!read_sector(dsec)) return false;
    uint8_t *ent = &g_sector[doff];
    to_83(leaf, ent);
    ent[11] = 0x10; // ATTR_DIRECTORY
    for (uint32_t i = 12; i < 26; ++i) ent[i] = 0;
    wr16(&ent[26], nc);
    wr32(&ent[28], 0); // directories report size 0
    if (!write_sector(dsec)) return false;
    return true;
}

// lstat: ext2 reports the symlink's own inode; other backends (tmpfs/9p/proc)
// fall back to follow-stat (their symlink use is incidental).
bool lateran_stat_nofollow(const char *path, LateranEntry *out) {
    {
        FsGuard _g;
        if (!tmpfs_owns_path(path) && strip_host_prefix(path) == nullptr &&
            !procfs_owns_path(path) && g_backend == Backend::ext2) {
            return ext2_stat_nofollow(path, out);
        }
    }
    return lateran_stat(path, out);
}

bool lateran_stat(const char *path, LateranEntry *out) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_stat(path, out);
    if (const char *sub = strip_host_prefix(path)) {
        return drivers::stiyl_stat(sub, out);
    }
    if (procfs_owns_path(path)) {
        // Synthetic /proc entries. Required so openat/fstat / fopen don't
        // ENOENT on /proc/meminfo etc. busybox `free` / `uptime` / `ps`
        // all open and read these via fopen, which fstats first.
        if (out == nullptr) return false;
        *out = LateranEntry{};
        out->is_dir = procfs_is_dir(path);
        const int64_t sz = out->is_dir ? 0 : procfs_file_size(path);
        if (!out->is_dir && sz < 0) return false; // missing /proc leaf
        out->size = static_cast<uint32_t>(sz > 0 ? sz : 0);
        out->mode = out->is_dir ? (0x4000u | 0555u) : (0x8000u | 0444u);
        out->nlink = 1;
        return true;
    }
    if (g_backend == Backend::ext2) return ext2_stat(path, out);
    // FAT fallback: synthesise mode/size from the existing helpers; times are 0.
    if (out == nullptr) return false;
    if (lateran_is_dir(path)) {
        out->is_dir = true;
        out->size = 0;
        out->mode = 0x4000u | 0755u;
    } else {
        const int64_t sz = lateran_read_file(path, nullptr, 0); // best effort
        if (sz < 0) return false;
        out->is_dir = false;
        out->size = static_cast<uint32_t>(sz);
        out->mode = 0x8000u | 0644u;
    }
    out->uid = 0; out->gid = 0;
    out->atime = out->mtime = out->ctime = 0;
    out->first_cluster = 0;
    out->name[0] = 0;
    return true;
}

int32_t lateran_readlink(const char *path, char *out, uint32_t cap) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_readlink(path, out, cap);
    if (g_backend == Backend::ext2) return ext2_readlink(path, out, cap);
    return -1; // FAT has no symlinks
}

bool lateran_symlink(const char *target, const char *path) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_symlink(target, path);
    if (g_backend == Backend::ext2) return ext2_symlink(target, path);
    return false;
}

bool lateran_rename(const char *old_path, const char *new_path) {
    FsGuard _g;
    if (tmpfs_owns_path(old_path) || tmpfs_owns_path(new_path))
        return tmpfs_rename(old_path, new_path); // cross-fs rename unsupported -> false
    if (g_backend == Backend::ext2) return ext2_rename(old_path, new_path);
    return false;
}

bool lateran_truncate(const char *path, uint64_t new_size) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_truncate(path, new_size);
    if (const char *sub = strip_host_prefix(path)) {
        return drivers::stiyl_truncate(sub, new_size);
    }
    if (g_backend == Backend::ext2) return ext2_truncate(path, new_size);
    return false;
}

bool lateran_chmod(const char *path, uint32_t new_mode) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_chmod(path, new_mode);
    if (g_backend == Backend::ext2) return ext2_chmod(path, new_mode);
    return false;
}

bool lateran_chown(const char *path, uint32_t uid, uint32_t gid) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_chown(path, uid, gid);
    if (g_backend == Backend::ext2) return ext2_chown(path, uid, gid);
    return false;
}

bool lateran_link(const char *target, const char *link_path) {
    FsGuard _g;
    if (tmpfs_owns_path(link_path) || tmpfs_owns_path(target)) return false; // tmpfs: no hard links
    if (g_backend == Backend::ext2) return ext2_link(target, link_path);
    return false;
}

bool lateran_utime(const char *path, int64_t atime, int64_t mtime) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_utime(path, atime, mtime);
    if (g_backend == Backend::ext2) return ext2_utime(path, atime, mtime);
    return false;
}

bool lateran_tmpfs_mount(const char *point) {
    FsGuard _g;
    return testament_mount(point);
}

bool lateran_tmpfs_umount(const char *point) {
    FsGuard _g;
    return testament_umount(point);
}

bool lateran_is_dir(const char *path) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_is_dir(path);
    if (const char *sub = strip_host_prefix(path)) {
        return drivers::stiyl_is_dir(sub);
    }
    if (procfs_owns_path(path)) {
        return procfs_is_dir(path);
    }
    if (g_backend == Backend::ext2) {
        return ext2_is_dir(path);
    }
    if (!g_fs.mounted) return false;
    if (path == nullptr) return true;
    const char *p = path;
    while (*p == '/') ++p;
    if (*p == 0) return true; // root
    DirLoc parent;
    char leaf[16];
    if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) return false;
    uint32_t dsec = 0, doff = 0;
    if (!dir_find(parent, leaf, &dsec, &doff)) return false;
    if (!read_sector(dsec)) return false;
    return (g_sector[doff + 11] & 0x10) != 0;
}

uint32_t lateran_list_dir(const char *path, LateranEntry *out, uint32_t max) {
    FsGuard _g;
    if (tmpfs_owns_path(path)) return tmpfs_list_dir(path, out, max);
    if (const char *sub = strip_host_prefix(path)) {
        return drivers::stiyl_list_dir(sub, out, max);
    }
    if (procfs_owns_path(path)) {
        return procfs_list_dir(path, out, max);
    }
    if (g_backend == Backend::ext2) {
        return ext2_list_dir(path, out, max);
    }
    if (!g_fs.mounted) {
        return 0;
    }
    // An empty path / "/" lists the root; otherwise resolve `path` as a dir.
    DirLoc d; // defaults to root
    if (path != nullptr && path[0] != 0) {
        const char *p = path;
        while (*p == '/') ++p;
        if (*p != 0) {
            DirLoc parent;
            char leaf[16];
            if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) return 0;
            uint32_t dsec = 0, doff = 0;
            if (!dir_find(parent, leaf, &dsec, &doff)) return 0;
            if (!read_sector(dsec)) return 0;
            if (!(g_sector[doff + 11] & 0x10)) return 0; // not a directory
            d.is_root = false;
            d.cluster = rd16(&g_sector[doff + 26]);
        }
    }
    uint32_t count = 0;
    for (uint32_t n = 0; count < max; ++n) {
        uint32_t sec = 0;
        if (!dir_nth_sector(d, n, &sec)) break;
        if (!read_sector(sec)) break;
        for (uint32_t e = 0; e < kSector / 32 && count < max; ++e) {
            const uint8_t *ent = &g_sector[e * 32];
            if (ent[0] == 0x00) return count; // end of directory
            if (ent[0] == 0xE5) continue;
            const uint8_t attr = ent[11];
            if (attr == 0x0F || (attr & 0x08)) continue; // LFN / volume label
            format_83(ent, out[count].name);
            out[count].first_cluster = rd16(&ent[26]);
            out[count].size = rd32(&ent[28]);
            out[count].is_dir = (attr & 0x10) != 0;
            out[count].uid = 0;
            out[count].gid = 0;
            uint32_t base = out[count].is_dir ? (0040000u | 0755u) : (0100000u | 0644u);
            if (attr & 0x01) base &= ~0222u;
            out[count].mode = base;
            ++count;
        }
    }
    return count;
}

} // namespace index
