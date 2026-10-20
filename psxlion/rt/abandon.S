# lion_abandon(pc): drop every host frame of the guest run and re-enter the dispatcher from the base of the stack.
#
# The recompiled PS1 code is "flat": any function can be re-entered at a block label from cpu->pc, and guest state lives in
# CPUState and PS1 RAM, never in host frames. So switching PS1 threads inside an interrupt does not need one host stack per
# thread (the PC runtime uses fibers for that) -- it is enough to forget the frames of the thread being left and carry on
# at the new thread's EPC. Nothing returns into the dropped frames, so no registers need saving.
        .text
        .globl  lion_abandon
        .type   lion_abandon, @function
lion_abandon:
        lis     9, g_loop_sp@ha
        lwz     1, g_loop_sp@l(9)        # r1 = base of the guest run (set by lion_main), r3 = pc argument is kept
        b       psx_resume
        .size   lion_abandon, .-lion_abandon
