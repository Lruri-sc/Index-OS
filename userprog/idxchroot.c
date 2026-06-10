/* idxchroot DIR CMD [args...] -- chroot(2) into DIR, chdir("/"), exec CMD.
 * A tiny static-musl helper (Index runs unmodified static musl binaries) that
 * calls chroot(2) directly, so it both (a) exercises the kernel's real chroot
 * and (b) serves as the chroot tool for the isolated apt environment
 * (`idxchroot /apt-debian /usr/bin/apt ...`). busybox doesn't expose a `chroot`
 * applet symlink here, hence this. */
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: idxchroot DIR CMD [args...]\n");
        return 2;
    }
    if (chroot(argv[1]) != 0) { perror("chroot"); return 3; }
    if (chdir("/") != 0)      { perror("chdir");  return 4; }
    execv(argv[2], argv + 2);
    perror("execv");
    return 127;
}
