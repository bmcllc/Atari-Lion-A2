# The PS1 BIOS image the recompiled kernel runs against (OpenBIOS, bundled with psxrecomp).
        .section .rodata
        .balign 4
        .globl  g_openbios
g_openbios:
        .incbin BIOS_PATH
