#include "drivers/artificial_heaven_canvas.hpp"

namespace index::drivers {

namespace {

uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

void rect(volatile uint32_t *pixels, uint32_t words_per_row, uint32_t x0, uint32_t y0,
          uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t y = y0; y < y0 + h; ++y) {
        for (uint32_t x = x0; x < x0 + w; ++x) {
            pixels[y * words_per_row + x] = color;
        }
    }
}

} // namespace

void draw_index_glyph(const ArtificialHeavenCanvas &canvas) {
    if (!canvas.valid || canvas.stride < 4) {
        return;
    }

    auto *pixels = reinterpret_cast<volatile uint32_t *>(canvas.base);
    const uint32_t max_y = canvas.height < 240 ? canvas.height : 240;
    const uint32_t max_x = canvas.width < 640 ? canvas.width : 640;
    const uint32_t words_per_row = canvas.stride / 4;
    const uint32_t ink = rgb(235, 243, 236);
    const uint32_t veil = rgb(46, 96, 123);
    const uint32_t accent = rgb(78, 178, 145);
    const uint32_t shadow = rgb(16, 22, 29);

    for (uint32_t y = 0; y < max_y; ++y) {
        for (uint32_t x = 0; x < max_x; ++x) {
            pixels[y * words_per_row + x] = ((x / 28 + y / 28) & 1) ? veil : shadow;
        }
    }

    if (max_x >= 180 && max_y >= 120) {
        rect(pixels, words_per_row, 32, 24, 116, 12, accent);
        rect(pixels, words_per_row, 32, 96, 116, 12, accent);
        rect(pixels, words_per_row, 78, 36, 24, 60, ink);
        rect(pixels, words_per_row, 52, 48, 10, 36, ink);
        rect(pixels, words_per_row, 118, 48, 10, 36, ink);
    }

    asm volatile("dsb sy" ::: "memory");
}

} // namespace index::drivers
