/* dirtest.c: a Linux/musl program that makes a subdirectory, writes a file
 * inside it, then reads it back and lists the directory. Reboot and
 * `cat WORK/LOG.TXT` in the kernel shell to prove the subdir + file persisted.
 * Exercises mkdir + path resolution + write-in-subdir + readdir(getdents64). */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

int main(void) {
    mkdir("WORK", 0755);

    int fd = open("WORK/LOG.TXT", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { printf("dirtest: open WORK/LOG.TXT failed\n"); return 1; }
    const char *msg = "entry inside a subdirectory\n";
    write(fd, msg, strlen(msg));
    close(fd);

    char buf[128];
    FILE *f = fopen("WORK/LOG.TXT", "r");
    size_t r = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) fclose(f);
    buf[r] = 0;
    printf("dirtest: read WORK/LOG.TXT: %s", buf);

    printf("dirtest: listing WORK/:\n");
    DIR *d = opendir("WORK");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            printf("  %s%s\n", de->d_name, de->d_type == DT_DIR ? "/" : "");
        }
        closedir(d);
    }
    return 0;
}
