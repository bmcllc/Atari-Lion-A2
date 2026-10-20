/* psxlion/rt/interp_deps.c — what the framework's dirty_ram_interp.c needs from the rest of the PC runtime,
 * reduced to the Lion: no overlays, no PGXP, no widescreen, no debugger, no netplay.
 */
#include "psx_runtime.h"

uint64_t psx_cycle_count;            /* the interpreter counts instructions here; hw.c uses it as the PS1 clock */
uint64_t s_frame_count;
uint32_t g_ws_backdrop_lo, g_ws_backdrop_hi;
int      g_ws_bd_from_interp;
extern uint8_t *g_psx_ram;

uint8_t *memory_get_ram_ptr(void) { return g_psx_ram; }
uint32_t cycles_to_next_event(void) { return 0xFFFFFFFFu; }
void debug_server_poll(void) { }
void debug_server_wait_if_paused(void) { }
void fntrace_maybe_mark_game_started(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; }
void psx_fatal_halt(const char *reason) { extern void lion_log(const char *); lion_log("psx_fatal_halt:"); lion_log(reason); for (;;) { } }
void psx_bail_record(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; }
int  psx_defer_switch_pending(void) { return 0; }
int  psx_get_in_exception(void) { extern int g_in_exception; return g_in_exception; }
void psx_get_freeze_diag(uint64_t *a, uint32_t *b, int *c, int *d, uint64_t *e, uint64_t *f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
int  psx_interrupts_checked_at_current_cycle(uint32_t pc) { (void)pc; return 0; }
int  psx_mod_function_entry(CPUState *cpu, uint32_t address) { (void)cpu; (void)address; return 0; }
void psx_exception_longjmp(void) { }

/* overlays: none */
int  overlay_fp_enabled(void) { return 0; }
void overlay_fp_log(uint32_t addr, const uint32_t *in, CPUState *cpu, int native) { (void)addr; (void)in; (void)cpu; (void)native; }
int  overlay_loader_call_native(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int  overlay_loader_dispatch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int  overlay_loader_is_candidate(uint32_t phys) { (void)phys; return 0; }
void overlay_regs_snap(uint32_t out[34], const CPUState *cpu) { (void)out; (void)cpu; }

/* PGXP: off */
void psx_pgxp_alu(CPUState *c, uint32_t i, uint32_t r, uint32_t a, uint32_t b) { (void)c; (void)i; (void)r; (void)a; (void)b; }
void psx_pgxp_cop2(CPUState *c, uint32_t i, uint32_t v, uint32_t a) { (void)c; (void)i; (void)v; (void)a; }
void psx_pgxp_load(CPUState *c, uint32_t i, uint32_t a, uint32_t v) { (void)c; (void)i; (void)a; (void)v; }
void psx_pgxp_store(CPUState *c, uint32_t i, uint32_t a, uint32_t v) { (void)c; (void)i; (void)a; (void)v; }
void psx_pgxp_muldiv(CPUState *c, uint32_t i, uint32_t h, uint32_t l, uint32_t a, uint32_t b) { (void)c; (void)i; (void)h; (void)l; (void)a; (void)b; }

/* Widescreen: off. The *_site predicates say "not a patched instruction", the value hooks return the vanilla value. */
#define WS0(name, ...) int name(__VA_ARGS__) { return 0; }
int  psx_ws_activation_margin(void) { return 0; }
int  psx_ws_auto_cull_on(void) { return 0; }
int  psx_ws_x_margin(void) { return 0; }
int  psx_ws_backdrop_x(int x) { return x; }
int32_t  psx_ws_plane_nx(int32_t nx) { return nx; }
int32_t  psx_ws_player_x_bound(int32_t v) { return v; }
int32_t  psx_ws_depth_bound(int32_t imm) { return imm; }
uint32_t psx_ws_xclip_bound(uint32_t v) { return v; }
WS0(psx_ws_angle_site, uint32_t pc, uint32_t instr, uint32_t *out)
WS0(psx_ws_aspect_cone_site, CPUState *cpu, uint32_t pc, uint32_t instr, uint32_t a, uint32_t b, uint32_t c)
void psx_ws_backdrop_preload(void) { }
int  psx_ws_backdrop_value(uint32_t pc) { (void)pc; return 0; }
void psx_ws_backdrop_ring_note(uint32_t pc, int kind, int wcols, uint32_t orig, uint32_t a, uint32_t b) { (void)pc; (void)kind; (void)wcols; (void)orig; (void)a; (void)b; }
WS0(psx_ws_cull_bltz, uint32_t v)
WS0(psx_ws_cull_bltz_at, const uint32_t *w, int n, int idx)
WS0(psx_ws_cull_keep_site, uint32_t pc, uint32_t instr, uint32_t vanilla, uint32_t a, uint32_t b)
WS0(psx_ws_cull_slti, uint32_t sx, uint32_t imm)
WS0(psx_ws_cull_slti_lower, uint32_t sx, uint32_t imm)
WS0(psx_ws_cull_sltiu, uint32_t sx, uint32_t imm)
WS0(psx_ws_cull_vxrange, uint32_t x, uint32_t imm)
WS0(psx_ws_func_has_screen_cull, const uint32_t *w, int n)
WS0(psx_ws_is_backdrop_site, uint32_t pc)
WS0(psx_ws_is_cull_bias_site, uint32_t pc)
WS0(psx_ws_is_cull_depth_site, uint32_t pc)
WS0(psx_ws_is_cull_negsub_site, uint32_t pc)
WS0(psx_ws_is_cull_plane_nx_site, uint32_t pc)
WS0(psx_ws_is_cull_range_site, uint32_t pc)
WS0(psx_ws_is_cull_slti_lower_site, uint32_t pc)
WS0(psx_ws_is_cull_slti_site, uint32_t pc)
WS0(psx_ws_is_cull_vxrange_site, uint32_t pc)
WS0(psx_ws_is_cull_w_imm, uint32_t imm)
WS0(psx_ws_is_cull_xclip_load_site, uint32_t pc)
WS0(psx_ws_is_signed_x_bound_site, uint32_t pc, uint32_t instr)
