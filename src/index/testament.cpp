#include "index/testament.hpp"

#include "index/dark_matter.hpp"
#include "index/idol_theory.hpp"
#include "index/lateran.hpp" // LateranEntry

namespace index {

namespace {

constexpr uint32_t kMaxNodes  = 1024;   // total tmpfs inodes across all mounts
constexpr uint32_t kMaxMounts = 8;
constexpr uint32_t kNameCap   = 64;     // matches LateranEntry::name
constexpr uint32_t kPointCap  = 128;

constexpr uint32_t kModeDir  = 0x4000u; // S_IFDIR
constexpr uint32_t kModeReg  = 0x8000u; // S_IFREG
constexpr uint32_t kModeLnk  = 0xA000u; // S_IFLNK

struct TmNode {
    bool      used = false;
    bool      is_dir = false;
    bool      is_symlink = false;
    char      name[kNameCap] = {};
    int32_t   parent = -1;     // index of parent dir node; mount roots have -1
    uint8_t  *data = nullptr;  // file bytes, or NUL-terminated symlink target
    uint32_t  size = 0;        // logical length
    uint32_t  cap = 0;         // allocated bytes in `data`
    uint32_t  perm = 0;        // permission bits (low 12)
    uint32_t  uid = 0, gid = 0;
    uint32_t  mtime = 0;
    uint32_t  ino = 0;
};

struct TmMount {
    bool    active = false;
    char    point[kPointCap] = {};
    int32_t root = -1;         // root dir node index
};

TmNode  g_nodes[kMaxNodes];
TmMount g_mounts[kMaxMounts];
uint32_t g_ino_next = 1;

// --- tiny string helpers --------------------------------------------------
uint32_t s_len(const char *s) { uint32_t n = 0; while (s && s[n]) ++n; return n; }

bool s_eq_n(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}

void s_copy(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    for (; src[i] && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = 0;
}

uint32_t now_secs() { return static_cast<uint32_t>(idol_theory_epoch_seconds()); }

// `point` is a mount prefix of `path` iff path == point, or path starts with
// "point/" (so /tmp covers /tmp and /tmp/x but not /tmpx).
bool point_covers(const char *point, const char *path) {
    const uint32_t pl = s_len(point);
    if (pl == 0) return false;
    if (!s_eq_n(point, path, pl)) return false;
    return path[pl] == 0 || path[pl] == '/';
}

int find_mount(const char *path) {
    int best = -1;
    uint32_t best_len = 0;
    for (uint32_t i = 0; i < kMaxMounts; ++i) {
        if (!g_mounts[i].active) continue;
        if (point_covers(g_mounts[i].point, path)) {
            const uint32_t pl = s_len(g_mounts[i].point);
            if (pl >= best_len) { best = static_cast<int>(i); best_len = pl; }
        }
    }
    return best;
}

int alloc_node() {
    for (uint32_t i = 0; i < kMaxNodes; ++i) {
        if (!g_nodes[i].used) {
            g_nodes[i] = TmNode{};
            g_nodes[i].used = true;
            g_nodes[i].ino = g_ino_next++;
            g_nodes[i].mtime = now_secs();
            return static_cast<int>(i);
        }
    }
    return -1; // table full
}

void free_node(int idx) {
    if (idx < 0) return;
    if (g_nodes[idx].data) { dark_matter_free(g_nodes[idx].data); }
    g_nodes[idx] = TmNode{};
}

int lookup_child(int dir, const char *name, uint32_t name_len) {
    for (uint32_t i = 0; i < kMaxNodes; ++i) {
        if (!g_nodes[i].used || g_nodes[i].parent != dir) continue;
        if (s_len(g_nodes[i].name) == name_len && s_eq_n(g_nodes[i].name, name, name_len)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Walk `sub` (the path tail after the mount point, e.g. "/a/b" or "") from the
// mount root, returning the resolved node index or -1. resolve_path already
// normalized away "." / ".." and redundant slashes.
int walk(int root, const char *sub) {
    int cur = root;
    const char *p = sub;
    while (*p == '/') ++p;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') ++p;
        const uint32_t len = static_cast<uint32_t>(p - start);
        if (len > 0) {
            if (!g_nodes[cur].is_dir) return -1;
            cur = lookup_child(cur, start, len);
            if (cur < 0) return -1;
        }
        while (*p == '/') ++p;
    }
    return cur;
}

int resolve(const char *path) {
    const int m = find_mount(path);
    if (m < 0) return -1;
    return walk(g_mounts[m].root, path + s_len(g_mounts[m].point));
}

// Resolve the parent directory of `path` and the leaf component. Returns the
// parent dir node index (or -1), and writes the leaf name into `leaf`.
int resolve_parent(const char *path, char *leaf, uint32_t leaf_cap) {
    const int m = find_mount(path);
    if (m < 0) return -1;
    const char *sub = path + s_len(g_mounts[m].point);
    // Find the last component.
    const char *end = sub + s_len(sub);
    while (end > sub && *(end - 1) == '/') --end; // trim trailing '/'
    const char *leaf_start = end;
    while (leaf_start > sub && *(leaf_start - 1) != '/') --leaf_start;
    const uint32_t ll = static_cast<uint32_t>(end - leaf_start);
    if (ll == 0 || ll + 1 > leaf_cap) return -1;
    for (uint32_t i = 0; i < ll; ++i) leaf[i] = leaf_start[i];
    leaf[ll] = 0;
    // Walk to the parent: everything before leaf_start.
    int cur = g_mounts[m].root;
    const char *p = sub;
    while (p < leaf_start) {
        while (p < leaf_start && *p == '/') ++p;
        const char *start = p;
        while (p < leaf_start && *p != '/') ++p;
        const uint32_t len = static_cast<uint32_t>(p - start);
        if (len > 0) {
            if (!g_nodes[cur].is_dir) return -1;
            cur = lookup_child(cur, start, len);
            if (cur < 0) return -1;
        }
    }
    return cur;
}

// Grow node `n`'s data buffer to at least `need` bytes (preserving content).
bool ensure_cap(TmNode &n, uint32_t need) {
    if (need <= n.cap) return true;
    uint32_t nc = n.cap ? n.cap : 64;
    // Overflow-safe doubling: once nc passes 2 GiB the next `*= 2` would wrap to 0
    // and the loop would spin forever (a hang/DoS reachable via a >2 GiB write /
    // pwrite offset / ftruncate). Past that point use `need` exactly -- the alloc
    // then just fails cleanly if the heap can't satisfy it.
    while (nc < need) {
        if (nc > (0xFFFFFFFFu / 2)) { nc = need; break; }
        nc *= 2;
    }
    uint8_t *nb = static_cast<uint8_t *>(dark_matter_alloc(nc));
    if (nb == nullptr) return false;
    for (uint32_t i = 0; i < n.size; ++i) nb[i] = n.data[i];
    for (uint32_t i = n.size; i < nc; ++i) nb[i] = 0;
    if (n.data) dark_matter_free(n.data);
    n.data = nb;
    n.cap = nc;
    return true;
}

void fill_entry(const TmNode &n, LateranEntry *out) {
    s_copy(out->name, n.name, sizeof(out->name));
    out->size = n.size;
    out->is_dir = n.is_dir;
    out->first_cluster = 0;
    out->uid = n.uid;
    out->gid = n.gid;
    const uint32_t type = n.is_dir ? kModeDir : (n.is_symlink ? kModeLnk : kModeReg);
    out->mode = type | (n.perm & 0xFFFu);
    out->atime = out->mtime = out->ctime = n.mtime;
    out->nlink = n.is_dir ? 2 : 1;
    out->ino = n.ino;
}

} // namespace

// --- mount management ------------------------------------------------------

bool testament_mount(const char *point) {
    if (point == nullptr || point[0] != '/') return false;
    // Already mounted there? (idempotent re-mount keeps existing tree.)
    for (uint32_t i = 0; i < kMaxMounts; ++i) {
        if (g_mounts[i].active && point_covers(g_mounts[i].point, point) &&
            s_len(g_mounts[i].point) == s_len(point)) {
            return true;
        }
    }
    int slot = -1;
    for (uint32_t i = 0; i < kMaxMounts; ++i) {
        if (!g_mounts[i].active) { slot = static_cast<int>(i); break; }
    }
    if (slot < 0) return false;
    const int root = alloc_node();
    if (root < 0) return false;
    g_nodes[root].is_dir = true;
    g_nodes[root].parent = -1;
    g_nodes[root].perm = 0777;
    g_mounts[slot].active = true;
    g_mounts[slot].root = root;
    s_copy(g_mounts[slot].point, point, sizeof(g_mounts[slot].point));
    return true;
}

bool testament_umount(const char *point) {
    if (point == nullptr) return false;
    for (uint32_t i = 0; i < kMaxMounts; ++i) {
        if (g_mounts[i].active && s_len(g_mounts[i].point) == s_len(point) &&
            point_covers(g_mounts[i].point, point)) {
            // Free every node belonging to this mount (root + descendants).
            const int root = g_mounts[i].root;
            for (uint32_t k = 0; k < kMaxNodes; ++k) {
                if (g_nodes[k].used) {
                    // Belongs to this tree if any ancestor chain reaches root.
                    int a = static_cast<int>(k);
                    while (a >= 0 && a != root) a = g_nodes[a].parent;
                    if (a == root) free_node(static_cast<int>(k));
                }
            }
            g_mounts[i] = TmMount{};
            return true;
        }
    }
    return false;
}

bool tmpfs_owns_path(const char *path) {
    return path != nullptr && find_mount(path) >= 0;
}

// --- VFS ops ---------------------------------------------------------------

bool tmpfs_stat(const char *path, LateranEntry *out) {
    const int n = resolve(path);
    if (n < 0) return false;
    fill_entry(g_nodes[n], out);
    return true;
}

bool tmpfs_is_dir(const char *path) {
    const int n = resolve(path);
    return n >= 0 && g_nodes[n].is_dir;
}

int64_t tmpfs_read_file(const char *path, char *buf, uint32_t cap) {
    const int n = resolve(path);
    if (n < 0 || g_nodes[n].is_dir) return -1;
    const uint32_t take = g_nodes[n].size < cap ? g_nodes[n].size : cap;
    for (uint32_t i = 0; i < take; ++i) buf[i] = static_cast<char>(g_nodes[n].data[i]);
    return static_cast<int64_t>(take);
}

int64_t tmpfs_pread(const char *path, uint64_t offset, char *buf, uint32_t len) {
    const int n = resolve(path);
    if (n < 0 || g_nodes[n].is_dir) return -1;
    if (offset >= g_nodes[n].size) return 0;
    const uint64_t avail = g_nodes[n].size - offset;
    const uint32_t take = (len < avail) ? len : static_cast<uint32_t>(avail);
    for (uint32_t i = 0; i < take; ++i) buf[i] = static_cast<char>(g_nodes[n].data[offset + i]);
    return static_cast<int64_t>(take);
}

int64_t tmpfs_write_file(const char *path, const char *buf, uint32_t len) {
    int n = resolve(path);
    if (n < 0) {
        char leaf[kNameCap];
        const int parent = resolve_parent(path, leaf, sizeof(leaf));
        if (parent < 0 || !g_nodes[parent].is_dir) return -1;
        if (lookup_child(parent, leaf, s_len(leaf)) >= 0) return -1; // shouldn't happen (resolve failed)
        n = alloc_node();
        if (n < 0) return -1;
        s_copy(g_nodes[n].name, leaf, sizeof(g_nodes[n].name));
        g_nodes[n].parent = parent;
        g_nodes[n].perm = 0644;
    }
    if (g_nodes[n].is_dir) return -1;
    // Overwrite from offset 0.
    if (!ensure_cap(g_nodes[n], len ? len : 1)) return -1;
    for (uint32_t i = 0; i < len; ++i) g_nodes[n].data[i] = static_cast<uint8_t>(buf[i]);
    g_nodes[n].size = len;
    g_nodes[n].mtime = now_secs();
    return static_cast<int64_t>(len);
}

int64_t tmpfs_append_file(const char *path, const char *buf, uint32_t len, uint64_t off) {
    const int n = resolve(path);
    if (n < 0 || g_nodes[n].is_dir) return -1;
    const uint64_t end = off + len;
    if (end > 0xFFFFFFFFu) return -1;
    if (!ensure_cap(g_nodes[n], static_cast<uint32_t>(end ? end : 1))) return -1;
    for (uint32_t i = 0; i < len; ++i)
        g_nodes[n].data[static_cast<uint32_t>(off) + i] = static_cast<uint8_t>(buf[i]);
    if (static_cast<uint32_t>(end) > g_nodes[n].size) g_nodes[n].size = static_cast<uint32_t>(end);
    g_nodes[n].mtime = now_secs();
    return static_cast<int64_t>(len);
}

bool tmpfs_create(const char *path) {
    if (resolve(path) >= 0) return true; // already exists
    char leaf[kNameCap];
    const int parent = resolve_parent(path, leaf, sizeof(leaf));
    if (parent < 0 || !g_nodes[parent].is_dir) return false;
    const int n = alloc_node();
    if (n < 0) return false;
    s_copy(g_nodes[n].name, leaf, sizeof(g_nodes[n].name));
    g_nodes[n].parent = parent;
    g_nodes[n].perm = 0644;
    return true;
}

bool tmpfs_unlink(const char *path) {
    const int n = resolve(path);
    if (n < 0 || g_nodes[n].is_dir) return false;
    free_node(n);
    return true;
}

bool tmpfs_mkdir(const char *path) {
    if (resolve(path) >= 0) return false; // exists
    char leaf[kNameCap];
    const int parent = resolve_parent(path, leaf, sizeof(leaf));
    if (parent < 0 || !g_nodes[parent].is_dir) return false;
    const int n = alloc_node();
    if (n < 0) return false;
    s_copy(g_nodes[n].name, leaf, sizeof(g_nodes[n].name));
    g_nodes[n].parent = parent;
    g_nodes[n].is_dir = true;
    g_nodes[n].perm = 0755;
    return true;
}

bool tmpfs_rmdir(const char *path) {
    const int n = resolve(path);
    if (n < 0 || !g_nodes[n].is_dir) return false;
    for (uint32_t i = 0; i < kMaxNodes; ++i)
        if (g_nodes[i].used && g_nodes[i].parent == n) return false; // not empty
    // Don't remove a mount root via rmdir.
    for (uint32_t i = 0; i < kMaxMounts; ++i)
        if (g_mounts[i].active && g_mounts[i].root == n) return false;
    free_node(n);
    return true;
}

bool tmpfs_truncate(const char *path, uint64_t new_size) {
    const int n = resolve(path);
    if (n < 0 || g_nodes[n].is_dir || g_nodes[n].is_symlink) return false;
    if (new_size > 0xFFFFFFFFu) return false;
    const uint32_t ns = static_cast<uint32_t>(new_size);
    if (ns > g_nodes[n].size) {
        if (!ensure_cap(g_nodes[n], ns)) return false; // ensure_cap zero-fills the tail
    }
    g_nodes[n].size = ns;
    g_nodes[n].mtime = now_secs();
    return true;
}

bool tmpfs_rename(const char *old_path, const char *new_path) {
    const int src = resolve(old_path);
    if (src < 0) return false;
    // Never move a mount root (it would orphan the mount's tree).
    for (uint32_t i = 0; i < kMaxMounts; ++i)
        if (g_mounts[i].active && g_mounts[i].root == src) return false;
    char leaf[kNameCap];
    const int parent = resolve_parent(new_path, leaf, sizeof(leaf));
    if (parent < 0 || !g_nodes[parent].is_dir) return false;
    // Reject moving a directory into itself or its own subtree: that makes the
    // node its own ancestor, and every later ancestor walk (umount free, ".."
    // resolution) would loop forever. Classic rename pitfall -- Linux returns
    // -EINVAL. Walk the destination parent's ancestors; if we reach src, refuse.
    if (g_nodes[src].is_dir) {
        for (int a = parent; a >= 0; a = g_nodes[a].parent)
            if (a == src) return false;
    }
    const int existing = lookup_child(parent, leaf, s_len(leaf));
    if (existing >= 0) {
        if (existing == src) return true;
        if (g_nodes[existing].is_dir) return false; // don't clobber a dir
        free_node(existing);
    }
    g_nodes[src].parent = parent;
    s_copy(g_nodes[src].name, leaf, sizeof(g_nodes[src].name));
    return true;
}

bool tmpfs_symlink(const char *target, const char *path) {
    if (target == nullptr || resolve(path) >= 0) return false;
    char leaf[kNameCap];
    const int parent = resolve_parent(path, leaf, sizeof(leaf));
    if (parent < 0 || !g_nodes[parent].is_dir) return false;
    const int n = alloc_node();
    if (n < 0) return false;
    s_copy(g_nodes[n].name, leaf, sizeof(g_nodes[n].name));
    g_nodes[n].parent = parent;
    g_nodes[n].is_symlink = true;
    g_nodes[n].perm = 0777;
    const uint32_t tl = s_len(target);
    if (!ensure_cap(g_nodes[n], tl + 1)) { free_node(n); return false; }
    for (uint32_t i = 0; i < tl; ++i) g_nodes[n].data[i] = static_cast<uint8_t>(target[i]);
    g_nodes[n].data[tl] = 0;
    g_nodes[n].size = tl;
    return true;
}

int32_t tmpfs_readlink(const char *path, char *out, uint32_t cap) {
    const int n = resolve(path);
    if (n < 0 || !g_nodes[n].is_symlink) return -1;
    const uint32_t tl = g_nodes[n].size;
    if (tl + 1 > cap) return -1;
    for (uint32_t i = 0; i < tl; ++i) out[i] = static_cast<char>(g_nodes[n].data[i]);
    out[tl] = 0;
    return static_cast<int32_t>(tl);
}

bool tmpfs_chmod(const char *path, uint32_t mode) {
    const int n = resolve(path);
    if (n < 0) return false;
    g_nodes[n].perm = mode & 0xFFFu;
    return true;
}

bool tmpfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    const int n = resolve(path);
    if (n < 0) return false;
    if (uid != ~0u) g_nodes[n].uid = uid;
    if (gid != ~0u) g_nodes[n].gid = gid;
    return true;
}

bool tmpfs_utime(const char *path, int64_t atime, int64_t mtime) {
    const int n = resolve(path);
    if (n < 0) return false;
    if (mtime >= 0) g_nodes[n].mtime = static_cast<uint32_t>(mtime);
    else if (atime >= 0) g_nodes[n].mtime = static_cast<uint32_t>(atime);
    return true;
}

uint32_t tmpfs_list_dir(const char *path, LateranEntry *out, uint32_t max) {
    const int dir = resolve(path);
    if (dir < 0 || !g_nodes[dir].is_dir) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < kMaxNodes && count < max; ++i) {
        if (g_nodes[i].used && g_nodes[i].parent == dir) {
            fill_entry(g_nodes[i], &out[count]);
            ++count;
        }
    }
    return count;
}

uint32_t tmpfs_proc_mounts(char *buf, uint32_t cap, uint32_t off) {
    for (uint32_t i = 0; i < kMaxMounts; ++i) {
        if (!g_mounts[i].active) continue;
        const char *pt = g_mounts[i].point;
        const char *a = "tmpfs ";
        const char *b = " tmpfs rw,nosuid,nodev 0 0\n";
        for (uint32_t k = 0; a[k] && off + 1 < cap; ++k) buf[off++] = a[k];
        for (uint32_t k = 0; pt[k] && off + 1 < cap; ++k) buf[off++] = pt[k];
        for (uint32_t k = 0; b[k] && off + 1 < cap; ++k) buf[off++] = b[k];
    }
    return off;
}

} // namespace index
