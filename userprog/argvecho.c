/* argvecho.c: prints argc and each argv to stdout, then exits 0. Built
 * with zig (musl, aarch64) and dropped into the ext2 rootfs. Used as a
 * sanity check for the new linux_build_startup_stack argv/envp path -- if
 * `linuxrun /bin/argvecho one two` shows "argc=3" and the three strings,
 * the SysV initial stack is well-formed. */

#include <stdio.h>

int main(int argc, char **argv) {
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; ++i) {
        printf("argv[%d]=%s\n", i, argv[i]);
    }
    return 0;
}
