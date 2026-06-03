/* hellospie.c: a minimal static-PIE binary. zig's default for
 * aarch64-linux-musl WITHOUT -static is a static-pie ELF (ET_DYN, no PT_INTERP,
 * musl's crt self-relocates R_AARCH64_RELATIVE at entry). OpenSSH's sshd is
 * static-pie too, so this isolates "does Index's loader run static-PIE?" from
 * sshd's complexity. Expected: prints STATIC_PIE_OK. */

#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("STATIC_PIE_OK pid=%d uid=%d\n", (int)getpid(), (int)getuid());
    return 0;
}
