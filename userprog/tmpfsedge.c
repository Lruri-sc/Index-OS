// Edge-case test for the Testament (tmpfs) audit fixes:
//  - rename a directory into its own subtree must FAIL (-EINVAL), not create a
//    cycle / hang a later ancestor walk.
//  - a >2 GiB ftruncate must fail cleanly, not spin forever in ensure_cap's
//    doubling loop. (The test merely COMPLETING proves there is no hang.)
// Built static-musl, baked as /bin/tmpfsedge. Prints TMPFSEDGE_OK on success.
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>

int main(void) {
    mkdir("/tmp/rd", 0755);
    mkdir("/tmp/rd/sub", 0755);

    // Move a dir into its own subtree -> must be rejected.
    int r = rename("/tmp/rd", "/tmp/rd/sub/loop");
    printf("RENAME_CYCLE r=%d (want <0)\n", r);

    // The tree must still be walkable (no corruption from the rejected rename).
    struct stat st;
    int fs_ok = (stat("/tmp/rd/sub", &st) == 0);
    printf("FS_OK=%d\n", fs_ok);

    // A 3 GiB truncate must return cleanly (the ensure_cap doubling used to wrap
    // to 0 and loop forever for need > 2 GiB). Reaching the next line == no hang.
    int fd = open("/tmp/big", O_CREAT | O_RDWR, 0644);
    int t = (fd >= 0) ? ftruncate(fd, 3UL * 1024 * 1024 * 1024) : -1;
    printf("BIGTRUNC t=%d (want <0, no hang)\n", t);
    if (fd >= 0) close(fd);

    if (r < 0 && fs_ok && t < 0) printf("TMPFSEDGE_OK\n");
    else printf("TMPFSEDGE_FAIL\n");
    return 0;
}
