// auxvdump.S: a hand-written AArch64 Linux-ABI ELF that validates the Phase C
// process startup stack. At _start, the SysV AArch64 ABI guarantees:
//
//   [sp]      = argc
//   [sp+8]    = argv[0], ... argv[argc-1], then a NULL
//   then        envp[0], ... , then a NULL
//   then        auxv: pairs of (type, value), terminated by AT_NULL (type 0)
//
// This program prints argc as a single digit, then walks the aux vector and
// prints one line per recognised entry (AT_ENTRY=9, AT_RANDOM=25, AT_PHDR=3)
// as "T=<hex>" so a human/grep can confirm the kernel built the frame right.
// It uses only write(64) + exit_group(94), no libc.

.text
.global _start
.type _start, @function
_start:
    mov     x19, sp                 // x19 = pointer to argc

    // --- print "argc=" then the digit ---
    ldr     x20, [x19]              // x20 = argc
    adr     x1, s_argc
    mov     x2, #5
    bl      write1
    add     x0, x20, #'0'           // assume argc < 10
    bl      putc

    mov     w0, #'\n'
    bl      putc

    // --- walk to auxv: skip argc(1) + argv(argc+1 incl NULL) + envp(NULL..) ---
    add     x21, x19, #8            // x21 -> argv[0]
    // skip argv: advance until we pass the NULL terminator
    add     x21, x21, x20, lsl #3   // x21 -> &argv[argc] (the NULL)
    add     x21, x21, #8            // x21 -> envp[0]
1:  ldr     x0, [x21]               // skip envp until NULL
    add     x21, x21, #8
    cbnz    x0, 1b                   // x21 now -> auxv[0]

    // --- walk auxv pairs ---
2:  ldr     x22, [x21]              // type
    ldr     x23, [x21, #8]          // value
    cbz     x22, 3f                 // AT_NULL -> done

    // print "a T=" where T is the type as two hex digits, then value hex
    adr     x1, s_at
    mov     x2, #2
    bl      write1
    mov     x0, x22
    bl      puthex
    mov     w0, #'='
    bl      putc
    mov     x0, x23
    bl      puthex
    mov     w0, #'\n'
    bl      putc

    add     x21, x21, #16
    b       2b

3:  mov     x0, #0
    mov     x8, #94                 // exit_group
    svc     #0
4:  b       4b

// write1(x1=buf, x2=len) to fd 1. Leaf (only svc), so x30 is preserved.
write1:
    mov     x0, #1
    mov     x8, #64
    svc     #0
    ret

// putc(w0 = char): write one byte from a 16-byte scratch on the stack. Leaf.
putc:
    sub     sp, sp, #16
    strb    w0, [sp]
    mov     x0, #1
    mov     x1, sp
    mov     x2, #1
    mov     x8, #64
    svc     #0
    add     sp, sp, #16
    ret

// puthex(x0 = value): print "0x" + 16 hex digits. Calls putc, so save lr on
// the stack (x9/x10/x11 survive putc since putc never touches them).
puthex:
    stp     x29, x30, [sp, #-16]!
    mov     x9, x0                  // value
    mov     w0, #'0'
    bl      putc
    mov     w0, #'x'
    bl      putc
    mov     x10, #60                // shift, start at the top nibble
5:  lsr     x11, x9, x10
    and     x11, x11, #0xf
    cmp     x11, #10
    b.lo    6f
    add     x11, x11, #('a' - 10)
    b       7f
6:  add     x11, x11, #'0'
7:  mov     w0, w11
    bl      putc
    subs    x10, x10, #4
    b.ge    5b
    ldp     x29, x30, [sp], #16
    ret

.section .rodata, "a"
s_argc:
    .ascii  "argc="
s_at:
    .ascii  "a "
