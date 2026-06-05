#pragma once

#include <stdint.h>

namespace index {

// PersonalRealityV1 (パーソナルリアリティ): the private, self-consistent world that
// is the source of an esper's power -- each esper's own reality. Here it is a
// per-process address space: a private TTBR0 page table that maps the process's
// code and stack at fixed low virtual addresses, backed by physical pages no
// other process can see. Two Espers can both run at the same VA yet have
// completely separate memory.
constexpr uint64_t kUserCodeBase = 0x10000;   // where a process's image loads
constexpr uint64_t kUserStackTop = 0x40000;   // initial SP_EL0 (grows down)

struct PersonalRealityV1 {
    uint64_t ttbr0 = 0;       // physical address of the L1 table
    uint64_t code_phys = 0;   // physical page backing kUserCodeBase (for `ps`)
    uint32_t code_pages = 0;  // code pages mapped (so fork knows what to copy)
    bool valid = false;
};

// Build a private address space for Esper `slot`, loading the flat program image
// `image` (size `image_size`, already laid out at kUserCodeBase) into private
// pages and mapping a stack. Returns a PersonalRealityV1 (valid=false on failure).
PersonalRealityV1 personal_reality_build(uint32_t slot, const uint8_t *image,
                                       uint64_t image_size);

// Clone parent_slot's address space into child_slot (fork): copy the parent's
// code+stack pages into the child slot's pool pages and build the child's page
// tables mapping them at the same VAs. `code_pages` is the parent's code page
// count. Eager whole-copy -- no sharing, so the child is fully independent.
PersonalRealityV1 personal_reality_fork(uint32_t parent_slot, uint32_t child_slot,
                                      uint32_t code_pages);

} // namespace index
