// idxcpus N: cap the number of cores Index schedules EL0 on (Index-private
// prctl 'IDXC'). The apt wrapper drops to 1 around a real-mirror download to
// dodge the deep-SMP large-download crash (smp=1 is stable), then restores.
#include <unistd.h>
#include <sys/syscall.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 8;
    if (n < 1) n = 1;
    syscall(167 /*prctl*/, 0x49445843L /*PR_INDEX_SET_CPUS*/, n, 0L, 0L, 0L);
    return 0;
}
