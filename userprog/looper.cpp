// looper: an EL0 program that runs forever, printing its pid + a counter once
// per iteration with a busy-compute delay between. Designed to be a kill
// target: `LOOPER.ELF &` in Komoe, then `kill <pid>` to terminate it via
// Fortis931. Without the kill, looper prints indefinitely.

#include "academy_city.h"

extern "C" [[noreturn]] void _start() {
    const long pid = sys_getpid();
    for (long i = 0;; ++i) {
        ac_printf("looper pid=%d i=%d\n", pid, i);
        for (volatile long d = 0; d < 30000000; d = d + 1) {
        }
    }
}
