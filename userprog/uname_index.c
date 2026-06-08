/* Index's own uname -- a standard Linux-ABI coreutils-style uname, compiled with
 * the normal musl toolchain (zig cc -target aarch64-linux-musl). The ONE change
 * from a stock uname: it prints "Index" for the kernel-name field instead of
 * uname(2).sysname. The kernel's uname(2) still reports sysname="Linux" so that
 * platform-detecting Linux binaries (the JVM's os.name, glibc/musl) keep working;
 * this tool is Index's own, so it shows the honest name. All other fields come
 * straight from uname(2) (nodename="index", release="5.0.0-Index", machine
 * "aarch64"), so -n/-r/-m already carry the Index identity. */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char **argv) {
    struct utsname u;
    if (uname(&u) != 0) { perror("uname"); return 1; }
    const char *sysname = "Index";        /* the modification */

    int s = 0, n = 0, r = 0, v = 0, m = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-a") || !strcmp(a, "--all")) { s = n = r = v = m = 1; any = 1; }
        else if (!strcmp(a, "-s") || !strcmp(a, "--kernel-name")) { s = 1; any = 1; }
        else if (!strcmp(a, "-n") || !strcmp(a, "--nodename")) { n = 1; any = 1; }
        else if (!strcmp(a, "-r") || !strcmp(a, "--kernel-release")) { r = 1; any = 1; }
        else if (!strcmp(a, "-v") || !strcmp(a, "--kernel-version")) { v = 1; any = 1; }
        else if (!strcmp(a, "-m") || !strcmp(a, "--machine")
              || !strcmp(a, "-p") || !strcmp(a, "-i")) { m = 1; any = 1; }
        else if (!strcmp(a, "--help")) {
            printf("Usage: uname [-asnrvm]\n"); return 0;
        }
    }
    if (!any) s = 1;  /* bare `uname` prints the kernel name */

    int first = 1;
    #define EMIT(cond, str) do { if (cond) { \
        if (!first) putchar(' '); fputs((str), stdout); first = 0; } } while (0)
    EMIT(s, sysname);
    EMIT(n, u.nodename);
    EMIT(r, u.release);
    EMIT(v, u.version);
    EMIT(m, u.machine);
    putchar('\n');
    return 0;
}
