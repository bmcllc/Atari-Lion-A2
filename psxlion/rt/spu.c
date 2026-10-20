/* psxlion/rt/spu.c — the PS1 SPU (0x1F801C00..0x1F801FFF) played on the Lion's Tyke.
 *
 * The SPU has 24 ADPCM voices reading a 512 KB sound RAM; the Tyke's mixer firmware has 24 PCM16 voices reading SDRAM
 * (docs/hardware.md, "Tyke"). Voice n of the SPU is Tyke voice n:
 *
 *   - Key-on decodes the voice's ADPCM, from its start address to the block with the end flag, into a PCM16 big-endian
 *     buffer (a 1.5 MB ring), and keys the Tyke voice on it. A looping sample (end + repeat flags) loops back to its loop
 *     address -- the last block with the loop-start flag, else the repeat register -- which becomes the Tyke voice's
 *     LOOP_START; a loop target outside the decoded run is decoded too and appended. Decodes are cached by address and
 *     reused while the sound RAM they came from is unchanged (write sequence per 1 KB page).
 *   - Pitch: SPU 0x1000 = 44.1 kHz = Tyke 0x10000.
 *   - ADSR envelopes, sweeps and ENDX run on the Tuffy once per VI frame (735 samples), and the Tyke volume of each voice
 *     is volume x envelope x main volume.
 *   - Sound RAM transfers: the data FIFO (manual write) and DMA channel 4, both directions.
 *
 * Not done: reverb, noise, pitch modulation, the SPU IRQ, CD audio/XA mixing, sweep curves (a sweeping volume jumps to its
 * end value).
 */
#include <stdint.h>

extern void lion_logf(const char *, uint32_t, uint32_t);

#define TYKE(o)     (*(volatile uint32_t *)(0x1F020000u + (o)))
#define TV(ch, r)   TYKE(0x100u + 0x20u * (ch) + (r))
enum { TV_START = 0x00, TV_LENGTH = 0x04, TV_LOOP = 0x08, TV_PITCH = 0x0C, TV_VOLUME = 0x10, TV_CTRL = 0x14, TV_POS = 0x18 };
#define VI_FRAME (*(volatile uint32_t *)0x1F030010u)

#define SPU_RAM 0x80000u
static uint8_t  s_ram[SPU_RAM] __attribute__((aligned(32)));
static uint16_t s_reg[0x200];                       /* 0x1F801C00..0x1F801FFF, halfword-indexed */
static uint32_t s_xfer;                             /* transfer address (bytes) */
static uint32_t s_endx;

/* ---- sound RAM write tracking --------------------------------------------------------------------------------- */
static uint32_t s_wseq = 1, s_page_seq[SPU_RAM >> 10];
static inline void ram_wr16(uint32_t a, uint16_t v) {
    a &= SPU_RAM - 2;
    s_ram[a] = (uint8_t)v; s_ram[a + 1] = (uint8_t)(v >> 8);
    s_page_seq[a >> 10] = s_wseq;
}
static inline uint16_t ram_rd16(uint32_t a) { a &= SPU_RAM - 2; return (uint16_t)(s_ram[a] | s_ram[a + 1] << 8); }

/* ---- decoded sample cache ------------------------------------------------------------------------------------- */
#define POOL_SAMPLES (768u * 1024u)                 /* 1.5 MB of PCM16 */
static int16_t s_pool[POOL_SAMPLES] __attribute__((aligned(32)));
static uint32_t s_pool_pos;
typedef struct { uint32_t start, repeat, seq, lo, hi, off, len, loop_at, target; uint8_t valid, loops, uses_repeat; } Entry;
#define ENTRIES 256
static Entry s_ent[ENTRIES];
static uint32_t s_ent_next;
#define RUN_CAP (256u * 1024u)                      /* samples per run: ~6 s; a run without an end flag stops here */

static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };

/* Walks the blocks of a run from `addr` to the one with the end flag: block count, flags of that block, the address after it,
 * and the last block with the loop-start flag (~0 if none). */
static uint32_t scan_run(uint32_t addr, uint8_t *end_flags, uint32_t *end_addr, uint32_t *loop_blk) {
    uint32_t n = 0;
    *loop_blk = ~0u; *end_flags = 0;
    while (n < RUN_CAP / 28) {
        const uint8_t *b = &s_ram[addr & (SPU_RAM - 16)];
        if (b[1] & 4) *loop_blk = addr & (SPU_RAM - 16);
        n++; addr = (addr + 16) & (SPU_RAM - 1);
        if (b[1] & 1) { *end_flags = b[1]; break; }
    }
    *end_addr = addr;
    return n;
}

/* Decodes `blocks` ADPCM blocks from `addr` into pool[at..] as big-endian PCM16 (what the Tyke reads). */
static void decode_run(uint32_t addr, uint32_t blocks, uint32_t at) {
    int32_t old = 0, older = 0;
    uint8_t *out = (uint8_t *)&s_pool[at];
    for (uint32_t k = 0; k < blocks; k++, addr = (addr + 16) & (SPU_RAM - 1)) {
        const uint8_t *b = &s_ram[addr & (SPU_RAM - 16)];
        int shift = b[0] & 15, filt = (b[0] >> 4) & 7;
        if (shift > 12) shift = 9;
        if (filt > 4) filt = 4;
        for (int i = 0; i < 28; i++) {
            int nib = (b[2 + (i >> 1)] >> ((i & 1) * 4)) & 15;
            int32_t s = ((int32_t)(int16_t)(nib << 12) >> shift) + ((old * f0[filt] + older * f1[filt] + 32) >> 6);
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            older = old; old = s;
            *out++ = (uint8_t)((uint32_t)s >> 8); *out++ = (uint8_t)s;
        }
    }
}

/* Has sound RAM in [lo, hi) been written since `seq`? (Writes stamp their 1 KB page with the current s_wseq; a decode takes a
 * fresh, larger sequence number.) */
static int range_clean(uint32_t lo, uint32_t hi, uint32_t seq) {
    for (uint32_t p = lo >> 10; p < SPU_RAM >> 10 && p <= (hi - 1) >> 10; p++) if (s_page_seq[p] >= seq) return 0;
    return 1;
}

/* Finds or makes the decode of a voice keyed on at `start` with repeat register `repeat`. */
static const Entry *sample_for(uint32_t start, uint32_t repeat) {
    for (int i = 0; i < ENTRIES; i++) {
        Entry *e = &s_ent[i];
        if (e->valid && e->start == start && (!e->uses_repeat || e->repeat == repeat) && range_clean(e->lo, e->hi, e->seq)) return e;
    }
    uint8_t fl, fl2; uint32_t end, lblk, end2, lblk2, n2 = 0, target = 0;
    uint32_t n = scan_run(start, &fl, &end, &lblk);
    int loops = (fl & 3) == 3, uses_repeat = 0, inside = 0;
    if (loops) {
        uses_repeat = lblk == ~0u;
        target = uses_repeat ? (repeat & (SPU_RAM - 16)) : lblk;
        inside = target >= start && target < start + n * 16;
        if (!inside) n2 = scan_run(target, &fl2, &end2, &lblk2);
    }
    uint32_t len = (n + n2) * 28;
    if (s_pool_pos + len > POOL_SAMPLES) s_pool_pos = 0;               /* ring: start over at the bottom */
    uint32_t off = s_pool_pos;
    for (int i = 0; i < ENTRIES; i++)                                  /* entries whose PCM this overwrites */
        if (s_ent[i].valid && s_ent[i].off < off + len && off < s_ent[i].off + s_ent[i].len) s_ent[i].valid = 0;
    decode_run(start, n, off);
    if (n2) decode_run(target, n2, off + n * 28);
    Entry *e = &s_ent[s_ent_next++ % ENTRIES];
    e->start = start; e->repeat = repeat; e->seq = ++s_wseq; e->off = off; e->len = len;
    e->loops = (uint8_t)loops; e->uses_repeat = (uint8_t)uses_repeat; e->target = target;
    e->loop_at = !loops ? len : inside ? (target - start) / 16 * 28 : n * 28;
    e->lo = start; e->hi = start + n * 16;
    if (n2) { if (target < e->lo) e->lo = target; if (target + n2 * 16 > e->hi) e->hi = target + n2 * 16; }
    e->valid = 1;
    s_pool_pos = (off + len + 15) & ~15u;
    return e;
}

/* ---- voices --------------------------------------------------------------------------------------------------- */
enum { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };
typedef struct { int phase; int32_t level; int32_t counter; uint32_t len, loop_at; int loops; uint32_t last_pos; } Voice;
static Voice s_v[24];

static uint32_t adsr(int ch) { return s_reg[ch * 8 + 4] | (uint32_t)s_reg[ch * 8 + 5] << 16; }

/* One envelope step's parameters for the voice's phase (psx-spx "SPU Envelope"). */
static void env_params(int ch, int *cycles, int *step) {
    uint32_t a = adsr(ch);
    int shift, stepv, expo = 0, dec = 0;
    Voice *v = &s_v[ch];
    switch (v->phase) {
    case PH_ATTACK:  shift = (a >> 10) & 31; stepv = 7 - (int)((a >> 8) & 3); expo = (a >> 15) & 1; break;
    case PH_DECAY:   shift = (a >> 4) & 15;  stepv = -8; expo = 1; dec = 1; break;
    case PH_SUSTAIN: shift = (a >> 24) & 31; dec = (a >> 30) & 1; expo = (a >> 31) & 1;
                     stepv = dec ? -8 + (int)((a >> 22) & 3) : 7 - (int)((a >> 22) & 3); break;
    default:         shift = (a >> 16) & 31; stepv = -8; expo = (a >> 21) & 1; dec = 1; break;
    }
    int c = 1 << (shift > 11 ? shift - 11 : 0);
    int s = stepv << (shift < 11 ? 11 - shift : 0);
    if (expo && !dec && v->level > 0x6000) c *= 4;
    if (expo && dec) s = (int)(((int64_t)s * v->level) >> 15);
    *cycles = c; *step = s;
}

static void env_advance(int ch, int samples) {
    Voice *v = &s_v[ch];
    while (samples > 0 && v->phase != PH_OFF) {
        int cycles, step;
        env_params(ch, &cycles, &step);
        if (v->counter <= 0) v->counter = cycles;
        if (v->counter > samples) { v->counter -= samples; break; }
        samples -= v->counter; v->counter = 0;
        v->level += step;
        if (v->level > 0x7FFF) v->level = 0x7FFF;
        if (v->level < 0) v->level = 0;
        if (v->phase == PH_ATTACK && v->level >= 0x7FFF) v->phase = PH_DECAY;
        else if (v->phase == PH_DECAY && v->level <= (int32_t)((adsr(ch) & 15) + 1) * 0x800) v->phase = PH_SUSTAIN;
        else if (v->phase == PH_RELEASE && v->level == 0) v->phase = PH_OFF;
    }
}

/* A volume register: fixed (bit 15 = 0, 15-bit signed half-volume) or a sweep, taken at its end value. */
static int32_t vol_of(uint16_t r) {
    if (!(r & 0x8000)) { int32_t v = (int32_t)(int16_t)(r << 1); return v < 0 ? -v : v; }
    return (r & 0x2000) ? 0 : 0x7FFF;                               /* sweep: decrease -> 0, increase -> max */
}

static void voice_volume(int ch) {
    uint16_t cnt = s_reg[0x1AA >> 1];
    uint32_t l = 0, r = 0;
    if ((cnt & 0x8000) && (cnt & 0x4000) && s_v[ch].phase != PH_OFF) {
        int32_t env = s_v[ch].level, ml = vol_of(s_reg[0x180 >> 1]), mr = vol_of(s_reg[0x182 >> 1]);
        l = (uint32_t)(((vol_of(s_reg[ch * 8 + 0]) * env >> 15) * ml >> 15) >> 7);
        r = (uint32_t)(((vol_of(s_reg[ch * 8 + 1]) * env >> 15) * mr >> 15) >> 7);
    }
    TV(ch, TV_VOLUME) = l << 16 | r;
}

static void key_on(int ch) {
    uint32_t start = (uint32_t)s_reg[ch * 8 + 3] * 8u, repeat = (uint32_t)s_reg[ch * 8 + 7] * 8u;
    const Entry *e = sample_for(start & (SPU_RAM - 16), repeat);
    Voice *v = &s_v[ch];
    v->phase = PH_ATTACK; v->level = 0; v->counter = 0; v->len = e->len; v->loop_at = e->loop_at; v->loops = e->loops; v->last_pos = 0;
    s_endx &= ~(1u << ch);
    if (e->loops) s_reg[ch * 8 + 7] = (uint16_t)(e->target >> 3);     /* the loop-start block latches the repeat address */
    uint32_t pitch = s_reg[ch * 8 + 2]; if (pitch > 0x4000) pitch = 0x4000;
    TV(ch, TV_CTRL) = 0;
    TV(ch, TV_START) = (uint32_t)(uintptr_t)&s_pool[e->off];
    TV(ch, TV_LENGTH) = e->len;
    TV(ch, TV_LOOP) = e->loops ? e->loop_at : e->len;
    TV(ch, TV_PITCH) = pitch << 4;
    voice_volume(ch);
    TV(ch, TV_CTRL) = 1u | (e->loops ? 2u : 0u);
    { static int n; if (n < 6) { n++; lion_logf("spu: key on voice / start", ch, start); lion_logf("spu:   samples / loop", e->len, e->loops ? e->loop_at : ~0u); } }
}

static void key_off(int ch) { if (s_v[ch].phase != PH_OFF) { s_v[ch].phase = PH_RELEASE; s_v[ch].counter = 0; } }

/* Once per VI frame (irq.c): envelopes, ends of samples, volumes. */
void spu_frame_tick(void) {
    static uint32_t last;
    uint32_t f = VI_FRAME;
    int frames = (int)(f - last); last = f;
    if (frames <= 0) return;
    if (frames > 6) frames = 6;
    for (int ch = 0; ch < 24; ch++) {
        Voice *v = &s_v[ch];
        if (v->phase == PH_OFF) continue;
        env_advance(ch, 735 * frames);
        uint32_t ctrl = TV(ch, TV_CTRL), pos = TV(ch, TV_POS);
        if (!(ctrl & 1)) { s_endx |= 1u << ch; v->phase = PH_OFF; v->level = 0; }                    /* one-shot sample ran out */
        else if (v->loops && pos < v->last_pos) s_endx |= 1u << ch;                                 /* passed its end and looped */
        v->last_pos = pos;
        if (v->phase == PH_OFF) TV(ch, TV_CTRL) = 0;
        voice_volume(ch);
    }
}

/* ---- registers ------------------------------------------------------------------------------------------------ */
static uint16_t stat(void) {
    uint16_t cnt = s_reg[0x1AA >> 1], mode = (cnt >> 4) & 3;
    return (uint16_t)((cnt & 0x3F) | (mode == 2 ? 0x100 : 0) | (mode == 3 ? 0x200 : 0) | (mode >= 2 ? 0x80 : 0));
}

static uint16_t rd16(uint32_t off) {
    if (off < 0x180 && (off & 15) == 0xC) return (uint16_t)s_v[off >> 4].level;                   /* current envelope */
    switch (off) {
    case 0x19C: return (uint16_t)s_endx;
    case 0x19E: return (uint16_t)(s_endx >> 16);
    case 0x1AE: return stat();
    case 0x1B8: return s_reg[0x180 >> 1];
    case 0x1BA: return s_reg[0x182 >> 1];
    }
    return s_reg[off >> 1];
}

static void wr16(uint32_t off, uint16_t v) {
    s_reg[off >> 1] = v;
    switch (off) {
    case 0x188: for (int i = 0; i < 16; i++) if (v >> i & 1) key_on(i); break;
    case 0x18A: for (int i = 0; i < 8; i++) if (v >> i & 1) key_on(16 + i); break;
    case 0x18C: for (int i = 0; i < 16; i++) if (v >> i & 1) key_off(i); break;
    case 0x18E: for (int i = 0; i < 8; i++) if (v >> i & 1) key_off(16 + i); break;
    case 0x19C: case 0x19E: break;                                  /* ENDX is read-only */
    case 0x1A6: s_xfer = (uint32_t)v * 8u; break;
    case 0x1A8: ram_wr16(s_xfer, v); s_xfer = (s_xfer + 2) & (SPU_RAM - 1); break;   /* data FIFO (written through) */
    case 0x1AA:
        if (!(v & 0x8000)) for (int ch = 0; ch < 24; ch++) { s_v[ch].phase = PH_OFF; TV(ch, TV_CTRL) = 0; }
        break;
    default:
        if (off < 0x180) {
            int ch = off >> 4, r = (off & 15) >> 1;
            if (r == 2) { uint32_t p = v > 0x4000 ? 0x4000 : v; TV(ch, TV_PITCH) = p << 4; }          /* pitch changes take effect live */
            else if (r <= 1) voice_volume(ch);
        } else if (off == 0x180 || off == 0x182) for (int ch = 0; ch < 24; ch++) voice_volume(ch);
        break;
    }
}

uint32_t spu_read(uint32_t phys, int size) {
    uint32_t off = (phys - 0x1F801C00u) & 0x3FE;
    if (size == 4) return rd16(off) | (uint32_t)rd16(off + 2) << 16;
    uint32_t v = rd16(off);
    return size == 1 ? (v >> ((phys & 1) * 8)) & 0xFF : v;
}

void spu_write(uint32_t phys, uint32_t val, int size) {
    uint32_t off = (phys - 0x1F801C00u) & 0x3FE;
    if (size == 4) { wr16(off, (uint16_t)val); wr16(off + 2, (uint16_t)(val >> 16)); return; }
    if (size == 1) { uint16_t cur = s_reg[off >> 1]; val = (phys & 1) ? (cur & 0xFF) | (val & 0xFF) << 8 : (cur & 0xFF00) | (val & 0xFF); }
    wr16(off, (uint16_t)val);
}

/* DMA channel 4, one word at a time (hw.c). */
void spu_dma_write(uint32_t w) {
    ram_wr16(s_xfer, (uint16_t)w); ram_wr16(s_xfer + 2, (uint16_t)(w >> 16));
    s_xfer = (s_xfer + 4) & (SPU_RAM - 1);
}
uint32_t spu_dma_read(void) {
    uint32_t w = ram_rd16(s_xfer) | (uint32_t)ram_rd16(s_xfer + 2) << 16;
    s_xfer = (s_xfer + 4) & (SPU_RAM - 1);
    return w;
}
