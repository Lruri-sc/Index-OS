#include "index/dark_matter.hpp"

#include "index/anti_skill.hpp"
#include "index/types.hpp"

namespace index {

namespace {

// 64 MiB kernel heap. Each dynamically-linked Linux exec needs ~2 MiB
// transient ELF read buffer + ~1.1 MiB resident main + ld-musl + libstdc++
// (~600 KiB) + libgcc resident. With multi-core EL0 several heavy C++
// programs now exec CONCURRENTLY (one per core), so the transient + resident
// peak scales with core count: 4x mt_cxx at once overran the old 16 MiB
// ("exec: out of heap"). 64 MiB gives all 8 cores headroom on the 1 GiB
// machine. (This is a capacity limit, not a leak -- /proc/index_resources
// shows the heap plateauing once the programs are resident.)
constexpr uint64_t kArenaSize = 64 * 1024 * 1024;

AntiSkill g_lock; // serializes the free list across cores

// 16-byte block header so payloads stay 16-byte aligned. Blocks tile the arena
// contiguously (an implicit free list): a block's neighbour sits immediately
// after its payload.
struct Block {
    uint64_t size; // payload bytes
    uint64_t free; // 1 if free, 0 if allocated
};

alignas(16) uint8_t g_arena[kArenaSize];
bool g_ready = false;
uint64_t g_allocations = 0;
uint64_t g_frees = 0;

Block *first_block() {
    return reinterpret_cast<Block *>(g_arena);
}

Block *next_block(Block *b) {
    uint8_t *p = reinterpret_cast<uint8_t *>(b) + sizeof(Block) + b->size;
    if (p >= g_arena + kArenaSize) {
        return nullptr;
    }
    return reinterpret_cast<Block *>(p);
}

// Merge each free block with the run of free blocks that follow it.
void coalesce() {
    for (Block *b = first_block(); b != nullptr; b = next_block(b)) {
        if (!b->free) {
            continue;
        }
        Block *n = next_block(b);
        while (n != nullptr && n->free) {
            b->size += sizeof(Block) + n->size;
            n = next_block(b);
        }
    }
}

} // namespace

void dark_matter_init() {
    Block *b = first_block();
    b->size = kArenaSize - sizeof(Block);
    b->free = 1;
    g_ready = true;
}

void *dark_matter_alloc(uint64_t size) {
    if (!g_ready || size == 0) {
        return nullptr;
    }
    size = align_up(size, 16);

    const uint64_t flags = anti_skill_lock_irqsave(g_lock);
    void *result = nullptr;
    for (Block *b = first_block(); b != nullptr; b = next_block(b)) {
        if (!b->free || b->size < size) {
            continue;
        }
        // Split if the leftover can hold a header plus a minimum payload.
        if (b->size >= size + sizeof(Block) + 16) {
            Block *split = reinterpret_cast<Block *>(
                reinterpret_cast<uint8_t *>(b) + sizeof(Block) + size);
            split->size = b->size - size - sizeof(Block);
            split->free = 1;
            b->size = size;
        }
        b->free = 0;
        ++g_allocations;
        result = reinterpret_cast<uint8_t *>(b) + sizeof(Block);
        break;
    }
    anti_skill_unlock_irqrestore(g_lock, flags);
    return result; // nullptr if out of heap
}

void dark_matter_free(void *ptr) {
    if (!g_ready || ptr == nullptr) {
        return;
    }
    uint8_t *p = static_cast<uint8_t *>(ptr);
    if (p < g_arena + sizeof(Block) || p >= g_arena + kArenaSize) {
        return; // not one of ours
    }
    const uint64_t flags = anti_skill_lock_irqsave(g_lock);
    Block *b = reinterpret_cast<Block *>(p - sizeof(Block));
    b->free = 1;
    ++g_frees;
    coalesce();
    anti_skill_unlock_irqrestore(g_lock, flags);
}

DarkMatterStats dark_matter_stats() {
    DarkMatterStats s;
    s.arena_size = kArenaSize;
    const uint64_t flags = anti_skill_lock_irqsave(g_lock);
    s.allocations = g_allocations;
    s.frees = g_frees;
    for (Block *b = first_block(); b != nullptr; b = next_block(b)) {
        ++s.blocks;
        if (b->free) {
            s.free_bytes += b->size;
            ++s.free_blocks;
        } else {
            s.used += b->size;
        }
    }
    anti_skill_unlock_irqrestore(g_lock, flags);
    return s;
}

} // namespace index
