// Bridge between the kernel (EL1) and a user program (EL0).
//
// enter_user() saves the kernel's callee-saved context (like the "prev" half of
// a context switch) and then `eret`s down to EL0. It appears to return only
// when the user finishes: the syscall dispatcher calls leave_user(), which
// restores that saved context and `ret`s back into enter_user()'s caller.
//
// SMP: g_user_kctx is a per-CPU array. Each CPU's slot lives 128 bytes
// apart and is indexed by TPIDR_EL1 (set to the logical CPU id at boot for
// every core, both primary and secondary -- arch::this_cpu_id() reads it).
// Linux uses the same idea (TPIDR_EL1 = per-cpu offset / current_task ptr).
// Without this any two cores doing enter_user / leave_user concurrently
// would race on the same kernel-context save area.

.section .text

// Helper: x9 = address of this CPU's g_user_kctx slot.
//   clobbers x9, x10.
// Each slot is 104 bytes of saved state; we round to 128 for nice alignment.
.macro CPU_KCTX_PTR
    adrp    x9, g_user_kctx
    add     x9, x9, :lo12:g_user_kctx
    mrs     x10, tpidr_el1
    add     x9, x9, x10, lsl #7  // cpu_id * 128
.endm

.global enter_user
.type enter_user, %function
enter_user:                 // x0 = user entry, x1 = user stack top, x2 = ttbr0
    CPU_KCTX_PTR
    stp     x19, x20, [x9, #0]
    stp     x21, x22, [x9, #16]
    stp     x23, x24, [x9, #32]
    stp     x25, x26, [x9, #48]
    stp     x27, x28, [x9, #64]
    stp     x29, x30, [x9, #80]
    mov     x10, sp
    str     x10, [x9, #96]

    msr     ttbr0_el1, x2   // install this process's private address space
    dsb     ish
    tlbi    vmalle1is       // broadcast: a thread can migrate to another CPU
    dsb     ish              // whose TLB still has the old EL0 translations
    isb

    msr     sp_el0, x1
    msr     elr_el1, x0
    mov     x11, #0x340     // EL0t, DAIF: F/A/D masked, IRQ enabled (preemptible)
    msr     spsr_el1, x11
    mov     x0, #0
    mov     x1, #0
    isb
    eret

.global leave_user
.type leave_user, %function
leave_user:                 // restores the kernel context saved by enter_user
    // Reinstate the kernel page table in TTBR0 BEFORE returning. The Esper that
    // just parked/exited left its own (low-VA, possibly about-to-be-freed) page
    // table in TTBR0. The kernel context we resume -- the run_espers / idle loop
    // -- and even this function's `ret` may run at LOW identity VAs (the boot
    // core's idle Sister, idle_entry, does), which translate through TTBR0. If we
    // ret into the idle loop with the Esper's table still installed, the next
    // kernel instruction fetch / the loop's own code goes through it; when that
    // table is freed and reused under SMP the fetch hits garbage -> the cpu-N
    // wild jump (faulting PC == SP, ELR in the idle-Sister stack). The
    // secondaries' high-VA become_idle is immune (TTBR1), but this is cheap and
    // semantically required: a parked Esper's address space must not stay live.
    // bl clobbers x0-x18 + x30, all dead here (we restore x19-x30 + sp below).
    bl      kernel_ttbr0_phys     // x0 = kernel ttbr0 (phys)
    msr     ttbr0_el1, x0
    dsb     ish
    tlbi    vmalle1is
    dsb     ish
    isb
    CPU_KCTX_PTR
    ldp     x19, x20, [x9, #0]
    ldp     x21, x22, [x9, #16]
    ldp     x23, x24, [x9, #32]
    ldp     x25, x26, [x9, #48]
    ldp     x27, x28, [x9, #64]
    ldp     x29, x30, [x9, #80]
    ldr     x10, [x9, #96]
    mov     sp, x10
    ret

.global resume_user_eret
.type resume_user_eret, %function
// resume_user_eret(regs_ptr, sp_el0, elr, spsr, tpidr, ttbr0)
//   = "sibling of enter_user that resumes a previously-saved Esper context".
// Used by run_espers's loop to bring back an Esper whose state was saved by
// save_ctx (ELR -=4 for the parked case; full live state for the preempted
// case). Same g_user_kctx save preamble as enter_user so leave_user() unwinds
// back into run_espers's loop, identical to the enter_user contract.
// Caller MUST have already installed e->ttbr0 (via switch_ttbr0) before this
// runs -- deliver_pending_status writes to the user's VA, and we don't want
// the redundant tlbi vmalle1 hit per resume.
resume_user_eret:                 // x0=regs, x1=sp_el0, x2=elr, x3=spsr, x4=tpidr
    CPU_KCTX_PTR
    stp     x19, x20, [x9, #0]
    stp     x21, x22, [x9, #16]
    stp     x23, x24, [x9, #32]
    stp     x25, x26, [x9, #48]
    stp     x27, x28, [x9, #64]
    stp     x29, x30, [x9, #80]
    mov     x10, sp
    str     x10, [x9, #96]

    msr     sp_el0, x1
    msr     elr_el1, x2
    msr     spsr_el1, x3
    msr     tpidr_el0, x4

    // Use x0 as the index pointer; load x1..x30 first, x0 last (overwrites ptr).
    ldr     x1,  [x0, #8]
    ldp     x2,  x3,  [x0, #16]
    ldp     x4,  x5,  [x0, #32]
    ldp     x6,  x7,  [x0, #48]
    ldp     x8,  x9,  [x0, #64]
    ldp     x10, x11, [x0, #80]
    ldp     x12, x13, [x0, #96]
    ldp     x14, x15, [x0, #112]
    ldp     x16, x17, [x0, #128]
    ldp     x18, x19, [x0, #144]
    ldp     x20, x21, [x0, #160]
    ldp     x22, x23, [x0, #176]
    ldp     x24, x25, [x0, #192]
    ldp     x26, x27, [x0, #208]
    ldp     x28, x29, [x0, #224]
    ldr     x30, [x0, #240]
    ldr     x0,  [x0, #0]

    isb
    eret

// FP/SIMD (NEON) save/restore. The kernel is built -mgeneral-regs-only, so the
// integrated assembler defaults to +nofp+nosimd and would reject q-register
// access; re-enable the base ARMv8-A FP/SIMD here (mandatory in armv8-a).
// Layout of the 520-byte area (x0): q0..q31 at byte 0..511, FPSR at 512,
// FPCR at 516. Used by save_ctx / esper_preempt / load_ctx / resume_user_eret
// so an EL0 thread preempted mid-NEON keeps its own vector state.
.arch armv8-a

.global fpsimd_save
.type fpsimd_save, %function
fpsimd_save:                 // x0 = &Esper.fpsimd (16-byte aligned)
    stp     q0,  q1,  [x0, #16*0]
    stp     q2,  q3,  [x0, #16*2]
    stp     q4,  q5,  [x0, #16*4]
    stp     q6,  q7,  [x0, #16*6]
    stp     q8,  q9,  [x0, #16*8]
    stp     q10, q11, [x0, #16*10]
    stp     q12, q13, [x0, #16*12]
    stp     q14, q15, [x0, #16*14]
    stp     q16, q17, [x0, #16*16]
    stp     q18, q19, [x0, #16*18]
    stp     q20, q21, [x0, #16*20]
    stp     q22, q23, [x0, #16*22]
    stp     q24, q25, [x0, #16*24]
    stp     q26, q27, [x0, #16*26]
    stp     q28, q29, [x0, #16*28]
    stp     q30, q31, [x0, #16*30]
    mrs     x1, fpsr
    mrs     x2, fpcr
    str     w1, [x0, #512]
    str     w2, [x0, #516]
    ret
.size fpsimd_save, . - fpsimd_save

.global fpsimd_restore
.type fpsimd_restore, %function
fpsimd_restore:              // x0 = &Esper.fpsimd (16-byte aligned)
    ldp     q0,  q1,  [x0, #16*0]
    ldp     q2,  q3,  [x0, #16*2]
    ldp     q4,  q5,  [x0, #16*4]
    ldp     q6,  q7,  [x0, #16*6]
    ldp     q8,  q9,  [x0, #16*8]
    ldp     q10, q11, [x0, #16*10]
    ldp     q12, q13, [x0, #16*12]
    ldp     q14, q15, [x0, #16*14]
    ldp     q16, q17, [x0, #16*16]
    ldp     q18, q19, [x0, #16*18]
    ldp     q20, q21, [x0, #16*20]
    ldp     q22, q23, [x0, #16*22]
    ldp     q24, q25, [x0, #16*24]
    ldp     q26, q27, [x0, #16*26]
    ldp     q28, q29, [x0, #16*28]
    ldp     q30, q31, [x0, #16*30]
    ldr     w1, [x0, #512]
    ldr     w2, [x0, #516]
    msr     fpsr, x1
    msr     fpcr, x2
    ret
.size fpsimd_restore, . - fpsimd_restore

.bss
.align 7
// Per-CPU kernel-context save area. 128 bytes per CPU (104 used, padded to
// 128 for cache-line + index-shift convenience). kMaxCpus = 8 per
// artificial_heaven.hpp; bump together if that grows.
g_user_kctx:
    .skip 128 * 8
