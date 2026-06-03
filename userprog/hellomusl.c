/* hellomusl.c: a normal C program built against musl libc with a stock
 * Linux/aarch64 toolchain (zig cc -target aarch64-linux-musl). It exercises
 * the Linux ABI end to end: musl's startup reads the aux vector we built,
 * sets up its allocator via brk/mmap, and printf goes out through writev to
 * the console. If "hello musl" appears and the process exits 0, the kernel's
 * Linux compatibility layer is genuinely running an unmodified Linux binary. */

#include <stdio.h>

int main(void) {
    printf("hello musl: a real Linux binary on arm-Index\n");
    return 0;
}
