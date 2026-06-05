// Edge-case test for the mmap length-overflow guard: an absurd (near-2^64)
// length must fail with MAP_FAILED rather than wrapping the page-round to a
// bogus/inverted VMA, while a normal anonymous mmap still works (map, write,
// read back, unmap). Built static-musl, baked as /bin/mmapedge.
#include <sys/mman.h>
#include <stdio.h>
#include <stdint.h>

int main(void) {
    // Huge length -> must be rejected (MAP_FAILED), no hang, no bogus pointer.
    void *huge = mmap(NULL, (size_t)0xFFFFFFFFFFFFF000ULL,
                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("HUGE_REJECTED=%d\n", huge == MAP_FAILED);

    // A normal 64 KiB anonymous mapping must work and be usable.
    const size_t n = 64 * 1024;
    unsigned char *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ok = (p != MAP_FAILED);
    if (ok) {
        for (size_t i = 0; i < n; i += 4096) p[i] = (unsigned char)(i >> 12);
        for (size_t i = 0; i < n; i += 4096) if (p[i] != (unsigned char)(i >> 12)) ok = 0;
        munmap(p, n);
    }
    printf("NORMAL_MMAP_OK=%d\n", ok);

    if (huge == MAP_FAILED && ok) printf("MMAPEDGE_OK\n");
    else printf("MMAPEDGE_FAIL\n");
    return 0;
}
