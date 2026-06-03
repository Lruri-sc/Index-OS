#pragma once

#include <stddef.h>
#include <stdint.h>

namespace index {

constexpr uint64_t kib(uint64_t value) {
    return value * 1024ULL;
}

constexpr uint64_t mib(uint64_t value) {
    return kib(value) * 1024ULL;
}

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

} // namespace index
