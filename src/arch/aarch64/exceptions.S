.section .text.exceptions, "ax"
.align 11
.global exception_vectors
.type exception_vectors, %function

.macro VENTRY label, kind
.align 7
\label:
    mov     x0, #\kind
    b       exception_common
    .space  128 - (. - \label)
.endm

.macro VIRQ label
.align 7
\label:
    b       irq_entry
    .space  128 - (. - \label)
.endm

exception_vectors:
    VENTRY sync_current_sp0,       0
    VIRQ   irq_current_sp0
    VENTRY fiq_current_sp0,        2
    VENTRY serror_current_sp0,     3
    VENTRY sync_current_spx,       4
    VIRQ   irq_current_spx
    VENTRY fiq_current_spx,        6
    VENTRY serror_current_spx,     7
.align 7
sync_lower_aarch64:
    b       el0_sync_entry
    .space  128 - (. - sync_lower_aarch64)
    VIRQ   irq_lower_aarch64
    VENTRY fiq_lower_aarch64,      10
    VENTRY serror_lower_aarch64,   11
    VENTRY sync_lower_aarch32,     12
    VIRQ   irq_lower_aarch32
    VENTRY fiq_lower_aarch32,      14
    VENTRY serror_lower_aarch32,   15

exception_common:
    mrs     x5, CurrentEL
    cmp     x5, #(2 << 2)
    b.eq    1f
    cmp     x5, #(3 << 2)
    b.eq    2f

    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    mrs     x4, spsr_el1
    b       3f

1:
    mrs     x1, esr_el2
    mrs     x2, elr_el2
    mrs     x3, far_el2
    mrs     x4, spsr_el2
    b       3f

2:
    mrs     x1, esr_el3
    mrs     x2, elr_el3
    mrs     x3, far_el3
    mrs     x4, spsr_el3

3:
    mov     x5, x30            // pass faulting LR (helps pinpoint null/indirect calls)
    mov     x6, x29            // frame pointer -> FP-chain backtrace in exception_report
    mov     x7, sp             // stack pointer -> raw stack scan fallback
    bl      exception_report
4:
    wfe
    b       4b

.global install_exception_vectors
.type install_exception_vectors, %function
install_exception_vectors:
    adrp    x0, exception_vectors
    add     x0, x0, :lo12:exception_vectors
    mrs     x1, CurrentEL

    cmp     x1, #(2 << 2)
    b.eq    1f
    cmp     x1, #(3 << 2)
    b.eq    2f

    msr     vbar_el1, x0
    isb
    ret

1:
    msr     vbar_el2, x0
    isb
    ret

2:
    msr     vbar_el3, x0
    isb
    ret

/*
 * IRQ entry: save the full general-purpose context plus ELR_EL1/SPSR_EL1 on
 * the *current thread's* stack, hand control to Aleister's C dispatcher, then
 * restore and return. Saving ELR/SPSR per-thread is what lets MisakaNetwork
 * preempt one Sister and resume another with the correct PC and PSTATE: each
 * frozen IRQ frame carries its own thread's return state.
 */
.global irq_entry
.type irq_entry, %function
irq_entry:
    sub     sp, sp, #272
    stp     x0, x1,   [sp, #16 * 0]
    stp     x2, x3,   [sp, #16 * 1]
    stp     x4, x5,   [sp, #16 * 2]
    stp     x6, x7,   [sp, #16 * 3]
    stp     x8, x9,   [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #16 * 15]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1,   [sp, #256]

    mov     x0, sp          // pass the saved frame so the tick hook can preempt EL0
    bl      irq_dispatch

    ldp     x0, x1,   [sp, #256]
    msr     elr_el1, x0
    msr     spsr_el1, x1
    ldp     x0, x1,   [sp, #16 * 0]
    ldp     x2, x3,   [sp, #16 * 1]
    ldp     x4, x5,   [sp, #16 * 2]
    ldp     x6, x7,   [sp, #16 * 3]
    ldp     x8, x9,   [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #16 * 15]
    add     sp, sp, #272
    eret

/*
 * Synchronous exceptions from EL0 (lower AArch64): system calls and user
 * faults. Save the user's GP registers to a frame on SP_EL1, hand ESR + frame
 * to the C dispatcher, then restore (x0 may now hold a syscall result) and
 * eret back to EL0. SYS_exit / a fatal user fault never returns here -- the
 * dispatcher calls leave_user() to unwind straight back to the kernel.
 */
.global el0_sync_entry
.type el0_sync_entry, %function
el0_sync_entry:
    sub     sp, sp, #256
    stp     x0, x1,   [sp, #16 * 0]
    stp     x2, x3,   [sp, #16 * 1]
    stp     x4, x5,   [sp, #16 * 2]
    stp     x6, x7,   [sp, #16 * 3]
    stp     x8, x9,   [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #16 * 15]

    mrs     x0, esr_el1
    mov     x1, sp
    bl      el0_sync_dispatch

    ldp     x0, x1,   [sp, #16 * 0]
    ldp     x2, x3,   [sp, #16 * 1]
    ldp     x4, x5,   [sp, #16 * 2]
    ldp     x6, x7,   [sp, #16 * 3]
    ldp     x8, x9,   [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #16 * 15]
    add     sp, sp, #256
    eret
