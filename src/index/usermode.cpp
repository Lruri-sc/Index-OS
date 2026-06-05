#include <stdint.h>

#include "arch/aarch64/cpu.hpp"
#include "drivers/misaka_mail.hpp"
#include "drivers/othinus.hpp"
#include "index/aiwass.hpp"
#include "index/anti_skill.hpp"
#include "index/antenna.hpp"
#include "index/event_queue.hpp"
#include "index/imaginary_number_channel.hpp"
#include "index/sister_relay.hpp"
#include "index/bookshelf.hpp"
#include "index/dark_matter.hpp"
#include "index/esper.hpp"
#include "index/fortis931.hpp"
#include "index/grimoire_fs.hpp"
#include "index/imaginary_number_district.hpp"
#include "index/last_order.hpp"
#include "index/lateran.hpp"
#include "index/linux_abi.hpp"
#include "index/misaka_network.hpp"
#include "index/personal_reality.hpp"
#include "index/personal_reality_v2.hpp"
#include "index/teleport.hpp"
#include "index/usermode.hpp"

// Provided by user_switch.S.
extern "C" void enter_user(uint64_t entry, uint64_t user_sp, uint64_t ttbr0);
extern "C" [[noreturn]] void leave_user();
// Sibling of enter_user: rather than starting fresh, resume a previously-saved
// EL0 context. Caller MUST switch_ttbr0(e->ttbr0) before calling. Used by
// run_espers's loop to bring back a parked or preempted Esper after WFI /
// leave_user.
//
// NOT marked [[noreturn]] even though the asm eret never falls through: the
// effective return path is leave_user (called later from an EL0 syscall
// handler), which `ret`s to the LR saved by the asm's prologue stp -- i.e.,
// the instruction AFTER the bl in run_espers. If we mark this [[noreturn]],
// the compiler emits no continuation code and the "instruction after bl"
// ends up being the START of whatever function the linker placed next --
// e.g., run_user_fault, which then runs unexpectedly. Leaving this NOT
// noreturn forces the compiler to emit real loop continuation code at LR+0.
extern "C" void resume_user_eret(
    uint64_t *regs_ptr, uint64_t sp_el0, uint64_t elr,
    uint64_t spsr, uint64_t tpidr);

// FP/SIMD (NEON) save/restore for the EL0 context (user_switch.S). The kernel
// never uses FP (-mgeneral-regs-only), so these only need to run when switching
// between EL0 Espers -- saving the outgoing thread's q0..q31/FPSR/FPCR and
// restoring the incoming thread's -- so a thread preempted mid-NEON (OpenSSL
// crypto during the SSH KEX) doesn't resume with another thread's vectors.
extern "C" void fpsimd_save(void *area);
extern "C" void fpsimd_restore(const void *area);

// Embedded user entry points (in .user_text, EL0-executable via TTBR1).
extern "C" void user_entry();
extern "C" void user_fault_entry();

// Stack for the embedded users (high VA, EL0 RW via the kernel mapping).
extern "C" char __user_stack_end[];

namespace index {

namespace {

// Syscall numbers (x8).
constexpr uint64_t kSysPutc = 1;
constexpr uint64_t kSysGetpid = 2;
constexpr uint64_t kSysExit = 3;
constexpr uint64_t kSysYield = 4;
constexpr uint64_t kSysFork = 5;
constexpr uint64_t kSysExec = 6;
constexpr uint64_t kSysWait = 7;
constexpr uint64_t kSysWrite = 8;
constexpr uint64_t kSysRead = 9;
constexpr uint64_t kSysOpen = 10;
constexpr uint64_t kSysClose = 11;
constexpr uint64_t kSysPipe = 12;
constexpr uint64_t kSysDup = 13;
constexpr uint64_t kSysDup2 = 14;
constexpr uint64_t kSysKill = 15;

constexpr uint32_t kEsrEcShift = 26;
constexpr uint32_t kEcSvc64 = 0x15;
constexpr uint32_t kEcInstrAbortLowerEl = 0x20;
constexpr uint32_t kEcDataAbortLowerEl = 0x24;
constexpr uint32_t kEcSoftStepLowerEl = 0x32; // ptrace single-step debug exception

// ptrace "Mental Out" single-step: arm/disarm hardware software-step (MDSCR_EL1
// .SS, bit 0) for the EL0 context about to be resumed. Read-modify-write so the
// other MDSCR debug bits are preserved. SPSR.SS (PSTATE single-step) is bit 21;
// set in the resumed SPSR alongside this so exactly one EL0 instruction retires
// before the Software Step exception (EC 0x32) re-traps. Called from every
// resume-to-EL0 path (load_ctx, esper_preempt) keyed on the resuming Esper's
// flag, so a preempt to a non-stepping Esper can never leave stepping armed.
constexpr uint64_t kSpsrSsBit = 1ULL << 21;
inline void mental_out_arm_step(bool on) {
    uint64_t mdscr;
    asm volatile("mrs %0, mdscr_el1" : "=r"(mdscr));
    if (on) mdscr |= 1ULL; else mdscr &= ~1ULL;
    asm volatile("msr mdscr_el1, %0" ::"r"(mdscr));
    asm volatile("isb");
}

constexpr uint64_t kSpsrEl0 = 0x340; // EL0t, IRQ enabled (preemptible), F/A/D masked

// IRQ-frame offsets (irq_entry): x0..x30 at [0..30], ELR at [32], SPSR at [33].
constexpr uint32_t kFrameElr = 32;
constexpr uint32_t kFrameSpsr = 33;

uint64_t g_last_user_el = 99;

// Switch the live user address space (TTBR0) and flush stale translations.
// Use the inner-shareable broadcast variant so an Esper resumed on a CPU
// that previously ran a *different* address space (under SMP EL0 the
// scheduler may pick this Esper on any core) does not see stale TLB
// entries from the prior occupant.
void switch_ttbr0(uint64_t ttbr0) {
    asm volatile("dsb ish" ::: "memory");
    asm volatile("msr ttbr0_el1, %0" ::"r"(ttbr0));
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");
}

// --- ELF64 (AArch64) parser: lay a PIE program out into a flat image ---

// Sniff which ABI an ELF wants, by walking program headers + a couple of
// header fields. Returns Abi::Linux if any of the following hold:
//   - PT_INTERP segment (i.e. needs a dynamic linker -> definitely Linux).
//   - e_ident[EI_OSABI] == 3 (ELFOSABI_LINUX).
//   - e_type == ET_EXEC (Index always builds PIE; ET_EXEC means external).
//   - e_entry >= 0x400000 (default base for `aarch64-linux-*-gcc -static`,
//     well outside our kUserCodeBase=0x10000 Index window).
// Otherwise returns Abi::Index. The rules are ordered cheap-first so the
// common "this is an Index PIE we built" path returns after a few field reads.
Abi sniff_abi(const uint8_t *file, int64_t len) {
    if (len < 64 || file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' || file[3] != 'F') {
        return Abi::Index; // garbage will be rejected by build_image anyway
    }
    const uint8_t osabi = file[7];
    const uint16_t e_type = static_cast<uint16_t>(file[16] | (file[17] << 8));
    const uint64_t e_entry =
        static_cast<uint64_t>(file[24]) | (static_cast<uint64_t>(file[25]) << 8) |
        (static_cast<uint64_t>(file[26]) << 16) | (static_cast<uint64_t>(file[27]) << 24) |
        (static_cast<uint64_t>(file[28]) << 32) | (static_cast<uint64_t>(file[29]) << 40) |
        (static_cast<uint64_t>(file[30]) << 48) | (static_cast<uint64_t>(file[31]) << 56);
    const uint64_t e_phoff =
        static_cast<uint64_t>(file[32]) | (static_cast<uint64_t>(file[33]) << 8) |
        (static_cast<uint64_t>(file[34]) << 16) | (static_cast<uint64_t>(file[35]) << 24) |
        (static_cast<uint64_t>(file[36]) << 32) | (static_cast<uint64_t>(file[37]) << 40) |
        (static_cast<uint64_t>(file[38]) << 48) | (static_cast<uint64_t>(file[39]) << 56);
    const uint16_t e_phentsize = static_cast<uint16_t>(file[54] | (file[55] << 8));
    const uint16_t e_phnum = static_cast<uint16_t>(file[56] | (file[57] << 8));

    for (uint16_t i = 0; i < e_phnum; ++i) {
        const uint64_t phoff = e_phoff + static_cast<uint64_t>(i) * e_phentsize;
        if (phoff + 4 > static_cast<uint64_t>(len)) {
            break;
        }
        const uint32_t p_type = static_cast<uint32_t>(file[phoff]) |
                                (static_cast<uint32_t>(file[phoff + 1]) << 8) |
                                (static_cast<uint32_t>(file[phoff + 2]) << 16) |
                                (static_cast<uint32_t>(file[phoff + 3]) << 24);
        if (p_type == 3 /*PT_INTERP*/) {
            return Abi::Linux;
        }
    }
    if (osabi == 3 /*ELFOSABI_LINUX*/) {
        return Abi::Linux;
    }
    if (e_type == 2 /*ET_EXEC*/) {
        return Abi::Linux;
    }
    if (e_entry >= 0x400000) {
        return Abi::Linux;
    }
    return Abi::Index;
}

uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t *p) {
    return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
}

// Parse a PIE ELF, copy its PT_LOAD segments into `img` (at their vaddrs), and
// report the in-memory span. Returns the entry offset (e_entry) or 0 on error.
uint64_t build_image(const uint8_t *file, int64_t len, uint8_t *img, uint64_t cap,
                     uint64_t *span) {
    namespace district = imaginary_number_district;
    if (len < 64 || file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' || file[3] != 'F') {
        district::writeln("exec: not an ELF file.");
        return 0;
    }
    const uint16_t e_type = rd16(file + 16);
    if (file[4] != 2 || rd16(file + 18) != 183 /*EM_AARCH64*/ ||
        (e_type != 3 /*ET_DYN*/ && e_type != 2 /*ET_EXEC*/)) {
        district::writeln("exec: not an AArch64 ET_EXEC/ET_DYN executable.");
        return 0;
    }
    const uint64_t e_entry = rd64(file + 24);
    const uint64_t e_phoff = rd64(file + 32);
    const uint16_t e_phentsize = rd16(file + 54);
    const uint16_t e_phnum = rd16(file + 56);

    uint64_t max_end = 0;
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const uint8_t *ph = file + e_phoff + static_cast<uint64_t>(i) * e_phentsize;
        if (rd32(ph) != 1) {
            continue;
        }
        const uint64_t end = rd64(ph + 16) + rd64(ph + 40); // vaddr + memsz
        if (end > max_end) {
            max_end = end;
        }
    }
    if (max_end == 0 || max_end > cap) {
        district::writeln("exec: image too large.");
        return 0;
    }
    for (uint64_t b = 0; b < max_end; ++b) {
        img[b] = 0;
    }
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const uint8_t *ph = file + e_phoff + static_cast<uint64_t>(i) * e_phentsize;
        if (rd32(ph) != 1) {
            continue;
        }
        const uint64_t off = rd64(ph + 8);
        const uint64_t vaddr = rd64(ph + 16);
        const uint64_t filesz = rd64(ph + 32);
        if (off + filesz > static_cast<uint64_t>(len)) {
            district::writeln("exec: bad segment.");
            return 0;
        }
        for (uint64_t b = 0; b < filesz; ++b) {
            img[vaddr + b] = file[off + b];
        }
    }
    *span = max_end;
    return e_entry;
}

// Copy a NUL-terminated string (from user or kernel memory) into a bounded
// kernel buffer. The user's address space is the live TTBR0 during a syscall and
// EL0 pages are RW at EL1, so reading a user pointer here is a direct load.
void copy_str(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    if (src != nullptr) {
        for (; i + 1 < cap && src[i]; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = 0;
}

// Read a whole file by name from any mounted filesystem (Lateran FAT disk,
// then Bookshelf, then GrimoireFS). Returns bytes read, or -1 if not found.
// Serialises all disk-backed filesystem access. Lateran/ext2 + the Underline
// virtio-blk driver keep mutable global state (block requests, ext2 inode
// walk scratch) with no internal locking -- fine single-core, but multi-core
// EL0 now execs concurrently (one heavy C++ program per core), so several
// cores read ELF/.so files from ext2 at once. Without serialisation the
// shared virtio queue + ext2 state race and reads spuriously fail ("/bin/
// mt_cxx: not found" on all-but-one core). The disk is a single device, so a
// global lock is the natural granularity (Linux uses a per-bdev request
// queue lock for the same reason). g_esper_lock is NOT reused: FS reads can
// be long (synchronous virtio polling) and must not block the scheduler.
AntiSkill g_fs_lock;

int64_t read_named_file_inner(const char *name, char *buf, uint32_t cap) {
    if (lateran_mounted()) {
        const int64_t n = lateran_read_file(name, buf, cap);
        if (n >= 0) {
            return n;
        }
    }
    const BookshelfFile *bf = bookshelf_find(name);
    if (bf != nullptr && bf->data != nullptr) {
        uint32_t n = 0;
        while (n < cap && n < bf->size) {
            buf[n] = bf->data[n];
            ++n;
        }
        return static_cast<int64_t>(n);
    }
    const Grimoire *g = grimoire_fs_find(name);
    if (g != nullptr && g->text != nullptr) {
        uint32_t n = 0;
        while (n < cap && g->text[n] != 0) {
            buf[n] = g->text[n];
            ++n;
        }
        return static_cast<int64_t>(n);
    }
    return -1;
}

int64_t read_named_file(const char *name, char *buf, uint32_t cap) {
    const uint64_t flags = anti_skill_lock_irqsave(g_fs_lock);
    const int64_t r = read_named_file_inner(name, buf, cap);
    anti_skill_unlock_irqrestore(g_fs_lock, flags);
    return r;
}

// SYS_read(fd 0): block on the console for a line, echoing as it arrives. Safe
// to busy-poll here -- IRQs are masked for the duration of the svc, and only the
// foreground (reading) process stalls; secondary cores keep running Sisters.
uint64_t read_console_line(char *buf, uint64_t cap) {
    namespace district = imaginary_number_district;
    uint64_t n = 0;
    for (;;) {
        const int ch = district::try_read();
        if (ch < 0) {
            asm volatile("yield");
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            district::putc('\n');
            break;
        }
        if (ch == '\b' || ch == 0x7f) {
            if (n > 0) {
                --n;
                district::write("\b \b");
            }
            continue;
        }
        if (ch < 0x20 || ch > 0x7e) {
            continue;
        }
        if (n < cap) {
            buf[n++] = static_cast<char>(ch);
            district::putc(static_cast<char>(ch));
        } else {
            break;
        }
    }
    return n;
}

// Read buffer cap for loading a file off disk. Big enough for ld-musl
// (~633 KiB), busybox (~1.1 MiB), and Alpine's libstdc++.so.6 (~2.75 MiB).
// Anything larger would currently get silently truncated by
// read_file_fd / read_named_file -- bump if needed.
constexpr uint64_t kElfReadCap = 4 * 1024 * 1024;

// SYS_read(fd >= 3): serve the next slice of an open file. Re-reads the whole
// (small) file each call and copies from the saved offset -- simple and stateless.
// Allocates only as much temporary buffer as the file actually needs so it
// doesn't compete with mmap's 4 MiB-per-call buffers on a small heap.
uint64_t read_file_fd(Esper *e, uint32_t fd, char *buf, uint64_t cap) {
    if (fd >= kMaxFds || e->fds[fd].kind != FdKind::file) {
        return static_cast<uint64_t>(-1);
    }
    const uint64_t off = e->fds[fd].off;
    // Fast path: ext2/tmpfs support POSITIONAL reads -- fetch only [off, off+cap),
    // not the whole file. Critical for large files: java's rt.jar is 33.5 MiB and
    // class loading issues thousands of small reads; the old "re-read the whole
    // file every call" path (kept as the fallback below) re-fetched all 33.5 MiB
    // (~65k virtio-blk blocks) on EVERY 202-byte read -- which looked exactly like
    // a hang (CPU 0% in virtio-blk I/O wait, PC pinned in underline_read). The
    // lldb backtrace at the "hang" was the smoking gun: linux_file_read(cap=202)
    // -> read_named_file(cap=35126412). lateran_pread already existed (added for
    // demand paging) but this SYS_read path never used it.
    if (cap > 0) {
        const uint32_t want = cap > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(cap);
        const int64_t pn = lateran_pread(e->fds[fd].path, off, buf, want);
        if (pn >= 0) { // ext2/tmpfs served it (0 = EOF)
            e->fds[fd].off += static_cast<uint64_t>(pn);
            return static_cast<uint64_t>(pn);
        }
    }
    // Fallback for in-memory filesystems (Bookshelf / GrimoireFS) that don't do
    // positional reads: re-read the whole (small) file and copy the slice.
    const int64_t fsz = linux_file_size(e->fds[fd].path);
    if (fsz < 0) return 0;
    const uint64_t need = static_cast<uint64_t>(fsz);
    auto *tmp = static_cast<char *>(dark_matter_alloc(need > 0 ? need : 1));
    if (tmp == nullptr) {
        return static_cast<uint64_t>(-1);
    }
    const int64_t total = read_named_file(e->fds[fd].path, tmp, static_cast<uint32_t>(need));
    if (total < 0) {
        dark_matter_free(tmp);
        return 0;
    }
    uint64_t n = 0;
    while (n < cap && off + n < static_cast<uint64_t>(total)) {
        buf[n] = tmp[off + n];
        ++n;
    }
    e->fds[fd].off += n;
    dark_matter_free(tmp);
    return n;
}

// Load PIE ELF `name` into Esper slot `slot`'s address space, (re)building it.
// Linux ABI ELF base address. PIE binaries have small p_vaddr (often 0) so
// we add this offset to every VA, putting the program well above the NULL
// trap and inside L1 entry 0 (0..1 GiB). The Linux toolchain's default
// static-linked layout also lives here, so heuristics catch it.
constexpr uint64_t kLinuxElfBase = 0x400000;
// User stack for Linux ABI Espers: 16 KiB just under the top of the 39-bit
// TTBR0 window. Sits in L1 entry 511, so the demand pager has to install a
// fresh L2/L3 for it -- proves arbitrary VAs work, not just entry 0.
constexpr uint64_t kLinuxStackTop = 0x7FFFFFC000ULL;
constexpr uint64_t kLinuxStackSize = 0x100000; // 1 MiB stack window (busybox sh needs >64K)
// Heap (brk) region: a generous reservation just past the program. Pages
// fault in on demand as the break grows, so reserving a large VMA costs
// nothing until touched.
constexpr uint64_t kLinuxBrkMax = 0x4000000; // 64 MiB max heap
// mmap region: anonymous mappings bump up from here (well clear of the program
// and the heap, below the stack). 0x10_0000_0000 = 64 GiB.
constexpr uint64_t kLinuxMmapBase = 0x1000000000ULL;
// Dynamic-linker load base: where the PT_INTERP interpreter (ld-musl) is
// mapped. Clear of the program (~0x400000), the mmap region (64 GiB) and the
// stack (~512 GiB). 0x50_0000_0000 = 320 GiB.
constexpr uint64_t kLinuxInterpBase = 0x5000000000ULL;
// (kElfReadCap declared earlier, before read_file_fd)

// Small refcount table for shared ELF images. fork() shares the read-only ELF
// image bytes between parent and child (file-backed VMAs read from them), so a
// raw free on either's exit would dangle the other. Track refs by pointer; free
// only when the last reference drops. A handful of live images at once, so a
// tiny linear table suffices.
struct ImageRef {
    uint8_t *img = nullptr;
    uint32_t refs = 0;
};
ImageRef g_image_refs[2 * kMaxEspers];

void image_ref(uint8_t *img) {
    if (img == nullptr) return;
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    for (auto &r : g_image_refs) {
        if (r.img == img) { ++r.refs; anti_skill_unlock_irqrestore(g_esper_lock, flags); return; }
    }
    for (auto &r : g_image_refs) {
        if (r.img == nullptr) { r.img = img; r.refs = 1; anti_skill_unlock_irqrestore(g_esper_lock, flags); return; }
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    // Table full (shouldn't happen): leak rather than corrupt.
}

// Drop one reference; returns true if the caller should actually free `img`.
// Same SMP rationale as as_ref/as_unref: line-scanned table, two concurrent
// callers could collide without the scheduler lock.
bool image_unref(uint8_t *img) {
    if (img == nullptr) return false;
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    for (auto &r : g_image_refs) {
        if (r.img == img) {
            if (--r.refs == 0) {
                r.img = nullptr;
                anti_skill_unlock_irqrestore(g_esper_lock, flags);
                return true;
            }
            anti_skill_unlock_irqrestore(g_esper_lock, flags);
            return false; // still referenced by another Esper
        }
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    return true; // untracked (loaded before refcounting) -> free
}

// Address-space refcounting now lives in PersonalReality (personal_reality_v2):
// reality_alloc / reality_ref / reality_unref replace the old as_ref/as_unref +
// g_as_refs table. The refcount is PersonalReality::refs; the page tables are
// reclaimed (pr2_destroy) when the last sharer drops, and reality_unref does the
// same switch-to-kernel-TTBR0 SMP use-after-free guard as_unref used to (a
// secondary core may still be fetching kernel code through this CPU's TTBR0 when
// the space is freed -- mirror Linux switching to init_mm before mmdrop).

// Translate an ELF p_flags (PF_R=4, PF_W=2, PF_X=1) into our Vma prot bits.
uint8_t elf_prot_to_vma_prot(uint32_t pflags) {
    uint8_t prot = 0;
    if (pflags & 4) prot |= kVmaProtR;
    if (pflags & 2) prot |= kVmaProtW;
    if (pflags & 1) prot |= kVmaProtX;
    return prot;
}

// Load a Linux-ABI ELF: keep the file buffer alive on the Esper (file-backed
// VMAs reference it on every fault), create a fresh demand-paged address
// space, add a VMA per PT_LOAD plus a stack VMA, and set the EL0 entry
// state. Caller has already validated the ELF magic + ABI.
//
// Map every PT_LOAD of an ELF image into the Esper's VMA list at `load_base`
// (0 for a fixed ET_EXEC, a chosen base for PIE / the interpreter). Reports the
// highest mapped VA in *out_max_end and the program-header VA in *out_phdr_va.
// Returns false if the VMA table fills up. `image` is the kernel buffer the
// file-backed VMAs will read from on each fault (must stay resident).
bool map_elf_loads(Esper *e, const uint8_t *image, uint64_t image_len,
                   uint64_t load_base, uint64_t *out_max_end, uint64_t *out_phdr_va) {
    namespace district = imaginary_number_district;
    const uint64_t e_phoff = rd64(image + 32);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    // The phdr table fields are attacker-controlled (any user can exec a crafted
    // ELF). Validate the whole table lies within the image before walking it,
    // else `ph = image + e_phoff + i*e_phentsize` reads kernel heap past the
    // buffer. Mirrors Linux fs/binfmt_elf.c. (phnum*phentsize <= 2^32, no wrap.)
    if (e_phentsize < 56 || e_phnum == 0 || e_phoff > image_len ||
        static_cast<uint64_t>(e_phnum) * e_phentsize > image_len - e_phoff) {
        district::writeln("exec: bad ELF phdr table.");
        return false;
    }
    constexpr uint64_t kUserCeiling = 0x8000000000ULL;
    uint64_t max_end = 0;
    uint64_t phdr_va = 0;
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const uint8_t *ph = image + e_phoff + static_cast<uint64_t>(i) * e_phentsize;
        if (rd32(ph) != 1 /*PT_LOAD*/) {
            continue;
        }
        const uint64_t p_offset = rd64(ph + 8);
        const uint64_t p_vaddr = rd64(ph + 16);
        const uint64_t p_filesz = rd64(ph + 32);
        const uint64_t p_memsz = rd64(ph + 40);
        const uint32_t p_flags = rd32(ph + 4);
        const uint64_t va_start = load_base + p_vaddr;
        // Reject a segment that escapes the user VA window (a crafted p_vaddr
        // could otherwise place a VMA over the kernel's inherited mappings).
        if (va_start >= kUserCeiling || p_memsz > kUserCeiling ||
            va_start + p_memsz > kUserCeiling) {
            district::writeln("exec: PT_LOAD outside user VA.");
            return false;
        }
        // Clamp the file-backed extent to what actually lies in the image, so a
        // crafted p_offset/p_filesz can't make the fault handler copy kernel heap
        // past the image into the user's page (an info leak). The tail beyond
        // eff_filesz is zero-filled (.bss) as usual.
        uint64_t eff_filesz = 0;
        if (p_offset < image_len) {
            const uint64_t avail = image_len - p_offset;
            eff_filesz = (p_filesz < avail) ? p_filesz : avail;
        }
        const uint8_t prot = elf_prot_to_vma_prot(p_flags);
        if (!pr2_add_vma(e, va_start, va_start + p_memsz, prot,
                         static_cast<uint8_t>(VmaKind::File),
                         image + p_offset, 0, eff_filesz, va_start)) {
            district::writeln("exec: too many VMAs / bad PT_LOAD.");
            return false;
        }
        if (va_start + p_memsz > max_end) {
            max_end = va_start + p_memsz;
        }
        if (e_phoff >= p_offset && e_phoff < p_offset + p_filesz) {
            phdr_va = va_start + (e_phoff - p_offset);
        }
    }
    if (out_max_end != nullptr) *out_max_end = max_end;
    if (out_phdr_va != nullptr) *out_phdr_va = phdr_va;
    return true;
}

// Find the PT_INTERP segment and copy its NUL-terminated interpreter path into
// `out` (bounded by cap). Returns true if a PT_INTERP exists (i.e. the binary
// is dynamically linked and needs a runtime loader).
bool elf_interp_path(const uint8_t *file, int64_t len, char *out, uint32_t cap) {
    const uint64_t e_phoff = rd64(file + 32);
    const uint16_t e_phentsize = rd16(file + 54);
    const uint16_t e_phnum = rd16(file + 56);
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const uint8_t *ph = file + e_phoff + static_cast<uint64_t>(i) * e_phentsize;
        if (rd32(ph) != 3 /*PT_INTERP*/) {
            continue;
        }
        const uint64_t off = rd64(ph + 8);
        const uint64_t sz = rd64(ph + 32);
        uint32_t n = 0;
        for (; n + 1 < cap && n < sz && off + n < static_cast<uint64_t>(len); ++n) {
            out[n] = static_cast<char>(file[off + n]);
            if (out[n] == 0) break;
        }
        out[n] = 0;
        return true;
    }
    return false;
}

// Takes ownership of `file` on success (assigned to e->linux_elf_image,
// freed on the next exec/exit). Returns false without freeing on failure
// so the caller can clean up.
bool load_linux_elf_into_slot(Esper *e, uint8_t *file, int64_t len) {
    namespace district = imaginary_number_district;
    if (e == nullptr || file == nullptr || len < 64) {
        return false;
    }
    const uint16_t e_type = rd16(file + 16);
    const uint64_t e_entry_raw = rd64(file + 24);
    const uint64_t e_phoff = rd64(file + 32);
    const uint16_t e_phentsize = rd16(file + 54);
    const uint16_t e_phnum = rd16(file + 56);

    // ET_DYN (PIE) segment vaddrs are relative to a load base we choose;
    // ET_EXEC vaddrs are absolute and must be honoured as-is. So the base
    // offset is kLinuxElfBase for PIE and 0 for a fixed-address executable.
    const uint64_t load_base = (e_type == 2 /*ET_EXEC*/) ? 0 : kLinuxElfBase;

    // Free any stale interpreter image from a previous exec on this slot (the
    // old main image is freed just before we reassign it, lower down). MUST
    // go through image_unref -- a forked sibling may still hold a reference
    // to the same buffer (linux_clone copies the pointer and image_ref's it).
    // The legacy direct dark_matter_free here was a slow leak / double-free
    // generator: each fork+exec freed the shared interp under the parent,
    // leaving the parent with a dangling pointer that turned into a real
    // double-free on the parent's next exec / exit. Manifested as "sh: out
    // of memory" after ~20-30 unames as the heap freelist corrupted.
    if (e->linux_interp_image != nullptr) {
        if (image_unref(e->linux_interp_image)) {
            dark_matter_free(e->linux_interp_image);
        }
        e->linux_interp_image = nullptr;
    }
    // Tear down a prior Linux address space on this slot (exec replacing a Linux
    // program): reality_unref drops this Esper's reference and frees the space if
    // it was the last sharer. No-op for an Index legacy-pool Esper (null mm; the
    // Komoe-fork-then-exec case).
    reality_unref(e);

    // A fresh, independent address space (PersonalReality) for the new program.
    e->mm = reality_alloc();
    if (e->mm == nullptr) {
        district::writeln("exec: address-space pool exhausted.");
        return false;
    }
    if (!pr2_create_addr_space(e)) {
        district::writeln("exec: pr2_create_addr_space failed.");
        reality_unref(e);
        return false;
    }
    // (reality_alloc already set refs=1 -- this process owns its new space.)

    // Map the main program's PT_LOADs.
    uint64_t max_seg_end = 0;
    uint64_t phdr_va = 0;
    if (!map_elf_loads(e, file, static_cast<uint64_t>(len), load_base, &max_seg_end, &phdr_va)) {
        return false;
    }

    // Dynamically linked? If the binary names a PT_INTERP, load that
    // interpreter (the musl dynamic linker) and start execution there instead
    // of at the program's own entry. The interpreter self-relocates, wires up
    // the program via its phdrs (AT_PHDR), then jumps to AT_ENTRY.
    uint64_t start_pc = load_base + e_entry_raw;
    uint64_t interp_base = 0;
    char interp_path[64];
    if (elf_interp_path(file, len, interp_path, sizeof(interp_path))) {
        // Read the dynamic linker from its real PT_INTERP path (e.g.
        // /lib/ld-musl-aarch64.so.1 -- ext2 resolves it). Fall back to the
        // flat-FS name LD-MUSL.SO so the legacy FAT image still works.
        auto *itmp = static_cast<uint8_t *>(dark_matter_alloc(kElfReadCap));
        if (itmp == nullptr) {
            district::writeln("exec: out of heap for interpreter.");
            return false;
        }
        int64_t ilen = read_named_file(interp_path, reinterpret_cast<char *>(itmp), kElfReadCap);
        if (ilen < 0) {
            ilen = read_named_file("LD-MUSL.SO", reinterpret_cast<char *>(itmp), kElfReadCap);
        }
        if (ilen < 0) {
            district::write("exec: interpreter not found (");
            district::write(interp_path);
            district::writeln(").");
            dark_matter_free(itmp);
            return false;
        }
        // Shrink to the interpreter's real size before it becomes resident.
        uint8_t *ibuf = static_cast<uint8_t *>(dark_matter_alloc(static_cast<uint64_t>(ilen)));
        if (ibuf != nullptr) {
            for (int64_t b = 0; b < ilen; ++b) ibuf[b] = itmp[b];
            dark_matter_free(itmp);
        } else {
            ibuf = itmp;
        }
        interp_base = kLinuxInterpBase;
        uint64_t interp_max = 0;
        if (!map_elf_loads(e, ibuf, static_cast<uint64_t>(ilen), interp_base, &interp_max, nullptr)) {
            dark_matter_free(ibuf);
            return false;
        }
        if (e->linux_interp_image != nullptr) {
            dark_matter_free(e->linux_interp_image);
        }
        e->linux_interp_image = ibuf; // keep resident for file-backed faults
        image_ref(ibuf);
        start_pc = interp_base + rd64(ibuf + 24); // interpreter's e_entry
    }

    if (!pr2_add_vma(e, kLinuxStackTop - kLinuxStackSize, kLinuxStackTop,
                     kVmaProtR | kVmaProtW, static_cast<uint8_t>(VmaKind::Stack),
                     nullptr, 0, 0, 0)) {
        district::writeln("exec: stack VMA failed.");
        return false;
    }

    // Heap: a Brk VMA just past the program. brk() moves brk_cur within it;
    // pages fault in (zero) on demand, so reserving 64 MiB is free until used.
    const uint64_t brk_base = (max_seg_end + 0xFFF) & ~uint64_t(0xFFF);
    if (!pr2_add_vma(e, brk_base, brk_base + kLinuxBrkMax, kVmaProtR | kVmaProtW,
                     static_cast<uint8_t>(VmaKind::Brk), nullptr, 0, 0, 0)) {
        district::writeln("exec: brk VMA failed.");
        return false;
    }
    e->mm->brk_start = brk_base;
    e->mm->brk_cur = brk_base;
    e->mm->mmap_next = kLinuxMmapBase;

    // Replace any prior image (a previous exec on this slot) and take
    // ownership of `file`. image_unref so a forked sibling sharing the old
    // image isn't left dangling.
    if (e->linux_elf_image != nullptr) {
        if (image_unref(e->linux_elf_image)) {
            dark_matter_free(e->linux_elf_image);
        }
    }
    e->linux_elf_image = file;
    image_ref(file);
    e->linux_elf_image_size = static_cast<uint64_t>(len);
    e->entry = start_pc; // interpreter entry if dynamic, else program entry
    e->stack_top = kLinuxStackTop;
    e->started = false;
    e->abi = Abi::Linux;
    // ELF metadata for the aux vector. AT_PHDR/PHENT/PHNUM and AT_ENTRY always
    // describe the *main program* (so the interpreter can find and run it);
    // AT_BASE is the interpreter's load base (0 if static).
    e->elf_phdr_va = phdr_va != 0 ? phdr_va : (load_base + e_phoff);
    e->elf_phnum = e_phnum;
    e->elf_phentsize = e_phentsize;
    e->elf_entry = load_base + e_entry_raw;
    e->elf_base = interp_base;

    // Build the SysV startup stack (argc/argv/envp/auxv) and use its SP as the
    // initial SP_EL0. linux_build_startup_stack faults in the stack pages it
    // writes, so the prefault below only needs to cover anything it didn't.
    const uint64_t sp = linux_build_startup_stack(e);
    if (sp != 0) {
        e->stack_top = sp;
    }

    // Pre-fault only the entry page and the top stack page so the very first
    // instruction fetch and the first stack touch don't have to round-trip
    // through the abort handler. Everything else faults in on demand.
    (void)pr2_prefault_range(e, e->entry, 4);
    (void)pr2_prefault_range(e, e->stack_top - 0x1000, 0x1000);

    district::write("  loaded ");
    district::write("[Index] ");
    district::write(" entry VA ");
    district::hex(e->entry);
    district::write(" stack top ");
    district::hex(e->stack_top);
    district::write("\n");
    return true;
}

// Resolve symlinks and normalize "."/".."/"//" in an absolute path so that
// readlink("/proc/self/exe") returns the *canonical real* path. musl resolves
// $ORIGIN in a RUNPATH from /proc/self/exe; OpenJDK's launcher is reached via
// the /usr/bin/java -> ../lib/jvm/.../bin/java symlink, and its RUNPATH
// ($ORIGIN/../lib/aarch64/jli) only finds libjli.so when $ORIGIN is the *real*
// bin dir. Mirrors Linux, where /proc/self/exe always names the resolved file.
static void canonicalize_exe_path(const char *name, char *out, uint32_t cap) {
    if (out == nullptr || cap == 0) return;
    out[0] = 0;
    if (name == nullptr || name[0] == 0) return;

    char cur[kCwdCap];
    uint32_t cn = 0;
    for (; name[cn] != 0 && cn + 1 < kCwdCap; ++cn) cur[cn] = name[cn];
    cur[cn] = 0;

    for (int iter = 0; iter < 16; ++iter) {
        // --- normalize cur: collapse '.', '..', and repeated '/' ---
        char norm[kCwdCap];
        uint32_t nl = 0;
        norm[nl++] = '/';
        const char *s = cur;
        while (*s == '/') ++s;
        while (*s != 0) {
            const char *comp = s;
            uint32_t clen = 0;
            while (s[clen] != 0 && s[clen] != '/') ++clen;
            s += clen;
            while (*s == '/') ++s;
            if (clen == 1 && comp[0] == '.') continue;
            if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
                while (nl > 1 && norm[nl - 1] != '/') --nl;
                if (nl > 1) --nl; // drop the separating '/'
                if (nl == 0) norm[nl++] = '/';
                continue;
            }
            if (nl > 1 && nl + 1 < kCwdCap) norm[nl++] = '/';
            for (uint32_t k = 0; k < clen && nl + 1 < kCwdCap; ++k) norm[nl++] = comp[k];
        }
        norm[nl] = 0;
        for (uint32_t k = 0; k <= nl; ++k) cur[k] = norm[k];

        // --- resolve one symlink level; stop when cur is not a symlink ---
        char tgt[kCwdCap];
        const int32_t n = lateran_readlink(cur, tgt, kCwdCap - 1);
        if (n <= 0) break; // not a symlink (or error) -> cur is the real path
        tgt[n] = 0;

        if (tgt[0] == '/') {
            uint32_t k = 0;
            for (; tgt[k] != 0 && k + 1 < kCwdCap; ++k) cur[k] = tgt[k];
            cur[k] = 0;
        } else {
            // relative target: dirname(cur) + '/' + tgt
            uint32_t slash = 0;
            for (uint32_t k = 0; cur[k] != 0; ++k) if (cur[k] == '/') slash = k;
            char tmp[kCwdCap];
            uint32_t tl = 0;
            for (uint32_t k = 0; k <= slash && tl + 1 < kCwdCap; ++k) tmp[tl++] = cur[k];
            for (uint32_t k = 0; tgt[k] != 0 && tl + 1 < kCwdCap; ++k) tmp[tl++] = tgt[k];
            tmp[tl] = 0;
            for (uint32_t k = 0; k <= tl; ++k) cur[k] = tmp[k];
        }
    }

    uint32_t k = 0;
    for (; cur[k] != 0 && k + 1 < cap; ++k) out[k] = cur[k];
    out[k] = 0;
}

// Returns false on any failure -- the ELF is validated into a temp buffer before
// the slot's address space is rebuilt, so a failed exec leaves the old image
// intact. Sets entry/stack/ttbr0/code_pages and started=false (fresh start).
bool load_elf_into_slot(uint32_t slot, const char *name) {
    namespace district = imaginary_number_district;
    Esper *e = esper_at(slot);
    auto *file = static_cast<uint8_t *>(dark_matter_alloc(kElfReadCap));
    auto *img = static_cast<uint8_t *>(dark_matter_alloc(32 * 1024));
    if (file == nullptr || img == nullptr) {
        district::writeln("exec: out of heap.");
        dark_matter_free(file);
        dark_matter_free(img);
        return false;
    }
    const int64_t len = read_named_file(name, reinterpret_cast<char *>(file), kElfReadCap);
    if (len < 0) {
        district::write("exec: no such file: ");
        district::writeln(name);
        dark_matter_free(file);
        dark_matter_free(img);
        return false;
    }
    // Record the full image path so readlink("/proc/self/exe") works (the musl
    // loader resolves $ORIGIN in a RUNPATH from it -- OpenJDK's launcher needs it).
    if (e != nullptr) {
        // Store the *canonical* image path (symlinks resolved + normalized), so
        // readlink("/proc/self/exe") yields the real binary and musl's $ORIGIN
        // resolves correctly even when invoked via a symlink like /usr/bin/java.
        canonicalize_exe_path(name, e->exe_path, kCwdCap);
    }
    // Sniff which ABI the ELF wants *before* building anything else. Linux
    // Espers go through pr2 (VMA + demand-paging) so they can use arbitrary
    // VAs; Index Espers stay on the legacy pool with zero behavioural change.
    const Abi abi = sniff_abi(file, len);

    if (abi == Abi::Linux) {
        dark_matter_free(img); // not used by the Linux path
        // Shrink the 1 MiB read buffer down to the ELF's real size before it
        // becomes the resident image (file-backed VMAs read from it for the
        // process's whole life). Keeping the full 1 MiB per process would
        // exhaust the heap after a few programs.
        uint8_t *image = static_cast<uint8_t *>(dark_matter_alloc(static_cast<uint64_t>(len)));
        if (image != nullptr) {
            for (int64_t b = 0; b < len; ++b) image[b] = file[b];
            dark_matter_free(file);
        } else {
            image = file; // fall back to the big buffer if the tight alloc fails
        }
        if (!load_linux_elf_into_slot(e, image, len)) {
            dark_matter_free(image);
            return false;
        }
        district::write("  loaded ");
        district::writeln(name);
        return true;
    }

    uint64_t span = 0;
    const uint64_t e_entry = build_image(file, len, img, 32 * 1024, &span);
    bool ok = e_entry != 0 || span != 0;
    PersonalRealityV1 pr;
    if (ok) {
        pr = personal_reality_build(slot, img, span);
        ok = pr.valid;
        if (!ok) {
            district::writeln("exec: could not build address space.");
        }
    }
    dark_matter_free(file);
    dark_matter_free(img);
    if (!ok) {
        return false;
    }

    e->entry = kUserCodeBase + e_entry;
    e->stack_top = kUserStackTop;
    e->ttbr0 = pr.ttbr0;
    e->code_pages = pr.code_pages;
    e->started = false;
    e->abi = abi;

    district::write("  loaded ");
    district::write(name);
    district::write(" at VA ");
    district::hex(kUserCodeBase);
    district::write(" -> PA ");
    district::hex(pr.code_phys);
    district::writeln(" (private)");
    return true;
}

// Save the running Esper's EL0 context out of the trap frame and system regs.
// [WD] A valid EL0 Linux user PC is a low user VA (< 1 GiB). If a saved/restored
// elr lands in the kernel/identity range (>=0x40000000, e.g. 0x40081318) or
// near-null (<0x10000, e.g. 0x11), the user context was corrupted -- report it
// AT THE SOURCE (save vs load) to localize whether user space jumped wild or the
// kernel overwrote the saved context between save and resume.
static inline bool dbg_in_ktext(uint64_t v) {
    return v >= 0x40080000ULL && v < 0x400b9000ULL; // kernel text (low/identity)
}
static inline void dbg_check_ctx(const char *tag, const Esper *e) {
    if (e == nullptr || e->abi != Abi::Linux || !e->started) return;
    // A Linux user context must never hold a kernel-text pointer. Flag elr or
    // any GP reg that landed in kernel text -- that value was corrupted into the
    // saved/restored user context. SAVE vs LOAD localizes user-space (the user
    // already had it at trap) vs kernel-overwrote-between-save-and-resume.
    int bad = -2;
    if (dbg_in_ktext(e->elr)) bad = -1;
    else for (int i = 0; i < 31; ++i) if (dbg_in_ktext(e->regs[i])) { bad = i; break; }
    if (bad == -2) return;
    namespace district = imaginary_number_district;
    district::write(tag);
    district::write(" pid="); district::dec(e->pid);
    district::write(" badreg="); district::dec(static_cast<uint64_t>(static_cast<uint32_t>(bad)));
    district::write(" elr="); district::hex(e->elr);
    district::write(" x30="); district::hex(e->regs[30]);
    district::write(" "); district::write(e->name);
    district::write("\n");
}

void save_ctx(Esper *e, const uint64_t *frame) {
    for (uint32_t i = 0; i < 31; ++i) {
        e->regs[i] = frame[i];
    }
    asm volatile("mrs %0, sp_el0" : "=r"(e->sp_el0));
    asm volatile("mrs %0, elr_el1" : "=r"(e->elr));
    asm volatile("mrs %0, spsr_el1" : "=r"(e->spsr));
    dbg_check_ctx("[CTXBAD-SAVE]", e);
    // Preserve a user-set TLS base: musl sets TPIDR_EL0 directly in user space
    // (no syscall), so we must capture it here or it would be lost when load_ctx
    // restores e->tpidr on the way back in.
    asm volatile("mrs %0, tpidr_el0" : "=r"(e->tpidr));
    // The kernel hasn't touched FP/SIMD since the trap, so the vector regs still
    // hold this Esper's values -- snapshot them alongside the GP context.
    fpsimd_save(e->fpsimd);
}

// A waiting parent woken by a child's exit cannot have its *status written at
// exit time (the parent's address space is not active then). Deliver it now,
// once `e`'s address space is the live TTBR0. Call right after switch_ttbr0.
void deliver_pending_status(Esper *e) {
    if (!e->has_pending_status) {
        return;
    }
    if (e->wait_status_ptr != 0) {
        // Use pr2_write_user (walks e's page table, breaks CoW, faults pages in)
        // instead of a raw deref. The raw write relied on the live TTBR0 already
        // being e's AND bypassed copy-on-write: if wait_status_ptr sat in a CoW
        // page shared with a just-forked child, the raw kernel write landed on
        // the shared physical page without triggering the CoW copy -> the other
        // sharer's view of that VA got the wait status, smashing whatever it held
        // (a pointer -> the SMP EL0 crash: FAR=0x11 near-null deref / a kernel
        // address in the user's PC). pr2_write_user is what signal-frame delivery
        // already uses for exactly this reason.
        if (e->wait_status_is_stop) {
            // ptrace-stop / job-control stop: WSTOPPED -> (stopsig << 8) | 0x7f.
            const int32_t st = static_cast<int32_t>(((e->pending_status & 0xff) << 8) | 0x7f);
            pr2_write_user(e, e->wait_status_ptr, &st, sizeof(st));
        } else if (e->wait_status_is_linux) {
            // Linux wait status: normal exit -> (code & 0xff) << 8, a 32-bit int.
            const int32_t st = static_cast<int32_t>((e->pending_status & 0xff) << 8);
            pr2_write_user(e, e->wait_status_ptr, &st, sizeof(st));
        } else {
            const int64_t st = e->pending_status;
            pr2_write_user(e, e->wait_status_ptr, &st, sizeof(st));
        }
    }
    e->has_pending_status = false;
    e->wait_status_ptr = 0;
    e->wait_status_is_linux = false;
    e->wait_status_is_stop = false;
}

// Forward decl so park_on_pipe can hand the CPU to the next Esper.
void load_ctx(Esper *e, uint64_t *frame);

// Flip any Espers parked on a pipe back to ready. The dispatcher rewound their
// ELR by 4 before parking them, so when they next get scheduled the svc fires
// again and SYS_read/SYS_write retry; if conditions still aren't right they
// just re-park. Symmetric helpers wake the side opposite to whoever just acted.
// Both grab g_esper_lock: the wake flips state from waiting -> ready, and a
// concurrent pick_and_claim on another CPU must see either the old state
// (skip) or the new state with predicates cleared (claim atomically).
void wake_pipe_readers(int pipe_idx) {
    if (pipe_idx < 0) {
        return;
    }
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        Esper *e = esper_at(i);
        if (e == nullptr || e->state != EsperState::waiting) continue;
        // Wake blocking readers of this pipe, AND any ppoll/pselect waiter (a
        // poller parks on PollWait, not wait_pipe_idx; the tickless idle tick
        // won't re-poll it, so the pipe write must kick it -- see ipc_wake_impl).
        if ((e->wait_pipe_idx == pipe_idx && !e->wait_pipe_is_write) ||
            e->ipc_wait_kind == Esper::IpcWaitKind::PollWait) {
            e->wait_pipe_idx = -1;
            e->ipc_wait_kind = Esper::IpcWaitKind::None;
            e->ipc_wait_id = -1;
            e->state = EsperState::ready;
        }
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    esper_kick_secondaries();
}

void wake_pipe_writers(int pipe_idx) {
    if (pipe_idx < 0) {
        return;
    }
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        Esper *e = esper_at(i);
        if (e == nullptr || e->state != EsperState::waiting) continue;
        if ((e->wait_pipe_idx == pipe_idx && e->wait_pipe_is_write) ||
            e->ipc_wait_kind == Esper::IpcWaitKind::PollWait) {
            e->wait_pipe_idx = -1;
            e->ipc_wait_kind = Esper::IpcWaitKind::None;
            e->ipc_wait_id = -1;
            e->state = EsperState::ready;
        }
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    esper_kick_secondaries();
}

// Release every pipe / socket ref this Esper holds (called from SYS_close,
// SYS_exit, the fault path, and dup2's overwrite). Decrementing a pipe ref
// might land the pipe in "no readers" or "no writers"; wake the opposite side
// so any blocked party re-runs its svc and observes broken-pipe / EOF.
// Antenna refs are dropped via antenna_close (which also frees the slot when
// the last reference goes).
void release_esper_fd(Esper *e, uint32_t fd) {
    if (fd >= kMaxFds) {
        return;
    }
    linux_release_fd_backend(e->fds[fd]);
    e->fds[fd] = Fd{};
}

} // namespace [anonymous] -- lift release_esper_pipes to external linkage so
  // Fortis931 (and any future kernel caller) can release a target's pipes.

void release_esper_pipes(Esper *e) {
    for (uint32_t i = 0; i < kMaxFds; ++i) {
        release_esper_fd(e, i);
    }
}

// Take/drop one backend reference for a standalone Fd value. The dispatch
// mirrors what linux_dup_fd (ref) and release_esper_fd (drop) inline for the
// fd-table case; SCM_RIGHTS reuses them so a passed fd keeps its backend alive
// across the send->receive gap. Drop also wakes pipe peers, matching close().
void linux_ref_fd_backend(const Fd &f) {
    switch (f.kind) {
    case FdKind::pipe_read:  aiwass_inc_read(f.pipe_idx); break;
    case FdKind::pipe_write: aiwass_inc_write(f.pipe_idx); break;
    case FdKind::socket:     antenna_inc_ref(f.sock_idx); break;
    case FdKind::unix_sock:  inc_inc_ref(f.sock_idx); break;
    case FdKind::eventfd:    eventfd_inc_ref(f.sock_idx); break;
    case FdKind::epoll:      epoll_inc_ref(f.sock_idx); break;
    case FdKind::timerfd:    timerfd_inc_ref(f.sock_idx); break;
    case FdKind::signalfd:   signalfd_inc_ref(f.sock_idx); break;
    case FdKind::inotify:    inotify_inc_ref(f.sock_idx); break;
    case FdKind::pty_master: sr_master_inc_ref(f.sock_idx); break;
    case FdKind::pty_slave:  sr_slave_inc_ref(f.sock_idx); break;
    default: break; // closed/console/file/dev* hold no shared backend
    }
}

void linux_release_fd_backend(const Fd &f) {
    switch (f.kind) {
    case FdKind::pipe_read:
        aiwass_close_read(f.pipe_idx);
        wake_pipe_writers(f.pipe_idx);
        break;
    case FdKind::pipe_write:
        aiwass_close_write(f.pipe_idx);
        wake_pipe_readers(f.pipe_idx);
        break;
    case FdKind::socket:     antenna_close(f.sock_idx); break;
    case FdKind::unix_sock:  inc_close(f.sock_idx); break;
    case FdKind::eventfd:    eventfd_close(f.sock_idx); break;
    case FdKind::epoll:      epoll_close(f.sock_idx); break;
    case FdKind::timerfd:    timerfd_close(f.sock_idx); break;
    case FdKind::signalfd:   signalfd_close(f.sock_idx); break;
    case FdKind::inotify:    inotify_close(f.sock_idx); break;
    case FdKind::pty_master: sr_close_master(f.sock_idx); break;
    case FdKind::pty_slave:  sr_close_slave(f.sock_idx); break;
    default: break;
    }
}

int linux_install_fd(Esper *e, const Fd &f) {
    if (e == nullptr) return -14; // -EFAULT
    for (uint32_t i = 0; i < kMaxFds; ++i) {
        if (e->fds[i].kind == FdKind::closed) {
            e->fds[i] = f; // ownership transfers; no extra ref taken
            return static_cast<int>(i);
        }
    }
    return -24; // -EMFILE
}

// ---- Linux pipe2/dup/dup3 / nanosleep / execve helpers --------------------
// These live here because they touch the scheduler (save_ctx/pick_ready/
// load_ctx, release_esper_fd) and the Aiwass refcount path that the Index
// kSysPipe/kSysDup/kSysDup2 implementations already use.

int linux_pipe2(Esper *e, int *out_fds, uint64_t /*flags*/) {
    if (e == nullptr || out_fds == nullptr) {
        return -14; // -EFAULT
    }
    int rfd = -1, wfd = -1;
    for (uint32_t i = 3; i < kMaxFds; ++i) {
        if (e->fds[i].kind == FdKind::closed) {
            if (rfd < 0) {
                rfd = static_cast<int>(i);
            } else {
                wfd = static_cast<int>(i);
                break;
            }
        }
    }
    if (rfd < 0 || wfd < 0) {
        return -24; // -EMFILE
    }
    const int pi = aiwass_create();
    if (pi < 0) {
        return -12; // -ENOMEM
    }
    e->fds[rfd] = Fd{};
    e->fds[rfd].kind = FdKind::pipe_read;
    e->fds[rfd].pipe_idx = pi;
    e->fds[wfd] = Fd{};
    e->fds[wfd].kind = FdKind::pipe_write;
    e->fds[wfd].pipe_idx = pi;
    out_fds[0] = rfd;
    out_fds[1] = wfd;
    return 0;
}

int linux_dup_fd(Esper *e, int oldfd) {
    if (e == nullptr || oldfd < 0 || static_cast<uint32_t>(oldfd) >= kMaxFds ||
        e->fds[oldfd].kind == FdKind::closed) {
        return -9; // -EBADF
    }
    for (uint32_t i = 0; i < kMaxFds; ++i) {
        if (e->fds[i].kind == FdKind::closed) {
            e->fds[i] = e->fds[oldfd];
            linux_ref_fd_backend(e->fds[i]);
            return static_cast<int>(i);
        }
    }
    return -24; // -EMFILE
}

int linux_dup3_fd(Esper *e, int oldfd, int newfd, uint64_t /*flags*/) {
    if (e == nullptr || oldfd < 0 || newfd < 0 ||
        static_cast<uint32_t>(oldfd) >= kMaxFds ||
        static_cast<uint32_t>(newfd) >= kMaxFds ||
        e->fds[oldfd].kind == FdKind::closed) {
        return -9; // -EBADF
    }
    if (oldfd == newfd) {
        return newfd;
    }
    release_esper_fd(e, static_cast<uint32_t>(newfd));
    e->fds[newfd] = e->fds[oldfd];
    linux_ref_fd_backend(e->fds[newfd]);
    return newfd;
}

// rt_sigsuspend(mask_ptr, mask_size): atomically replace sig_mask with the
// user-supplied mask, park until a signal whose handler isn't SIG_DFL/SIG_IGN
// is delivered (the handler runs first), then return -EINTR with the
// caller's original mask restored. linux_deliver_signal_locked is the wake-
// side counterpart: it spots wait_sigsuspend, restores sigsuspend_saved_mask,
// flips state to ready, and proceeds to deliver the handler unconditionally.
void linux_rt_sigsuspend(int idx, uint64_t *frame) {
    Esper *me = esper_at(static_cast<uint32_t>(idx));
    if (me == nullptr) {
        frame[0] = static_cast<uint64_t>(-22); // -EINVAL
        return;
    }
    const uint64_t mask_ptr = frame[0];
    const uint64_t mask_size = frame[1];
    uint64_t new_mask = 0;
    if (mask_size >= 8 && mask_ptr != 0) {
        // me is currently running on this CPU; its TTBR0 is the live address
        // space, so the user-VA dereference is safe (matches how the rest of
        // the syscall layer reads small user-mode buffers).
        new_mask = *reinterpret_cast<const uint64_t *>(mask_ptr);
    }
    // SIGKILL/SIGSTOP must never be blocked.
    constexpr uint64_t kSigKillBit = 1ULL << (9 - 1);
    constexpr uint64_t kSigStopBit = 1ULL << (19 - 1);
    new_mask &= ~(kSigKillBit | kSigStopBit);

    save_ctx(me, frame);
    me->regs[0] = static_cast<uint64_t>(-4); // -EINTR (return value on wake)

    const uint64_t lock_flags = anti_skill_lock_irqsave(g_esper_lock);
    me->sigsuspend_saved_mask = me->sig_mask;
    me->sig_mask = new_mask;
    me->wait_sigsuspend = true;

    // Fast path: a signal is already deliverable under the new mask. Restore
    // immediately so the drain in esper_preempt (or a future call) sees the
    // pre-sigsuspend mask and a wake-up flag cleared.
    if ((me->sig_pending & ~new_mask) != 0) {
        // A signal is deliverable under the sigsuspend (new) mask. The awaited
        // signal's HANDLER must RUN, then sigsuspend returns -EINTR. Critical:
        // keep wait_sigsuspend TRUE and compute `deliverable` against new_mask --
        // do NOT restore saved_mask first. linux_deliver_signal's
        // waking_sigsuspend path restores saved_mask, SKIPS the mask check (so the
        // awaited signal runs instead of being re-queued under the restored mask),
        // and snapshots saved_mask into the sigframe for rt_sigreturn. The earlier
        // version restored saved_mask + cleared wait_sigsuspend FIRST, so (a) the
        // deliverable scan used saved_mask (SIGCHLD blocked -> nothing to deliver)
        // and (b) delivery saw wait_sigsuspend=false and re-queued SIGCHLD under
        // the blocked mask without running the handler -> ash's `wait`
        // sigsuspend-loops forever (its SIGCHLD handler never sets got-sigchld, so
        // it never reaps + returns). Also set the LIVE frame[0]=-EINTR (the fast
        // path doesn't load_ctx, so the eret uses the live frame; setting only
        // me->regs[0] left sigsuspend returning its mask_ptr arg instead).
        const uint64_t deliverable = me->sig_pending & ~new_mask;
        const int sig = __builtin_ctzll(deliverable) + 1;
        anti_skill_unlock_irqrestore(g_esper_lock, lock_flags);
        frame[0] = static_cast<uint64_t>(-4); // -EINTR (saved into the sigframe)
        // wait_sigsuspend is still true -> deliver via the waking_sigsuspend path.
        linux_deliver_signal(me, sig, frame); // runs handler, or SIG_DFL/IGN: just
                                              // restores mask + clears the wait
        me->sig_pending &= ~(1ULL << (sig - 1));
        return;
    }

    const int next = esper_park_and_pick_locked(idx);
    anti_skill_unlock_irqrestore(g_esper_lock, lock_flags);
    if (next < 0) {
        // No one else runnable. leave_user back to the scheduler loop; a
        // future linux_deliver_signal_locked will flip us back to ready and
        // the next pick will resume us.
        leave_user();
        __builtin_unreachable();
    }
    load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
}

void linux_nanosleep_park(int idx, uint64_t deadline_cntpct, uint64_t *frame) {
    Esper *me = esper_at(static_cast<uint32_t>(idx));
    if (me == nullptr) {
        frame[0] = 0;
        return;
    }
    save_ctx(me, frame);
    me->regs[0] = 0;             // nanosleep returns 0 on full completion
    me->wake_cntpct = deadline_cntpct;
    const int next = esper_park_and_pick(idx);
    if (next < 0) {
        // No other Esper to run: leave_user back to run_espers's loop so it
        // can WFI. Every pick path wakes due nanosleepers (wake_cntpct <= now)
        // before scanning ready, so the next timer-driven pick will resume us.
        leave_user();
        __builtin_unreachable();
    }
    load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
}

bool linux_execve_replace(int idx, const char *path, const char *const *user_argv,
                          uint32_t argc, const char *const *user_envp, uint32_t envc,
                          uint64_t *frame) {
    Esper *e = esper_at(static_cast<uint32_t>(idx));
    if (e == nullptr) return false;
    // Copy argv/envp strings into a kernel buffer because load_elf_into_slot
    // rebuilds the user address space (the user-side strings would disappear).
    // 32 KiB: the OpenJDK launcher carries a long LD_LIBRARY_PATH + classpath +
    // many env vars across its self-re-exec; 4 KiB truncated them.
    constexpr uint64_t kStageSize = 32768;
    char *stage = static_cast<char *>(dark_matter_alloc(kStageSize));
    if (stage == nullptr) return false;
    uint32_t pos = 0;
    auto stage_str = [&](const char *src, const char **dst) -> bool {
        if (src == nullptr) return false;
        char *start = stage + pos;
        uint64_t mapped = ~0ULL;
        while (pos + 1 < kStageSize) {
            const uint64_t a = reinterpret_cast<uint64_t>(src);
            // A user string (< the 39-bit ceiling) is prefaulted one page at a
            // time so a string running into an unmapped page fails cleanly rather
            // than faulting the kernel at EL1 (#5 class, like resolve_at's
            // copy_user_cstr). A kernel string (>= ceiling, always mapped) is read
            // directly -- run_elf_argv passes kernel argv for internal launches.
            if (a < 0x8000000000ULL) {
                const uint64_t pg = a & ~uint64_t(0xFFF);
                if (pg != mapped) {
                    if (!pr2_prefault_range(e, pg, 1)) return false;
                    mapped = pg;
                }
            }
            const char c = *src++;
            stage[pos++] = c;
            if (c == 0) {
                *dst = start;
                return true;
            }
        }
        return false; // string longer than the staging buffer
    };
    e->exec_argc = 0;
    e->exec_envc = 0;
    for (uint32_t i = 0; i < argc && i < kExecArgvCap; ++i) {
        if (!stage_str(user_argv[i], &e->exec_argv[i])) {
            dark_matter_free(stage);
            e->exec_argc = 0;
            return false;
        }
        ++e->exec_argc;
    }
    for (uint32_t i = 0; i < envc && i < kExecEnvpCap; ++i) {
        if (!stage_str(user_envp[i], &e->exec_envp[i])) {
            dark_matter_free(stage);
            e->exec_argc = 0;
            e->exec_envc = 0;
            return false;
        }
        ++e->exec_envc;
    }
    if (!load_elf_into_slot(static_cast<uint32_t>(idx), path)) {
        dark_matter_free(stage);
        e->exec_argc = 0;
        e->exec_envc = 0;
        return false;
    }
    // linux_build_startup_stack consumed exec_argv/exec_envp (cleared counts),
    // so the staging strings are no longer needed.
    dark_matter_free(stage);
    // POSIX close-on-exec: now that exec has succeeded, drop every fd marked
    // O_CLOEXEC/FD_CLOEXEC. sshd marks its listener/monitor/pty fds CLOEXEC;
    // without this they leak across the re-exec chain, pinning the pty
    // SisterRelay's refcount so its master read never EOFs and the session
    // never tears down -- which hung the ssh client on logout.
    for (uint32_t i = 0; i < kMaxFds; ++i) {
        if (e->fds[i].kind != FdKind::closed && e->fds[i].cloexec) {
            release_esper_fd(e, i);
        }
    }
    // execve resets CAUGHT signal handlers to SIG_DFL (Linux semantics): the new
    // image must NOT inherit the old program's handler addresses, which now point
    // into the replaced image. This was the worker-shutdown fault: java -version's
    // worker thread inherited sh's SIGCHLD handler 0x44a654 -- an address in sh's
    // code that became java brk heap after exec -- and on signal delivery the
    // kernel set ELR=that handler, so the worker took an instruction-abort blr'ing
    // into non-exec brk (JVMRC=139). SIG_DFL(0)/SIG_IGN(1) are preserved; only
    // caught handlers (>1) reset. (Mirrors Linux flush_signal_handlers on exec.)
    for (int s = 0; s < 64; ++s) {
        if (e->sig_handler[s] > 1) {
            e->sig_handler[s] = 0; // -> SIG_DFL
            e->sig_restorer[s] = 0;
            e->sig_flags[s] = 0;
            e->sig_act_mask[s] = 0;
        }
    }
    // CLONE_VFORK: the child has now replaced its address space (load_elf_into_
    // slot gave it a fresh ttbr0), so the parent can safely resume on the old
    // shared stack. Release the vfork-suspended parent (keyed by our pid).
    linux_ipc_wake(Esper::IpcWaitKind::VforkDone, static_cast<int>(e->pid));
    load_ctx(e, frame);
    return true;
}

// --- file helpers shared with the Linux ABI (declared in usermode.hpp) -----

int linux_file_open(Esper *e, const char *path) {
    if (e == nullptr) {
        return -1;
    }
    // Existence probe is cheap (inode read or Bookshelf/Grimoire lookup); the
    // earlier dark_matter_alloc(kElfReadCap)+read_whole_file was catastrophic
    // because dlopen mmaps several 4 MiB buffers up front and the heap then
    // can't satisfy this probe -- second-and-later opens spuriously ENOENT.
    if (linux_file_size(path) < 0) {
        return -1;
    }
    for (uint32_t i = 3; i < kMaxFds; ++i) {
        if (e->fds[i].kind == FdKind::closed) {
            e->fds[i] = Fd{};
            e->fds[i].kind = FdKind::file;
            copy_str(e->fds[i].path, path, sizeof(e->fds[i].path));
            e->fds[i].off = 0;
            return static_cast<int>(i);
        }
    }
    return -1; // fd table full
}

// linux_file_mmap_buf removed: file-backed mmap is now demand-paged. The fault
// handler reads each page from the filesystem via lateran_pread (see
// pr2_handle_fault + mmap case 222) -- no whole-file kernel buffer, no 8-cap.

int linux_file_open_ex(Esper *e, const char *path, bool create, bool trunc, bool writable) {
    if (e == nullptr) {
        return -1;
    }
    const bool exists = (linux_file_size(path) >= 0);
    if (!exists) {
        if (!create) return -1;        // O_CREAT not set -> ENOENT
        lateran_write_file(path, "", 0); // create an empty file on the FAT disk
    } else if (trunc && writable) {
        lateran_write_file(path, "", 0); // O_TRUNC: empty the existing file
    }
    const int fd = linux_file_open(e, path); // assigns an fd (also re-probes)
    if (fd >= 0) {
        e->fds[fd].writable = writable;
    }
    return fd;
}

int64_t linux_file_write(Esper *e, uint32_t fd, const char *buf, uint64_t len) {
    if (e == nullptr || fd >= kMaxFds || e->fds[fd].kind != FdKind::file ||
        !e->fds[fd].writable) {
        return -1;
    }
    // Read-modify-write the whole (small) file: load current content, splice the
    // new bytes at the fd's offset, write it all back to the FAT disk.
    auto *tmp = static_cast<char *>(dark_matter_alloc(kElfReadCap));
    if (tmp == nullptr) return -1;
    int64_t cur = read_named_file(e->fds[fd].path, tmp, kElfReadCap);
    if (cur < 0) cur = 0;
    const uint64_t off = e->fds[fd].off;
    if (off > kElfReadCap) { dark_matter_free(tmp); return -1; }
    if (off + len > kElfReadCap) len = kElfReadCap - off; // clamp to buffer
    for (uint64_t i = static_cast<uint64_t>(cur); i < off; ++i) tmp[i] = 0; // gap fill
    for (uint64_t i = 0; i < len; ++i) tmp[off + i] = buf[i];
    uint64_t newsize = off + len;
    if (static_cast<uint64_t>(cur) > newsize) newsize = static_cast<uint64_t>(cur);
    const int64_t w = lateran_write_file(e->fds[fd].path, tmp, static_cast<uint32_t>(newsize));
    dark_matter_free(tmp);
    if (w < 0) return -1;
    e->fds[fd].off += len;
    return static_cast<int64_t>(len);
}

int64_t linux_file_read(Esper *e, uint32_t fd, char *buf, uint64_t cap) {
    if (e == nullptr || fd >= kMaxFds || e->fds[fd].kind != FdKind::file) {
        return -1;
    }
    return static_cast<int64_t>(read_file_fd(e, fd, buf, cap));
}

int64_t linux_file_size(const char *path) {
    // Cheap path: stat the inode directly, no buffer allocation. Earlier
    // versions used to allocate kElfReadCap and read the whole file just to
    // count bytes -- which failed silently when the heap fragmented under
    // 4 MiB requests, causing fstat to report size 0 for large files (which
    // in turn broke dlopen / fseek SEEK_END for libstdc++.so.6 etc.).
    // Under g_fs_lock: lateran_stat walks ext2 over the shared virtio-blk
    // device; without serialisation a concurrent exec's read races it and
    // stat spuriously fails -> busybox reports "not found" on a core whose
    // pre-exec stat lost the race.
    const uint64_t flags = anti_skill_lock_irqsave(g_fs_lock);
    LateranEntry entry;
    if (lateran_stat(path, &entry)) {
        const int64_t sz = static_cast<int64_t>(entry.size);
        anti_skill_unlock_irqrestore(g_fs_lock, flags);
        return sz;
    }
    anti_skill_unlock_irqrestore(g_fs_lock, flags);
    // Bookshelf fallback (image-baked files: smaller, read-only).
    const BookshelfFile *bf = bookshelf_find(path);
    if (bf != nullptr) return static_cast<int64_t>(bf->size);
    return -1;
}

bool linux_fd_is_file(Esper *e, uint32_t fd) {
    return e != nullptr && fd < kMaxFds && e->fds[fd].kind == FdKind::file;
}

void linux_fd_seek(Esper *e, uint32_t fd, uint64_t off) {
    if (e != nullptr && fd < kMaxFds && e->fds[fd].kind == FdKind::file) {
        e->fds[fd].off = off;
    }
}

uint64_t linux_fd_tell(Esper *e, uint32_t fd) {
    return (e != nullptr && fd < kMaxFds) ? e->fds[fd].off : 0;
}

int64_t linux_fd_size(Esper *e, uint32_t fd) {
    if (e == nullptr || fd >= kMaxFds || e->fds[fd].kind != FdKind::file) {
        return -1;
    }
    return linux_file_size(e->fds[fd].path);
}

const char *linux_fd_path(Esper *e, uint32_t fd) {
    if (e == nullptr || fd >= kMaxFds || e->fds[fd].kind != FdKind::file) {
        return "";
    }
    return e->fds[fd].path;
}

int linux_dir_open(Esper *e, const char *path) {
    if (e == nullptr) return -1;
    for (uint32_t i = 3; i < kMaxFds; ++i) {
        if (e->fds[i].kind == FdKind::closed) {
            e->fds[i] = Fd{};
            e->fds[i].kind = FdKind::file; // a directory fd: path set, off = cursor
            copy_str(e->fds[i].path, path, sizeof(e->fds[i].path));
            e->fds[i].off = 0;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void linux_fd_close(Esper *e, uint32_t fd) {
    if (e == nullptr || fd >= kMaxFds) return;
    // close() on the standard fds (0/1/2): a blanket `fd >= 3` no-op (the old
    // behaviour) keeps getty/login's console wired -- they rely on the console
    // staying pinned at 0/1/2 across their close()/dup2() dance. But it also
    // silently dropped OpenSSH's monitor close()ing a pty *slave* that openpty
    // placed at fd 0, pinning the pty so its master never EOF'd -> the ssh
    // client hung on logout. Compromise: keep only console/tty kinds pinned at
    // the std fds; let every other fd kind (pty, file, socket, pipe) close
    // normally there. The ssh session child's 0/1/2 are never the console.
    if (fd < 3 && (e->fds[fd].kind == FdKind::console ||
                   e->fds[fd].kind == FdKind::devtty)) {
        return;
    }
    release_esper_fd(e, fd);
}

// Free a Linux Esper's retained ELF image buffer (file-backed VMAs reference
// it) once the process is gone. The VMA list and page tables themselves live
// in TreeDiagram pages, which the bump allocator never reclaims -- acceptable
// for now (a handful of short-lived Linux processes); a real free-list for
// user page tables is Phase F. Called from the exit + fault paths.
// Active-entry counts for /proc/index_resources leak monitoring. External
// linkage (defined outside the anonymous namespace) so procfs.cpp can call
// them; they read the anon-namespace g_as_refs / g_image_refs tables which
// are visible within this translation unit.
uint32_t as_ref_active_count() {
    return reality_active_count(); // address-space count now lives in the reality pool
}
uint32_t image_ref_active_count() {
    uint32_t n = 0;
    for (auto &r : g_image_refs) if (r.img != nullptr) ++n;
    return n;
}

void release_linux_image(Esper *e) {
    if (e == nullptr) {
        return;
    }
    // Reclaim the address space (page tables + leaf pages) -- but only if this
    // is the last sharer (threads keep it alive for siblings). Safe to free the
    // live TTBR0 here: we're in a syscall with IRQs masked, the kernel runs on
    // TTBR1, and the caller switches TTBR0 (with a TLBI) before any user access.
    reality_unref(e);
    if (e->linux_elf_image != nullptr) {
        if (image_unref(e->linux_elf_image)) {
            dark_matter_free(e->linux_elf_image);
        }
        e->linux_elf_image = nullptr;
        e->linux_elf_image_size = 0;
    }
    if (e->linux_interp_image != nullptr) {
        if (image_unref(e->linux_interp_image)) {
            dark_matter_free(e->linux_interp_image);
        }
        e->linux_interp_image = nullptr;
    }
    // (file-backed mmap is demand-paged now -- no per-Esper mmap buffers to free;
    // the VMA list belongs to the now-unref'd PersonalReality `mm`.)
}

// Tear the running Esper down with the given exit code and hand the CPU to
// the next ready Esper. Shared by Index SYS_exit, Linux exit/exit_group, and
// any future ABI's exit path. Releases pipe refs (so peers see EOF/EPIPE),
// transitions state -> exited, wakes a wait()-blocked parent if any, and
// schedules the next Esper via load_ctx. Calls leave_user() if no Esper is
// runnable (the EL0 batch is done).
void exit_and_schedule(int idx, int64_t code, uint64_t *frame) {
    Esper *me = esper_at(static_cast<uint32_t>(idx));
    if (me != nullptr) {
        // If a vfork() parent is suspended on us (we exited without exec),
        // release it now (CLONE_VFORK contract: child exec OR exit wakes it).
        linux_ipc_wake(Esper::IpcWaitKind::VforkDone, static_cast<int>(me->pid));
    }
    if (me != nullptr) {
        // me is still state=running on this CPU here, so no other CPU is
        // touching its pipes/image. Cleanup is safe outside the scheduler
        // lock; the atomic state transition (running -> exited, optional
        // parent wake, pick + claim next) happens in esper_exit_and_pick.
        release_esper_pipes(me); // drop pipe refs; readers see EOF, writers see EPIPE
        release_linux_image(me); // free retained ELF buffer (if Linux ABI)
    }
    const int next = esper_exit_and_pick(idx, code);
    if (next < 0) {
        leave_user();
    }
    load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
}

namespace { // resume helpers

// Park the running Esper on a pipe and switch to the next ready Esper. Caller
// is in a sync (svc) trap; the user buf/len/fd live in `frame`, so rewinding
// ELR by 4 makes the eret re-execute the same svc with the same args. Returns
// after handing the CPU to `next`; on next schedule of this Esper the svc
// fires again and the read/write retries.
// Park the Esper until `pipe_idx` is readable/writable. Re-checks readiness
// under g_esper_lock first (closing the lost-wakeup race against a peer that
// filled/drained + woke on another CPU between the aiwass_* probe and this
// park -- the same disease as the channel path). Returns true if it parked
// (caller returns kFdParkedSentinel); false if the pipe became ready in the
// window (caller retries the op). If nobody else is runnable it WFIs in the
// scheduler loop until a peer wakes it; it must NOT fail the syscall, because
// under SMP the peer usually exists but is transiently not-ready -- the old
// next<0 -> frame[0]=-1 here surfaced as -EPERM and was the OpenSSH privsep
// monitor log-pipe ("log fd read: Operation not permitted") failure on smp>1.
bool park_on_pipe(int idx, int pipe_idx, bool is_write, uint64_t *frame) {
    Esper *e = esper_at(idx);
    if (e == nullptr) return false;
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    const bool ready = is_write ? aiwass_writable(pipe_idx)
                                : aiwass_readable(pipe_idx);
    if (ready) {
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        return false; // delivered in the window -- don't sleep, retry the op
    }
    save_ctx(e, frame);
    e->elr -= 4; // svc re-execution on resume
    e->wait_pipe_idx = pipe_idx;
    e->wait_pipe_is_write = is_write;
    const int next = esper_park_and_pick_locked(idx);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    if (next < 0) {
        leave_user(); // peer on another CPU will wake us; WFI in the meantime
        __builtin_unreachable();
    }
    load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
    return true;
}

// Generic in-kernel IPC park (anon-namespace impl; lifted to external linkage
// after the namespace closes). Atomic state=waiting + pick_next under
// g_esper_lock via esper_park_and_pick.
int ipc_park_impl(int idx, Esper::IpcWaitKind kind, int id, uint64_t *frame) {
    Esper *e = esper_at(idx);
    if (e == nullptr) return -1;
    save_ctx(e, frame);
    e->elr -= 4; // svc re-execution on resume
    e->ipc_wait_kind = kind;
    e->ipc_wait_id = id;
    const int next = esper_park_and_pick(idx);
    if (next < 0) {
        // Nobody else runnable. Don't spin -- jump back to run_espers's clean
        // kernel context (the SP saved in g_user_kctx by enter_user/resume_user)
        // so it can WFI in its loop. The in-flight svc trap frame + C call
        // chain (el0_sync_dispatch -> linux_dispatch -> fd_read_dispatch ->
        // ...read handler... -> linux_ipc_park -> here) are abandoned by the
        // SP restore. This is the architecturally-correct Phase A path: WFI
        // happens in kernel ctx, never inside an svc handler (the failure mode
        // of all 3 prior attempts).
        leave_user();
        __builtin_unreachable();
    }
    load_ctx(esper_at(next), frame);
    return next;
}

// Monotonic "poll generation", bumped under g_esper_lock on every IPC wake so a
// multi-fd ppoll waiter can detect -- with an O(1) under-lock recheck -- that a
// producer fired during its check-then-park window. This closes the ppoll
// lost-wakeup race WITHOUT re-scanning all N fds under the scheduler lock (that
// rescan was tried and reverted for regressing KEX timing). Mirrors a seqlock /
// condition-generation: ppoll snapshots it before scanning its fds, then parks
// only if it is still unchanged when re-read inside the same lock the waker took.
// Read outside the lock as a plain snapshot; the authoritative compare is the
// under-lock recheck in poll_gen_changed. Internal linkage; exported via
// linux_poll_gen().
volatile uint32_t g_poll_gen = 0;

int ipc_wake_impl(Esper::IpcWaitKind kind, int id) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    // Publish "a producer fired" under the lock, before scanning waiters: a
    // poller still in its check-then-park window will observe this when it
    // rechecks the generation under this same lock, and re-scan instead of
    // sleeping through the event. Must bump even when no waiter is woken below
    // (woken==0) -- that is precisely the missed-poller case.
    __atomic_add_fetch(&g_poll_gen, 1, __ATOMIC_RELAXED);
    int woken = 0;
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        Esper *e = esper_at(i);
        if (e == nullptr || e->state != EsperState::waiting) continue;
        // Wake the targeted waiters, AND every ppoll/pselect waiter. A poller
        // parks on PollWait, but the fd it watches may be a socketpair / pty /
        // eventfd whose producer raises a *specific* kind (ChannelRecv,
        // PtySlaveRead, ...). The 100 Hz tick that would otherwise re-poll it
        // is suppressed at idle (tickless idle), and only NIC traffic carries
        // an IRQ to break that idle -- so a poller waiting on a non-NIC fd
        // would sleep forever (the OpenSSH privsep monitor<->child and the
        // session pty both hit this). Kicking PollWait here, in the single
        // place every producer already funnels through, covers them all
        // without sprinkling wakes across N call sites. Pollers just re-check
        // and re-park if their fd isn't actually ready -- cheap and correct.
        const bool hit = (e->ipc_wait_kind == kind &&
                          (id < 0 /*wildcard*/ || e->ipc_wait_id == id)) ||
                         (kind != Esper::IpcWaitKind::PollWait &&
                          e->ipc_wait_kind == Esper::IpcWaitKind::PollWait);
        if (hit) {
            e->ipc_wait_kind = Esper::IpcWaitKind::None;
            e->ipc_wait_id = -1;
            e->state = EsperState::ready;
            ++woken;
        }
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    if (woken > 0) esper_kick_secondaries(); // let secondaries pick the woken
    return woken;
}

// Make `e` the context the pending eret will resume (from a synchronous trap):
// switch to its address space, then load its registers/SP/ELR/SPSR.
void load_ctx(Esper *e, uint64_t *frame) {
    switch_ttbr0(e->ttbr0);
    deliver_pending_status(e);
    asm volatile("msr tpidr_el0, %0" ::"r"(e->tpidr)); // per-thread TLS base
    if (e->started) {
        dbg_check_ctx("[CTXBAD-LOAD]", e);
        for (uint32_t i = 0; i < 31; ++i) {
            frame[i] = e->regs[i];
        }
        asm volatile("msr sp_el0, %0" ::"r"(e->sp_el0));
        asm volatile("msr elr_el1, %0" ::"r"(e->elr));
        // ptrace single-step: arm MDSCR.SS + set PSTATE.SS in the resumed SPSR so
        // exactly one EL0 instruction runs before the step exception re-traps.
        const uint64_t spsr = e->ptrace_singlestep ? (e->spsr | kSpsrSsBit) : e->spsr;
        asm volatile("msr spsr_el1, %0" ::"r"(spsr));
        mental_out_arm_step(e->ptrace_singlestep);
    } else {
        for (uint32_t i = 0; i < 31; ++i) {
            frame[i] = 0;
        }
        asm volatile("msr sp_el0, %0" ::"r"(e->stack_top));
        asm volatile("msr elr_el1, %0" ::"r"(e->entry));
        asm volatile("msr spsr_el1, %0" ::"r"(kSpsrEl0));
        mental_out_arm_step(false);
        e->started = true;
    }
    // Restore e's NEON state (zeroed for a fresh Esper). The caller erets via
    // the trap frame; nothing in between touches FP (-mgeneral-regs-only).
    fpsimd_restore(e->fpsimd);
}

// resume_user_eret's asm assumes Esper::regs[i] is at byte offset 8*i. Catch a
// future Esper-struct rearrangement before it silently corrupts EL0 context.
static_assert(sizeof(reinterpret_cast<Esper*>(0)->regs) == 31 * 8,
              "Esper::regs must be 31 contiguous uint64_t for resume_user_eret");

// Preempt the running Esper from IRQ context: its EL0 state is in the IRQ frame
// (x0..x30, ELR at [32], SPSR at [33]) plus the live SP_EL0. Swap it (and the
// address space) for the next ready Esper so the pending eret resumes it.
void esper_preempt(uint64_t *frame) {
    if (((frame[kFrameSpsr] >> 2) & 0x3) != 0) {
        return; // interrupted the kernel, not a user process
    }
    const int idx = esper_running_index();
    if (idx < 0) {
        return;
    }

    // Pre-save cur's EL0 context into the Esper struct WHILE cur is still
    // state=running on this CPU. No other CPU can pick a running Esper, so
    // these writes don't race. The atomic [cur->state=ready + pick + claim]
    // happens inside esper_preempt_and_pick under g_esper_lock; once the
    // lock is released, cur's regs are visible to whichever CPU resumes
    // cur later (lock release-acquire ordering).
    Esper *cur = esper_at(idx);
    for (uint32_t i = 0; i < 31; ++i) {
        cur->regs[i] = frame[i];
    }
    cur->elr = frame[kFrameElr];
    cur->spsr = frame[kFrameSpsr];
    asm volatile("mrs %0, sp_el0" : "=r"(cur->sp_el0));
    asm volatile("mrs %0, tpidr_el0" : "=r"(cur->tpidr)); // preserve user-set TLS
    fpsimd_save(cur->fpsimd); // vectors still hold cur's values (kernel uses none)

    const int next = esper_preempt_and_pick(idx);

    // Drain one pending unblocked signal on cur. This is Linux's IRQ-return
    // TIF_SIGPENDING check, collapsed into the timer-tick preempt path.
    // Without it a fortis931_kill_pgrp on a state=running target (e.g. busybox
    // sh busy-looping a not-yet-implemented syscall) would leave the SIGINT
    // bit set in sig_pending with nobody to consume it -- Ctrl-C lost.
    // When cur is being preempted (next >= 0), write the handler entry into
    // cur's saved Esper struct (frame=null path); cur enters the handler on
    // its next schedule. When cur keeps running (next < 0), rewrite the live
    // IRQ frame so the eret immediately enters the handler.
    if (cur->abi == Abi::Linux) {
        const uint64_t lock_flags = anti_skill_lock_irqsave(g_esper_lock);
        const uint64_t deliverable = cur->sig_pending & ~cur->sig_mask;
        if (deliverable != 0) {
            const int sig = __builtin_ctzll(deliverable) + 1;
            const uint64_t bit = 1ULL << (sig - 1);
            uint64_t *deliver_to = (next < 0) ? frame : nullptr;
            const bool ok = linux_deliver_signal_locked(cur, sig, deliver_to);
            // Consume the pending bit only when it was really handled, NEVER drop
            // a handler'd signal on a transient frame-build failure (that lost
            // SIGCHLD and hung the ssh logout). For deliver_to==null (cur is being
            // preempted) linux_deliver_signal_locked RE-QUEUES it (returns true)
            // so the target delivers it on its next schedule -- must keep the bit.
            if (ok && deliver_to != nullptr) {
                cur->sig_pending &= ~bit; // handler frame built into the live frame
                // CRITICAL (SMP logout-hang root cause): linux_deliver_signal_locked
                // entered the handler by MSR'ing elr_el1/spsr_el1 live, but this is
                // the IRQ frame and irq_entry RESTORES elr/spsr from frame[32]/[33]
                // on its eret -- so without syncing those slots the eret returns to
                // the OLD interrupted PC (with the handler's args in x0..x2) and the
                // signal handler never runs: sshd's SIGCHLD reaper is silently
                // skipped -> the exited shell is never collected -> ssh logout hangs.
                // Only happens when the reaper is the sole runnable Esper (next<0,
                // common at logout) AND a 100 Hz preempt lands in the brief window
                // SIGCHLD is unblocked -- hence multi-core-only / intermittent, and
                // why SMP=1 never hangs. sp_el0 needs no sync (irq_entry leaves it).
                uint64_t live_elr, live_spsr;
                asm volatile("mrs %0, elr_el1" : "=r"(live_elr));
                asm volatile("mrs %0, spsr_el1" : "=r"(live_spsr));
                frame[kFrameElr] = live_elr;
                frame[kFrameSpsr] = live_spsr;
            } else if (!ok && (cur->sig_handler[sig] == 0 || cur->sig_handler[sig] == 1)) {
                cur->sig_pending &= ~bit; // genuine SIG_DFL/SIG_IGN
            }
            // else: keep pending (re-queued for preempt, or transient build fail).
        }
        anti_skill_unlock_irqrestore(g_esper_lock, lock_flags);
    }

    if (next < 0) {
        // No other ready Esper -- preempt_and_pick left cur in state=running
        // and did not write g_running. Keep running cur (the drain above may
        // have rewritten frame to enter a handler).
        return;
    }

    Esper *n = esper_at(next);
    switch_ttbr0(n->ttbr0);
    deliver_pending_status(n);
    asm volatile("msr tpidr_el0, %0" ::"r"(n->tpidr)); // per-thread TLS base
    if (n->started) {
        for (uint32_t i = 0; i < 31; ++i) {
            frame[i] = n->regs[i];
        }
        frame[kFrameElr] = n->elr;
        // ptrace single-step: irq_entry erets from frame[kFrameSpsr], so set
        // PSTATE.SS there (+ arm MDSCR) when resuming a stepping tracee. Keyed on
        // n's flag so preempting to a non-stepping Esper always disarms stepping.
        frame[kFrameSpsr] = n->ptrace_singlestep ? (n->spsr | kSpsrSsBit) : n->spsr;
        asm volatile("msr sp_el0, %0" ::"r"(n->sp_el0));
        mental_out_arm_step(n->ptrace_singlestep);
    } else {
        for (uint32_t i = 0; i < 31; ++i) {
            frame[i] = 0;
        }
        frame[kFrameElr] = n->entry;
        frame[kFrameSpsr] = kSpsrEl0;
        asm volatile("msr sp_el0, %0" ::"r"(n->stack_top));
        mental_out_arm_step(false);
        n->started = true;
    }
    // Load n's vectors so it resumes with its own NEON state. Safe to do now:
    // nothing between here and the irq_entry eret touches FP (-mgeneral-regs-only).
    fpsimd_restore(n->fpsimd);
}

} // namespace [anonymous]

// Schedule a previously claimed Esper (`next` already has state=running and
// g_running[cpu]=next via esper_pick_and_claim) onto the current CPU.
// Switches address space, then either enters fresh or resumes saved context.
// Returns via leave_user when the Esper parks / exits. Used by run_espers
// (boot core) and by el0_try_run_one (secondary cores in SMP EL0 mode).
// Tear down an Esper whose saved EL0 context was found corrupt (PC or SP
// outside the 39-bit EL0 VA range). The residual SMP wild-write occasionally
// smashes a parked Esper's saved e->elr/e->sp_el0 to a kernel high-half
// address; eret'ing there faults EL0 on a UXN kernel page. Releases the Esper's
// resources and faults it WITHOUT picking a successor -- the scheduler loop
// re-picks a clean Esper. Shared by run_one_esper's C-side guard and
// resume_user_eret's asm-side guard (resume_user_elr_corrupt).
static void kill_corrupt_esper(int next, const char *where) {
    namespace district = imaginary_number_district;
    Esper *e = (next >= 0) ? esper_at(static_cast<uint32_t>(next)) : nullptr;
    district::write("\n[ELRGUARD");
    district::write(where);
    district::write("] corrupt resume ctx");
    if (e != nullptr) {
        district::write(" pid "); district::dec(e->pid);
        district::write(" elr="); district::hex(e->elr);
        district::write(" sp_el0="); district::hex(e->sp_el0);
        district::write(" name "); district::write(e->name);
    }
    district::writeln(" -> esper killed (kernel survives)");
    if (e != nullptr) {
        release_esper_pipes(e);
        release_linux_image(e);
    }
    esper_fault_current(next); // fault + free this CPU's slot; loop re-picks
}

// Asm-side ELRGUARD fallback, called from resume_user_eret right before `eret`
// when the PC/SP about to be installed into EL0 is not a canonical EL0 VA. This
// closes the SMP window between run_one_esper's C-side check (below) and the
// actual resume: a concurrent wild-write can smash this Esper's saved e->elr to
// a kernel address after the C check but before the value is reloaded for the
// resume call. We fault THIS CPU's running Esper; the asm then unwinds via
// leave_user back into the scheduler.
extern "C" void resume_user_elr_corrupt() {
    kill_corrupt_esper(esper_running_index(), "-asm");
}

void run_one_esper(int next) {
    namespace district = imaginary_number_district;
    Esper *e = esper_at(static_cast<uint32_t>(next));
    if (e->started) {
        // SPSR sanity check: M[4:0] == 0 means EL0t (only valid target).
        if ((e->spsr & 0x1F) != 0) {
            district::write("resume BAD SPSR=");
            district::hex(e->spsr);
            district::writeln("");
            for (;;) asm volatile("wfi");
        }
        // Corrupt-context guard. A resumed Esper's PC and SP must be canonical
        // EL0 VAs (below the 39-bit user ceiling 0x80_0000_0000). The residual
        // SMP wild-write occasionally smashes a parked Esper's saved e->elr to a
        // KERNEL high-half address (observed on HVF: resume_user_eret itself) --
        // eret'ing there faults EL0 on a UXN kernel page and looks like a random
        // crash. Catch it deterministically (this check is free and prints ONLY
        // on corruption, so it doesn't perturb timing / hide the Heisenbug) and
        // kill THIS Esper cleanly instead of wild-jumping; kernel + peers live.
        constexpr uint64_t kUserCeiling = 0x8000000000ULL; // 39-bit EL0 VA ceiling
        if (e->elr >= kUserCeiling || e->sp_el0 >= kUserCeiling) {
            // C-side catch. resume_user_eret's asm guard closes the remaining
            // window between here and the actual eret (see kill_corrupt_esper).
            kill_corrupt_esper(next, "");
            return;
        }
        switch_ttbr0(e->ttbr0);
        // [L1GUARD] Detect + self-heal the residual SMP wild-write that smashes a
        // process's L1[0] (its 0..1 GiB user window) from {invalid|table} into a
        // 1 GiB BLOCK. That block is the inherited kernel device/RAM mapping
        // (UXN), so the process then takes an L1 *instruction* permission fault on
        // its OWN code (ESR 0x8200000d, seen at 8 cores on a reconnect) and dies,
        // and a follow-on wild kernel branch (ELR=0) can take the box down. L1[0]
        // is NEVER legitimately a block (pr2_create_addr_space clears it; faults
        // only ever make it a table), so a block there is unambiguous corruption.
        // Reset it to 0: the next fault rebuilds the table cleanly and the process
        // survives instead of crashing. Logged so the corruption stays visible
        // (root-causing the writer is the remaining deep-SMP work). Reading the
        // L1 via its high-half alias works regardless of which TTBR0 is live.
        {
            uint64_t *l1hi = reinterpret_cast<uint64_t *>(teleport_high_alias(e->ttbr0));
            if ((l1hi[0] & 0b11ULL) == 0b01ULL /*1 GiB block*/) {
                district::write("\n[L1GUARD] pid="); district::dec(e->pid);
                district::write(" L1[0] smashed to block "); district::hex(l1hi[0]);
                district::writeln(" -> reset to 0 (healed; process survives)");
                l1hi[0] = 0;
                asm volatile("dsb ish; tlbi vmalle1is; dsb ish; isb" ::: "memory");
            }
        }
        deliver_pending_status(e);
        fpsimd_restore(e->fpsimd); // resume this Esper's NEON state before eret
        resume_user_eret(e->regs, e->sp_el0, e->elr, e->spsr, e->tpidr);
    } else {
        e->started = true;
        fpsimd_restore(e->fpsimd); // zeroed for a fresh Esper: a clean FP slate
        enter_user(e->entry, e->stack_top, e->ttbr0);
    }
    // enter_user / resume_user_eret return only via leave_user. Control
    // lands here on this CPU; caller decides what to do next.
}

// Try to run one EL0 Esper on the current CPU. Returns true if one ran (and
// has since exited / parked). Returns false if no Esper was ready. Idle
// Sisters call this each loop so secondary cores participate in EL0
// scheduling (SMP). The pick + claim is atomic so two CPUs cannot both
// resume the same Esper -- the bug that motivated the SMP locking pass.
bool el0_try_run_one() {
    if (!misaka_network_user_mode_active()) return false;
    const int idx = esper_running_index();
    const int next = esper_pick_and_claim(idx);
    if (next < 0) return false;
    run_one_esper(next);
    return true;
}

namespace { // re-enter anon for the rest (run_espers, spawn_elf, etc.)

void run_espers() {
    namespace district = imaginary_number_district;

    // Boot core's primary EL0 scheduler loop. Other CPUs participate via
    // idle_entry calling el0_try_run_one() once misaka_network_set_user_mode
    // has been activated by us below. With per-CPU g_running + g_user_kctx
    // (see esper.cpp + user_switch.S) any CPU can hold a different Esper.
    if (arch::this_cpu_id() != 0) {
        district::writeln("run_espers: not on boot core, returning.");
        return;
    }

    if (esper_pick_ready(-1) < 0) {
        district::writeln("no runnable Esper.");
        return;
    }

    district::writeln("Scheduling Esper(s) at EL0...");
    misaka_network_set_user_tick(&esper_preempt);
    misaka_network_set_user_mode(true);

    for (;;) {
        const int next = esper_pick_and_claim(-1);
        if (next < 0) {
            // No ready Esper. Check whether any are still alive (waiting on
            // IPC, sleep, or wait4). If none are, every Esper has exited and
            // we can fall back to the kernel shell.
            bool alive = false;
            for (uint32_t i = 0; i < kMaxEspers; ++i) {
                Esper *e = esper_at(i);
                if (e != nullptr && (e->state == EsperState::ready ||
                                     e->state == EsperState::running ||
                                     e->state == EsperState::waiting)) {
                    alive = true;
                    break;
                }
            }
            if (!alive) break;

            // Tickless idle: instead of ticking 100 Hz while there is nothing
            // to do, reprogram the next beat to the nearest Esper deadline
            // (capped at one second so something external -- console RX IRQ,
            // network IRQ -- still gets a chance to nudge us if we missed an
            // event). PL011 RX IRQ wakes ConsoleRead-parked Espers directly,
            // so the only deadlines we still need to honor are wake_cntpct
            // (nanosleep / clock_nanosleep).
            uint64_t now;
            asm volatile("mrs %0, cntpct_el0" : "=r"(now));
            uint64_t freq;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
            uint64_t cap = freq; // 1 second deep-idle fallback
            if (cap == 0) cap = last_order_interval_cycles(); // shouldn't happen
            // If anything is blocked on the NIC -- a TCP recv/accept/connect or a
            // ppoll/pselect that the 100 Hz network_tick services -- poll at the
            // full tick rate instead of deep-idling for up to a second. The
            // virtio-net RX IRQ is gated off (HVF GICv3 SPI hang), so network_tick
            // is the ONLY path that drains incoming packets and wakes the waiter;
            // a 1 s cap there added up to a second of latency PER ssh round-trip,
            // so the post-auth session setup (pty-req/shell/... many round-trips)
            // looked hung on SMP. Mainstream OS wakes on the RX IRQ; this is the
            // timer-poll fallback for when that IRQ is unusable. Truly idle (no
            // network waiter) still deep-idles at 1 s to stay cool.
            // Any live TCP connection means incoming segments may arrive that
            // only network_tick can drain (RX IRQ gated off) -- poll at 100 Hz.
            bool net_poll = antenna_has_active_socket();
            for (uint32_t i = 0; !net_poll && i < kMaxEspers; ++i) {
                Esper *e = esper_at(i);
                if (e == nullptr || e->state != EsperState::waiting) continue;
                const Esper::IpcWaitKind k = e->ipc_wait_kind;
                if (k == Esper::IpcWaitKind::AntennaRecv ||
                    k == Esper::IpcWaitKind::AntennaAccept ||
                    k == Esper::IpcWaitKind::AntennaConnect ||
                    k == Esper::IpcWaitKind::PollWait ||
                    k == Esper::IpcWaitKind::EpollWait) {
                    // EpollWait MUST keep the core at 100 Hz: network_tick fires
                    // linux_ipc_wake(EpollWait,-1) every tick to re-scan epoll sets
                    // (a timerfd/eventfd in the set becomes ready with no producer
                    // event). If we deep-idle 1 s here, that re-scan stops and an
                    // epoll_wait without a timeout never wakes -- HVF SMP=1 java
                    // (HotSpot uses epoll) froze at CPU 0%. Mirrors the PollWait case.
                    net_poll = true;
                }
            }
            if (net_poll) cap = last_order_interval_cycles(); // ~10 ms (100 Hz)
            uint64_t delta = cap;
            for (uint32_t i = 0; i < kMaxEspers; ++i) {
                Esper *e = esper_at(i);
                if (e == nullptr) continue;
                if (e->state != EsperState::waiting) continue;
                if (e->wake_cntpct == 0) continue;
                if (e->wake_cntpct <= now) { delta = 1; break; }
                const uint64_t remaining = e->wake_cntpct - now;
                if (remaining < delta) delta = remaining;
            }
            last_order_set_oneshot_cycles(delta);

            // One last RX drain before parking the core: ensures vmnet has
            // empty buffers to fill for the next host broadcast it receives,
            // so it stays quiet during our sleep.
            drivers::misaka_mail_drain();

            const uint64_t saved_daif = arch::read_daif();
            arch::enable_irq();
            drivers::othinus_cpu_suspend();
            arch::write_daif(saved_daif);
            continue;
        }

        run_one_esper(next);
        // enter_user / resume_user_eret return only via leave_user, which
        // restores run_espers's saved kernel context (callee-saved regs + SP).
        // Control lands here; the loop re-picks the next ready Esper.
    }

    misaka_network_set_user_mode(false);
    switch_ttbr0(teleport_kernel_ttbr0()); // back to the kernel address space
    district::write("All Espers finished; back in kernel (EL");
    district::dec(arch::current_el());
    district::writeln(").");
    arch::enable_irq();
}

// Embedded program: runs from .user_text at its high VA, so it uses the kernel
// address space (no private map needed) and a shared high-VA stack.
void spawn_embedded(const char *name, void (*fn)()) {
    const int slot = esper_create(name);
    if (slot < 0) {
        imaginary_number_district::writeln("process table full.");
        return;
    }
    Esper *e = esper_at(slot);
    e->entry = reinterpret_cast<uint64_t>(fn);
    e->stack_top = reinterpret_cast<uint64_t>(__user_stack_end);
    e->ttbr0 = teleport_kernel_ttbr0();
    e->started = false;
    esper_make_ready(slot); // publish: scheduler can now pick this Esper
}

// Disk program: create an Esper and load a PIE ELF into its private address space.
bool spawn_elf(const char *name) {
    const int slot = esper_create(name);
    if (slot < 0) {
        imaginary_number_district::writeln("process table full.");
        return false;
    }
    if (!load_elf_into_slot(static_cast<uint32_t>(slot), name)) {
        esper_at(slot)->state = EsperState::free;
        return false;
    }
    esper_make_ready(slot);
    return true;
}

} // namespace

// External wrappers around the anon-namespace IPC park/wake helpers so the
// Linux syscall layer (linux_abi.cpp) and IPC modules can call them.
int linux_ipc_park(int idx, Esper::IpcWaitKind kind, int id, uint64_t *frame) {
    return ipc_park_impl(idx, kind, id, frame);
}
int linux_ipc_wake(Esper::IpcWaitKind kind, int id) {
    return ipc_wake_impl(kind, id);
}
// Snapshot the poll generation (see g_poll_gen). Plain relaxed load: ppoll takes
// it before scanning its fds; the lost-wakeup-closing compare happens under
// g_esper_lock inside ipc_park_unless_ready's readiness probe.
uint32_t linux_poll_gen() { return __atomic_load_n(&g_poll_gen, __ATOMIC_RELAXED); }

// Park on (kind,id) ONLY if `ready(id)` is still false when re-checked under
// g_esper_lock -- the same lock every producer's linux_ipc_wake takes. This
// closes the lost-wakeup race in the "try-nonblocking-then-park" idiom: between
// a non-blocking try returning empty and the park taking effect, a producer on
// another CPU can deliver data and wake. Re-checking readiness *inside* the lock
// makes park-vs-wake atomic -- either we observe the just-delivered data (the
// waker's lock release published it) and skip the sleep, or we are already
// state=waiting when the waker scans, so its wake finds us. Mirrors Linux's
// prepare_to_wait() + condition recheck before schedule(). Without it, on a
// tickless-idle SMP box (no 100 Hz re-poll to paper over the miss) the waiter
// sleeps forever -- the OpenSSH privsep monitor<->child RPC hang on -smp >1.
// Returns true if it parked (caller returns the kFdParked sentinel); false if
// the fd became ready in the window (caller should retry the non-blocking op).
// Commit a park while ALREADY holding g_esper_lock (flags = the saved DAIF from
// the acquire). The caller must have re-checked its wait condition under the
// same lock first -- that re-check + this park being one critical section is
// what closes the lost-wakeup race (a producer's wake also runs under
// g_esper_lock, so it either already fired our condition or finds us waiting).
// Saves the EL0 context, marks (kind,id), picks the next Esper, RELEASES the
// lock, and switches. Does not return in the no-runnable case (leave_user).
void ipc_park_locked(int idx, Esper::IpcWaitKind kind, int id, uint64_t *frame,
                     uint64_t flags) {
    Esper *e = esper_at(idx);
    if (e == nullptr) {
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        return;
    }
    save_ctx(e, frame);
    e->elr -= 4; // re-execute the svc on resume
    e->ipc_wait_kind = kind;
    e->ipc_wait_id = id;
    const int next = esper_park_and_pick_locked(idx);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    if (next < 0) {
        leave_user(); // no one else runnable -> WFI in the scheduler loop
        __builtin_unreachable();
    }
    load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
}

bool ipc_park_unless_ready(int idx, Esper::IpcWaitKind kind, int id,
                           uint64_t *frame, bool (*ready)(int),
                           uint64_t park_sig_mask) {
    Esper *e = esper_at(idx);
    if (e == nullptr) return false;
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    if (ready != nullptr && ready(id)) {
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        return false; // delivered in the window -- don't sleep, re-check
    }
    // Don't sleep through a deliverable signal. A signal sender on another CPU
    // (maybe_wake_parent / linux_deliver_signal_locked, e.g. SIGCHLD on a child
    // exit) marks sig_pending under g_esper_lock; if it ran AFTER our caller's
    // top-of-loop signal-check but BEFORE this park, the signal would be lost
    // until the next 100 Hz re-kick (and even then the re-kick replays into the
    // same check-then-park gap, so SIGCHLD can stay undelivered indefinitely ->
    // sshd never reaps the exited shell -> logout hangs). Re-checking pending
    // signals here, under the same lock the sender takes, makes park-vs-signal
    // atomic: either we see the pending signal and bail (caller re-loops, its
    // signal-check delivers it), or we are already waiting when the sender scans
    // and its wake finds us. Mirrors Linux signal_pending_state() in schedule().
    if ((e->sig_pending & ~park_sig_mask) != 0) {
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        return false;
    }
    ipc_park_locked(idx, kind, id, frame, flags); // releases lock + switches
    return true;
}


// Linux pipe write/read with the same park-and-retry semantics the Index ABI
// uses (declared in usermode.hpp; called from linux_abi.cpp's fd dispatcher).
constexpr int64_t kFdParkedSentinel = -1000;
int64_t linux_pipe_write(int idx, uint32_t fd, const char *buf, uint64_t len,
                         uint64_t *frame) {
    Esper *e = esper_at(static_cast<uint32_t>(idx));
    if (e == nullptr || fd >= kMaxFds || e->fds[fd].kind != FdKind::pipe_write) {
        return -9; // -EBADF
    }
    const int pi = e->fds[fd].pipe_idx;
    for (;;) {
        const int64_t n = aiwass_write(pi, buf, len);
        if (n == kAiwassBrokenPipe) {
            return -32; // -EPIPE
        }
        if (n == kAiwassWouldBlock) {
            if (park_on_pipe(idx, pi, /*is_write=*/true, frame)) {
                return kFdParkedSentinel;
            }
            continue; // became writable in the park window -- retry
        }
        wake_pipe_readers(pi);
        return n;
    }
}

int64_t linux_pipe_read(int idx, uint32_t fd, char *buf, uint64_t len,
                        uint64_t *frame) {
    Esper *e = esper_at(static_cast<uint32_t>(idx));
    if (e == nullptr || fd >= kMaxFds || e->fds[fd].kind != FdKind::pipe_read) {
        return -9; // -EBADF
    }
    const int pi = e->fds[fd].pipe_idx;
    for (;;) {
        const int64_t n = aiwass_read(pi, buf, len);
        if (n == kAiwassBrokenPipe) {
            return 0; // EOF: writers all closed
        }
        if (n == kAiwassWouldBlock) {
            if (park_on_pipe(idx, pi, /*is_write=*/false, frame)) {
                return kFdParkedSentinel;
            }
            continue; // became readable in the park window -- retry
        }
        if (n > 0) wake_pipe_writers(pi); // freed space; writers retry
        return n;
    }
}

void run_user() {
    spawn_embedded("user", &user_entry);
    run_espers();
}

void run_user_fault() {
    spawn_embedded("userfault", &user_fault_entry);
    run_espers();
}

void run_elf(const char *name) {
    if (spawn_elf(name)) {
        run_espers();
    }
}

// Spawn a Linux ELF with a real argv (and optional envp), then run. Used by
// the Necessarius `linuxrun` shell command -- the equivalent of `run_elf` but
// passing argv so e.g. busybox can pick its applet from argv[0]. argv strings
// are copied into a kernel buffer so the caller can reuse its own line buffer.
void run_elf_argv(const char *name, const char *const *argv, uint32_t argc,
                  const char *const *envp, uint32_t envc) {
    namespace district = imaginary_number_district;
    const int slot = esper_create(name);
    if (slot < 0) {
        district::writeln("linuxrun: no Esper slot.");
        return;
    }
    Esper *e = esper_at(slot);

    constexpr uint64_t kStageSize = 2048;
    char *stage = static_cast<char *>(dark_matter_alloc(kStageSize));
    if (stage == nullptr) {
        district::writeln("linuxrun: out of heap.");
        e->state = EsperState::free;
        return;
    }
    uint32_t pos = 0;
    auto stage_str = [&](const char *src, const char **dst) -> bool {
        if (src == nullptr) return false;
        char *start = stage + pos;
        while (*src != 0 && pos + 1 < kStageSize) stage[pos++] = *src++;
        if (pos + 1 >= kStageSize) return false;
        stage[pos++] = 0;
        *dst = start;
        return true;
    };
    e->exec_argc = 0;
    e->exec_envc = 0;
    for (uint32_t i = 0; i < argc && i < kExecArgvCap; ++i) {
        if (!stage_str(argv[i], &e->exec_argv[i])) {
            district::writeln("linuxrun: argv staging overflow.");
            dark_matter_free(stage);
            e->state = EsperState::free;
            return;
        }
        ++e->exec_argc;
    }
    for (uint32_t i = 0; i < envc && i < kExecEnvpCap; ++i) {
        if (!stage_str(envp[i], &e->exec_envp[i])) {
            district::writeln("linuxrun: envp staging overflow.");
            dark_matter_free(stage);
            e->state = EsperState::free;
            return;
        }
        ++e->exec_envc;
    }

    if (!load_elf_into_slot(static_cast<uint32_t>(slot), name)) {
        dark_matter_free(stage);
        e->state = EsperState::free;
        return;
    }
    dark_matter_free(stage);
    esper_make_ready(slot); // publish: scheduler can now pick this Esper
    run_espers();
}

void run_coexec(const char *names) {
    char name[32];
    uint32_t spawned = 0;
    while (*names) {
        while (*names == ' ') {
            ++names;
        }
        uint32_t n = 0;
        while (*names && *names != ' ' && n + 1 < sizeof(name)) {
            name[n++] = *names++;
        }
        name[n] = 0;
        if (n > 0 && spawn_elf(name)) {
            ++spawned;
        }
    }
    if (spawned > 0) {
        run_espers();
    } else {
        imaginary_number_district::writeln("coexec: nothing to run.");
    }
}

// Linux's futex serialises WAIT and WAKE on a per-hash-bucket spinlock so
// the "read uaddr, then queue self" and "find waiters, wake them" sequences
// are atomic w.r.t. each other; without this, a WAKE that fires between a
// WAIT's value-check and its state-transition is lost (the WAKE walks an
// empty queue, then WAIT parks itself with no one left to wake it).
// We piggyback on the global g_esper_lock for this rather than a separate
// futex bucket lock: kMaxEspers <= 16 keeps the scan O(N), and folding
// futex onto the scheduler lock means the FUTEX_WAIT value-check, the
// state=waiting + queue-insert, AND the pick+claim of the next runnable
// Esper happen in one critical section (eliminating both lost-wakeup and
// the SMP "two CPUs claim same Esper" race in one shot).

// Wake up to `count` Espers blocked in futex(FUTEX_WAIT) on the physical word
// `phys`. Returns the number woken. Shared by FUTEX_WAKE and thread exit
// (clear_child_tid). Defined here (not linux_abi.cpp) since it touches Esper
// scheduling state.
int futex_wake_phys(uint64_t phys, int count) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    int woken = 0;
    for (uint32_t i = 0; i < kMaxEspers && woken < count; ++i) {
        Esper *e = esper_at(i);
        if (e != nullptr && e->state == EsperState::waiting && e->wait_futex &&
            e->wait_futex_phys == phys) {
            e->wait_futex = false;
            e->wait_futex_phys = 0;
            e->state = EsperState::ready;
            ++woken;
        }
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    if (woken > 0) esper_kick_secondaries(); // futex wake (mutex/join) -> spread
    return woken;
}

// Linux futex(uaddr, op, val, ...). Implements the private FUTEX_WAIT (0) and
// FUTEX_WAKE (1) ops musl uses for pthread mutexes/joins. Runs in the parent's
// svc trap (frame = caller context) because WAIT must block + reschedule.
void linux_futex(int idx, uint64_t *frame) {
    Esper *me = esper_at(static_cast<uint32_t>(idx));
    const uint64_t uaddr = frame[0];
    const uint32_t op_raw = static_cast<uint32_t>(frame[1]);
    const uint32_t op = op_raw & 0x7f; // mask FUTEX_PRIVATE + FUTEX_CLOCK_REALTIME
    const uint32_t val = static_cast<uint32_t>(frame[2]);

    const uint64_t phys = pr2_user_phys(me, uaddr);
    if (phys == 0) {
        frame[0] = static_cast<uint64_t>(-14); // -EFAULT
        return;
    }
    if (op == 1 /*FUTEX_WAKE*/) {
        frame[0] = static_cast<uint64_t>(futex_wake_phys(phys, static_cast<int>(val)));
        return;
    }
    if (op == 0 /*FUTEX_WAIT*/ || op == 9 /*FUTEX_WAIT_BITSET*/) {
        // Linux futex protocol: hold the bucket lock across the value check
        // AND the queue insertion AND the pick-next of the successor so a
        // concurrent WAKE either sees us queued or hasn't fired yet, and a
        // concurrent CPU's pick can't grab us mid-flip. One critical section
        // on g_esper_lock covers all three.
        const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
        const uint32_t cur = *reinterpret_cast<const uint32_t *>(uaddr);
        if (cur != val) {
            anti_skill_unlock_irqrestore(g_esper_lock, flags);
            frame[0] = static_cast<uint64_t>(-11); // -EAGAIN
            return;
        }
        save_ctx(me, frame);
        me->regs[0] = 0; // futex returns 0 when woken
        me->wait_futex = true;
        me->wait_futex_phys = phys;
        const int next = esper_park_and_pick_locked(idx);
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        if (next < 0) {
            // Nothing else runnable. Park the CPU via leave_user; an eventual
            // FUTEX_WAKE (mutex release, thread exit, etc.) flips us back to
            // ready so the next schedule pass picks us up. Returning EAGAIN
            // here used to deadlock musl in a user-space spin.
            leave_user();
            __builtin_unreachable();
        }
        load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
        return;
    }
    if (op == 10 /*FUTEX_WAKE_BITSET*/) {
        // Identical to FUTEX_WAKE since we ignore bitmask filtering.
        frame[0] = static_cast<uint64_t>(futex_wake_phys(phys, static_cast<int>(val)));
        return;
    }
    if (op == 3 /*FUTEX_REQUEUE*/ || op == 4 /*FUTEX_CMP_REQUEUE*/) {
        // cv.broadcast wants to move waiters from cv->seq onto the mutex
        // wait queue without thundering-herd-waking them all. We don't have
        // a separate "park here, not there" mechanism -- everyone parked
        // on a futex is on the same global list keyed by phys -- so the
        // simplest correct thing is to wake them all and let them race on
        // the mutex lock cmpxchg. Slower than real REQUEUE but functional.
        if (op == 4) {
            const uint32_t expect = static_cast<uint32_t>(frame[5]);
            const uint32_t cur = *reinterpret_cast<const uint32_t *>(uaddr);
            if (cur != expect) {
                frame[0] = static_cast<uint64_t>(-11); // -EAGAIN
                return;
            }
        }
        // val = max waiters to wake; val2 (frame[3]) = max to requeue. We
        // wake (val + val2) total -- thundering herd but correct.
        const int wake_n = static_cast<int>(val);
        const int req_n = static_cast<int>(frame[3]);
        const int n = futex_wake_phys(phys, wake_n + req_n);
        frame[0] = static_cast<uint64_t>(n);
        return;
    }
    frame[0] = static_cast<uint64_t>(-38); // -ENOSYS for other ops
}

// Linux clone(flags, child_stack, parent_tid, tls, child_tid). Without
// CLONE_VM (0x100) it's a fork: the child gets an independent copy of the
// address space. With CLONE_VM it's a thread sharing the address space (Wave
// F4). Runs in the svc trap of the parent (frame = parent's context).
void linux_clone(int idx, uint64_t *frame) {
    namespace district = imaginary_number_district;
    Esper *p = esper_at(static_cast<uint32_t>(idx));
    const uint64_t flags = frame[0];
    const uint64_t child_stack = frame[1];
    const uint64_t new_tls = frame[3];        // CLONE_SETTLS
    const uint64_t child_tid_ptr = frame[4];  // CHILD_SETTID / CHILD_CLEARTID
    const bool share_vm = (flags & 0x100) != 0; // CLONE_VM -> thread
    const bool is_vfork = (flags & 0x4000) != 0; // CLONE_VFORK -> parent suspends

    const int cslot = esper_create(p->name);
    if (cslot < 0) {
        frame[0] = static_cast<uint64_t>(-11); // -EAGAIN
        return;
    }
    Esper *c = esper_at(static_cast<uint32_t>(cslot));

    if (share_vm) {
        // Thread: share the parent's address space + images (no copy). The
        // images stay alive until the last sharer exits (refcounted), and the
        // shared address space is reclaimed only when the last thread exits.
        // SHARE the parent's address space: one PersonalReality (same VMA list,
        // same page tables). Because the VMA list lives in the shared mm, a
        // region mmap'd by ANY thread is instantly visible to all siblings --
        // this is the fix for the CLONE_VM "VMA divergence" (gap 9) that killed
        // the multi-threaded JVM. The mm + images stay alive until the last
        // sharer exits (all refcounted).
        c->mm = p->mm;
        reality_ref(c->mm);  // one more sharer of this address space
        c->ttbr0 = p->ttbr0; // fast-path cache mirrors the shared mm->ttbr0
        c->linux_elf_image = p->linux_elf_image;
        c->linux_interp_image = p->linux_interp_image;
        c->linux_elf_image_size = p->linux_elf_image_size;
        image_ref(p->linux_elf_image);
        image_ref(p->linux_interp_image);
        c->is_thread = true;
    } else {
        // fork: independent (copy-on-write) address space. pr2_fork reality_allocs
        // the child its own PersonalReality (refs=1) and copies parent's VMAs +
        // brk/mmap bookkeeping + CoW pages into it.
        if (!pr2_fork(p, c)) {
            c->state = EsperState::free;
            frame[0] = static_cast<uint64_t>(-12); // -ENOMEM
            return;
        }
        c->linux_elf_image = p->linux_elf_image;
        c->linux_interp_image = p->linux_interp_image;
        c->linux_elf_image_size = p->linux_elf_image_size;
        image_ref(p->linux_elf_image);
        image_ref(p->linux_interp_image);
    }

    // Shared bookkeeping.
    c->abi = Abi::Linux;
    // POSIX: a forked child inherits parent's pgrp + sid. setpgid (or the
    // child setting it on itself) can move it out before exec.
    c->pgrp = p->pgrp;
    c->sid = p->sid;
    // (brk/mmap_next live in the shared PersonalReality `mm`: fork copies them in
    // pr2_fork; a CLONE_VM thread shares them automatically.)
    for (uint32_t i = 0; i < kCwdCap; ++i) c->cwd[i] = p->cwd[i]; // inherit cwd
    for (uint32_t i = 0; i < kCwdCap; ++i) c->exe_path[i] = p->exe_path[i]; // same image until execve
    c->uid = p->uid; c->euid = p->euid; c->suid = p->suid;
    c->gid = p->gid; c->egid = p->egid; c->sgid = p->sgid;
    for (uint32_t i = 0; i < 64; ++i) {
        c->sig_handler[i] = p->sig_handler[i];
        c->sig_restorer[i] = p->sig_restorer[i];
        c->sig_flags[i] = p->sig_flags[i];
    }
    c->elf_phdr_va = p->elf_phdr_va;
    c->elf_phnum = p->elf_phnum;
    c->elf_phentsize = p->elf_phentsize;
    c->elf_base = p->elf_base;
    c->elf_entry = p->elf_entry;
    c->parent = idx;

    // The child inherits the parent's live EL0 context and returns 0 from
    // clone; a thread starts on the stack clone() supplied.
    for (uint32_t i = 0; i < 31; ++i) c->regs[i] = frame[i];
    c->regs[0] = 0;
    uint64_t sp_el0 = 0, elr = 0, spsr = 0;
    asm volatile("mrs %0, sp_el0" : "=r"(sp_el0));
    asm volatile("mrs %0, elr_el1" : "=r"(elr));
    asm volatile("mrs %0, spsr_el1" : "=r"(spsr));
    c->sp_el0 = (share_vm && child_stack != 0) ? child_stack : sp_el0;
    c->elr = elr;
    c->spsr = spsr;
    c->stack_top = p->stack_top;
    c->started = true;
    // The child inherits the parent's live FP/SIMD too: snapshot the vectors
    // (still the parent's, kernel uses none) straight into the child's slot, so
    // callee-saved q8..q15 survive the fork() the same way x19..x28 do.
    fpsimd_save(c->fpsimd);
    // TLS: a fork inherits the parent's live TLS base (same address space copy);
    // a thread with CLONE_SETTLS gets its own. Read the live TPIDR_EL0 (the
    // parent is running, so its saved p->tpidr may be stale).
    uint64_t live_tpidr = 0;
    asm volatile("mrs %0, tpidr_el0" : "=r"(live_tpidr));
    c->tpidr = live_tpidr;
    // Thread setup: CLONE_SETTLS sets the child's TLS base; CHILD_CLEARTID asks
    // the kernel to zero+wake *child_tid on the child's exit (pthread_join);
    // CHILD_SETTID writes the new tid there now; PARENT_SETTID writes the new
    // tid to *parent_tid in the PARENT's memory (the new thread's `pthread`
    // struct's tid field on musl) so `pthread_self()->tid` reflects the real
    // tid right after clone returns. Missing PARENT_SETTID handling makes
    // every child see self->tid = 0, which breaks musl's __tl_lock recursion
    // check (`if (val == tid)` treats lock-is-free as "I already hold it")
    // and deadlocks the second pthread_create in a row.
    if (flags & 0x80000 /*CLONE_SETTLS*/) {
        c->tpidr = new_tls;
    }
    if ((flags & 0x00200000 /*CLONE_CHILD_CLEARTID*/) && child_tid_ptr != 0) {
        c->clear_child_tid = child_tid_ptr;
    }
    if ((flags & 0x01000000 /*CLONE_CHILD_SETTID*/) && child_tid_ptr != 0) {
        pr2_write_user(c, child_tid_ptr, &c->pid, 4);
    }
    if ((flags & 0x00100000 /*CLONE_PARENT_SETTID*/) && frame[2] != 0) {
        // frame[2] is the parent_tid pointer (third clone syscall arg).
        // CLONE_VM is set for threads, so the parent's address space is
        // shared with the child; writing through `p` (parent) hits the
        // same physical page the child will read via its TLS-resolved
        // `&pthread_self()->tid`.
        pr2_write_user(p, frame[2], &c->pid, 4);
    }
    // Inherit open fds (and bump pipe/socket refs so close accounting stays
    // correct after the child's lifetime diverges from the parent's).
    for (uint32_t i = 0; i < kMaxFds; ++i) {
        c->fds[i] = p->fds[i];
        if (c->fds[i].kind == FdKind::pipe_read) {
            aiwass_inc_read(c->fds[i].pipe_idx);
        } else if (c->fds[i].kind == FdKind::pipe_write) {
            aiwass_inc_write(c->fds[i].pipe_idx);
        } else if (c->fds[i].kind == FdKind::socket) {
            antenna_inc_ref(c->fds[i].sock_idx);
        } else if (c->fds[i].kind == FdKind::unix_sock) {
            inc_inc_ref(c->fds[i].sock_idx);
        } else if (c->fds[i].kind == FdKind::eventfd) {
            eventfd_inc_ref(c->fds[i].sock_idx);
        } else if (c->fds[i].kind == FdKind::epoll) {
            epoll_inc_ref(c->fds[i].sock_idx);
        } else if (c->fds[i].kind == FdKind::pty_master) {
            sr_master_inc_ref(c->fds[i].sock_idx);
        } else if (c->fds[i].kind == FdKind::pty_slave) {
            sr_slave_inc_ref(c->fds[i].sock_idx);
        }
    }
    if (is_vfork) {
        // CLONE_VFORK: the parent MUST suspend until the child execve()s or
        // exits. Parent + child share the address space AND (child_stack==0)
        // the same SP_EL0, so they must NOT run concurrently or the stack is
        // corrupted. esper_vfork_handoff does the whole transition under one
        // lock -- child->ready, parent->waiting, pick child on THIS cpu --
        // and crucially does NOT kick secondaries (an early kick let a
        // secondary start the child while the parent was still running on this
        // core -> shared-stack corruption -> child's /bin/sh never loaded).
        // The child runs here while the parent waits on VforkDone (keyed by
        // the child pid), released from the child's execve / exit path.
        save_ctx(p, frame);
        p->regs[0] = c->pid;     // parent's vfork() return value (child pid)
        p->ipc_wait_kind = Esper::IpcWaitKind::VforkDone;
        p->ipc_wait_id = static_cast<int>(c->pid);
        const int next = esper_vfork_handoff(idx, cslot);
        if (next < 0) {
            // No runnable Esper (shouldn't happen -- child was just readied).
            p->ipc_wait_kind = Esper::IpcWaitKind::None;
            p->ipc_wait_id = -1;
            esper_set_running(idx);
            frame[0] = c->pid;
            return;
        }
        load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
        return;
    }

    // Non-vfork (fork / pthread): publish the child as runnable (release-
    // ordered) and kick secondaries so one can run it in parallel.
    esper_make_ready(cslot);
    frame[0] = c->pid; // parent gets the child/thread tid
}

// Linux wait4(pid, status, options, rusage): reap a child. Reuses the Esper
// wait machinery; encodes the exit code into the Linux wait status format
// ((code & 0xff) << 8 for a normal exit).
void linux_wait4(int idx, uint64_t *frame) {
    Esper *p = esper_at(static_cast<uint32_t>(idx));
    const uint64_t status_ptr = frame[1];
    const uint64_t options = frame[2];

    // ptrace "Mental Out": report a ptrace-stopped tracee first (WSTOPPED status
    // (stopsig<<8)|0x7f). The tracee stays parked on PtraceStop -- NOT freed --
    // and the tracer resumes it later via PTRACE_CONT/SYSCALL/SINGLESTEP/DETACH.
    {
        const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
        int found = -1; uint32_t spid = 0; int ssig = 0;
        for (uint32_t i = 0; i < kMaxEspers; ++i) {
            Esper *c = esper_at(i);
            if (c != nullptr && c->ptrace_tracer == idx && c->ptrace_report) {
                c->ptrace_report = false; // consumed by this wait
                spid = c->pid; ssig = c->ptrace_stop_sig; found = static_cast<int>(i);
                break;
            }
        }
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        if (found >= 0) {
            if (status_ptr != 0) {
                const int32_t st = static_cast<int32_t>(((ssig & 0xff) << 8) | 0x7f);
                pr2_write_user(p, status_ptr, &st, sizeof(st));
            }
            frame[0] = spid;
            return;
        }
    }

    // Reap-or-park, all under ONE g_esper_lock critical section so a child's
    // concurrent exit (maybe_wake_parent_locked takes the same lock) cannot slip
    // into a gap between "scan finds no exited child" and "park". With one lock
    // it either lands BEFORE the scan (we reap the zombie) or AFTER we park
    // (parent_is_in_wait4_locked is true, so it wakes us). The old code split
    // scan and park into two separate locked regions; a child exiting in that
    // window left a zombie AND skipped the wake -> the waiter parked forever.
    // That lost-wakeup hung `cmd & cmd & wait` (several children exiting in
    // parallel). This now mirrors linux_futex's check-then-park, which holds one
    // lock across the value check AND the queue insertion for the same reason.
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    // Find an exited/faulted child and atomically transition it to free so a
    // concurrent kill/exit on another CPU can't re-reap the same slot.
    uint32_t cpid = 0;
    int64_t code = 0;
    int reaped = -1;
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        Esper *cand = esper_at(i);
        if (cand != nullptr && cand->parent == idx &&
            (cand->state == EsperState::exited ||
             cand->state == EsperState::faulted)) {
            cpid = cand->pid;
            code = cand->exit_code;
            cand->state = EsperState::free;
            reaped = static_cast<int>(i);
            break;
        }
    }
    if (reaped >= 0) {
        // We just consumed one child's SIGCHLD by reaping it. If NO other
        // exited/zombie child remains, clear the parent's pending SIGCHLD bit so
        // a later blocking read (the shell's next prompt) isn't spuriously
        // interrupted by a stale SIGCHLD whose child is already gone -- that
        // stale signal hung the command *after* `cmd & wait`. Mirrors Linux,
        // where SIGCHLD is recomputed from the actual set of zombies.
        bool more_zombies = false;
        for (uint32_t j = 0; j < kMaxEspers; ++j) {
            Esper *o = esper_at(j);
            if (o != nullptr && o->parent == idx &&
                (o->state == EsperState::exited || o->state == EsperState::faulted)) {
                more_zombies = true; break;
            }
        }
        if (!more_zombies) p->sig_pending &= ~(1ULL << (17 - 1)); // clear SIGCHLD
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        if (status_ptr != 0) {
            // pr2_write_user (not a raw deref): breaks CoW + faults the page in.
            // A raw write to a status pointer in a fork-shared CoW page would hit
            // the shared physical page without copying, smashing the child's view
            // (the SMP EL0 corruption: FAR=0x11 / kernel addr in the user PC).
            const int32_t st = static_cast<int32_t>((code & 0xff) << 8);
            pr2_write_user(p, status_ptr, &st, sizeof(st));
        }
        frame[0] = cpid;
        return;
    }
    // No exited child yet -- decide ECHILD / WNOHANG / park WITHOUT releasing the
    // lock, so the scan above and the park below are one atomic step.
    bool has_tracee = false;
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        Esper *c = esper_at(i);
        if (c != nullptr && c->ptrace_tracer == idx && c->state != EsperState::free) {
            has_tracee = true; break;
        }
    }
    if (esper_child_count(idx) == 0 && !has_tracee) {
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        frame[0] = static_cast<uint64_t>(-10); // -ECHILD
        return;
    }
    if (options & 1 /*WNOHANG*/) {
        // WNOHANG stays strictly non-blocking: no exited child -> return 0 now.
        // sshd's reaper polls waitpid(-1, WNOHANG) and must never block (blocking
        // it cratered ssh_truth to OK=2/CRASH=8). ash's `cmd & wait` also reaches
        // here with WNOHANG, gets 0, then sigsuspends for SIGCHLD -- and the real
        // fix is in parent_is_in_wait4_locked: a child's exit now correctly takes
        // ash's sigsuspend down the SIGCHLD path (not a mis-reap), so ash's handler
        // runs and it reaps. While ash is parked in sigsuspend (a real WFI), the
        // background sleeper is scheduled and runs to exit. So a plain non-blocking
        // return 0 here is correct for both; no yield/preempt needed (an extra
        // esper_preempt here only added TCG-SMP wild-jump races without helping).
        anti_skill_unlock_irqrestore(g_esper_lock, flags);
        frame[0] = 0; // non-blocking WNOHANG, no exited child -> 0
        return;
    }
    // Block until a child exits. maybe_wake_parent_locked sets regs[0]=pid +
    // pending_status and flips us ready; deliver-on-resume encodes pending_status
    // into the user's int*. esper_park_and_pick_locked runs under the lock we
    // already hold (same idiom as linux_futex's FUTEX_WAIT).
    save_ctx(p, frame);
    p->wait_status_ptr = status_ptr;
    p->wait_status_is_linux = true; // encode as Linux wait status on delivery
    // Park as a GENERIC wait4 waiter so a child's exit takes the REAP branch in
    // maybe_wake_parent (parent_is_in_wait4_locked). That predicate requires
    // ipc_wait_kind==None && wait_pipe_idx<0 && !wait_futex && wake_cntpct==0;
    // those fields can carry STALE values from this Esper's PREVIOUS park (e.g.
    // ConsoleRead, set while the shell read the command line) -- which makes the
    // predicate false, so the child takes the SIGCHLD branch instead of reaping,
    // waitpid returns -EINTR, and ash's dowait retries forever (the `cmd & wait`
    // hang). Clear them so this is unambiguously a wait4 park.
    p->ipc_wait_kind = Esper::IpcWaitKind::None;
    p->wait_pipe_idx = -1;
    p->wait_futex = false;
    p->wake_cntpct = 0;
    const int next = esper_park_and_pick_locked(idx);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    if (next < 0) {
        // No other Esper is runnable right now, but we ARE legitimately parked
        // (state=waiting) with live children -- a child's exit wakes us via
        // maybe_wake_parent_locked. Return to run_espers's WFI loop instead of
        // falsely reporting ECHILD.
        leave_user();
        __builtin_unreachable();
    }
    load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
}

// ===================== ptrace "Mental Out" =====================
// Named for Shokuhou Misaki's Mental Out (心理掌握): read another mind (PEEK),
// rewrite it (POKE), and puppeteer the body (CONT / single-step). A tracer
// attaches to a tracee; the tracee ptrace-stops (parks on PtraceStop, which no
// generic wake touches -- only the tracer resumes it) and reports each stop to
// the tracer's wait4. See the intercept in linux_deliver_signal_locked.

namespace {
// True if `p` is parked in a generic wait4 (mirrors esper.cpp's
// parent_is_in_wait4_locked, which has internal linkage there). Used to pre-set
// the tracer's wait4 result when a tracee stops.
bool mental_out_tracer_in_wait4(const Esper &p) {
    return p.state == EsperState::waiting && p.wait_pipe_idx < 0 &&
           p.ipc_wait_kind == Esper::IpcWaitKind::None && !p.wait_futex &&
           p.wake_cntpct == 0;
}
// PTRACE request numbers (Linux uapi/ptrace.h; aarch64 uses GETREGSET, not GETREGS).
constexpr long kPtTraceme    = 0;
constexpr long kPtPeektext   = 1;
constexpr long kPtPeekdata   = 2;
constexpr long kPtPoketext   = 4;
constexpr long kPtPokedata   = 5;
constexpr long kPtCont       = 7;
constexpr long kPtKill       = 8;
constexpr long kPtSinglestep = 9;
constexpr long kPtAttach     = 16;
constexpr long kPtDetach     = 17;
constexpr long kPtSyscall    = 24;
constexpr long kPtSetoptions = 0x4200;
constexpr long kPtGetsiginfo = 0x4202;
constexpr long kPtGetregset  = 0x4204;
constexpr long kPtSetregset  = 0x4205;
constexpr long kPtSeize      = 0x4206;
constexpr uint32_t kNtPrstatus      = 1; // GETREGSET/SETREGSET: GP register set
constexpr uint32_t kPtOTracesysgood = 1; // PTRACE_O_TRACESYSGOOD
constexpr int kSigStop = 19, kSigTrap = 5;

// The Esper this tracer (slot) is tracing that carries this pid.
Esper *mental_out_tracee_of(int tracer_idx, uint32_t pid) {
    for (uint32_t i = 0; i < kMaxEspers; ++i) {
        Esper *t = esper_at(i);
        if (t != nullptr && t->pid == pid && t->ptrace_tracer == tracer_idx &&
            t->state != EsperState::free) {
            return t;
        }
    }
    return nullptr;
}

// Syscall-stop signal: SIGTRAP, or SIGTRAP|0x80 when PTRACE_O_TRACESYSGOOD is set
// (lets the tracer distinguish syscall-stops from genuine SIGTRAPs).
int mental_out_syscall_sig(const Esper *t) {
    return (t->ptrace_options & kPtOTracesysgood) ? (kSigTrap | 0x80) : kSigTrap;
}
} // namespace

// Park the currently-running tracee `idx` in a ptrace-stop reporting `sig`, wake
// the tracer's wait4 (or SIGCHLD it), and switch to the next runnable Esper.
// rewind_elr re-executes the trapping svc on resume (syscall-entry-stop, which
// must re-run the syscall); the signal/exit stops leave elr so resume continues
// past the current point. Returns having switched away (caller just returns).
void mental_out_stop(int idx, int sig, uint32_t event, uint64_t *frame, bool rewind_elr) {
    Esper *t = esper_at(static_cast<uint32_t>(idx));
    if (t == nullptr) return;
    save_ctx(t, frame);
    if (rewind_elr) t->elr -= 4; // replay the svc on resume (syscall-entry-stop)
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    t->ptrace_stopped = true;
    t->ptrace_report = true;
    t->ptrace_stop_sig = sig;
    t->ptrace_event = event;
    t->ipc_wait_kind = Esper::IpcWaitKind::PtraceStop;
    t->ipc_wait_id = -1;
    Esper *tr = (t->ptrace_tracer >= 0)
                    ? esper_at(static_cast<uint32_t>(t->ptrace_tracer)) : nullptr;
    if (tr != nullptr) {
        if (mental_out_tracer_in_wait4(*tr)) {
            tr->regs[0] = t->pid;
            tr->pending_status = sig;
            tr->wait_status_is_stop = true;
            tr->has_pending_status = (tr->wait_status_ptr != 0);
            tr->state = EsperState::ready;
        } else {
            const uint64_t chld = tr->sig_handler[17 /*SIGCHLD*/];
            if (chld != 0 && chld != 1)
                linux_deliver_signal_locked(tr, 17, nullptr);
        }
    }
    const int next = esper_park_and_pick_locked(idx);
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    esper_kick_secondaries();
    if (next < 0) {
        leave_user();
        __builtin_unreachable();
    }
    load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
}

// Syscall-return drain hook: if the running tracee has a pending ptrace-stop
// (a signal the intercept absorbed), park it in a signal-stop. Returns true if
// it stopped (caller must just return -- we switched away).
bool mental_out_check_stop(int idx, uint64_t *frame) {
    Esper *me = esper_at(static_cast<uint32_t>(idx));
    if (me == nullptr || me->ptrace_tracer < 0) return false;
    // Read pending_stop + clear the absorbed sig_pending bit under g_esper_lock:
    // a cross-CPU signal sender mutates sig_pending under the same lock (the Mental
    // Out intercept in linux_deliver_signal_locked), so an unlocked &= here could
    // lose a concurrently-set bit. Mirrors Linux taking the sighand siglock.
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    const int sig = me->ptrace_pending_stop;
    if (sig != 0) {
        me->ptrace_pending_stop = 0;
        me->sig_pending &= ~(1ULL << (sig - 1)); // absorbed into the stop (CONT re-injects)
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    if (sig == 0) return false;
    mental_out_stop(idx, sig, 0, frame, /*rewind_elr=*/false);
    return true;
}

// PTRACE_SYSCALL entry/exit-stop. entry=true is called before the syscall runs
// (rewinds elr so the syscall re-runs on resume); entry=false after it ran.
// Returns true if it stopped.
bool mental_out_syscall_trap(int idx, uint64_t *frame, bool entry) {
    Esper *me = esper_at(static_cast<uint32_t>(idx));
    if (me == nullptr || me->ptrace_tracer < 0 || !me->ptrace_syscall) return false;
    if (entry) {
        if (me->ptrace_in_syscall) return false; // already past entry (replay)
        me->ptrace_in_syscall = true;
        mental_out_stop(idx, mental_out_syscall_sig(me), 0, frame, /*rewind_elr=*/true);
        return true;
    }
    if (!me->ptrace_in_syscall) return false;
    me->ptrace_in_syscall = false;
    mental_out_stop(idx, mental_out_syscall_sig(me), 0, frame, /*rewind_elr=*/false);
    return true;
}

// Resume a stopped tracee with a new run mode. inject_sig (CONT/SYSCALL data) is
// re-delivered to the tracee (passing the Mental Out intercept once).
static void mental_out_resume(Esper *t, bool single_step, bool syscall_mode, int inject_sig) {
    const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
    t->ptrace_stopped = false;
    t->ptrace_report = false;
    t->ptrace_singlestep = single_step;
    t->ptrace_syscall = syscall_mode;
    // A non-PTRACE_SYSCALL resume (CONT/SINGLESTEP/DETACH) ends any in-progress
    // syscall-stop sequence: the syscall runs to completion during the free run,
    // so the next PTRACE_SYSCALL must start fresh at an ENTRY stop. Without this
    // reset, CONT'ing out of an entry-stop leaves ptrace_in_syscall==true and the
    // next syscall-entry is mis-reported as an exit-stop (toggle desync).
    if (!syscall_mode) t->ptrace_in_syscall = false;
    if (inject_sig > 0 && inject_sig < 64) {
        t->ptrace_inject_sig = inject_sig;
        t->sig_pending |= (1ULL << (inject_sig - 1));
    }
    if (t->state == EsperState::waiting &&
        t->ipc_wait_kind == Esper::IpcWaitKind::PtraceStop) {
        t->ipc_wait_kind = Esper::IpcWaitKind::None;
        t->ipc_wait_id = -1;
        t->state = EsperState::ready;
    }
    anti_skill_unlock_irqrestore(g_esper_lock, flags);
    esper_kick_secondaries();
}

// ptrace(request, pid, addr, data). tracer_idx is the calling Esper's slot.
void linux_ptrace(int tracer_idx, uint64_t *frame) {
    Esper *tracer = esper_at(static_cast<uint32_t>(tracer_idx));
    if (tracer == nullptr) { frame[0] = static_cast<uint64_t>(-3); return; } // -ESRCH
    const long request = static_cast<long>(frame[0]);
    const uint32_t pid = static_cast<uint32_t>(frame[1]);
    const uint64_t addr = frame[2];
    const uint64_t data = frame[3];

    // PTRACE_TRACEME: the caller marks itself traced by its parent. No stop yet
    // -- the next signal/exec is where it first reports.
    if (request == kPtTraceme) {
        tracer->ptrace_tracer = tracer->parent;
        tracer->ptrace_in_syscall = false;
        frame[0] = 0;
        return;
    }

    // ATTACH/SEIZE create the trace relationship for a running/ready tracee.
    if (request == kPtAttach || request == kPtSeize) {
        Esper *t = nullptr;
        for (uint32_t i = 0; i < kMaxEspers; ++i) {
            Esper *c = esper_at(i);
            if (c != nullptr && c->pid == pid && c->state != EsperState::free &&
                c->state != EsperState::exited && c->state != EsperState::faulted) {
                t = c; break;
            }
        }
        if (t == nullptr) { frame[0] = static_cast<uint64_t>(-3); return; } // -ESRCH
        if (t->ptrace_tracer >= 0) { frame[0] = static_cast<uint64_t>(-1); return; } // -EPERM
        t->ptrace_tracer = tracer_idx;
        t->ptrace_in_syscall = false;
        if (request == kPtSeize) {
            t->ptrace_options = static_cast<uint32_t>(data);
        } else {
            // ATTACH delivers a SIGSTOP so the tracee stops promptly; the
            // intercept turns it into the first ptrace-stop.
            if (t->ptrace_pending_stop == 0) t->ptrace_pending_stop = kSigStop;
            t->sig_pending |= (1ULL << (kSigStop - 1));
            const uint64_t flags = anti_skill_lock_irqsave(g_esper_lock);
            if (t->state == EsperState::waiting && t->wait_pipe_idx < 0 &&
                t->ipc_wait_kind != Esper::IpcWaitKind::PtraceStop) {
                t->regs[0] = static_cast<uint64_t>(-4); // -EINTR a blocked syscall
                t->ipc_wait_kind = Esper::IpcWaitKind::None;
                t->ipc_wait_id = -1;
                t->wake_cntpct = 0;
                t->state = EsperState::ready;
            }
            anti_skill_unlock_irqrestore(g_esper_lock, flags);
            esper_kick_secondaries();
        }
        frame[0] = 0;
        return;
    }

    // All remaining requests target an established tracee identified by pid.
    Esper *t = mental_out_tracee_of(tracer_idx, pid);
    if (t == nullptr) { frame[0] = static_cast<uint64_t>(-3); return; } // -ESRCH

    // Linux requires the tracee to be ptrace-stopped for every request except
    // PTRACE_KILL: reading/writing its registers or memory, or resuming it, only
    // makes sense at a stop. Enforcing this also prevents racing a tracee that is
    // still running on another CPU (its saved regs/state would be stale).
    if (request != kPtKill && !t->ptrace_stopped) {
        frame[0] = static_cast<uint64_t>(-3); // -ESRCH
        return;
    }

    switch (request) {
    case kPtPeektext:
    case kPtPeekdata: {
        // Read one word from the tracee at addr; the raw syscall stores it to
        // *data (in the tracer's address space) and returns 0.
        uint64_t word = 0;
        if (!pr2_read_user(t, addr, &word, sizeof(word))) {
            frame[0] = static_cast<uint64_t>(-14); return; // -EFAULT
        }
        if (!pr2_write_user(tracer, data, &word, sizeof(word))) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        frame[0] = 0;
        return;
    }
    case kPtPoketext:
    case kPtPokedata: {
        const uint64_t word = data;
        frame[0] = pr2_write_user(t, addr, &word, sizeof(word))
                       ? 0 : static_cast<uint64_t>(-14);
        return;
    }
    case kPtGetregset: {
        if (addr != kNtPrstatus) { frame[0] = static_cast<uint64_t>(-22); return; } // -EINVAL
        // data -> struct iovec { void *base; size_t len }.
        uint64_t iov[2] = {0, 0};
        if (!pr2_read_user(tracer, data, iov, sizeof(iov))) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        uint64_t blob[34]; // x0..x30, sp, pc, pstate
        for (uint32_t i = 0; i < 31; ++i) blob[i] = t->regs[i];
        blob[31] = t->sp_el0;
        blob[32] = t->elr;
        blob[33] = t->spsr;
        uint64_t len = iov[1];
        if (len > sizeof(blob)) len = sizeof(blob);
        if (iov[0] != 0 && !pr2_write_user(tracer, iov[0], blob, len)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        iov[1] = len; // report bytes filled
        pr2_write_user(tracer, data, iov, sizeof(iov));
        frame[0] = 0;
        return;
    }
    case kPtSetregset: {
        if (addr != kNtPrstatus) { frame[0] = static_cast<uint64_t>(-22); return; }
        uint64_t iov[2] = {0, 0};
        if (!pr2_read_user(tracer, data, iov, sizeof(iov))) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        uint64_t blob[34];
        uint64_t len = iov[1];
        if (len > sizeof(blob)) len = sizeof(blob);
        if (iov[0] == 0 || !pr2_read_user(tracer, iov[0], blob, len)) {
            frame[0] = static_cast<uint64_t>(-14); return;
        }
        const uint32_t nwords = static_cast<uint32_t>(len / 8);
        for (uint32_t i = 0; i < nwords && i < 31; ++i) t->regs[i] = blob[i];
        if (nwords > 31) t->sp_el0 = blob[31];
        if (nwords > 32) t->elr   = blob[32];
        if (nwords > 33) {
            // SECURITY: the tracer is EL0 and supplies blob[] verbatim. Restoring
            // the tracee's PSTATE unsanitised would let any process (fork a child,
            // child PTRACE_TRACEME, parent SETREGSET) set the tracee's spsr mode
            // bits to EL1 -- on its next load_ctx eret the tracee would run at EL1
            // = EL0->EL1 privilege escalation. Mirror Linux's valid_user_regs():
            // keep only the condition flags (NZCV) and force every other bit to the
            // canonical EL0 PSTATE (kSpsrEl0 = EL0t, IRQ-enabled, AArch64).
            constexpr uint64_t kNzcvMask = 0xF0000000ULL;
            t->spsr = (blob[33] & kNzcvMask) | (kSpsrEl0 & ~kNzcvMask);
        }
        frame[0] = 0;
        return;
    }
    case kPtGetsiginfo: {
        // Minimal siginfo: si_signo = the stop signal; rest zeroed (128 bytes).
        uint8_t si[128];
        for (uint32_t i = 0; i < sizeof(si); ++i) si[i] = 0;
        *reinterpret_cast<int32_t *>(si) = t->ptrace_stop_sig;
        frame[0] = pr2_write_user(tracer, data, si, sizeof(si))
                       ? 0 : static_cast<uint64_t>(-14);
        return;
    }
    case kPtSetoptions:
        t->ptrace_options = static_cast<uint32_t>(data);
        frame[0] = 0;
        return;
    case kPtCont:
        mental_out_resume(t, /*single_step=*/false, /*syscall_mode=*/false, static_cast<int>(data));
        frame[0] = 0;
        return;
    case kPtSyscall:
        mental_out_resume(t, /*single_step=*/false, /*syscall_mode=*/true, static_cast<int>(data));
        frame[0] = 0;
        return;
    case kPtSinglestep:
        mental_out_resume(t, /*single_step=*/true, /*syscall_mode=*/false, static_cast<int>(data));
        frame[0] = 0;
        return;
    case kPtDetach:
        t->ptrace_tracer = -1;
        t->ptrace_pending_stop = 0;
        mental_out_resume(t, false, false, static_cast<int>(data));
        frame[0] = 0;
        return;
    case kPtKill:
        // Deprecated: resume the tracee with a pending SIGKILL. Inject it via
        // mental_out_resume so sig_pending is set under g_esper_lock (SIGKILL is
        // never trapped by the intercept, so it kills the tracee on resume).
        mental_out_resume(t, false, false, 9 /*SIGKILL*/);
        frame[0] = 0;
        return;
    default:
        frame[0] = static_cast<uint64_t>(-38); // -ENOSYS for unhandled requests
        return;
    }
}

extern "C" void el0_sync_dispatch(uint64_t esr, uint64_t *frame) {
    namespace district = imaginary_number_district;

    uint64_t spsr = 0;
    asm volatile("mrs %0, spsr_el1" : "=r"(spsr));
    g_last_user_el = (spsr >> 2) & 0x3;

    const uint32_t ec = static_cast<uint32_t>(esr >> kEsrEcShift);

    // ptrace "Mental Out" single-step: the EL0 instruction retired with MDSCR.SS
    // armed, so a Software Step exception (EC 0x32) re-trapped here. Disarm and
    // re-stop the tracee with SIGTRAP for the tracer to collect; the next
    // PTRACE_SINGLESTEP re-arms via load_ctx. A stray step (no stepping tracee)
    // just disarms and resumes.
    if (ec == kEcSoftStepLowerEl) {
        mental_out_arm_step(false);
        const int idx = esper_running_index();
        Esper *me = (idx >= 0) ? esper_at(static_cast<uint32_t>(idx)) : nullptr;
        if (me != nullptr && me->ptrace_tracer >= 0 && me->ptrace_singlestep) {
            me->ptrace_singlestep = false; // consumed this step
            mental_out_stop(idx, kSigTrap, 0, frame, /*rewind_elr=*/false);
        }
        return;
    }

    if (ec == kEcSvc64) {
        const int idx = esper_running_index();

        // ABI dispatch: a Linux-ABI Esper uses an entirely separate syscall
        // table (Linux AArch64 numbers; see linux_abi.cpp). The two tables
        // never share a number, so cross-ABI calls are impossible by
        // construction. Index callers continue into the switch below.
        if (idx >= 0) {
            Esper *me = esper_at(static_cast<uint32_t>(idx));
            if (me != nullptr && me->abi == Abi::Linux) {
                const uint64_t nr = frame[8];
                // ptrace "Mental Out" syscall-entry-stop: a PTRACE_SYSCALL-resumed
                // tracee stops BEFORE the syscall runs (rewinds elr so it re-runs
                // on resume). Placed ahead of the specials so clone/wait4/etc. are
                // trapped too. No-op unless this Esper is a traced tracee.
                if (mental_out_syscall_trap(idx, frame, /*entry=*/true)) return;
                // Process-control syscalls need the scheduler internals
                // (save/load ctx, pick_ready), so they're handled here rather
                // than in linux_abi.cpp. Everything else goes to the table.
                if (nr == 117 /*ptrace*/) {
                    linux_ptrace(idx, frame);
                    return;
                }
                if (nr == 220 /*clone*/) {
                    linux_clone(idx, frame);
                    return;
                }
                if (nr == 260 /*wait4*/) {
                    linux_wait4(idx, frame);
                    return;
                }
                if (nr == 98 /*futex*/) {
                    linux_futex(idx, frame);
                    return;
                }
                if (nr == 133 /*rt_sigsuspend*/) {
                    linux_rt_sigsuspend(idx, frame);
                    return;
                }
                linux_syscall_dispatch(frame);
                if (nr == 93 /*exit*/ || nr == 94 /*exit_group*/) {
                    Esper *self = esper_at(static_cast<uint32_t>(idx));
                    if (self != nullptr && self->clear_child_tid != 0) {
                        const uint64_t ctid = self->clear_child_tid;
                        const uint64_t phys = pr2_user_phys(self, ctid);
                        const uint32_t zero = 0;
                        pr2_write_user(self, ctid, &zero, 4);
                        self->clear_child_tid = 0;
                        if (phys != 0) futex_wake_phys(phys, 1);
                    }
                    // exit_group(94) kills the WHOLE thread group, not just the
                    // caller (exit(93) kills only the caller). Without this,
                    // java -version's worker (HotSpot VM thread, a CLONE_VM
                    // sibling) stays runnable after main exits and the scheduler
                    // resumes it into a stale ctx -> [EL0 FAULT] 0x44a654 and the
                    // shell never gets its prompt back. Terminate every Esper
                    // sharing this address space (same mm) the same way
                    // exit_and_schedule cleans up the caller: drop pipe refs +
                    // release the image (release_linux_image reality_unref's the
                    // shared mm), then mark it exited so it's never resumed. (java
                    // runs single-vCPU here so a sibling is never running on
                    // another core; true SMP is separately blocked by gap 12.)
                    if (nr == 94 /*exit_group*/ && self != nullptr && self->mm != nullptr) {
                        for (uint32_t i = 0; i < kMaxEspers; ++i) {
                            if (static_cast<int>(i) == idx) continue;
                            Esper *o = esper_at(i);
                            if (o != nullptr && o->mm == self->mm &&
                                o->state != EsperState::free &&
                                o->state != EsperState::exited &&
                                o->state != EsperState::faulted) {
                                release_esper_pipes(o);
                                release_linux_image(o); // reality_unref + image_unref
                                o->exit_code = static_cast<int64_t>(frame[0]);
                                o->state = EsperState::exited;
                            }
                        }
                    }
                    exit_and_schedule(idx, static_cast<int64_t>(frame[0]), frame);
                }
                // ptrace "Mental Out" post-syscall stops (no-op unless traced):
                // (1) PTRACE_SYSCALL exit-stop, then (2) a signal the intercept
                // absorbed into a pending ptrace signal-stop. Either switches away.
                if (mental_out_syscall_trap(idx, frame, /*entry=*/false)) return;
                if (mental_out_check_stop(idx, frame)) return;
                return;
            }
        }

        const uint64_t number = frame[8];
        switch (number) {
        case kSysPutc: {
            // putc is syntactic sugar for "write one byte to fd 1": route it
            // through fd 1 so Komoe-style redirection (dup2 fd1 -> pipe_write)
            // captures it. Without this, older user programs that still call
            // SYS_putc would bypass the pipe and print directly to the console
            // (so `INIT.ELF | WC.ELF` would land 0 bytes in WC).
            const char c = static_cast<char>(frame[0] & 0xff);
            if (idx < 0) {
                district::putc(c);
                frame[0] = 0;
                return;
            }
            Esper *e = esper_at(idx);
            const FdKind kind = e->fds[1].kind;
            if (kind == FdKind::console) {
                district::putc(c);
                frame[0] = 0;
            } else if (kind == FdKind::pipe_write) {
                const int pi = e->fds[1].pipe_idx;
                const int64_t n = aiwass_write(pi, &c, 1);
                if (n == kAiwassBrokenPipe) {
                    frame[0] = static_cast<uint64_t>(-1);
                } else if (n == kAiwassWouldBlock) {
                    park_on_pipe(idx, pi, /*is_write=*/true, frame);
                } else {
                    frame[0] = 0;
                    wake_pipe_readers(pi);
                }
            } else {
                frame[0] = static_cast<uint64_t>(-1);
            }
            return;
        }
        case kSysGetpid:
            frame[0] = esper_current_pid();
            return;
        case kSysYield: {
            frame[0] = 0;
            save_ctx(esper_at(idx), frame);
            // preempt_and_pick: if a different ready Esper exists, atomically
            // flip cur -> ready + pick + claim it; otherwise leave cur
            // running and return -1 (yield is a no-op).
            const int next = esper_preempt_and_pick(idx);
            if (next < 0) {
                return;
            }
            load_ctx(esper_at(next), frame);
            return;
        }
        case kSysFork: {
            if (idx < 0) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            Esper *p = esper_at(idx);
            const int child = esper_create(p->name);
            if (child < 0) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            Esper *c = esper_at(child);
            PersonalRealityV1 pr = personal_reality_fork(
                static_cast<uint32_t>(idx), static_cast<uint32_t>(child), p->code_pages);
            if (!pr.valid) {
                c->state = EsperState::free;
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            // Child inherits the parent's live EL0 context, returning 0 from fork.
            uint64_t sp_el0 = 0, elr = 0, spsr = 0;
            asm volatile("mrs %0, sp_el0" : "=r"(sp_el0));
            asm volatile("mrs %0, elr_el1" : "=r"(elr));
            asm volatile("mrs %0, spsr_el1" : "=r"(spsr));
            for (uint32_t i = 0; i < 31; ++i) {
                c->regs[i] = frame[i];
            }
            c->regs[0] = 0;
            c->sp_el0 = sp_el0;
            c->elr = elr;
            c->spsr = spsr;
            c->entry = p->entry;
            c->stack_top = p->stack_top;
            c->ttbr0 = pr.ttbr0;
            c->code_pages = pr.code_pages;
            c->started = true;
            c->parent = idx;
            c->abi = p->abi; // ABI is inherited until exec re-sniffs
            // Phase H: inherit the parent's identity (uid/gid). exec doesn't
            // toggle these unless we honour suid bits later.
            c->uid = p->uid; c->euid = p->euid; c->suid = p->suid;
            c->gid = p->gid; c->egid = p->egid; c->sgid = p->sgid;
            for (uint32_t i = 0; i < kMaxFds; ++i) {
                c->fds[i] = p->fds[i]; // child inherits open fds
                if (c->fds[i].kind == FdKind::pipe_read) {
                    aiwass_inc_read(c->fds[i].pipe_idx);
                } else if (c->fds[i].kind == FdKind::pipe_write) {
                    aiwass_inc_write(c->fds[i].pipe_idx);
                }
            }
            esper_make_ready(child); // publish; matches linux_clone's pattern
            frame[0] = c->pid; // parent gets the child's pid
            return;
        }
        case kSysExec: {
            if (idx < 0) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            char path[24];
            copy_str(path, reinterpret_cast<const char *>(frame[0]), sizeof(path));
            if (!load_elf_into_slot(static_cast<uint32_t>(idx), path)) {
                frame[0] = static_cast<uint64_t>(-1); // failed; old image intact
                return;
            }
            load_ctx(esper_at(idx), frame); // resume at the new program's entry
            return;
        }
        case kSysWait: {
            if (idx < 0) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            const uint64_t status_ptr = frame[0];
            // Atomic reap under g_esper_lock so a concurrent exit / kill
            // on another CPU can't be observed twice (same race fix as
            // linux_wait4's reap path).
            {
                const uint64_t lock_flags = anti_skill_lock_irqsave(g_esper_lock);
                uint32_t cpid = 0;
                int64_t code = 0;
                int reaped = -1;
                for (uint32_t i = 0; i < kMaxEspers; ++i) {
                    Esper *cand = esper_at(i);
                    if (cand != nullptr && cand->parent == idx &&
                        (cand->state == EsperState::exited ||
                         cand->state == EsperState::faulted)) {
                        cpid = cand->pid;
                        code = cand->exit_code;
                        cand->state = EsperState::free;
                        reaped = static_cast<int>(i);
                        break;
                    }
                }
                anti_skill_unlock_irqrestore(g_esper_lock, lock_flags);
                if (reaped >= 0) {
                    if (status_ptr != 0) {
                        // pr2_write_user: break CoW + fault in. A raw deref
                        // smashes a fork-shared CoW status page (SMP EL0 crash).
                        const int64_t st = code;
                        pr2_write_user(esper_at(static_cast<uint32_t>(idx)),
                                       status_ptr, &st, sizeof(st));
                    }
                    frame[0] = cpid;
                    return;
                }
            }
            if (esper_child_count(idx) == 0) {
                frame[0] = static_cast<uint64_t>(-1); // no children to wait on
                return;
            }
            // Block until a child exits; the exit handler wakes us with its result.
            Esper *p = esper_at(idx);
            save_ctx(p, frame);
            p->wait_status_ptr = status_ptr;
            const int next = esper_park_and_pick(idx);
            if (next < 0) {
                esper_set_running(idx); // nothing runnable; unpark and give up
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            load_ctx(esper_at(next), frame);
            return;
        }
        case kSysWrite: {
            const uint64_t fd = frame[0];
            const char *buf = reinterpret_cast<const char *>(frame[1]);
            const uint64_t len = frame[2];
            if (buf == nullptr || idx < 0 || fd >= kMaxFds) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            Esper *e = esper_at(idx);
            const FdKind kind = e->fds[fd].kind;
            if (kind == FdKind::console) {
                for (uint64_t i = 0; i < len; ++i) {
                    district::putc(buf[i]);
                }
                frame[0] = len;
            } else if (kind == FdKind::pipe_write) {
                const int pi = e->fds[fd].pipe_idx;
                const int64_t n = aiwass_write(pi, buf, len);
                if (n == kAiwassBrokenPipe) {
                    frame[0] = static_cast<uint64_t>(-1);
                } else if (n == kAiwassWouldBlock) {
                    park_on_pipe(idx, pi, /*is_write=*/true, frame);
                } else {
                    frame[0] = static_cast<uint64_t>(n);
                    wake_pipe_readers(pi); // data arrived; let readers retry
                }
            } else {
                frame[0] = static_cast<uint64_t>(-1);
            }
            return;
        }
        case kSysRead: {
            const uint64_t fd = frame[0];
            char *buf = reinterpret_cast<char *>(frame[1]);
            const uint64_t len = frame[2];
            if (buf == nullptr || idx < 0 || fd >= kMaxFds) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            Esper *e = esper_at(idx);
            const FdKind kind = e->fds[fd].kind;
            if (kind == FdKind::console) {
                frame[0] = read_console_line(buf, len);
            } else if (kind == FdKind::pipe_read) {
                const int pi = e->fds[fd].pipe_idx;
                const int64_t n = aiwass_read(pi, buf, len);
                if (n == kAiwassWouldBlock) {
                    park_on_pipe(idx, pi, /*is_write=*/false, frame);
                } else {
                    // n is 0 (EOF) or >0 (bytes); both flow back as-is.
                    frame[0] = static_cast<uint64_t>(n);
                    if (n > 0) {
                        wake_pipe_writers(pi); // space freed; writers retry
                    }
                }
            } else if (kind == FdKind::file) {
                frame[0] = read_file_fd(e, static_cast<uint32_t>(fd), buf, len);
            } else {
                frame[0] = static_cast<uint64_t>(-1);
            }
            return;
        }
        case kSysOpen: {
            if (idx < 0) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            Esper *e = esper_at(idx);
            char path[24];
            copy_str(path, reinterpret_cast<const char *>(frame[0]), sizeof(path));
            auto *probe = static_cast<char *>(dark_matter_alloc(64 * 1024));
            if (probe == nullptr) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            const int64_t n = read_named_file(path, probe, 64 * 1024);
            dark_matter_free(probe);
            if (n < 0) {
                frame[0] = static_cast<uint64_t>(-1); // not found
                return;
            }
            int fd = -1;
            for (uint32_t i = 3; i < kMaxFds; ++i) {
                if (e->fds[i].kind == FdKind::closed) {
                    fd = static_cast<int>(i);
                    break;
                }
            }
            if (fd < 0) {
                frame[0] = static_cast<uint64_t>(-1); // fd table full
                return;
            }
            e->fds[fd].kind = FdKind::file;
            copy_str(e->fds[fd].path, path, sizeof(e->fds[fd].path));
            e->fds[fd].off = 0;
            frame[0] = static_cast<uint64_t>(fd);
            return;
        }
        case kSysClose: {
            const uint64_t fd = frame[0];
            if (idx < 0 || fd >= kMaxFds) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            Esper *e = esper_at(idx);
            if (e->fds[fd].kind == FdKind::closed) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            // release_esper_fd handles pipe refs + wakes the other side; for
            // file/console it just clears the slot.
            release_esper_fd(e, static_cast<uint32_t>(fd));
            frame[0] = 0;
            return;
        }
        case kSysPipe: {
            if (idx < 0) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            Esper *e = esper_at(idx);
            int *out = reinterpret_cast<int *>(frame[0]);
            if (out == nullptr) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            int rfd = -1, wfd = -1;
            for (uint32_t i = 3; i < kMaxFds; ++i) {
                if (e->fds[i].kind == FdKind::closed) {
                    if (rfd < 0) {
                        rfd = static_cast<int>(i);
                    } else {
                        wfd = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (rfd < 0 || wfd < 0) {
                frame[0] = static_cast<uint64_t>(-1); // fd table full
                return;
            }
            const int pi = aiwass_create();
            if (pi < 0) {
                frame[0] = static_cast<uint64_t>(-1); // no pipe slot / no heap
                return;
            }
            e->fds[rfd] = Fd{};
            e->fds[rfd].kind = FdKind::pipe_read;
            e->fds[rfd].pipe_idx = pi;
            e->fds[wfd] = Fd{};
            e->fds[wfd].kind = FdKind::pipe_write;
            e->fds[wfd].pipe_idx = pi;
            out[0] = rfd;
            out[1] = wfd;
            frame[0] = 0;
            return;
        }
        case kSysDup: {
            if (idx < 0) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            Esper *e = esper_at(idx);
            const uint64_t oldfd = frame[0];
            if (oldfd >= kMaxFds || e->fds[oldfd].kind == FdKind::closed) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            int newfd = -1;
            for (uint32_t i = 0; i < kMaxFds; ++i) {
                if (e->fds[i].kind == FdKind::closed) {
                    newfd = static_cast<int>(i);
                    break;
                }
            }
            if (newfd < 0) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            e->fds[newfd] = e->fds[oldfd];
            if (e->fds[newfd].kind == FdKind::pipe_read) {
                aiwass_inc_read(e->fds[newfd].pipe_idx);
            } else if (e->fds[newfd].kind == FdKind::pipe_write) {
                aiwass_inc_write(e->fds[newfd].pipe_idx);
            }
            frame[0] = static_cast<uint64_t>(newfd);
            return;
        }
        case kSysDup2: {
            if (idx < 0) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            Esper *e = esper_at(idx);
            const uint64_t oldfd = frame[0];
            const uint64_t newfd = frame[1];
            if (oldfd >= kMaxFds || newfd >= kMaxFds ||
                e->fds[oldfd].kind == FdKind::closed) {
                frame[0] = static_cast<uint64_t>(-1);
                return;
            }
            if (oldfd == newfd) {
                frame[0] = newfd;
                return;
            }
            // Drop any prior occupant of newfd (with pipe-ref bookkeeping).
            release_esper_fd(e, static_cast<uint32_t>(newfd));
            e->fds[newfd] = e->fds[oldfd];
            if (e->fds[newfd].kind == FdKind::pipe_read) {
                aiwass_inc_read(e->fds[newfd].pipe_idx);
            } else if (e->fds[newfd].kind == FdKind::pipe_write) {
                aiwass_inc_write(e->fds[newfd].pipe_idx);
            }
            frame[0] = newfd;
            return;
        }
        case kSysKill: {
            // kill(pid, sig): name an Esper by pid and have Fortis931 end it.
            // MVP only supports default termination; signal handlers are TBD.
            const uint64_t pid = frame[0];
            const uint64_t sig = frame[1];
            if (pid == 0) {
                frame[0] = static_cast<uint64_t>(-1); // pid 0 reserved
                return;
            }
            int target = -1;
            for (uint32_t i = 0; i < kMaxEspers; ++i) {
                Esper *cand = esper_at(i);
                if (cand != nullptr && cand->pid == pid &&
                    cand->state != EsperState::free &&
                    cand->state != EsperState::exited &&
                    cand->state != EsperState::faulted) {
                    target = static_cast<int>(i);
                    break;
                }
            }
            if (target < 0) {
                frame[0] = static_cast<uint64_t>(-1); // no such pid
                return;
            }
            const bool killed_self = (target == idx);
            fortis931_kill(target, static_cast<int>(sig));
            if (killed_self) {
                // We just torn down our own state; we can't return to user.
                // pick_and_claim overwrites g_running[cpu] for us.
                const int next = esper_pick_and_claim(idx);
                if (next < 0) {
                    leave_user();
                }
                load_ctx(esper_at(next), frame);
                return;
            }
            frame[0] = 0;
            return;
        }
        case kSysExit: {
            const int64_t code = static_cast<int64_t>(frame[0]);
            exit_and_schedule(idx, code, frame); // shared with Linux exit/exit_group
            return;
        }
        default:
            frame[0] = static_cast<uint64_t>(-1);
            return;
        }
    }

    const int idx = esper_running_index();
    uint64_t elr = 0;
    uint64_t far = 0;
    asm volatile("mrs %0, elr_el1" : "=r"(elr));
    asm volatile("mrs %0, far_el1" : "=r"(far));

    // Phase B: data/instruction aborts from EL0 first get a chance at the
    // VMA-driven page-fault handler. If the faulting VA falls inside a VMA,
    // pr2_handle_fault installs the missing page and we return -- the eret
    // re-runs the original instruction and it succeeds. Only a miss falls
    // through to the existing "terminate the Esper" path.
    if (idx >= 0 && (ec == kEcDataAbortLowerEl || ec == kEcInstrAbortLowerEl)) {
        Esper *cur = esper_at(static_cast<uint32_t>(idx));
        if (cur != nullptr && cur->abi == Abi::Linux) {
            // Fault-loop guard: pr2_handle_fault can "resolve" a fault (return
            // true) that the eret then RE-faults on -- an unaligned access or a
            // write to a page we leave read-only. That spins forever (100% CPU,
            // silent) instead of progressing. If the SAME address keeps faulting,
            // stop retrying: diagnose the ESR once and fall through to terminate
            // (Linux would SIGSEGV/SIGBUS it). [FAULTLOOP] is a JVM-bringup probe.
            static uint64_t lf = 0; static uint32_t ln = 0;
            ln = (far == lf) ? (ln + 1) : 0; lf = far;
            if (ln < 4000) {
                if (pr2_handle_fault(cur, far)) return; // page populated; retry on eret
            } else if (ln == 4000) {
                district::write("\n[FAULTLOOP] far="); district::hex(far);
                district::write(" esr="); district::hex(esr);
                district::write(" elr="); district::hex(elr); district::writeln("");
                // fall through to the [EL0 FAULT] terminate path -> breaks the spin
            }
        }
    }

    district::write("\n[EL0 FAULT] ESR ");
    district::hex(esr);
    district::write(" ELR ");
    district::hex(elr);
    district::write(" FAR ");
    district::hex(far);
    if (idx >= 0) {
        Esper *bad = esper_at(static_cast<uint32_t>(idx));
        if (bad != nullptr) {
            district::write(" pid ");
            district::dec(bad->pid);
            district::write(" name ");
            district::write(bad->name);
            // [gap12 diag] regs at fault: x30(LR)==ELR ⇒ ret to a smashed return
            // address (stack wild-write); else a bad branch register. Localizes
            // the HVF-SMP clone-thread wild-write source.
            // [WD] Walk the faulting VA to expose page-table corruption: a
            // level-1 perm fault whose L1 entry is garbage / a reused page is
            // the signature of a use-after-free of a table page under SMP.
            if (ec == kEcInstrAbortLowerEl || ec == kEcDataAbortLowerEl) {
                district::writeln("");
                pr2_dump_walk(bad->ttbr0, far);
                pr2_dump_vma(bad, far); // [JVMDIAG] VMA prot for the faulting va
                pr2_dump_all_vmas(bad); // [JVMDIAG] full VMA list (thread divergence?)
            }
        }
    }
    district::writeln("\n  Esper terminated; kernel survives.");
    Esper *bad2 = (idx >= 0) ? esper_at(static_cast<uint32_t>(idx)) : nullptr;
    PersonalReality *gmm = (bad2 != nullptr) ? bad2->mm : nullptr;
    if (gmm != nullptr) {
        // A fatal fault (SIGSEGV) in one CLONE_VM thread takes down the WHOLE
        // thread group on Linux. Mark every sibling (same mm) exited + wake their
        // wait4 parents, then release each sibling's resources. Order matters:
        // the group-exit (which matches on mm) MUST run before release_linux_image
        // (which reality_unref's, clearing o->mm). Without this, java -version's
        // VM thread faulting on shutdown left the main thread blocked in join --
        // or, once woken, resumed into the freed address space (vma_count=0). Now
        // the whole JVM process exits and the shell gets its prompt back.
        const int next = esper_group_exit_and_pick(idx, static_cast<const void *>(gmm),
                                                   128 + 11 /*SIGSEGV*/);
        for (uint32_t i = 0; i < kMaxEspers; ++i) {
            Esper *o = esper_at(i);
            if (o != nullptr && o->mm == gmm && o->state == EsperState::exited) {
                release_esper_pipes(o);
                release_linux_image(o); // reality_unref (clears o->mm) + image_unref
            }
        }
        if (next < 0) {
            leave_user();
        }
        load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
        return;
    }
    if (idx >= 0) {
        release_esper_pipes(esper_at(idx)); // pipes get EOF/EPIPE before the slot is recycled
        release_linux_image(esper_at(idx)); // free retained ELF buffer (if Linux ABI)
    }
    const int next = esper_fault_and_pick(idx);
    if (next < 0) {
        leave_user();
    }
    load_ctx(esper_at(static_cast<uint32_t>(next)), frame);
}

void linux_exit_running(int idx, int64_t code, uint64_t *frame) {
    exit_and_schedule(idx, code, frame);
}

} // namespace index
