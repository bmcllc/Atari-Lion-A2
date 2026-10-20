/* psxlion/rt/sio.c — SIO0 (0x1F801040..4F): the controller / memory card port, with a digital pad on port 1.
 *
 * Byte protocol (the host writes DATA, the device answers in RX, and pulls ACK low -> IRQ7 after each byte but the last):
 *   pad:  01 42 00 00 00  ->  FF 41 5A <buttons lo> <buttons hi>      (41 = digital pad, 1 data word; buttons are active low)
 *   card: 81 ...          ->  no card in the slot: no ACK
 * Buttons come from the Lion's controller port (0x1F050000 + 0x10 * port).
 */
#include <stdint.h>

extern void psx_irq_raise(uint32_t bit, uint32_t detail);

#define LION_PAD(port, off) (*(volatile uint32_t *)(0x1F050000u + 0x10u * (port) + (off)))

static uint16_t s_mode, s_ctrl, s_baud;
static uint8_t  s_rx, s_rx_next;
static int      s_rx_valid, s_dsr, s_irq, s_phase;
static uint32_t s_ntx, s_npad;                             /* bring-up counters (sio_stats) */

/* Byte timing, in PS1 clocks (hw.c's psx_clock): the received byte is ready when it has shifted (~1088 at the pads' 250 kHz),
 * and the device's /ACK pulse -- IRQ7 -- comes ~170 later. Drivers depend on it: OpenBIOS's readPad waits for RXRDY and then
 * spins a short, bounded loop on I_STAT bit 7, and libpad clears I_STAT bit 7 after the write and waits for it to come back.
 * An answer at the instant of the write is either missed or cleared, and the pad looks unplugged. */
#define SIO_SHIFT_CLK 1088u
#define SIO_ACK_CLK   (SIO_SHIFT_CLK + 170u)
extern uint64_t psx_clock(void);
static int      s_busy, s_ack_pending;
static uint64_t s_t_rx, s_t_ack;

/* Lion buttons -> PS1 digital pad (1 = pressed here; inverted when sent). */
static uint16_t pad_bits(int port) {
    uint32_t b = LION_PAD(port, 0), r = 0;
    if (b & (1u << 13)) r |= 1u << 0;     /* Select */
    if (b & (1u << 12)) r |= 1u << 3;     /* Start */
    if (b & (1u << 0))  r |= 1u << 4;     /* Up */
    if (b & (1u << 3))  r |= 1u << 5;     /* Right */
    if (b & (1u << 1))  r |= 1u << 6;     /* Down */
    if (b & (1u << 2))  r |= 1u << 7;     /* Left */
    if (b & (1u << 8))  r |= 1u << 8;     /* Y  -> L2 */
    if (b & (1u << 9))  r |= 1u << 9;     /* Z  -> R2 */
    if (b & (1u << 10)) r |= 1u << 10;    /* L  -> L1 */
    if (b & (1u << 11)) r |= 1u << 11;    /* R  -> R1 */
    if (b & (1u << 7))  r |= 1u << 12;    /* X  -> Triangle */
    if (b & (1u << 5))  r |= 1u << 13;    /* B  -> Circle */
    if (b & (1u << 4))  r |= 1u << 14;    /* A  -> Cross */
    if (b & (1u << 6))  r |= 1u << 15;    /* C  -> Square */
    return (uint16_t)~r;
}

static void answer(uint8_t rx, int ack) {
    uint64_t now = psx_clock();
    s_rx_next = rx; s_busy = 1; s_t_rx = now + SIO_SHIFT_CLK;
    s_ack_pending = ack; s_t_ack = now + SIO_ACK_CLK;
    s_dsr = 0;
}

/* Lets time pass for the byte in flight: called on every SIO register access and on I_STAT reads (hw.c). */
void sio_poll(void) {
    if (!s_busy && !s_ack_pending) return;
    uint64_t now = psx_clock();
    if (s_busy && now >= s_t_rx) { s_busy = 0; s_rx = s_rx_next; s_rx_valid = 1; }
    if (s_ack_pending && now >= s_t_ack) {
        s_ack_pending = 0; s_dsr = 1; s_irq = 1;
        if (s_ctrl & 0x1000) psx_irq_raise(7, 0);
    }
}

static void tx(uint8_t b) {
    int port = (s_ctrl >> 13) & 1;
    if (!(s_ctrl & 0x0002)) return;                                  /* /CS high: nothing selected */
    int present = LION_PAD(port, 8) != 0;
    switch (s_phase) {
    case 0:
        if (b == 0x01 && present) { answer(0xFF, 1); s_phase = 1; }
        else answer(0xFF, 0);                                         /* no pad / memory card (0x81): no ACK */
        break;
    case 1: answer(0x41, 1); s_phase = 2; break;
    case 2: answer(0x5A, 1); s_phase = 3; break;
    case 3: {
        uint16_t bits = pad_bits(port);
        s_npad++;
        { static int n; extern void lion_logf(const char *, uint32_t, uint32_t);       /* bring-up: input reaches the game */
          if (bits != 0xFFFFu && n < 8) { n++; lion_logf("sio: pad read with buttons / port", (uint16_t)~bits, port); } }
        answer((uint8_t)(bits & 0xFF), 1); s_phase = 4; break;
    }
    default: answer((uint8_t)(pad_bits(port) >> 8), 0); s_phase = 0; break;   /* last byte: no ACK */
    }
}

/* bring-up: the first register accesses, then a count of transferred bytes every 256 VI frames (gpu.c calls sio_stats) */
static int s_tr;
extern int fntrace_is_game_started(void);
#define SIOTR(w, a, b) do { if (s_tr < 60 && fntrace_is_game_started()) { s_tr++; extern void lion_logf(const char *, uint32_t, uint32_t); lion_logf(w, a, b); } } while (0)
void sio_stats(void) { extern void lion_logf(const char *, uint32_t, uint32_t); lion_logf("sio: bytes / pad polls per 256 frames", s_ntx, s_npad); s_ntx = s_npad = 0; }

uint32_t sio_read(uint32_t reg, int size) {
    (void)size;
    sio_poll();
    SIOTR("sio rd reg / ctrl", reg, s_ctrl);
    switch (reg) {
    case 0x0: { uint32_t v = s_rx; s_rx_valid = 0; return v; }
    case 0x4: {                                                       /* TXRDY, RXRDY, TXEMPTY, DSR (/ACK level), IRQ */
        uint32_t v = (s_busy ? 0u : 0x5u) | (s_rx_valid ? 2u : 0u) | (s_dsr ? 0x80u : 0u) | (s_irq ? 0x200u : 0u);
        if (s_dsr) s_dsr = 0;                                         /* the /ACK pulse is short: seen by one status read */
        return v;
    }
    case 0x8: return s_mode;
    case 0xA: return s_ctrl;
    case 0xE: return s_baud;
    }
    return 0;
}

void sio_write(uint32_t reg, uint32_t v, int size) {
    (void)size;
    sio_poll();
    SIOTR("sio wr reg / val", reg, v);
    if (reg == 0) s_ntx++;
    switch (reg) {
    case 0x0: tx((uint8_t)v); break;
    case 0x8: s_mode = (uint16_t)v; break;
    case 0xA:
        if (v & 0x40) { s_ctrl = 0; s_phase = 0; s_rx_valid = 0; s_dsr = 0; s_irq = 0; s_busy = s_ack_pending = 0; break; }   /* reset */
        if (!(v & 0x2)) { s_phase = 0; s_ack_pending = 0; }                                                /* deselected: transaction ends */
        if (v & 0x10) { s_irq = 0; }                                                                        /* acknowledge */
        s_ctrl = (uint16_t)(v & ~0x10u);
        break;
    case 0xE: s_baud = (uint16_t)v; break;
    }
}
