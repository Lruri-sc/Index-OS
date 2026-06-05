// Functional test for ptrace ("Mental Out"): a parent traces a child through a
// SIGSTOP stop, reads/writes its registers, peeks its memory, then continues it
// to exit. Built static-musl, baked into the rootfs as /bin/ptracetest.
// Prints PTRACE_OK on full success so a boot harness can grep for it.
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <linux/elf.h>      // NT_PRSTATUS
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>

// aarch64 GP register set (NT_PRSTATUS payload).
struct gpregs { uint64_t regs[31]; uint64_t sp; uint64_t pc; uint64_t pstate; };

int main(void) {
    pid_t child = fork();
    if (child == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);          // first stop: parent inspects us here
        _exit(42);               // after PTRACE_CONT, exit with a known code
    }

    int status;

    // 1) Signal-stop: expect WIFSTOPPED with SIGSTOP.
    if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status) ||
        WSTOPSIG(status) != SIGSTOP) {
        printf("PTRACE_FAIL stop status=0x%x\n", status);
        return 1;
    }
    printf("PTRACE stop OK sig=%d\n", WSTOPSIG(status));

    // 2) GETREGSET: read the stopped child's registers.
    struct gpregs regs;
    struct iovec iov = { &regs, sizeof(regs) };
    if (ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &iov) != 0) {
        printf("PTRACE_FAIL getregset\n");
        return 1;
    }
    printf("PTRACE getregs OK pc=0x%llx\n", (unsigned long long)regs.pc);

    // 3) PEEKDATA: read a word of the child's memory (at its PC -- valid code).
    long word = ptrace(PTRACE_PEEKDATA, child, (void *)regs.pc, 0);
    if (word == 0) {
        printf("PTRACE_FAIL peek\n");
        return 1;
    }
    printf("PTRACE peek OK word=0x%lx\n", word);

    // 4) SETREGSET: write a sentinel into x9 and read it back to prove register
    //    writes land (x9 is caller-clobbered, so changing it can't break _exit).
    regs.regs[9] = 0xdeadbeefcafef00dULL;
    if (ptrace(PTRACE_SETREGSET, child, (void *)NT_PRSTATUS, &iov) != 0) {
        printf("PTRACE_FAIL setregset\n");
        return 1;
    }
    struct gpregs r2;
    struct iovec iov2 = { &r2, sizeof(r2) };
    ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &iov2);
    if (r2.regs[9] != 0xdeadbeefcafef00dULL) {
        printf("PTRACE_FAIL regwrite x9=0x%llx\n", (unsigned long long)r2.regs[9]);
        return 1;
    }
    printf("PTRACE setregs OK\n");

    // 4b) SINGLESTEP: step one instruction at a time; expect a SIGTRAP stop each
    //     time with the PC advancing. Restore x9 first so the bogus sentinel can't
    //     affect stepped code.
    regs.regs[9] = r2.regs[9] = 0;
    ptrace(PTRACE_SETREGSET, child, (void *)NT_PRSTATUS, &iov);
    uint64_t prev_pc = 0;
    {
        struct gpregs sr; struct iovec siov = { &sr, sizeof(sr) };
        ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &siov);
        prev_pc = sr.pc;
    }
    int steps_ok = 0;
    for (int s = 0; s < 3; ++s) {
        if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) != 0) break;
        if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status) ||
            WSTOPSIG(status) != SIGTRAP) {
            printf("PTRACE_FAIL singlestep status=0x%x\n", status);
            return 1;
        }
        struct gpregs sr; struct iovec siov = { &sr, sizeof(sr) };
        ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &siov);
        if (sr.pc != prev_pc) ++steps_ok; // PC moved => one instruction retired
        prev_pc = sr.pc;
    }
    if (steps_ok >= 2) printf("PTRACE singlestep OK steps=%d\n", steps_ok);
    else { printf("PTRACE_FAIL singlestep moved=%d\n", steps_ok); return 1; }

    // 5) CONT: let the child run to _exit(42); expect WIFEXITED == 42.
    ptrace(PTRACE_CONT, child, 0, 0);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 42) {
        printf("PTRACE_FAIL final status=0x%x\n", status);
        return 1;
    }
    printf("PTRACE_OK exit=%d\n", WEXITSTATUS(status));
    return 0;
}
