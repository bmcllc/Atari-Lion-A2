# The game's boot EXE text, as the recompiler saw it (PS-X EXE: 2048-byte header skipped). The dispatcher compares the
# live RAM against it to know whether a compiled function still matches the code in memory.
        .section .rodata
        .balign 4
        .globl  g_game_exe, g_game_exe_end
g_game_exe:
        .incbin GAME_EXE_PATH, 2048
g_game_exe_end:
