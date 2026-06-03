ENTRY(_start)

/*
 * Higher-half link. The kernel is loaded at physical 0x40080000 (UTM/QEMU
 * `virt` RAM 0x40000000 + the ARM64 Image text_offset 0x80000), but it is
 * LINKED at a high virtual address so it runs in the kernel half of the address
 * space. Teleport maps this high range via TTBR1 onto the same physical RAM,
 * and keeps an identity alias via TTBR0 for device MMIO and the boot DTB.
 *
 * Because a 39-bit translation region only looks at VA[38:0], the high VA's low
 * 39 bits equal the physical address (0x...40080000), so one page table serves
 * both TTBR0 (identity) and TTBR1 (high). Code is PC-relative (adrp), so the
 * raw image still boots at physical with the MMU off; only absolute data
 * pointers carry the high VA, and those are touched only after we run high.
 *
 * Sections are 4 KiB aligned so Teleport can map them W^X: .text read-only +
 * executable, .rodata read-only + no-execute, .data/.bss read-write + no-exec.
 */
HIGH_BASE = 0xFFFFFF8000000000;
LOAD_PHYS = 0x40080000;
KERNEL_VA = HIGH_BASE + LOAD_PHYS;

SECTIONS
{
    . = KERNEL_VA;
    __image_start = .;

    __text_start = .;
    .text : ALIGN(4096) {
        KEEP(*(.text.boot))
        *(EXCLUDE_FILE(*hello.o) .text EXCLUDE_FILE(*hello.o) .text.*)
    }
    . = ALIGN(4096);
    __text_end = .;

    __rodata_start = .;
    .rodata : ALIGN(4096) {
        *(EXCLUDE_FILE(*hello.o) .rodata EXCLUDE_FILE(*hello.o) .rodata.*)
    }
    . = ALIGN(4096);
    __rodata_end = .;

    /* User program: EL0-executable code, isolated on its own pages. */
    __user_text_start = .;
    .user_text : ALIGN(4096) {
        *hello.o(.text .text.*)
        *hello.o(.rodata .rodata.*)
    }
    . = ALIGN(4096);
    __user_text_end = .;

    __data_start = .;
    .data : ALIGN(4096) {
        *(.data .data.*)
        *(.got .got.* .got.plt)
    }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        . += 64K;
        __stack_top = .;
        /* Stack for the embedded EL0 users (which run from .user_text at high
         * VA). Disk-loaded ELF processes get private stacks in their own
         * PersonalReality address spaces instead. */
        . = ALIGN(4096);
        __user_stack_start = .;
        . += 16K;
        __user_stack_end = .;
        __bss_end = .;
    }

    . = ALIGN(4096);
    __image_end = .;
    __image_size = __image_end - __image_start;

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
        *(.eh_frame_hdr*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
