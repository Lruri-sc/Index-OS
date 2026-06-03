#include "index/bookshelf.hpp"

#include "index/dark_matter.hpp"

namespace index {

namespace {

BookshelfFile g_files[kBookshelfMaxFiles];

bool streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

uint32_t str_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        ++n;
    }
    return n;
}

void str_copy(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    for (; src[i] && i + 1 < cap; ++i) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

int find_index(const char *name) {
    for (uint32_t i = 0; i < kBookshelfMaxFiles; ++i) {
        if (g_files[i].used && streq(g_files[i].name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int free_index() {
    for (uint32_t i = 0; i < kBookshelfMaxFiles; ++i) {
        if (!g_files[i].used) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

bool bookshelf_write(const char *name, const char *text) {
    if (name == nullptr || name[0] == 0) {
        return false;
    }
    const uint32_t len = str_len(text);

    // Allocate first; only touch the table once the memory and a slot exist, so
    // a failure never corrupts or empties an existing file.
    char *buf = static_cast<char *>(dark_matter_alloc(len + 1));
    if (buf == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < len; ++i) {
        buf[i] = text[i];
    }
    buf[len] = 0;

    int idx = find_index(name);
    if (idx >= 0) {
        dark_matter_free(g_files[idx].data); // overwrite: release old contents
    } else {
        idx = free_index();
        if (idx < 0) {
            dark_matter_free(buf); // shelf full; undo our allocation
            return false;
        }
    }

    g_files[idx].used = true;
    str_copy(g_files[idx].name, name, kBookshelfNameMax);
    g_files[idx].data = buf;
    g_files[idx].size = len;
    return true;
}

bool bookshelf_remove(const char *name) {
    const int idx = find_index(name);
    if (idx < 0) {
        return false;
    }
    dark_matter_free(g_files[idx].data);
    g_files[idx].data = nullptr;
    g_files[idx].used = false;
    g_files[idx].size = 0;
    g_files[idx].name[0] = 0;
    return true;
}

const BookshelfFile *bookshelf_find(const char *name) {
    const int idx = find_index(name);
    return idx < 0 ? nullptr : &g_files[idx];
}

uint32_t bookshelf_capacity() {
    return kBookshelfMaxFiles;
}

const BookshelfFile *bookshelf_at(uint32_t index) {
    return index < kBookshelfMaxFiles ? &g_files[index] : nullptr;
}

} // namespace index
