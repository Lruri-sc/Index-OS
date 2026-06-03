.section .text.boot, "ax"
.align 11
.global _start
.type _start, %function

/*
 * ARM64 Linux Image header.
 * m1n1 recognizes this as a bootable payload and enters with x0 = DTB.
 */
_start:
    b       reset
    .word   0
    .quad   0x00080000
    .quad   __image_size
    .quad   0
    .quad   0
    .quad   0
    .quad   0
    .word   0x644d5241
    .word   0

reset:
    mov     x20, x0

    bl      el2_to_el1

    bl      enable_fp_simd

    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
0:
    cmp     x1, x2
    b.hs    1f
    str     xzr, [x1], #8
    b       0b

1:
    adrp    x1, __stack_top
    add     x1, x1, :lo12:__stack_top
    mov     sp, x1

    bl      install_exception_vectors

    mov     x0, x20
    adrp    x1, __image_start
    add     x1, x1, :lo12:__image_start
    adrp    x2, __image_end
    add     x2, x2, :lo12:__image_end
    bl      kmain

2:
    wfe
    b       2b

.size _start, . - _start

/*
 * Secondary core entry. PSCI CPU_ON lands a freshly woken core here at this
 * routine's PHYSICAL address with its MMU off and interrupts masked. We pass the
 * physical top of that core's stack as the PSCI context_id (x0), so the only
 * setup needed before C is to install the stack pointer. secondary_main turns
 * this core's MMU on, joins the Misaka Network, and never returns.
 */
.global secondary_entry
.type secondary_entry, %function
secondary_entry:
    mov     x19, x0          // physical stack top (survives the EL drop)
    bl      el2_to_el1       // secondaries are woken at the machine's boot EL too
    bl      enable_fp_simd   // each core enables its own FP/SIMD access
    mov     sp, x19
    bl      secondary_main
3:
    wfe
    b       3b
.size secondary_entry, . - secondary_entry

/*
 * Drop EL2 -> EL1 if we were entered at EL2. UTM/HVF on Apple Silicon (and QEMU
 * `virtualization=on`) boot the kernel at EL2, but the kernel programs the EL1
 * translation/timer regime -- which only governs execution once we are actually
 * at EL1. Without this, teleport_enable's SCTLR_EL1.M has no effect, the MMU
 * never turns on, and the first higher-half MMIO access wedges the machine.
 *
 * Returns to the caller at EL1 by pointing ELR_EL2 at the link register. If we
 * were already below EL2, just return. Clobbers x9 only (x30 preserved by eret).
 */
.global el2_to_el1
.type el2_to_el1, %function
el2_to_el1:
    mrs     x9, CurrentEL
    cmp     x9, #(2 << 2)
    b.ne    1f
    mov     x9, #(1 << 31)
    msr     hcr_el2, x9        // HCR_EL2.RW = 1: EL1 is AArch64
    msr     sctlr_el1, xzr     // EL1 starts with MMU + caches off
    mrs     x9, cnthctl_el2
    orr     x9, x9, #3         // let EL1 reach the timers (EL1PCTEN | EL1PCEN)
    msr     cnthctl_el2, x9
    msr     cntvoff_el2, xzr   // zero the virtual-counter offset

    // If this core implements the GICv3 CPU system-register interface
    // (ID_AA64PFR0_EL1.GIC != 0), enable it for EL1 via ICC_SRE_EL2 so EL1's
    // ICC_SRE_EL1.SRE write sticks and the ICC_* timer/IRQ registers work.
    // Required under Apple HVF (GICv3-only); harmless on GICv2 hardware that
    // still has the sysreg interface. Without it, EL1 ICC_SRE writes are WI and
    // the virtual-timer PPI never reaches the core. Clobbers x10.
    mrs     x10, id_aa64pfr0_el1
    ubfx    x10, x10, #24, #4  // GIC field: 0 = no sysreg CPU interface
    cbz     x10, 2f
    mov     x10, #0x9          // ICC_SRE_EL2: Enable (bit 3) | SRE (bit 0)
    msr     S3_4_C12_C9_5, x10 // ICC_SRE_EL2
    isb
2:
    mov     x9, #0x3c5         // SPSR: EL1h, DAIF masked
    msr     spsr_el2, x9
    msr     elr_el2, x30       // resume at the caller -- but now at EL1
    eret
1:
    ret
.size el2_to_el1, . - el2_to_el1

/*
 * Enable FP/SIMD (Advanced SIMD) access at EL1 and EL0 by clearing the trap in
 * CPACR_EL1.FPEN (bits [21:20] = 0b11 = no trapping at either EL). The kernel
 * itself is built -mgeneral-regs-only and never uses FP, but user programs do:
 * an optimized libc memcpy (and the musl dynamic linker) issues NEON loads, and
 * without this they trap as EC=0x07 "SIMD/FP access disabled". Run on every core
 * after the EL2->EL1 drop, before any EL0 code executes. Clobbers x9.
 */
.global enable_fp_simd
.type enable_fp_simd, %function
enable_fp_simd:
    mrs     x9, cpacr_el1
    orr     x9, x9, #(3 << 20)   // FPEN = 0b11: don't trap FP/SIMD at EL0/EL1
    msr     cpacr_el1, x9
    isb
    ret
.size enable_fp_simd, . - enable_fp_simd
