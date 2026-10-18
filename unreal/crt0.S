# Entry stub: stack, FPU on, interrupts off, then main().
        .section .text.start,"ax"
        .globl  _start
_start:
        lis     1, 0x017F            # stack top just under 24 MB (stack: 22-24 MB, heap: 6-22 MB)
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
        li      0, 0
        stwu    1, -16(1)
        bl      main
1:      b       1b
