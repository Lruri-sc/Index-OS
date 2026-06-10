#include "drivers/stiyl_magnus.hpp"

#include "index/imaginary_number_district.hpp"
#include "index/mmio.hpp"
#include "index/teleport.hpp"

namespace index::drivers {

namespace {

using index::mmio::read32;
using index::mmio::write32;

// virtio-mmio bus (shared with Underline / MisakaMail). 32 transport slots,
// each 0x200 bytes, at MMIO base 0xa000000 on QEMU's `virt` machine.
constexpr uint64_t kMmioBase = 0x0a000000;
constexpr uint64_t kMmioStride = 0x200;
constexpr uint32_t kMmioSlots = 32;
constexpr uint32_t kMagic = 0x74726976; // "virt"
constexpr uint32_t kDeviceId9p = 9;

// virtio-mmio register offsets (legacy + modern transports).
constexpr uint64_t kMagicValue = 0x000;
constexpr uint64_t kVersion = 0x004;
constexpr uint64_t kDeviceId = 0x008;
constexpr uint64_t kDriverFeaturesSel = 0x024;
constexpr uint64_t kDriverFeatures = 0x020;
constexpr uint64_t kGuestPageSize = 0x028; // legacy only
constexpr uint64_t kQueueSel = 0x030;
constexpr uint64_t kQueueNumMax = 0x034;
constexpr uint64_t kQueueNum = 0x038;
constexpr uint64_t kQueueAlign = 0x03c; // legacy
constexpr uint64_t kQueuePfn = 0x040;   // legacy
constexpr uint64_t kQueueReady = 0x044; // modern
constexpr uint64_t kQueueNotify = 0x050;
constexpr uint64_t kStatus = 0x070;
constexpr uint64_t kQueueDescLow = 0x080;   // modern
constexpr uint64_t kQueueDescHigh = 0x084;
constexpr uint64_t kQueueDriverLow = 0x090;
constexpr uint64_t kQueueDriverHigh = 0x094;
constexpr uint64_t kQueueDeviceLow = 0x0a0;
constexpr uint64_t kQueueDeviceHigh = 0x0a4;
constexpr uint64_t kConfig = 0x100;

constexpr uint32_t kStatusAck = 1;
constexpr uint32_t kStatusDriver = 2;
constexpr uint32_t kStatusDriverOk = 4;
constexpr uint32_t kStatusFeaturesOk = 8;

constexpr uint16_t kDescNext = 1;
constexpr uint16_t kDescWrite = 2; // device writes (i.e. guest reads) this desc

constexpr uint32_t kQueueSize = 8;
constexpr uint32_t kQueueAlignBytes = 4096;

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[kQueueSize];
    uint16_t used_event;
};
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem ring[kQueueSize];
    uint16_t avail_event;
};

alignas(4096) uint8_t g_queue[2 * kQueueAlignBytes];
virtq_desc *const g_desc = reinterpret_cast<virtq_desc *>(g_queue);
virtq_avail *const g_avail =
    reinterpret_cast<virtq_avail *>(g_queue + sizeof(virtq_desc) * kQueueSize);
virtq_used *const g_used = reinterpret_cast<virtq_used *>(g_queue + kQueueAlignBytes);
uint16_t g_last_used = 0;

// 9P message buffers (request out, reply in). 8 KiB each holds a full Tread
// reply at the negotiated msize without spilling.
constexpr uint32_t kBufSize = 8192;
alignas(8) uint8_t g_tx[kBufSize];
alignas(8) uint8_t g_rx[kBufSize];

StiylMagnus g_dev;

uint64_t phys(const volatile void *p) {
    return reinterpret_cast<uint64_t>(p) & ~index::kHighHalfBase;
}

void zero(void *p, uint32_t n) {
    auto *b = static_cast<uint8_t *>(p);
    for (uint32_t i = 0; i < n; ++i) b[i] = 0;
}

bool setup_legacy(uint64_t base) {
    write32(base + kStatus, 0);
    write32(base + kStatus, kStatusAck);
    write32(base + kStatus, kStatusAck | kStatusDriver);
    write32(base + kDriverFeaturesSel, 0);
    write32(base + kDriverFeatures, 0);
    write32(base + kGuestPageSize, kQueueAlignBytes);
    write32(base + kQueueSel, 0);
    if (read32(base + kQueueNumMax) < kQueueSize) return false;
    write32(base + kQueueNum, kQueueSize);
    write32(base + kQueueAlign, kQueueAlignBytes);
    write32(base + kQueuePfn, static_cast<uint32_t>(phys(g_queue) / kQueueAlignBytes));
    write32(base + kStatus, kStatusAck | kStatusDriver | kStatusDriverOk);
    return true;
}

bool setup_modern(uint64_t base) {
    write32(base + kStatus, 0);
    write32(base + kStatus, kStatusAck);
    write32(base + kStatus, kStatusAck | kStatusDriver);
    write32(base + kDriverFeaturesSel, 1);
    write32(base + kDriverFeatures, 1u); // VIRTIO_F_VERSION_1 (bit 32)
    write32(base + kDriverFeaturesSel, 0);
    write32(base + kDriverFeatures, 0u);
    write32(base + kStatus, kStatusAck | kStatusDriver | kStatusFeaturesOk);
    if ((read32(base + kStatus) & kStatusFeaturesOk) == 0) return false;
    write32(base + kQueueSel, 0);
    if (read32(base + kQueueNumMax) < kQueueSize) return false;
    write32(base + kQueueNum, kQueueSize);
    write32(base + kQueueDescLow, static_cast<uint32_t>(phys(g_desc)));
    write32(base + kQueueDescHigh, static_cast<uint32_t>(phys(g_desc) >> 32));
    write32(base + kQueueDriverLow, static_cast<uint32_t>(phys(g_avail)));
    write32(base + kQueueDriverHigh, static_cast<uint32_t>(phys(g_avail) >> 32));
    write32(base + kQueueDeviceLow, static_cast<uint32_t>(phys(g_used)));
    write32(base + kQueueDeviceHigh, static_cast<uint32_t>(phys(g_used) >> 32));
    write32(base + kQueueReady, 1);
    write32(base + kStatus, kStatusAck | kStatusDriver | kStatusFeaturesOk | kStatusDriverOk);
    return true;
}

// Submit a 9P request: txlen bytes from g_tx, expect reply written into g_rx
// up to rxcap bytes. Returns reply length, or 0 on timeout / bus error.
uint32_t txn(uint32_t txlen, uint32_t rxcap) {
    g_desc[0].addr = phys(g_tx);
    g_desc[0].len = txlen;
    g_desc[0].flags = kDescNext;
    g_desc[0].next = 1;
    g_desc[1].addr = phys(g_rx);
    g_desc[1].len = rxcap;
    g_desc[1].flags = kDescWrite;
    g_desc[1].next = 0;

    g_avail->ring[g_avail->idx % kQueueSize] = 0;
    asm volatile("dsb sy" ::: "memory");
    g_avail->idx = g_avail->idx + 1;
    asm volatile("dsb sy" ::: "memory");

    write32(g_dev.notify_addr, 0);

    for (uint64_t spin = 0; spin < 200000000ULL; ++spin) {
        asm volatile("dsb sy" ::: "memory");
        if (g_used->idx != g_last_used) {
            const uint16_t slot = (g_used->idx - 1) % kQueueSize;
            const uint32_t rlen = g_used->ring[slot].len;
            g_last_used = g_used->idx;
            asm volatile("dsb sy" ::: "memory");
            return rlen;
        }
    }
    return 0;
}

// --- 9P2000.L message construction helpers. ---------------------------------
// Wire format: little-endian u8/u16/u32/u64 + (u16 len, char[len]) strings.

void wr8(uint32_t &o, uint8_t v)  { g_tx[o++] = v; }
void wr16(uint32_t &o, uint16_t v) { g_tx[o++] = v & 0xff; g_tx[o++] = v >> 8; }
void wr32(uint32_t &o, uint32_t v) { wr16(o, v); wr16(o, v >> 16); }
void wr64(uint32_t &o, uint64_t v) { wr32(o, static_cast<uint32_t>(v));
                                     wr32(o, static_cast<uint32_t>(v >> 32)); }
void wrstr(uint32_t &o, const char *s) {
    uint32_t n = 0;
    while (s[n]) ++n;
    wr16(o, static_cast<uint16_t>(n));
    for (uint32_t i = 0; i < n; ++i) g_tx[o++] = static_cast<uint8_t>(s[i]);
}
void wrhdr(uint32_t txlen, uint8_t type, uint16_t tag) {
    g_tx[0] = txlen & 0xff;
    g_tx[1] = (txlen >> 8) & 0xff;
    g_tx[2] = (txlen >> 16) & 0xff;
    g_tx[3] = (txlen >> 24) & 0xff;
    g_tx[4] = type;
    g_tx[5] = tag & 0xff;
    g_tx[6] = tag >> 8;
}
uint8_t  rd8(uint32_t &o)  { return g_rx[o++]; }
uint16_t rd16(uint32_t &o) { uint16_t a = g_rx[o++]; a |= uint16_t(g_rx[o++]) << 8; return a; }
uint32_t rd32(uint32_t &o) { uint32_t a = rd16(o); a |= uint32_t(rd16(o)) << 16; return a; }
uint64_t rd64(uint32_t &o) { uint64_t a = rd32(o); a |= uint64_t(rd32(o)) << 32; return a; }

constexpr uint8_t kRlerror   = 7;
constexpr uint8_t kTlopen    = 12, kRlopen   = 13;
constexpr uint8_t kTlcreate  = 14, kRlcreate = 15;
constexpr uint8_t kTgetattr  = 24, kRgetattr = 25;
constexpr uint8_t kTsetattr  = 26, kRsetattr = 27;
constexpr uint8_t kTreaddir  = 40, kRreaddir = 41;
constexpr uint8_t kTversion  = 100, kRversion = 101;
constexpr uint8_t kTattach   = 104, kRattach = 105;
constexpr uint8_t kTwalk     = 110, kRwalk   = 111;
constexpr uint8_t kTread     = 116, kRread   = 117;
constexpr uint8_t kTwrite    = 118, kRwrite  = 119;
constexpr uint8_t kTclunk    = 120, kRclunk  = 121;
constexpr uint8_t kTremove   = 122, kRremove = 123;

// L_OPEN flags subset we use (Linux open(2) flags forwarded over 9P2000.L).
constexpr uint32_t kLOpenRdonly = 0;
constexpr uint32_t kLOpenWronly = 1;
constexpr uint32_t kLOpenRdwr   = 2;
constexpr uint32_t kLOpenCreat  = 0x40;
constexpr uint32_t kLOpenTrunc  = 0x200;

// Tsetattr valid mask bits (mirrors Linux struct iattr).
constexpr uint64_t kSetattrSize = 0x0008;
// 9P2000.L getattr request mask (we want size + mode + nlink + ...).
constexpr uint64_t kGetattrBasic = 0x000007ffULL;

// Fid 0 reserved for nofid; root fid is 1. New scratch fids start at 2; we
// reuse a single scratch fid per call and clunk after use so we never run
// out (host only sees TWALK->TCLUNK pairs).
constexpr uint32_t kRootFid    = 1;
constexpr uint32_t kScratchFid = 2;

// Negotiate 9P2000.L and request msize. Sets g_dev.msize on success.
bool do_version() {
    uint32_t o = 7;
    wr32(o, kBufSize);                // requested msize
    wrstr(o, "9P2000.L");
    wrhdr(o, kTversion, 0xffff);
    const uint32_t r = txn(o, kBufSize);
    if (r < 7) return false;
    if (g_rx[4] != kRversion) return false;
    uint32_t ro = 7;
    const uint32_t msize = rd32(ro);
    const uint16_t vlen = rd16(ro);
    // Reject anything that isn't 9P2000.L (or a prefix match, which means the
    // server downgraded -- we don't support that yet).
    if (vlen < 8) return false;
    static const char want[] = "9P2000.L";
    for (uint32_t i = 0; i < 8; ++i) {
        if (g_rx[ro + i] != static_cast<uint8_t>(want[i])) return false;
    }
    // Clamp the server's negotiated msize to our actual transport buffer: a
    // (malicious/buggy) 9P server could reply with msize > kBufSize, and msize
    // bounds later request/response framing into the fixed g_tx/g_rx[kBufSize].
    // Real 9P clients clamp to their own buffer the same way (RX writes are also
    // virtq-descriptor-bounded, but don't rely on that alone).
    g_dev.msize = (msize < kBufSize) ? msize : kBufSize;
    return true;
}

// Attach to the host's exported root (mount_tag in QEMU -virtfs / fsdev).
// `afid` = NOFID (0xffffffff) since we don't authenticate.
bool do_attach() {
    uint32_t o = 7;
    wr32(o, kRootFid);                // fid
    wr32(o, 0xffffffffu);             // afid (NOFID -- no auth)
    wrstr(o, "crowley");              // uname
    wrstr(o, "");                     // aname (root of export)
    wr32(o, 0);                       // n_uname (uid, 0 = crowley/root)
    wrhdr(o, kTattach, 1);
    const uint32_t r = txn(o, kBufSize);
    if (r < 7 || g_rx[4] != kRattach) return false;
    g_dev.root_fid = kRootFid;
    return true;
}

// Split path on '/' into wnames (max 16). Empty/"" path = 0 wnames = clone fid.
// Stores wname offsets into the start of g_tx after we've reserved header room.
uint32_t emit_walk_wnames(uint32_t &o, const char *path) {
    uint32_t count = 0;
    const char *p = path;
    while (*p == '/') ++p;
    while (*p && count < 16) {
        const char *seg = p;
        while (*p && *p != '/') ++p;
        const uint32_t seglen = static_cast<uint32_t>(p - seg);
        if (seglen == 0) break;
        wr16(o, static_cast<uint16_t>(seglen));
        for (uint32_t i = 0; i < seglen; ++i) g_tx[o++] = static_cast<uint8_t>(seg[i]);
        ++count;
        while (*p == '/') ++p;
    }
    return count;
}

// Walk from root_fid down `path`, install resulting fid at kScratchFid.
// Returns true on success (caller must clunk scratch fid).
bool do_walk(const char *path) {
    uint32_t o = 7;
    wr32(o, kRootFid);
    wr32(o, kScratchFid);
    const uint32_t name_count_off = o;
    wr16(o, 0); // placeholder nwname
    const uint32_t count = emit_walk_wnames(o, path);
    // patch nwname in place (little-endian)
    g_tx[name_count_off] = count & 0xff;
    g_tx[name_count_off + 1] = (count >> 8) & 0xff;
    wrhdr(o, kTwalk, 1);
    const uint32_t r = txn(o, kBufSize);
    if (r < 7) return false;
    if (g_rx[4] != kRwalk) return false;
    // Rwalk body: u16 nwqid, qid[nwqid]; each qid is 13 bytes. We just need
    // nwqid == count to confirm every step succeeded.
    uint32_t ro = 7;
    const uint16_t nwqid = rd16(ro);
    return nwqid == count;
}

void do_clunk(uint32_t fid) {
    uint32_t o = 7;
    wr32(o, fid);
    wrhdr(o, kTclunk, 1);
    (void)txn(o, kBufSize);
}

bool do_getattr(uint32_t fid, uint64_t *size, uint32_t *mode, uint32_t *nlink) {
    uint32_t o = 7;
    wr32(o, fid);
    wr64(o, kGetattrBasic);
    wrhdr(o, kTgetattr, 1);
    const uint32_t r = txn(o, kBufSize);
    if (r < 7 || g_rx[4] != kRgetattr) return false;
    // Rgetattr body: u64 valid; qid(13); u32 mode; u32 uid; u32 gid; u64 nlink;
    // u64 rdev; u64 size; u64 blksize; u64 blocks; ...
    uint32_t ro = 7;
    (void)rd64(ro);      // valid mask
    ro += 13;            // qid
    if (mode) *mode = rd32(ro); else (void)rd32(ro);
    (void)rd32(ro);      // uid
    (void)rd32(ro);      // gid
    if (nlink) *nlink = static_cast<uint32_t>(rd64(ro)); else (void)rd64(ro);
    (void)rd64(ro);      // rdev
    if (size) *size = rd64(ro); else (void)rd64(ro);
    return true;
}

bool do_lopen(uint32_t fid, uint32_t flags) {
    uint32_t o = 7;
    wr32(o, fid);
    wr32(o, flags);
    wrhdr(o, kTlopen, 1);
    const uint32_t r = txn(o, kBufSize);
    if (r < 7 || g_rx[4] != kRlopen) return false;
    return true;
}

// Returns bytes read (0 = EOF, <0 unused). Reads up to `count` from `offset`.
int64_t do_read(uint32_t fid, uint64_t offset, void *buf, uint32_t count) {
    uint32_t o = 7;
    wr32(o, fid);
    wr64(o, offset);
    wr32(o, count);
    wrhdr(o, kTread, 1);
    const uint32_t r = txn(o, kBufSize);
    if (r < 11 || g_rx[4] != kRread) return -1;
    uint32_t ro = 7;
    const uint32_t got = rd32(ro);
    auto *out = static_cast<uint8_t *>(buf);
    for (uint32_t i = 0; i < got; ++i) out[i] = g_rx[ro + i];
    return got;
}

// 9P2000.L Treaddir returns a packed stream of {qid(13), u64 offset, u8 type,
// u16 nlen, char name[nlen]}. We invoke callback once per entry until 0 bytes
// are returned (EOF) or the LateranEntry buffer fills up.
uint32_t do_readdir(uint32_t fid, LateranEntry *out, uint32_t max) {
    uint32_t total = 0;
    uint64_t offset = 0;
    while (total < max) {
        uint32_t o = 7;
        wr32(o, fid);
        wr64(o, offset);
        wr32(o, kBufSize - 11);   // count = reply body cap
        wrhdr(o, kTreaddir, 1);
        const uint32_t r = txn(o, kBufSize);
        if (r < 11 || g_rx[4] != kRreaddir) break;
        uint32_t ro = 7;
        const uint32_t blen = rd32(ro);
        if (blen == 0) break; // EOF
        uint32_t end = ro + blen;
        // blen is the host-supplied body length; clamp it to the bytes we
        // actually received (r <= kBufSize) and guard the u32 wrap, or a
        // malicious/buggy 9p server drives the dirent loop past g_rx[] -> OOB
        // read (stale BSS leaked into dirent names). Same class as the TCP
        // ip_total bound in handle_inbound.
        if (end < ro || end > r) end = r;
        while (ro + 24 <= end && total < max) {
            const uint8_t qtype = g_rx[ro];     // qid.type (bit 7 = dir)
            ro += 13;                            // skip qid (13 bytes)
            const uint64_t entry_off = rd64(ro);
            (void)rd8(ro);                       // d_type
            const uint16_t nlen = rd16(ro);
            if (ro + nlen > end) break;
            const bool is_dir = (qtype & 0x80) != 0;
            // Skip "." and ".." -- guest tools don't expect them in our
            // LateranEntry stream (ext2 listings don't include them either).
            const bool dot =
                (nlen == 1 && g_rx[ro] == '.') ||
                (nlen == 2 && g_rx[ro] == '.' && g_rx[ro + 1] == '.');
            if (!dot) {
                LateranEntry &e = out[total++];
                e = LateranEntry{};
                const uint32_t copy = nlen < sizeof(e.name) - 1 ? nlen : sizeof(e.name) - 1;
                for (uint32_t i = 0; i < copy; ++i) e.name[i] = static_cast<char>(g_rx[ro + i]);
                e.name[copy] = 0;
                e.is_dir = is_dir;
                e.mode = is_dir ? (0040000u | 0755u) : (0100000u | 0644u);
                e.nlink = 1;
            }
            ro += nlen;
            offset = entry_off;
        }
        if (ro == 7 + 4) break; // nothing parsed this round
    }
    return total;
}

int64_t do_write(uint32_t fid, uint64_t offset, const void *buf, uint32_t count) {
    uint32_t o = 7;
    wr32(o, fid);
    wr64(o, offset);
    wr32(o, count);
    auto *src = static_cast<const uint8_t *>(buf);
    for (uint32_t i = 0; i < count; ++i) g_tx[o++] = src[i];
    wrhdr(o, kTwrite, 1);
    const uint32_t r = txn(o, kBufSize);
    if (r < 11 || g_rx[4] != kRwrite) return -1;
    uint32_t ro = 7;
    return static_cast<int64_t>(rd32(ro));
}

// Walk to parent of `path`, create+open `leaf` with given flags+mode there,
// install the new file fid at kScratchFid. Returns true on success.
bool do_lcreate(const char *parent_path, const char *leaf, uint32_t flags, uint32_t mode) {
    if (!do_walk(parent_path)) return false;
    // Tlcreate moves the fid from parent to the newly-created file. Use the
    // scratch fid as both parent (after walk) and post-create handle.
    uint32_t o = 7;
    wr32(o, kScratchFid);
    wrstr(o, leaf);
    wr32(o, flags);
    wr32(o, mode);
    wr32(o, 0); // gid (root/crowley)
    wrhdr(o, kTlcreate, 1);
    const uint32_t r = txn(o, kBufSize);
    if (r < 7 || g_rx[4] != kRlcreate) {
        do_clunk(kScratchFid);
        return false;
    }
    return true;
}

bool do_setattr(uint32_t fid, uint64_t valid, uint64_t size) {
    uint32_t o = 7;
    wr32(o, fid);
    wr32(o, static_cast<uint32_t>(valid));
    wr32(o, 0);                // mode (ignored unless valid&MODE)
    wr32(o, 0);                // uid
    wr32(o, 0);                // gid
    wr64(o, size);             // size
    wr64(o, 0); wr64(o, 0);    // atime sec/nsec
    wr64(o, 0); wr64(o, 0);    // mtime sec/nsec
    wrhdr(o, kTsetattr, 1);
    const uint32_t r = txn(o, kBufSize);
    return r >= 7 && g_rx[4] == kRsetattr;
}

bool do_remove(uint32_t fid) {
    uint32_t o = 7;
    wr32(o, fid);
    wrhdr(o, kTremove, 1);
    const uint32_t r = txn(o, kBufSize);
    // Tremove always clunks the fid even on failure (per spec).
    return r >= 7 && g_rx[4] == kRremove;
}

// Split "/foo/bar/baz" into parent="/foo/bar", leaf="baz". Returns false if
// no separator (path is the root or a bare leaf with no parent context).
// leaf_out is a pointer INTO the original path buffer (no copy).
bool split_parent_leaf(const char *path, char *parent_buf, uint32_t parent_cap,
                       const char **leaf_out) {
    if (path == nullptr) return false;
    // Find last '/'. Treat trailing slash as "no leaf".
    const char *last_slash = nullptr;
    for (const char *p = path; *p; ++p) {
        if (*p == '/') last_slash = p;
    }
    if (last_slash == nullptr || last_slash[1] == 0) return false;
    const uint32_t pn = static_cast<uint32_t>(last_slash - path);
    if (pn + 1 > parent_cap) return false;
    for (uint32_t i = 0; i < pn; ++i) parent_buf[i] = path[i];
    parent_buf[pn] = 0;
    *leaf_out = last_slash + 1;
    return true;
}

bool init_after_bring_up() {
    if (!do_version()) return false;
    if (!do_attach()) return false;
    return true;
}

} // namespace

const StiylMagnus &stiyl_magnus_status() { return g_dev; }

StiylMagnus stiyl_magnus_probe() {
    namespace district = imaginary_number_district;
    zero(g_queue, sizeof(g_queue));
    for (uint32_t i = 0; i < kMmioSlots; ++i) {
        const uint64_t base = kMmioBase + i * kMmioStride;
        if (read32(base + kMagicValue) != kMagic) continue;
        if (read32(base + kDeviceId) != kDeviceId9p) continue;
        const uint32_t version = read32(base + kVersion);
        const bool ok = (version == 1) ? setup_legacy(base) : setup_modern(base);
        if (!ok) continue;
        g_last_used = g_used->idx;
        g_dev.base = base;
        g_dev.notify_addr = base + kQueueNotify;
        g_dev.present = true;
        if (!init_after_bring_up()) {
            district::write("  StiylMagnus(9p)   : transport up, 9P handshake failed\n");
            g_dev.present = false;
        }
        return g_dev;
    }
    return g_dev;
}

int64_t stiyl_read_file(const char *path, char *buf, uint32_t cap) {
    if (!g_dev.present || cap == 0) return -1;
    if (!do_walk(path)) return -1;
    if (!do_lopen(kScratchFid, kLOpenRdonly)) { do_clunk(kScratchFid); return -1; }
    int64_t total = 0;
    while (static_cast<uint32_t>(total) < cap) {
        const uint32_t want = cap - static_cast<uint32_t>(total);
        const uint32_t chunk = want < (kBufSize - 11) ? want : (kBufSize - 11);
        const int64_t got = do_read(kScratchFid, static_cast<uint64_t>(total), buf + total, chunk);
        if (got <= 0) break;
        total += got;
        if (static_cast<uint32_t>(got) < chunk) break; // short read = EOF
    }
    do_clunk(kScratchFid);
    return total;
}

bool stiyl_stat(const char *path, LateranEntry *out) {
    if (!g_dev.present || out == nullptr) return false;
    if (!do_walk(path)) return false;
    uint64_t size = 0;
    uint32_t mode = 0;
    uint32_t nlink = 1;
    const bool ok = do_getattr(kScratchFid, &size, &mode, &nlink);
    do_clunk(kScratchFid);
    if (!ok) return false;
    *out = LateranEntry{};
    out->size = static_cast<uint32_t>(size);
    out->mode = mode;
    out->nlink = static_cast<uint16_t>(nlink);
    out->is_dir = (mode & 0xF000) == 0x4000;
    return true;
}

bool stiyl_is_dir(const char *path) {
    LateranEntry e{};
    return stiyl_stat(path, &e) && e.is_dir;
}

uint32_t stiyl_list_dir(const char *path, LateranEntry *out, uint32_t max) {
    if (!g_dev.present || out == nullptr || max == 0) return 0;
    if (!do_walk(path)) return 0;
    if (!do_lopen(kScratchFid, kLOpenRdonly)) { do_clunk(kScratchFid); return 0; }
    const uint32_t n = do_readdir(kScratchFid, out, max);
    do_clunk(kScratchFid);
    return n;
}

int64_t stiyl_write_file(const char *path, const char *buf, uint32_t len) {
    if (!g_dev.present || path == nullptr) return -1;
    // Need parent dir + leaf. "/foo" has parent "", leaf "foo".
    char parent[256];
    const char *leaf = nullptr;
    if (!split_parent_leaf(path, parent, sizeof(parent), &leaf)) {
        // Path has no '/', treat as a leaf in the root.
        leaf = path;
        while (*leaf == '/') ++leaf; // strip any leading '/'s (shouldn't happen)
        if (*leaf == 0) return -1;
        parent[0] = 0;
    }
    const uint32_t flags = kLOpenWronly | kLOpenCreat | kLOpenTrunc;
    if (!do_lcreate(parent, leaf, flags, 0644)) return -1;
    int64_t total = 0;
    while (static_cast<uint32_t>(total) < len) {
        const uint32_t want = len - static_cast<uint32_t>(total);
        // Each Twrite chunk needs request header (7) + fid (4) + offset (8) +
        // count (4) + payload, so cap payload at msize minus those 23 bytes
        // (plus a little slack).
        const uint32_t chunk = want < (kBufSize - 32) ? want : (kBufSize - 32);
        const int64_t w = do_write(kScratchFid, static_cast<uint64_t>(total),
                                   buf + total, chunk);
        if (w <= 0) break;
        total += w;
        if (static_cast<uint32_t>(w) < chunk) break;
    }
    do_clunk(kScratchFid);
    return total;
}

bool stiyl_unlink(const char *path) {
    if (!g_dev.present) return false;
    if (!do_walk(path)) return false;
    // Tremove implicitly clunks the fid whether or not the unlink succeeded.
    return do_remove(kScratchFid);
}

bool stiyl_truncate(const char *path, uint64_t new_size) {
    if (!g_dev.present) return false;
    if (!do_walk(path)) return false;
    const bool ok = do_setattr(kScratchFid, kSetattrSize, new_size);
    do_clunk(kScratchFid);
    return ok;
}

} // namespace index::drivers
