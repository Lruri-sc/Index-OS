/* writefile.c: a real Linux/musl program that creates a file and writes to it
 * with the stdio + POSIX file APIs. Run it once to write OUT.TXT; reboot and
 * `cat OUT.TXT` in the kernel shell to prove it persisted to the FAT disk.
 * Exercises openat(O_CREAT|O_WRONLY|O_TRUNC) + write + the FAT write path. */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(void) {
    int fd = open("OUT.TXT", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        printf("writefile: open failed\n");
        return 1;
    }
    const char *msg = "written by a Linux program on arm-Index\n";
    long n = write(fd, msg, strlen(msg));
    close(fd);
    printf("writefile: wrote %ld bytes to OUT.TXT\n", n);

    /* Read it back through stdio to confirm in-session consistency. */
    FILE *f = fopen("OUT.TXT", "r");
    if (f) {
        char buf[128];
        size_t r = fread(buf, 1, sizeof(buf) - 1, f);
        buf[r] = 0;
        fclose(f);
        printf("writefile: read back: %s", buf);
    }
    return 0;
}
