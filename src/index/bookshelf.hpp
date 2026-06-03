#pragma once

#include <stdint.h>

namespace index {

// Bookshelf: the writable companion to GrimoireFS. Where GrimoireFS is a set of
// read-only grimoires baked into the image, the Bookshelf is a place to put and
// take books at will -- a small writable in-RAM filesystem whose file contents
// live in the DarkMatter heap (allocated on write, freed on remove).
constexpr uint32_t kBookshelfMaxFiles = 16;
constexpr uint32_t kBookshelfNameMax = 32;

struct BookshelfFile {
    bool used = false;
    char name[kBookshelfNameMax] = {};
    char *data = nullptr; // NUL-terminated, from DarkMatter
    uint32_t size = 0;
};

// Create or overwrite a file with `text`. Returns false if the shelf is full
// or the heap is exhausted.
bool bookshelf_write(const char *name, const char *text);

// Delete a file, freeing its heap storage. Returns false if not found.
bool bookshelf_remove(const char *name);

const BookshelfFile *bookshelf_find(const char *name);
uint32_t bookshelf_capacity();
const BookshelfFile *bookshelf_at(uint32_t index);

} // namespace index
