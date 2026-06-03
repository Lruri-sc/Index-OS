#include "index/grimoire_fs.hpp"

namespace index {

namespace {

bool streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

const Grimoire g_grimoires[] = {
    {"motd",
     "Index Librorum Prohibitorum -- one Index, many grimoires.\n"
     "Type 'help' for commands, 'ls' to see the Bookshelf.\n"},
    {"index.txt",
     "Index is a minimal ARM64 kernel for UTM's QEMU virt machine.\n"
     "Names follow A Certain Magical Index; real hardware roles live in\n"
     "the comments. Necessarius is the shell you are typing into now.\n"},
    {"network.txt",
     "MisakaNetwork schedules Sister threads preemptively, paced by the\n"
     "LastOrder timer. Sisters can sleep, yield, and block on an\n"
     "Imprimatur (counting semaphore). Try 'spawn', 'sleeper', 'prodcons'.\n"},
    {"bookshelf.txt",
     "GrimoireFS is the first shelf of the Bookshelf: a read-only set of\n"
     "grimoires baked into the image. A real on-disk filesystem is a\n"
     "future entry; for now 'ls' lists shelves and 'cat <name>' opens one.\n"},
};

constexpr uint32_t kCount = sizeof(g_grimoires) / sizeof(g_grimoires[0]);

} // namespace

uint32_t grimoire_fs_count() {
    return kCount;
}

const Grimoire *grimoire_fs_at(uint32_t index) {
    return index < kCount ? &g_grimoires[index] : nullptr;
}

const Grimoire *grimoire_fs_find(const char *name) {
    for (uint32_t i = 0; i < kCount; ++i) {
        if (streq(g_grimoires[i].name, name)) {
            return &g_grimoires[i];
        }
    }
    return nullptr;
}

} // namespace index
