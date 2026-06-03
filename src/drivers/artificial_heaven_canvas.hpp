#pragma once

#include <stdint.h>

namespace index::drivers {

struct ArtificialHeavenCanvas {
    uint64_t base = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    bool valid = false;
};

void draw_index_glyph(const ArtificialHeavenCanvas &canvas);

} // namespace index::drivers
