#pragma once

#include <stdint.h>

#include "index/artificial_heaven.hpp"

namespace index {

struct IndexLibrorumProhibitorum {
    uint64_t start = 0;
    uint64_t current = 0;
    uint64_t end = 0;
    bool ready = false;

    void init(const ArtificialHeaven &heaven, uint64_t kernel_end);
    void *allocate(uint64_t bytes, uint64_t alignment);
    uint64_t used() const;
    uint64_t available() const;
};

} // namespace index
