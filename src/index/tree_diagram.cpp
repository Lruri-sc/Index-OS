#include "index/tree_diagram.hpp"

#include "index/anti_skill.hpp"
#include "index/imaginary_number_district.hpp" // [WD] double-free reporting
#include "index/teleport.hpp" // kHighHalfBase, for accessing freed pages
#include "index/types.hpp"

namespace index {

namespace {

AntiSkill g_lock; // serializes page handout across cores

// [WD] Stamped at slot+8 of a freed page; cleared when the page is handed out.
// Seeing it still present at free time means the page is being freed while
// already on the free list = double-free = the same physical page is owned by
// two parties = the SMP memory corruption we're hunting.
constexpr uint64_t kFreeMagic = 0xDEADBEEFCAFEF00DULL;

// A freed page stores the next free page's physical address in its first 8
// bytes. The page is only reachable through the kernel's TTBR1 mapping once a
// per-process TTBR0 is live, so always go through the high-half alias.
uint64_t *free_slot(uint64_t phys) {
    return reinterpret_cast<uint64_t *>(phys | kHighHalfBase);
}

struct ReservedSpan {
    uint64_t start = 0;
    uint64_t end = 0;
};

void sort_reserved(ReservedSpan *spans, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = i + 1; j < count; ++j) {
            if (spans[j].start < spans[i].start) {
                const ReservedSpan tmp = spans[i];
                spans[i] = spans[j];
                spans[j] = tmp;
            }
        }
    }
}

void add_range(TreeDiagram &diagram, uint64_t start, uint64_t end) {
    start = align_up(start, kTreeDiagramPageSize);
    end = align_down(end, kTreeDiagramPageSize);

    if (start >= end || diagram.range_count >= kMaxTreeDiagramRanges) {
        return;
    }

    diagram.ranges[diagram.range_count++] = TreeDiagramRange{start, start, end};
    diagram.total_pages += (end - start) / kTreeDiagramPageSize;
    diagram.ready = true;
}

void carve_range(TreeDiagram &diagram, uint64_t start, uint64_t end,
                 const ReservedSpan *reserved, uint32_t reserved_count) {
    uint64_t cursor = align_up(start, kTreeDiagramPageSize);
    const uint64_t limit = align_down(end, kTreeDiagramPageSize);

    for (uint32_t i = 0; i < reserved_count && cursor < limit; ++i) {
        uint64_t res_start = align_down(reserved[i].start, kTreeDiagramPageSize);
        uint64_t res_end = align_up(reserved[i].end, kTreeDiagramPageSize);

        if (res_end <= cursor || res_start >= limit) {
            continue;
        }

        if (res_start > cursor) {
            add_range(diagram, cursor, res_start);
        }

        if (res_end > cursor) {
            cursor = res_end;
        }
    }

    if (cursor < limit) {
        add_range(diagram, cursor, limit);
    }
}

} // namespace

void TreeDiagram::init(const ArtificialHeaven &heaven, uint64_t image_start, uint64_t image_end,
                       uint64_t early_heap_start, uint64_t early_heap_end) {
    ReservedSpan reserved[kMaxAIMDiffusionRanges + 3];
    uint32_t reserved_count = 0;

    if (image_start < image_end) {
        reserved[reserved_count++] = ReservedSpan{image_start, image_end};
    }

    if (heaven.dtb_size != 0) {
        reserved[reserved_count++] = ReservedSpan{heaven.dtb_addr, heaven.dtb_addr + heaven.dtb_size};
    }

    if (early_heap_start < early_heap_end) {
        reserved[reserved_count++] = ReservedSpan{early_heap_start, early_heap_end};
    }

    for (uint32_t i = 0; i < heaven.aim_field_count &&
                         reserved_count < kMaxAIMDiffusionRanges + 3; ++i) {
        const uint64_t start = heaven.aim_diffusion_field[i].base;
        const uint64_t end = start + heaven.aim_diffusion_field[i].size;
        if (end > start) {
            reserved[reserved_count++] = ReservedSpan{start, end};
        }
    }

    sort_reserved(reserved, reserved_count);

    for (uint32_t i = 0; i < heaven.memory_count; ++i) {
        uint64_t start = heaven.memory[i].base;
        const uint64_t end = start + heaven.memory[i].size;
        if (image_start < end && image_end > start) {
            start = image_end;
        }
        if (end > start) {
            carve_range(*this, start, end, reserved, reserved_count);
        }
    }
}

void *TreeDiagram::allocate_page() {
    if (!ready) {
        return nullptr;
    }

    const uint64_t flags = anti_skill_lock_irqsave(g_lock);
    void *page = nullptr;
    // Reclaimed pages first: pop the free-list head, reading its stored "next".
    if (free_list != 0) {
        const uint64_t pa = free_list;
        free_list = *free_slot(pa);
        free_slot(pa)[1] = 0; // [WD] clear the free magic now it's owned again
        ++used_pages;
        page = reinterpret_cast<void *>(pa);
    } else {
        for (uint32_t i = 0; i < range_count; ++i) {
            TreeDiagramRange &range = ranges[i];
            if (range.current + kTreeDiagramPageSize <= range.end) {
                page = reinterpret_cast<void *>(range.current);
                range.current += kTreeDiagramPageSize;
                ++used_pages;
                break;
            }
        }
    }
    anti_skill_unlock_irqrestore(g_lock, flags);
    return page;
}

void TreeDiagram::free_page(uint64_t phys) {
    if (phys == 0) {
        return;
    }
    phys &= ~(kTreeDiagramPageSize - 1); // page-align
    const uint64_t flags = anti_skill_lock_irqsave(g_lock);
    if (free_slot(phys)[1] == kFreeMagic) { // [WD] already free -> double-free
        imaginary_number_district::write("[DBLFREE] pa=");
        imaginary_number_district::hex(phys);
        imaginary_number_district::write("\n");
    }
    *free_slot(phys) = free_list; // push onto the free-list stack (slot+0 = next)
    free_slot(phys)[1] = kFreeMagic; // mark free
    free_list = phys;
    if (used_pages > 0) {
        --used_pages;
    }
    anti_skill_unlock_irqrestore(g_lock, flags);
}

uint64_t TreeDiagram::available_pages() const {
    return total_pages >= used_pages ? total_pages - used_pages : 0;
}

namespace {
TreeDiagram *g_global = nullptr;
} // namespace

void tree_diagram_set_global(TreeDiagram *tree) {
    g_global = tree;
}

void *tree_diagram_alloc_page() {
    return g_global != nullptr ? g_global->allocate_page() : nullptr;
}

void tree_diagram_free_page(uint64_t phys) {
    if (g_global != nullptr) {
        g_global->free_page(phys);
    }
}

uint64_t tree_diagram_total_pages() {
    return g_global != nullptr ? g_global->total_pages : 0;
}

uint64_t tree_diagram_used_pages() {
    return g_global != nullptr ? g_global->used_pages : 0;
}

} // namespace index
