#pragma once

#include <stddef.h>
#include <stdint.h>

namespace index::fdt {

constexpr uint32_t kMagic = 0xd00dfeed;
constexpr uint32_t kBeginNode = 1;
constexpr uint32_t kEndNode = 2;
constexpr uint32_t kProp = 3;
constexpr uint32_t kNop = 4;
constexpr uint32_t kEnd = 9;

struct Header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct Property {
    const char *name = nullptr;
    const uint8_t *data = nullptr;
    uint32_t len = 0;
};

inline uint32_t be32(const void *ptr) {
    const auto *p = static_cast<const uint8_t *>(ptr);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline uint64_t be64(const void *ptr) {
    const auto *p = static_cast<const uint8_t *>(ptr);
    return (uint64_t(be32(p)) << 32) | be32(p + 4);
}

inline const uint8_t *align4(const uint8_t *p) {
    auto value = reinterpret_cast<uintptr_t>(p);
    value = (value + 3U) & ~uintptr_t(3U);
    return reinterpret_cast<const uint8_t *>(value);
}

inline bool streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

inline bool starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return false;
        }
    }
    return true;
}

inline size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        ++n;
    }
    return n;
}

inline bool bytes_eq_text(const Property &prop, const char *text) {
    const size_t text_len = strlen(text);
    if (prop.len != text_len + 1) {
        return false;
    }
    const auto *data = reinterpret_cast<const char *>(prop.data);
    for (size_t i = 0; i < text_len; ++i) {
        if (data[i] != text[i]) {
            return false;
        }
    }
    return data[text_len] == 0;
}

inline bool compatible_has(const Property &prop, const char *needle) {
    if (!prop.data || !prop.len) {
        return false;
    }

    const size_t needle_len = strlen(needle);
    uint32_t pos = 0;
    while (pos < prop.len) {
        const char *entry = reinterpret_cast<const char *>(prop.data + pos);
        uint32_t len = 0;
        while (pos + len < prop.len && entry[len]) {
            ++len;
        }
        if (len == needle_len) {
            bool same = true;
            for (uint32_t i = 0; i < len; ++i) {
                if (entry[i] != needle[i]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return true;
            }
        }
        pos += len + 1;
    }
    return false;
}

inline bool compatible_has_prefix(const Property &prop, const char *prefix) {
    if (!prop.data || !prop.len) {
        return false;
    }

    uint32_t pos = 0;
    while (pos < prop.len) {
        const char *entry = reinterpret_cast<const char *>(prop.data + pos);
        uint32_t len = 0;
        while (pos + len < prop.len && entry[len]) {
            ++len;
        }
        if (starts_with(entry, prefix)) {
            return true;
        }
        pos += len + 1;
    }
    return false;
}

inline uint32_t prop_u32(const Property &prop, uint32_t fallback = 0) {
    if (prop.len < 4) {
        return fallback;
    }
    return be32(prop.data);
}

inline uint64_t read_cells(const uint8_t *data, uint32_t cells) {
    if (cells == 1) {
        return be32(data);
    }
    if (cells == 2) {
        return be64(data);
    }
    return 0;
}

inline uint32_t total_size(uint64_t dtb_addr) {
    if (dtb_addr == 0) {
        return 0;
    }
    const auto *hdr = reinterpret_cast<const Header *>(dtb_addr);
    if (be32(&hdr->magic) != kMagic) {
        return 0;
    }
    return be32(&hdr->totalsize);
}

} // namespace index::fdt
