/* psxlion/rt/cd.c — the PS1 CD-ROM controller (0x1F801800..03): register banks, parameter/response FIFOs, the command set,
 * delayed INT1..5 responses, the sector data FIFO, and the sector source.
 *
 * Sector source: the raw 2352-byte .bin of the PS1 disc (data track) is one big file (GAME.BIN) on a Lion ISO. A PS1 sector N lives at
 * byte N*2352 of that file; it is fetched through the Lion's CD (2048-byte sectors, DMA to RAM) in read-ahead chunks.
 * Without that file the drive reads as empty (lid open).
 */
#include <stdint.h>

extern void psx_irq_raise(uint32_t bit, uint32_t detail);
extern void lion_log(const char *);
extern void lion_logf(const char *, uint32_t, uint32_t);
extern void lion_log_reset(void);
extern int g_ww_log_enabled;
extern uint8_t *g_psx_ram;
void ww_after(uint32_t phys, uint32_t len, uint32_t kind);      /* watch.c */
void ww_cd_event(uint32_t tag, uint32_t a, uint32_t b);         /* watch.c fill trace */
void ww_cd_dma(uint32_t dst, uint32_t bytes, uint32_t avail);   /* watch.c fill trace */

#include "lioncd.h"

static uint32_t s_disc_lba, s_disc_bytes;                  /* where GAME.BIN sits on the ISO, and its size */

/* Finds the PS1 image (GAME.BIN; MK4.BIN on older discs) in the root directory of the ISO the Lion booted from. Call once
 * at start-up. */
void cd_attach(void) {
    static uint8_t buf[2048] __attribute__((aligned(32)));
    s_disc_lba = s_disc_bytes = 0;
    if (lion_find_file("GAME.BIN", 8, buf, &s_disc_lba, &s_disc_bytes) || lion_find_file("MK4.BIN", 7, buf, &s_disc_lba, &s_disc_bytes))
        lion_logf("cd: game image lba / bytes", s_disc_lba, s_disc_bytes);
    else { s_disc_bytes = 0; lion_log("cd: no GAME.BIN on the disc"); }
}

/* ---- PS1 sector cache: chunks of the raw image, read ahead in the background ----------------------------------------
 * Every READ of the Lion's drive pays a seek (80 ms and up), so the image is read in chunks of CHUNK PS1 sectors, straight by
 * DMA into a chunk buffer (a sector is 2352 contiguous bytes of it: no copy). Serving a sector from chunk k starts the read of
 * chunk k+1 if nobody has it, without waiting: a streaming read (FMV, XA music: 150 sectors/s) then never waits for the drive,
 * where reading on demand blocked the Tuffy for most of every second. Only a sector in no chunk (a seek by the game) waits. */
#define SEC 2352u
#define CHUNK 64u                                           /* PS1 sectors per chunk: ~150 KB, ~70 ms of transfer at 12x */
#define NCHUNK 4u
#define CHUNK_LION ((CHUNK * SEC + 2047u) / 2048u + 1u)     /* Lion sectors a chunk can span */
static uint8_t  s_chunk[NCHUNK][CHUNK_LION * 2048u] __attribute__((aligned(32)));
static uint32_t s_ch_first[NCHUNK] = { ~0u, ~0u, ~0u, ~0u }; /* first PS1 sector of each chunk */
static uint32_t s_ch_skew[NCHUNK], s_ch_last[NCHUNK];       /* byte of that sector in the buffer; LRU stamp */
static int      s_ch_pending = -1;                          /* chunk with a READ in flight */
static uint32_t s_ch_clock;

static uint32_t total_sectors(void);
static int lcd_done(void) {                                 /* completes the read in flight if it has finished */
    if (s_ch_pending < 0) return 1;
    uint32_t st = LCD(0x00);
    if (!(st & 0xC)) return 0;
    LCD(0x04) = 3;                                          /* ACK */
    if (st & 4) s_ch_first[s_ch_pending] = ~0u;             /* error: the chunk holds nothing */
    s_ch_pending = -1;
    return 1;
}
static void lcd_wait(void) { while (!lcd_done()) { } }

static void chunk_start(int c, uint32_t lba) {              /* starts reading PS1 sectors [lba, lba + CHUNK) into chunk c */
    lcd_wait();
    uint64_t off = (uint64_t)lba * SEC;
    uint32_t first = (uint32_t)(off >> 11), n = CHUNK_LION, disc = (s_disc_bytes + 2047u) >> 11;
    if (first + n > disc) n = disc - first;
    s_ch_first[c] = lba; s_ch_skew[c] = (uint32_t)(off & 2047); s_ch_last[c] = ++s_ch_clock;
    LCD(0x08) = s_disc_lba + first; LCD(0x0C) = n; LCD(0x10) = (uint32_t)(uintptr_t)s_chunk[c];
    LCD(0x04) = 1;                                          /* READ */
    s_ch_pending = c;
}
static int chunk_of(uint32_t lba) {
    for (int c = 0; c < (int)NCHUNK; c++) if (s_ch_first[c] != ~0u && lba - s_ch_first[c] < CHUNK) return c;
    return -1;
}
static int chunk_victim(int keep) {                         /* least recently used chunk, never `keep` or the one in flight */
    int v = -1;
    for (int c = 0; c < (int)NCHUNK; c++)
        if (c != keep && c != s_ch_pending && (v < 0 || s_ch_last[c] < s_ch_last[v])) v = c;
    return v;
}

static const uint8_t *fetch(uint32_t lba) {
    lcd_done();
    int c = chunk_of(lba);
    if (c < 0) { c = chunk_victim(-1); chunk_start(c, lba); }
    if (c == s_ch_pending) lcd_wait();
    if (s_ch_first[c] == ~0u) return 0;                     /* the read failed */
    s_ch_last[c] = ++s_ch_clock;
    uint32_t next = s_ch_first[c] + CHUNK;                  /* read ahead: the following chunk, in the background */
    if (s_ch_pending < 0 && next < total_sectors() && chunk_of(next) < 0) chunk_start(chunk_victim(c), next);
    return s_chunk[c] + s_ch_skew[c] + (lba - s_ch_first[c]) * SEC;
}

/* ---- controller state ---------------------------------------------------------------------------------------- */
enum { STAT_ERROR = 0x01, STAT_MOTOR = 0x02, STAT_SHELL = 0x10, STAT_READ = 0x20, STAT_SEEK = 0x40 };

static uint8_t  s_index, s_enable, s_flag;
static uint8_t  s_param[16], s_nparam;
static uint8_t  s_resp[16], s_nresp, s_rpos;
static uint8_t  s_mode;
static uint8_t  s_filter_file, s_filter_chan;              /* Setfilter: the XA file/channel that plays */
static uint32_t s_loc;                                      /* Setloc target (PS1 LBA, pregap removed) */
static int      s_pending_seek;
static int      s_reading;
static uint32_t s_read_lba, s_read_delay;
static const uint8_t *s_data;                               /* current sector in the cache (all 2352 bytes) */
static uint32_t s_dpos, s_dlen;                             /* data FIFO window into it */
static uint32_t s_data_ready;                               /* a sector has been received */
static uint8_t  s_req;                                      /* request register: bit 7 BFRD = the host wants the data FIFO */

typedef struct { uint16_t delay; uint8_t type, n, b[8]; } Pending;
static Pending s_q[8]; static int s_nq;

static int has_disc(void) { return s_disc_bytes != 0; }
static uint8_t stat(void) { return (has_disc() ? STAT_MOTOR : STAT_SHELL) | (s_reading ? STAT_READ : 0); }
static uint32_t bcd(uint32_t v) { return ((v / 10) << 4) | (v % 10); }
static uint32_t unbcd(uint32_t v) { return (v >> 4) * 10 + (v & 15); }
static uint32_t total_sectors(void) { return s_disc_bytes / SEC; }

static void queue(uint16_t delay, uint8_t type, int n, const uint8_t *b) {
    if (s_nq >= 8) return;
    Pending *p = &s_q[s_nq++];
    p->delay = delay; p->type = type; p->n = (uint8_t)n;
    for (int i = 0; i < n; i++) p->b[i] = b[i];
}
static void q1(uint16_t d, uint8_t type) { uint8_t s = stat(); queue(d, type, 1, &s); }
static void qerr(uint8_t code) { uint8_t e[2] = { (uint8_t)(stat() | STAT_ERROR), code }; queue(2000, 5, 2, e); }

#ifdef PSX_TRACE_CD
static int s_tr;
#define TR(w, a, b) do { if (s_tr < 400) { s_tr++; lion_logf(w, a, b); } } while (0)
#else
#define TR(w, a, b) do { } while (0)
#endif
static void command(uint8_t cmd) {
    uint8_t s = stat();
    ww_cd_event(0xCD000000u | cmd, (uint32_t)s_param[0] | (uint32_t)s_param[1] << 8 | (uint32_t)s_param[2] << 16, s_loc);
    { static int ncmd; if (ncmd < 400) { ncmd++; lion_logf("cd cmd / params", cmd, (s_nparam > 0 ? s_param[0] : 0xFF) << 16 | (s_nparam > 1 ? s_param[1] : 0xFF) << 8 | (s_nparam > 2 ? s_param[2] : 0xFF)); } }
    switch (cmd) {
    case 0x01: q1(2000, 3); break;                                              /* Getstat */
    case 0x02:                                                                  /* Setloc */
        TR("cd setloc mm:ss / ff", s_param[0] << 8 | s_param[1], s_param[2]);
        if (s_nparam >= 3) { uint32_t l = (unbcd(s_param[0]) * 60 + unbcd(s_param[1])) * 75 + unbcd(s_param[2]); s_loc = l >= 150 ? l - 150 : 0; }
        q1(2000, 3); break;
    case 0x06: case 0x1B:                                                       /* ReadN / ReadS */
#ifdef PSX_TRACE_STREAM         /* bring-up of the STR streaming path: IRQ, DMA and data FIFO traces from the first ReadS on */
        if (cmd == 0x1B && !g_ww_log_enabled) { g_ww_log_enabled = 1; lion_logf("cd ReadS: stream trace on; lba / mode", s_loc, s_mode); }
#endif
        if (!has_disc()) { qerr(0x80); break; }
        s_reading = 1; s_read_lba = s_loc; s_read_delay = 3000; s_data_ready = 0;
        q1(2000, 3); break;
    case 0x09: s_reading = 0; q1(2000, 3); q1(8000, 2); break;                  /* Pause */
    case 0x08: s_reading = 0; q1(2000, 3); q1(8000, 2); break;                  /* Stop */
    case 0x0A: s_reading = 0; s_mode = 0; q1(2000, 3); q1(30000, 2); break;     /* Init */
    case 0x0B: case 0x0C: q1(2000, 3); break;                                   /* Mute / Demute */
    case 0x0D: if (s_nparam >= 2) { s_filter_file = s_param[0]; s_filter_chan = s_param[1]; } q1(2000, 3); break;   /* Setfilter */
    case 0x0E:
        if (s_nparam) s_mode = s_param[0];
        { static int n; static uint32_t last = ~0u; if (s_mode != last && n < 40) { n++; last = s_mode; lion_logf("cd setmode mode / read lba", s_mode, s_read_lba); } }
        q1(2000, 3); break;                                                     /* Setmode */
    case 0x0F: { uint8_t r[5] = { s, s_mode, 0, 0, 0 }; queue(2000, 3, 5, r); break; }   /* Getparam */
    case 0x10: case 0x11: {                                                     /* GetlocL / GetlocP */
        uint32_t l = s_read_lba + 150, m = l / 4500, sc = (l / 75) % 60, f = l % 75;
        if (cmd == 0x10) { uint8_t r[8] = { (uint8_t)bcd(m), (uint8_t)bcd(sc), (uint8_t)bcd(f), 2, 0, 0, 0x08, 0 }; queue(2000, 3, 8, r); }
        else { uint8_t r[8] = { 1, 1, (uint8_t)bcd(m), (uint8_t)bcd(sc), (uint8_t)bcd(f), (uint8_t)bcd(m), (uint8_t)bcd(sc), (uint8_t)bcd(f) }; queue(2000, 3, 8, r); }
        break; }
    case 0x13: { uint8_t r[3] = { s, 1, 1 }; queue(2000, 3, 3, r); break; }     /* GetTN: one track */
    case 0x14: {                                                                /* GetTD */
        uint32_t trk = s_nparam ? unbcd(s_param[0]) : 0, l = trk <= 1 && trk != 0 ? 150 : total_sectors() + 150;
        uint8_t r[3] = { s, (uint8_t)bcd(l / 4500), (uint8_t)bcd((l / 75) % 60) };
        queue(2000, 3, 3, r); break; }
    case 0x15: case 0x16: s_reading = 0; q1(2000, 3); q1(10000, 2); break;      /* SeekL / SeekP */
    case 0x12: case 0x1D: q1(2000, 3); break;                                   /* SetSession / GetQ-ish */
    case 0x19: {                                                                /* Test */
        if (s_nparam && s_param[0] == 0x20) { static const uint8_t v[4] = { 0x94, 0x09, 0x19, 0xC0 }; queue(2000, 3, 4, v); }
        else q1(2000, 3);
        break; }
    case 0x1A:                                                                  /* GetID */
        q1(2000, 3);
        if (has_disc()) { static const uint8_t id[8] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' }; queue(20000, 2, 8, id); }
        else { static const uint8_t none[8] = { 0x08, 0x40, 0, 0, 0, 0, 0, 0 }; queue(20000, 5, 8, none); }
        break;
    case 0x1E: q1(2000, 3); q1(30000, 2); break;                                /* ReadTOC */
    default: qerr(0x40); break;
    }
    s_nparam = 0;
}

static void deliver(uint8_t type, const uint8_t *b, int n) {
#ifdef PSX_TRACE_MMIO
    { extern void hw_trace_arm(void); static int k; if (type == 2 && ++k == 5) hw_trace_arm(); }   /* after the 2nd directory read's Pause */
#endif
    TR("cd INT / byte0", type, n ? b[0] : 0);
    { static int k, k1; if (type != 1 ? (k < 150 && ++k) : (k1 < 3 && ++k1)) lion_logf("cd INT type / enable", type, s_enable); }
    s_nresp = (uint8_t)n; s_rpos = 0;
    for (int i = 0; i < n; i++) s_resp[i] = b[i];
    s_flag = type;
    if (s_enable & s_flag) psx_irq_raise(2, 0);
}

/* Once per interrupt poll: queued responses, then the sector clock while a read is running. */
void cd_tick(void) {
    if (s_nq) {
        Pending *p = &s_q[0];
        if (p->delay) p->delay--;
        else if (!s_flag) {
            deliver(p->type, p->b, p->n);
            for (int i = 1; i < s_nq; i++) s_q[i - 1] = s_q[i];
            s_nq--;
        }
        return;
    }
    if (s_reading && !s_flag) {
        if (s_read_delay) { s_read_delay--; return; }
        const uint8_t *sec = s_read_lba < total_sectors() ? fetch(s_read_lba) : 0;
        if (!sec) { s_reading = 0; uint8_t e[2] = { (uint8_t)(stat() | STAT_ERROR), 0x04 }; deliver(5, e, 2); return; }
        /* XA-ADPCM on (mode bit 6): an audio sector (subheader submode bit 2) goes to the SPU and is never seen by the CPU —
         * no INT1, nothing in the data FIFO. With the filter (bit 3) only the selected file/channel plays; the others are
         * dropped as well. STR movies interleave these with the video sectors, and the stream reader expects to see video
         * only. */
        if ((s_mode & 0x40) && (sec[18] & 0x04)) {
            extern void xa_sector(const uint8_t *raw, int filtered_out);
            xa_sector(sec, (s_mode & 0x08) && (sec[16] != s_filter_file || sec[17] != s_filter_chan));
            s_read_lba++;
            s_read_delay = (s_mode & 0x80) ? 2500 : 5000;
            return;
        }
        s_data = sec; s_data_ready = 1; s_dpos = 0; s_dlen = (s_mode & 0x20) ? 2340 : 2048;
        if (g_ww_log_enabled) {
            static int k;
            if (k < 200) {
                k++;
                lion_logf("S lba/dlen", s_read_lba, s_dlen);
                lion_logf("S dpos/req", s_dpos, s_req);
                lion_logf("S mode/flag", s_mode, s_flag);
            }
        }
        ww_cd_event(0xCD100000u | (s_read_lba & 0xFFFFu), s_dlen, (uint32_t)s_mode);
        { static int once; if (!once) { once = 1; lion_logf("cd first sector mode / dlen", s_mode, s_dlen); } }
        { static int n; if (!(s_read_lba & 0x3F) && n < 200) { n++; lion_logf("cd INT1 lba", s_read_lba, 0); } }
        s_read_lba++;
        s_read_delay = (s_mode & 0x80) ? 2500 : 5000;                      /* 150 / 75 sectors per second, in poll ticks */
        uint8_t s = stat(); deliver(1, &s, 1);
    }
}

uint32_t cd_read(uint32_t reg) {
    switch (reg) {
    case 0: {
        uint32_t r = s_index | (s_nparam == 0 ? 0x08 : 0) | (s_nparam < 16 ? 0x10 : 0) | (s_rpos < s_nresp ? 0x20 : 0)
                  | (((s_req & 0x80) && s_dpos < s_dlen) ? 0x40 : 0) | (s_nq ? 0x80 : 0);
        if (g_ww_log_enabled) {
            static uint32_t last; static int k;
            if ((r & 0x40u) != last && k < 200) {
                k++; last = r & 0x40u;
                lion_logf("S stat6 bit/req", r & 0x40u, s_req);
                lion_logf("S stat6 dpos/len", s_dpos, s_dlen);
            }
        }
        return r;
    }
    case 1: return s_rpos < s_nresp ? s_resp[s_rpos++] : 0;
    case 2: return ((s_req & 0x80) && s_dpos < s_dlen) ? s_data[((s_mode & 0x20) ? 12 : 24) + s_dpos++] : 0;
    case 3: return (s_index & 1) ? (uint32_t)(0xE0 | s_flag) : (uint32_t)(0xE0 | s_enable);
    }
    return 0;
}

void cd_write(uint32_t reg, uint32_t v) {
    v &= 0xFF;
    if (reg == 0) { s_index = v & 3; return; }
    switch (s_index * 4 + reg) {
    case 0 * 4 + 1: command((uint8_t)v); break;
    case 0 * 4 + 2: if (s_nparam < 16) s_param[s_nparam++] = (uint8_t)v; break;
    case 0 * 4 + 3:                                                              /* request register: BFRD=0 rewinds the data FIFO; BFRD=1 leaves its position alone */
        if (g_ww_log_enabled) { static int k; if (k < 120) { k++; lion_logf("W req v/pos", v, s_dpos); lion_logf("W req len/flag", s_dlen, s_flag); } }
        TR("cd request reg / pos", v, s_dpos);
        ww_cd_event(0xCD300000u, v, s_dpos);
        s_req = (uint8_t)v;
        if (!(v & 0x80)) s_dpos = 0;
        break;
    case 1 * 4 + 2: s_enable = v & 0x1F; break;
    case 1 * 4 + 3:
        if (g_ww_log_enabled) { static int k; if (k < 120) { k++; lion_logf("W ack v/flag", v, s_flag); lion_logf("W ack resp", s_nresp, s_rpos); } }
        s_flag &= ~(v & 0x1F);
        if (v & 0x40) s_nparam = 0;
        if (!s_flag && s_nresp == s_rpos) s_nresp = s_rpos = 0;
        break;
    default: break;
    }
}

/* DMA channel 3: copy `bytes` from the data FIFO to RAM. */
void cd_dma(uint8_t *dst, uint32_t bytes) {
    const uint32_t base = (s_mode & 0x20) ? 12 : 24;
    TR("cd dma bytes / avail", bytes, s_dlen - s_dpos);
#ifdef PSX_TRACE_DATA
    { static int n; if (n < 700 && bytes >= 12) { n++; lion_logf("data0 w0 / w1", s_data[base] | s_data[base + 1] << 8 | s_data[base + 2] << 16 | (uint32_t)s_data[base + 3] << 24, s_data[base + 4] | s_data[base + 5] << 8 | s_data[base + 6] << 16 | (uint32_t)s_data[base + 7] << 24); } }
#endif
    if (s_data) TR("cd dma data[0..3] / [4..7]", s_data[base] << 24 | s_data[base + 1] << 16 | s_data[base + 2] << 8 | s_data[base + 3], s_data[base + 4] << 24 | s_data[base + 5] << 16 | s_data[base + 6] << 8 | s_data[base + 7]);
    { uintptr_t d = (uintptr_t)dst - (uintptr_t)g_psx_ram; if (d < 0x200000u) ww_cd_dma((uint32_t)d, bytes, s_dlen > s_dpos ? s_dlen - s_dpos : 0u); }
    { uintptr_t d = (uintptr_t)dst - (uintptr_t)g_psx_ram; uint32_t avail = s_dlen > s_dpos ? s_dlen - s_dpos : 0u;
      if (g_ww_log_enabled && d < 0x200000u) {
          static int k;
          if (k < 500) {
              k++;
              lion_logf("D3 dst / bytes", (uint32_t)d, bytes);
              lion_logf("D3 lba<<12|dpos / word0", (s_read_lba - 1) << 12 | s_dpos, ((s_req & 0x80) && s_dpos < s_dlen) ? (s_data[base + s_dpos] | s_data[base + s_dpos + 1] << 8 | s_data[base + s_dpos + 2] << 16 | (uint32_t)s_data[base + s_dpos + 3] << 24) : 0xDEADu);
          }
      } }
    for (uint32_t i = 0; i < bytes; i++) dst[i] = ((s_req & 0x80) && s_dpos < s_dlen) ? s_data[base + s_dpos++] : 0;
    { uint32_t off = (uint32_t)(dst - g_psx_ram); if (off < 0x500u) ww_after(off, bytes, 3); }
}

/* XA-ADPCM audio sectors routed to the SPU (cd_tick). Not decoded yet: the sound is dropped. */
__attribute__((weak)) void xa_sector(const uint8_t *raw, int filtered_out) { (void)raw; (void)filtered_out; }
