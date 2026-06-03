#pragma once

#include <stdint.h>

namespace index {

// DarkMatter: Kakine Teitoku's ability to create substance from nothing and
// dissolve it back at will. Here it is the kernel heap -- a first-fit free-list
// allocator that hands out arbitrary-sized blocks (kmalloc) and reclaims them
// (kfree), coalescing neighbours so freed space is reusable. Unlike the bump
// allocator (IndexLibrorumProhibitorum) and page allocator (TreeDiagram), this
// one can give memory back.
struct DarkMatterStats {
    uint64_t arena_size = 0;
    uint64_t used = 0;        // payload bytes currently allocated
    uint64_t free_bytes = 0;  // payload bytes currently free
    uint64_t allocations = 0; // lifetime alloc calls that succeeded
    uint64_t frees = 0;       // lifetime free calls
    uint32_t blocks = 0;      // total blocks (free + used)
    uint32_t free_blocks = 0; // currently free blocks
};

void dark_matter_init();
void *dark_matter_alloc(uint64_t size);
void dark_matter_free(void *ptr);
DarkMatterStats dark_matter_stats();

} // namespace index
