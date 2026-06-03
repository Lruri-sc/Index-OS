#include "index/index_librorum_prohibitorum.hpp"

#include "index/anti_skill.hpp"
#include "index/fdt.hpp"
#include "index/types.hpp"

namespace index {

namespace {

constexpr uint64_t kIndexLibrorumLimit = mib(2);

AntiSkill g_lock; // serializes the early bump cursor across cores

uint64_t cap_early_heap(uint64_t start, uint64_t limit) {
    const uint64_t capped = start + kIndexLibrorumLimit;
    if (capped < start || capped > limit) {
        return limit;
    }
    return capped;
}

bool overlaps(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
    return a_start < b_end && b_start < a_end;
}

void skip_aim_diffusion_field(const ArtificialHeaven &heaven, uint64_t &cursor, uint64_t limit) {
    bool moved = true;
    while (moved) {
        moved = false;
        for (uint32_t i = 0; i < heaven.aim_field_count; ++i) {
            const uint64_t res_start = heaven.aim_diffusion_field[i].base;
            const uint64_t res_end = res_start + heaven.aim_diffusion_field[i].size;
            if (res_end > res_start && overlaps(cursor, cursor + 1, res_start, res_end)) {
                cursor = align_up(res_end, kib(64));
                if (cursor >= limit) {
                    return;
                }
                moved = true;
            }
        }
    }
}

uint64_t next_aim_diffusion_start(const ArtificialHeaven &heaven, uint64_t cursor, uint64_t limit) {
    uint64_t next = limit;
    for (uint32_t i = 0; i < heaven.aim_field_count; ++i) {
        const uint64_t res_start = heaven.aim_diffusion_field[i].base;
        const uint64_t res_end = res_start + heaven.aim_diffusion_field[i].size;
        if (res_end > res_start && res_start > cursor && res_start < next) {
            next = res_start;
        }
    }
    return next;
}

} // namespace

void IndexLibrorumProhibitorum::init(const ArtificialHeaven &heaven, uint64_t kernel_end) {
    const uint64_t dtb_end = heaven.dtb_addr + heaven.dtb_size;

    for (uint32_t i = 0; i < heaven.memory_count; ++i) {
        const uint64_t base = heaven.memory[i].base;
        const uint64_t end_addr = base + heaven.memory[i].size;

        uint64_t cursor = align_up(base, kib(64));
        const uint64_t limit = align_down(end_addr, kib(64));

        if (kernel_end > base && kernel_end < end_addr) {
            cursor = align_up(kernel_end, kib(64));
        }

        uint64_t span_limit = limit;
        if (heaven.dtb_size != 0 && heaven.dtb_addr < limit && dtb_end > cursor) {
            const uint64_t before_dtb = align_down(heaven.dtb_addr, kib(64));
            if (cursor < before_dtb) {
                span_limit = before_dtb;
            } else {
                cursor = align_up(dtb_end, kib(64));
            }
        }

        skip_aim_diffusion_field(heaven, cursor, span_limit);

        if (cursor < span_limit) {
            const uint64_t reserved_limit = next_aim_diffusion_start(heaven, cursor, span_limit);
            start = cursor;
            current = start;
            end = cap_early_heap(start, reserved_limit);
            ready = current < end;
            return;
        }
    }
}

void *IndexLibrorumProhibitorum::allocate(uint64_t bytes, uint64_t alignment) {
    if (!ready || alignment == 0) {
        return nullptr;
    }

    const uint64_t flags = anti_skill_lock_irqsave(g_lock);
    void *result = nullptr;
    const uint64_t next = align_up(current, alignment);
    const uint64_t after = next + bytes;
    if (after >= next && after <= end) {
        current = after;
        result = reinterpret_cast<void *>(next);
    }
    anti_skill_unlock_irqrestore(g_lock, flags);
    return result;
}

uint64_t IndexLibrorumProhibitorum::used() const {
    return current >= start ? current - start : 0;
}

uint64_t IndexLibrorumProhibitorum::available() const {
    return end >= current ? end - current : 0;
}

} // namespace index
