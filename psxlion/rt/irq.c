/* psxlion/rt/irq.c — interrupt delivery for the recompiled PS1 code, without setjmp or fibers.
 *
 * The PC runtime takes an interrupt at a compiled block edge by running the kernel's exception handler as a nested
 * psx_dispatch() and leaving it with longjmp when the handler executes RFE. Here the nested frames are unwound with the
 * dispatch loop's own "call bail" protocol instead (g_psx_call_bail + cpu->pc, see cpu_state.h): every generated frame
 * and dispatch level between the RFE and the delivery point returns, leaving the resume address in cpu->pc.
 *
 * Not supported yet (fails loudly): a handler that resumes a *different* thread (ChangeThread). That needs one machine
 * stack per guest thread; MK4 has not been seen to use them.
 *
 * Interrupt sources: VBLANK follows the Lion's VI frame counter (the PS1's 60 Hz); the rest comes with the hardware
 * modules (timers, CD, DMA, SIO, SPU) calling psx_irq_raise().
 */
#include "psx_runtime.h"

extern uint32_t i_stat, i_mask;
extern volatile uint32_t g_psx_last_fn_entry;
extern CPUState g_cpu;
extern void lion_log(const char *);
extern void lion_logf(const char *, uint32_t, uint32_t);

int      g_in_exception;
int      g_rfe_escape_pending, g_exc_escape_reason;
uint32_t g_exception_real_epc;
static uint32_t s_resume_epc;
static int s_path;                      /* 1 = RFE, 2 = RestoreState: how the handler ended */           /* where the handler's RFE said to continue */
static int s_irq_detail;                /* trace this delivery: a source other than VBLANK is pending */

#ifdef PSX_TRACE_IRQ
static int s_tr_irq, s_tr_irq_detail;
#define IRQTR(w, a, b) do { if (s_tr_irq < 700) { s_tr_irq++; lion_logf(w, a, b); } } while (0)
/* Handler detail: only when a non-VBLANK source is pending, and only the first few (the CD raises one per sector). */
#define IRQD(w, a, b) do { if (s_irq_detail && s_tr_irq_detail < 150) { s_tr_irq_detail++; lion_logf(w, a, b); } } while (0)
#else
#define IRQTR(w, a, b) do { } while (0)
#define IRQD(w, a, b) do { } while (0)
#endif

#define VI_FRAME (*(volatile uint32_t *)0x1F030010u)

void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= 1u << bit; }

extern void cd_tick(void);
extern void hw_tick(void);
extern void sio_poll(void);
extern void gpu_frame_tick(void);
extern uint64_t s_frame_count;
uint32_t g_polls;                                       /* bring-up: polls per VI frame (gpu.c logs it with the flips) */
static void poll_sources(void) {
    g_polls++;
    cd_tick(); hw_tick(); sio_poll();
    static uint32_t last_frame, countdown;
#ifdef PSX_TRACE_IRQ
    { static uint32_t polls, n;
      if ((++polls & 0x3FFFFFu) == 0 && n < 40) {
          n++;
          lion_logf("irq: polls / last fn", polls, g_psx_last_fn_entry);
          lion_logf("irq:   i_stat / i_mask", i_stat, i_mask);
          lion_logf("irq:   SR / EPC", g_cpu.cop0[12], g_cpu.cop0[14]);
      } }
#endif
    if (countdown++ & 0x3FFu) return;                   /* the VI register is on the bus: look every 1024 block edges */
    uint32_t f = VI_FRAME;
    if (f != last_frame) {
        last_frame = f; s_frame_count = f; psx_irq_raise(0, 0); gpu_frame_tick();
        { extern void spu_frame_tick(void); spu_frame_tick(); }
        { extern void ww_checkpoint_frame(uint32_t); ww_checkpoint_frame(f); }
        { extern void core_stats_frame(uint32_t); core_stats_frame(f); }
        { static int vb; if (i_mask && vb < 3) { vb++; lion_logf("irq: vblank frame / i_mask", f, i_mask); } }
#ifdef PSX_TRACE_FRAMES        /* bring-up: where is the guest every few frames */
        static unsigned n; extern volatile uint32_t g_psx_last_fn_entry;
        if ((n++ & 15) == 0 && n < 400) { lion_logf("frame: last fn / SR", g_psx_last_fn_entry, g_cpu.cop0[12]); lion_logf("       i_stat / i_mask", i_stat, i_mask); }
#endif
    }
}

void psx_rfe_mark_escape(void) { if (g_in_exception) { g_rfe_escape_pending = 1; if (!g_exc_escape_reason) g_exc_escape_reason = 1; } }

/* ---- context switch inside an interrupt ----------------------------------------------------------------------------
 * The kernel's handler saves the interrupted thread in its TCB and may restore *another* thread before its RFE, so the
 * resume address differs from the one we interrupted. See abandon.S: the host frames of the thread being left are dropped
 * and the dispatcher is re-entered at the new EPC.
 */
extern void lion_abandon(uint32_t pc) __attribute__((noreturn));

void psx_rfe_escape_check(CPUState *cpu) {
    if (g_rfe_escape_pending && g_in_exception && cpu->pc != PSX_EXC_SENTINEL_PC) {
        g_rfe_escape_pending = 0;
        s_resume_epc = cpu->pc; s_path = 1;
        g_psx_call_bail = 1;                            /* unwind to run_handler(); cpu->pc stays the resume address */
    }
}

/* The kernel's RestoreState/longjmp-style return (generated code sets cpu->pc = $ra first). */
void psx_restore_state_escape(void) {
    if (!g_in_exception) return;
    s_resume_epc = g_cpu.pc; s_path = 2;
    g_psx_call_bail = 1;
}

/* Runs the kernel's exception handler for the pending interrupt and returns where execution continues.
 *
 * The handler may end in two ways: RFE (psx_rfe_escape_check) or RestoreState (psx_restore_state_escape), which is not an
 * end at all: it redirects to cpu->pc *inside* the exception (the kernel switching to its own context), so the dispatcher is
 * entered again there and we wait for the RFE. When the handler resumes the very context it interrupted, the interrupted
 * code gets all its registers back (the kernel's TCB restore is not complete for a compiled same-thread resume). */
static uint32_t run_handler(CPUState *cpu, uint32_t resume_pc) {
    uint32_t saved[32], shi = cpu->hi, slo = cpu->lo, sr = cpu->cop0[12];
    for (int i = 0; i < 32; i++) saved[i] = cpu->gpr[i];
    IRQD("irq: handler enter resume / sr", resume_pc, sr);
    cpu->cop0[14] = resume_pc;                                          /* EPC */
    cpu->cop0[13] = (cpu->cop0[13] & ~0x7Cu) | (1u << 10);              /* Cause: ExcCode 0 (interrupt), IP2 */
    cpu->cop0[12] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2);                /* SR: push the mode stack */
    g_in_exception = 1; g_rfe_escape_pending = 0; g_exc_escape_reason = 0; s_resume_epc = 0; s_path = 0;
    uint32_t target = (sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
    for (int hops = 0; hops < 64; hops++) {
        IRQD("irq: dispatch handler target / hop", target, hops);
        psx_dispatch(cpu, target);
        g_psx_call_bail = 0;
        if (s_path != 2) break;
        IRQD("irq: RestoreState redirect / epc", s_resume_epc, s_path);
        target = s_resume_epc; s_path = 0; s_resume_epc = 0;           /* RestoreState: carry on there, still in the exception */
        g_rfe_escape_pending = 0;
    }
    g_in_exception = 0; g_rfe_escape_pending = 0;
    uint32_t epc = s_resume_epc;
    IRQD("irq: handler end epc / path", epc, s_path);
    if (s_path == 0) {                                                  /* the handler simply returned: pop the SR stack ourselves */
        cpu->cop0[12] = (cpu->cop0[12] & ~0xFu) | ((cpu->cop0[12] >> 2) & 0xFu);
        epc = resume_pc;
    }
    cpu->pc = 0;
    if (!((epc ^ resume_pc) & 0x1FFFFFFFu)) {                           /* same context: give the interrupted code its registers back */
        for (int i = 1; i < 32; i++) cpu->gpr[i] = saved[i];
        cpu->hi = shi; cpu->lo = slo;
    }
    return epc;
}

static int deliverable(CPUState *cpu) {
    poll_sources();
    {   static int seen;                                                /* bring-up milestone: the kernel turned interrupts on */
        if (!seen && (cpu->cop0[12] & 1u) && (cpu->cop0[12] & (1u << 10))) { seen = 1; lion_logf("psx: interrupts enabled; SR / I_MASK", cpu->cop0[12], i_mask); }
    }
    {   static int seen_mask;
        if (!seen_mask && i_mask) { seen_mask = 1; lion_logf("psx: I_MASK set; mask / SR", i_mask, cpu->cop0[12]); }
    }
    if (!(i_stat & i_mask)) return 0;
    uint32_t sr = cpu->cop0[12];
    if (!((sr & 1u) && (sr & (1u << 10)))) {
        IRQTR("irq: pending but SR masked; stat/mask", i_stat, i_mask);
        { extern int g_ww_log_enabled; static int k; static uint32_t last;
          if (g_ww_log_enabled && k < 100 && (i_stat & 8u) && last != g_psx_last_fn_entry) { k++; last = g_psx_last_fn_entry; lion_logf("IRQ DMA pending, SR masked; sr / fn", sr, g_psx_last_fn_entry); } }
        return 0;
    }
    { static int n; if (n < 80) { n++; lion_logf("irq: deliverable stat/mask", i_stat, i_mask); } }
    return 1;                                                           /* IEc and IM2 */
}

#ifdef PSX_TRACE_PCS
uint32_t g_pc_ring[1024]; uint32_t g_pc_ring_pos;
#endif
/* From compiled code, at a block edge. */
void psx_check_interrupts_at(CPUState *cpu, uint32_t resume_pc) {
#ifdef PSX_TRACE_PCS
    g_pc_ring[g_pc_ring_pos++ & 1023] = resume_pc; g_pc_ring[0] = g_pc_ring_pos;
#endif
    if (g_in_exception) { poll_sources(); return; }                    /* time passes inside a handler too (callbacks wait on devices) */
    if (!deliverable(cpu)) return;
    s_irq_detail = (i_stat & ~1u) != 0;
    { extern int g_ww_log_enabled; static int k; uint32_t st = i_stat;
      if (g_ww_log_enabled && k < 400 && (st & ~1u)) { k++; lion_logf("IRQ deliver stat / mask", st, i_mask); lion_logf("IRQ   at pc / sr", resume_pc, cpu->cop0[12]); } }
    uint32_t epc = run_handler(cpu, resume_pc);
    { extern int g_ww_log_enabled; static int k;
      if (g_ww_log_enabled && k < 400 && s_irq_detail) { k++; lion_logf("IRQ   done epc / i_stat", epc, i_stat); } }
    if (!((epc ^ resume_pc) & 0x1FFFFFFFu)) return;                     /* same context: carry on where we were */
    { static int n; if (n < 12) { n++; lion_logf("psx: IRQ switched context; epc / interrupted", epc, resume_pc); lion_logf("psx:   path (1 RFE, 2 RestoreState) / ra", s_path, cpu->gpr[31]); lion_logf("psx:   sp / EPC reg", cpu->gpr[29], cpu->cop0[14]); } }
    lion_abandon(epc);
}
void psx_check_interrupts_dispatch_entry(CPUState *cpu, uint32_t resume_pc) { psx_check_interrupts_at(cpu, resume_pc); }

/* From the dirty-RAM interpreter, which publishes its precise resume address in g_dirty_safe_resume_pc and re-reads
 * cpu->pc afterwards, so a context switch needs nothing special there. */
extern uint32_t g_dirty_safe_resume_pc;
void psx_check_interrupts(CPUState *cpu) {
    uint32_t rp = g_dirty_safe_resume_pc;
    if (!rp) { cd_tick(); hw_tick(); sio_poll(); return; }                          /* the interpreter polls here too: keep the devices' clocks running */
    if (g_in_exception) { poll_sources(); return; }
    if (!deliverable(cpu)) return;
    s_irq_detail = (i_stat & ~1u) != 0;
    uint32_t epc = run_handler(cpu, rp);
    cpu->pc = epc ? epc : rp;
}
