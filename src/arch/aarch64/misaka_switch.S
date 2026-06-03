.section .text

/*
 * void misaka_switch(uint64_t *prev_ctx, uint64_t *next_ctx)
 *
 * Swap one Sister for another. Only the callee-saved registers (x19..x30) and
 * sp need preserving across a normal function call, so that is all a voluntary
 * switch must save; the caller-saved set was already spilled by irq_entry on
 * the outgoing Sister's stack. Context layout (uint64_t[13]):
 *   [0..11] = x19..x30, [12] = sp.
 */
.global misaka_switch
.type misaka_switch, %function
misaka_switch:
    mov     x9, sp
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    str     x9,       [x0, #96]

    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x9,       [x1, #96]
    mov     sp, x9
    ret
.size misaka_switch, . - misaka_switch

/*
 * First entry of a freshly spawned Sister. misaka_switch "returns" here with
 * x19 = entry function, x20 = argument. The scheduler lock was acquired by the
 * core that switched to us and handed over with the switch, so we release it
 * first (x19/x20 are callee-saved, so the call preserves them). Then we unmask
 * IRQs so the new Sister is itself preemptible, run it, and fall into
 * sister_finish if it ever returns.
 */
.global sister_trampoline
.type sister_trampoline, %function
sister_trampoline:
    bl      misaka_network_trampoline_unlock
    mov     x0, x20
    msr     daifclr, #2
    blr     x19
    bl      sister_finish
0:
    wfe
    b       0b
.size sister_trampoline, . - sister_trampoline
