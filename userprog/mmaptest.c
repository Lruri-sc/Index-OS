/* mmaptest.c: open a file, mmap it MAP_PRIVATE, and read its contents through
 * the mapping (not read()). Exercises the kernel's file-backed mmap: the VMA
 * is backed by a snapshot of the file and faults pages in from it. */

#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

int main(void) {
    int fd = open("HELLO.TXT", O_RDONLY);
    if (fd < 0) {
        printf("mmaptest: open failed\n");
        return 1;
    }
    size_t len = 64;
    char *p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        printf("mmaptest: mmap failed\n");
        return 1;
    }
    printf("mmap[0..20]: ");
    for (int i = 0; i < 20 && p[i]; i++) putchar(p[i]);
    putchar('\n');
    munmap(p, len);
    close(fd);
    return 0;
}
