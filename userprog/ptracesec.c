// Security test for the ptrace PSTATE-sanitization fix: a tracer tries to set
// the tracee's PSTATE mode bits to EL1 (privilege escalation / SROP) via
// PTRACE_SETREGSET, then continues it. The kernel must force the resumed PSTATE
// back to EL0, so the tracee runs normally at EL0 and exits 42 -- it must NOT
// run at EL1 (which would corrupt/crash the kernel). Built static-musl, baked as
// /bin/ptracesec. Prints PTRACESEC_OK on success.
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <linux/elf.h>      // NT_PRSTATUS
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>

struct gpregs { uint64_t regs[31]; uint64_t sp; uint64_t pc; uint64_t pstate; };

int main(void) {
    pid_t child = fork();
    if (child == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        _exit(42);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status)) {
        printf("PTRACESEC_FAIL stop\n"); return 1;
    }

    struct gpregs regs;
    struct iovec iov = { &regs, sizeof(regs) };
    ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &iov);

    // Try to escalate: set PSTATE mode to EL1h (0x5) + the EL1 SPSR shape.
    // A correct kernel strips the mode bits back to EL0t on resume.
    regs.pstate = 0x3c5; // EL1h, DAIF masked -- the classic SROP target
    ptrace(PTRACE_SETREGSET, child, (void *)NT_PRSTATUS, &iov);

    ptrace(PTRACE_CONT, child, 0, 0);
    if (waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 42) {
        // Tracee ran at EL0 and exited cleanly -> escalation was blocked.
        printf("PTRACESEC_OK exit=%d\n", WEXITSTATUS(status));
    } else {
        printf("PTRACESEC_FAIL status=0x%x\n", status);
    }
    return 0;
}
