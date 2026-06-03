// hellolinux.S: a hand-written AArch64 Linux-ABI PIE ELF for Phase A of the
// Linux compatibility plan. No libc, no academy_city -- just two svc
// instructions using Linux syscall numbers (write=64, exit_group=94).
//
// The point: prove the kernel correctly routes a Linux-ABI Esper to the
// Linux syscall table (linux_abi.cpp) instead of the Index switch.
//
// Built as PIE (small e_entry, low PT_LOAD vaddr) so it still fits inside the
// existing 28 KiB PersonalReality pool -- Phase B's demand-paged address
// space isn't required to run this. The Makefile patches EI_OSABI (byte 7
// of the ELF header) to 3 = ELFOSABI_LINUX after link, so sniff_abi() in
// usermode.cpp tags the loaded Esper as Linux.

.text
.global _start
.type _start, @function
_start:
    mov     x0, #1                          // fd = stdout
    adr     x1, msg                         // PC-relative string (PIE-safe)
    mov     x2, #(msg_end - msg)            // length
    mov     x8, #64                         // SYS_write (Linux AArch64)
    svc     #0

    mov     x0, #0                          // exit code 0
    mov     x8, #94                         // SYS_exit_group
    svc     #0

    // The kernel should never let us reach this; if it does, spin so the
    // failure mode is loud rather than wandering into garbage.
1:  b       1b

.section .rodata, "a"
msg:
    .ascii  "Hello from Linux ABI!\n"
msg_end:
