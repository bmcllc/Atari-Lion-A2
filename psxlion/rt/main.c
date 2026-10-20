/* psxlion/rt/main.c — Lion entry: set up the PS1 core and run the recompiled BIOS from the reset vector. */
#include "psx_runtime.h"
#include "bios_hle.h"

extern const uint8_t g_openbios[];
extern const uint8_t *g_psx_bios_rom;
extern CPUState g_cpu;
extern int g_psx_cps_mode;
void psx_cpu_init(CPUState *cpu);
void lion_log(const char *s);
void lion_logf(const char *what, uint32_t a, uint32_t b);
void cd_attach(void);
void gpu_init(void);
void mdec_init(void);

uint32_t g_loop_sp;                     /* base of the guest run's stack; lion_abandon() restarts the dispatcher from here */
extern int g_psx_dispatch_depth, g_psx_call_bail, g_in_exception, g_rfe_escape_pending;

/* The dispatcher's outermost frame. Re-entered by lion_abandon() with the guest's new pc. */
void psx_resume(uint32_t pc) {
    g_psx_dispatch_depth = 0; g_psx_call_bail = 0; g_in_exception = 0; g_rfe_escape_pending = 0;
    g_cpu.pc = 0;
    psx_dispatch(&g_cpu, pc);
    lion_logf("psxlion: BIOS returned, pc/ra =", g_cpu.pc, g_cpu.gpr[31]);
    for (;;) { }
}

void lion_main(void) {
    lion_log("psxlion: start");
    cd_attach();                        /* find MK4.BIN on the Lion disc */
    gpu_init();                         /* VRAM, rasterizer and the Lion VI framebuffer */
    mdec_init();                        /* FMV decoder */
    g_psx_bios_rom = g_openbios;
    g_psx_cps_mode = 1;                 /* generated dispatch constructors set this on the PC build */
    psx_cpu_init(&g_cpu);
    { extern void psx_boot_skip_install(void); psx_boot_skip_install(); }   /* no shell: the disc boots at once */
    g_loop_sp = ((uint32_t)__builtin_frame_address(0) - 128) & ~15u;
    *(uint32_t *)g_loop_sp = 0;         /* null back chain */
    __asm__ volatile("mr 1, %0" : : "r"(g_loop_sp));
    psx_resume(0xBFC00000u);
}
