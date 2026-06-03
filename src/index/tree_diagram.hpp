#pragma once

#include <stdint.h>

#include "index/artificial_heaven.hpp"

namespace index {

constexpr uint32_t kMaxTreeDiagramRanges = 32;
constexpr uint64_t kTreeDiagramPageSize = 4096;

struct TreeDiagramRange {
    uint64_t start = 0;
    uint64_t current = 0;
    uint64_t end = 0;
};

struct TreeDiagram {
    TreeDiagramRange ranges[kMaxTreeDiagramRanges];
    uint32_t range_count = 0;
    uint64_t total_pages = 0;
    uint64_t used_pages = 0;
    bool ready = false;
    // Physical address of the first reclaimed page, or 0 if the free list is
    // empty. Freed pages form a singly-linked stack whose "next" pointer is
    // stored in the page itself (read/written via its high-half alias). This
    // lets per-process pages be returned on exit instead of leaking, so the
    // machine can keep starting new programs indefinitely.
    uint64_t free_list = 0;

    void init(const ArtificialHeaven &heaven, uint64_t image_start, uint64_t image_end,
              uint64_t early_heap_start, uint64_t early_heap_end);
    void *allocate_page();      // free list first, then bump
    void free_page(uint64_t phys); // return a 4 KiB page to the free list
    uint64_t available_pages() const;
};

// Kernel-wide accessor: kmain calls tree_diagram_set_global(&tree) once the
// boot core has its TreeDiagram instance, after which any subsystem (Phase B
// PersonalReality v2, future fault handlers, etc.) can ask for a 4 KiB page
// without threading the instance through every call.
void tree_diagram_set_global(TreeDiagram *tree);
void *tree_diagram_alloc_page();
void tree_diagram_free_page(uint64_t phys);

// Lightweight read-side accessors used by /proc/meminfo and reporting. Each
// returns 0 if the global tree hasn't been installed yet (very early boot).
uint64_t tree_diagram_total_pages();
uint64_t tree_diagram_used_pages();

} // namespace index
