/* catfile.c: open a file by name and print its contents through musl stdio.
 * Exercises the Linux ABI file path end to end: openat -> read -> fstat (for
 * buffering) -> close, all backed by the kernel's Lateran/Bookshelf lookup.
 * Built dynamic so it also re-validates the ld-musl path. */

#include <stdio.h>

int main(void) {
    FILE *f = fopen("HELLO.TXT", "r");
    if (!f) {
        printf("catfile: cannot open HELLO.TXT\n");
        return 1;
    }
    char buf[128];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    fclose(f);
    return 0;
}
