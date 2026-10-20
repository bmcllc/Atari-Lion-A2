# Entry stub: stack, FPU on, Lion interrupts off, PIC masked, then lion_main().
        .section .text.start,"ax"
        .globl  _start
_start:
        lis     1, 0x017F            # stack top just under 24 MB
        mfmsr   3
        ori     3, 3, 0x2000         # MSR[FP]
        lis     4, 0
        ori     4, 4, 0x8000         # MSR[EE]
        andc    3, 3, 4
        mtmsr   3
        isync
        lis     3, 0x1F04            # PIC: mask all sources
        li      4, 0
        stw     4, 4(3)
#ifdef ZERO_BSS
        lis     5, __bss_start@ha        # the boot ROM leaves its buffers in RAM: clear our bss
        addi    5, 5, __bss_start@l
        lis     6, __bss_end@ha
        addi    6, 6, __bss_end@l
        li      7, 0
2:      cmplw   5, 6
        bge     3f
        stw     7, 0(5)
        addi    5, 5, 4
        b       2b
3:
#endif
        li      0, 0
        stwu    1, -16(1)
        bl      LION_MAIN
1:      b       1b
