#pragma once

#include <stdint.h>

namespace index {

struct Esper;
struct PersonalReality;

// PersonalReality v2: a per-process address space backed by a VMA list and a
// demand-paged page-table walker. Unlike the v1 pool (fixed 28 KiB code +
// 16 KiB stack at hard-coded VAs) this can host arbitrary Linux-style
// layouts -- code at 0x400000, stack at the top of the 39-bit window, mmap
// regions in between. L1 lives in a TreeDiagram-allocated physical page;
// L2/L3 tables are lazily allocated on the first fault into their region.
//
// Phase B uses this only for Linux Espers; Index Espers still go through
// personal_reality_build() in personal_reality.cpp. The split lets us
// add full VM semantics without touching the working Index code path.

// Address-space (PersonalReality / mm) lifecycle -- the Index analogue of
// Linux mm_struct refcounting. reality_alloc grabs a fresh PersonalReality from
// the pool (refs=1, empty VMA list); reality_ref bumps the sharer count when a
// clone(CLONE_VM) thread joins; reality_unref drops e's reference and, if it
// was the last, switches TTBR0 to the kernel table and reclaims the page tables
// (pr2_destroy) before returning the slot. reality_unref also clears e->mm.
// reality_alloc returns nullptr if the pool is exhausted.
PersonalReality *reality_alloc();
void reality_ref(PersonalReality *mm);
void reality_unref(Esper *e);
uint32_t reality_active_count(); // diagnostics (procfs)

// Build a fresh address space into e->mm (which the caller must have already
// reality_alloc'd): allocate L1, inherit the kernel's mappings (so the kernel's
// own low-VA stack/data stay reachable at EL1 while this TTBR0 is loaded), and
// set both mm->ttbr0 and the e->ttbr0 fast-path cache to the new L1's physical
// address. Returns false on out-of-physical-memory.
bool pr2_create_addr_space(Esper *e);

// Append a VMA to the Esper's list. `seg_vaddr` is the segment's true VA
// (== start if the segment is naturally page-aligned). For PIE ELFs whose
// PT_LOAD vaddrs are not 4 KiB-aligned, the VMA is page-aligned down and
// the first (seg_vaddr - start_page_aligned) bytes are pre-pad (zero, not
// read from the file). Does NOT allocate physical pages; faults populate
// them lazily.
bool pr2_add_vma(Esper *e, uint64_t start, uint64_t end, uint8_t prot,
                 uint8_t kind, const uint8_t *file_src, uint64_t file_off,
                 uint64_t file_size, uint64_t seg_vaddr,
                 const char *file_path = nullptr);

// Try to service a translation fault at `far` for this Esper. Returns true
// iff a matching VMA was found and the page was successfully installed; the
// caller can then return to user mode and the access will retry. Returns
// false on a miss (real segfault -- terminate the process).
bool pr2_handle_fault(Esper *e, uint64_t far);

// Pre-install every page covering [va, va+len). Used by syscall handlers in
// linux_abi.cpp before they dereference a user buffer, because the kernel's
// EL1 access would itself trigger an EL1 data abort (EC=0x25) that nobody
// handles -- pre-faulting ensures every page is already in the user's L3
// table by the time the kernel reads from it. Returns true iff every page
// in the range belongs to a VMA and is now installed.
bool pr2_prefault_range(Esper *e, uint64_t va, uint64_t len);

// True iff [lo, hi) overlaps no live VMA in e's address space -- used by mmap to
// honor a non-NULL address hint (Linux places the mapping at the hint if free).
bool pr2_range_free(const Esper *e, uint64_t lo, uint64_t hi);

// Copy `n` bytes from a kernel buffer into the Esper's user VA `va`, faulting
// in pages as needed and writing through each page's physical (high-half)
// alias -- so it works regardless of which TTBR0 is currently loaded. Used by
// the Linux startup-stack builder, which runs before the process's address
// space is the live TTBR0. Returns false if any page is outside a VMA.
bool pr2_write_user(Esper *e, uint64_t va, const void *src, uint64_t n);

// Read n bytes from e's user address space into kernel dst (mirror of
// pr2_write_user; used by ptrace PEEK to read a tracee's memory). False if any
// byte is unmapped.
bool pr2_read_user(Esper *e, uint64_t va, void *dst, uint64_t n);

// Resolve a user VA to its backing physical address (faulting the page in),
// or 0 if it isn't in any VMA. Used by futex to key its wait queue on the
// physical page so threads sharing an address space rendezvous correctly.
uint64_t pr2_user_phys(Esper *e, uint64_t va);

// mprotect: change the protection of [va, va+len) to `prot` (kVmaProtR/W/X
// bits). Updates the protection of any VMA that overlaps the range and
// rewrites the page-table entries of already-mapped pages so the new
// permissions take effect immediately. Needed because musl maps a thread
// stack and then mprotects it writable -- ignoring that leaves the stack
// read-only and write faults loop forever.
void pr2_mprotect(Esper *e, uint64_t va, uint64_t len, uint8_t prot);

// Punch a hole [lo, hi) out of the address space: invalidate or truncate any
// overlapping VMA and clear the L3 page table entries in range. Called by
// mmap with MAP_FIXED before installing the new mapping, since ld-musl
// expects MAP_FIXED to displace whatever was there.
void pr2_remove_vma_range(Esper *e, uint64_t lo, uint64_t hi);

// Tear down an address space (PersonalReality), returning every physical page
// to TreeDiagram: each leaf data page is page_unref'd (freed when its last
// sharer drops, so CoW-shared pages stay alive for the other process) and every
// per-process page-table page (L1/L2/L3) is freed. Inherited kernel L1 entries
// are left untouched. Sets mm->ttbr0 = 0. Idempotent. Normally reached via
// reality_unref when the last sharer drops; callers must ensure no other Esper
// still references this PersonalReality.
void pr2_destroy(PersonalReality *mm);

// fork() for a VMA-backed Linux Esper: reality_alloc a brand-new PersonalReality
// for `child` that is an independent copy of `parent`'s. Copies the VMA list,
// brk/mmap bookkeeping, and every currently-mapped page into fresh child pages
// (so parent and child never share writable memory); unmapped file/anon pages
// fault in independently on first touch. The caller copies sig/elf bookkeeping
// and the saved EL0 context separately. Returns false on OOM / VMA overflow.
bool pr2_fork(Esper *parent, Esper *child);

// [WD] Diagnostic: walk `l1_pa` (an Esper's ttbr0) for `va` and print the
// L1/L2/L3 descriptors. PA-bounds-checked so a corrupted descriptor cannot make
// the dumper itself fault. Called from the fatal EL0 fault path.
void pr2_dump_walk(uint64_t l1_pa, uint64_t va);

// [JVMDIAG] dump the VMA (prot/kind/range) covering `va`, for diagnosing a
// write fault that loops (is the VMA read-only by design or wrongly mapped?).
struct Esper;
void pr2_dump_vma(Esper *e, uint64_t va);
void pr2_dump_all_vmas(Esper *e);

} // namespace index
