// Tracker module player: see LionMod.h.
#include "LionMod.h"

#include <stdlib.h>
#include <string.h>

namespace lionmod {
namespace {

enum Format { FmtNone, FmtMod, FmtS3m, FmtXm, FmtIt };

// Effects in one numbering for all formats (the loaders translate; parameters keep the XM/MOD meaning).
enum Fx {
  X_NONE, X_ARPEG, X_PORTA_UP, X_PORTA_DN, X_TONEPORTA, X_VIBRATO, X_TPVOL, X_VIBVOL, X_TREMOLO, X_PAN, X_OFFSET,
  X_VSLIDE, X_JUMP, X_SETVOL, X_BREAK, X_SPEED, X_TEMPO, X_FPORTA_UP, X_FPORTA_DN, X_XFPORTA_UP, X_XFPORTA_DN,
  X_VIBWAVE, X_TREMWAVE, X_FINETUNE, X_LOOP, X_RETRIG, X_FVSLIDE_UP, X_FVSLIDE_DN, X_CUT, X_DELAY, X_PATDELAY,
  X_GVOL, X_GVSLIDE, X_KEYOFF, X_ENVPOS, X_PANSLIDE, X_FPANSLIDE_L, X_FPANSLIDE_R, X_TREMOR, X_FVIBRATO, X_CVOL,
  X_CVSLIDE, X_TEMPOSLIDE, X_NNA, X_OFFSETHI
};

// Note column: 0 none, 1..120 note (N = note - 1, middle C = 60), then the specials.
enum { NoteOff = 121, NoteCut = 122, NoteFade = 123 };

struct Cell {
  u8 note, ins, vfx, vpar, fx, par;
};

struct Env {
  u8 flags, n, susS, susE, loopS, loopE;  // flags: 1 on, 2 sustain, 4 loop
  u16 x[25];
  u8 y[25];
};

struct Sample {
  u8* data;
  u32 len, loopStart, loopEnd, susStart, susEnd;
  u8 loop, susLoop;  // 0 none, 1 forward, 2 ping-pong
  bool is16;
  u8 vol, gvol;
  int pan;  // -1: none
  u32 c5spd;
  s8 relnote;
  s8 finetune;
  u8 vibType, vibSweep, vibDepth, vibRate;
  bool owned;
};

struct Inst {
  Env vol, pan;
  u32 fade;  // taken off 65536 every tick after the key is released
  u8 nna, dct, dca;
  u8 gv;     // 0..128
  int pan0;  // -1: none
  u16 map[120];
  u8 noteMap[120];
  bool used;
};

struct Voice {
  const Sample* smp;
  const Inst* ins;
  long long p;  // position, 16.16
  u32 step;
  int dir;
  bool active, bg, keyoff;
  u8 nnaAct;  // background: 1 continue, 2 off, 3 fade
  float per;
  int vol, pan, cvol;
  u32 fade;
  u16 vtick, ptick;
  int avPos, avSweep;
  int tremolo;  // volume offset from the channel's tremolo, 0..64 units
  int host;
  u8 note;
  u8 insNum;
  s32 curL, curR, tgtL, tgtR;
};

struct Chan {
  Voice* v;
  int inst;     // last instrument number (1-based)
  int note;     // last note N
  int sampleNo;
  float per, targetPer;
  int vol, pan, cvol;
  bool muted;
  int sampleOffset;
  // memories
  u8 mPortaUp, mPortaDn, mTone, mVibSpd, mVibDep, mTremSpd, mTremDep, mVolSlide, mOffset, mRetrig, mArp, mPanSlide,
      mFPortaUp, mFPortaDn, mFVol, mTremor, mCvol, mGvol;
  u8 vibPos, tremPos, vibWave, tremWave;
  int arpTick;
  int loopRow, loopCnt;
  int cutTick, delayTick, retrigTick;
  Cell delayed;
  u8 tremorPos;
  bool hasNote;  // a note was played this row
  int volColTone;  // tone-porta from the volume column on this row
  int finetuneOverride;
};

struct Pattern {
  const u8* data;  // format specific (offset into the file image)
  u32 size;
  u16 rows;
};

}  // namespace

struct Impl {
  u8* file;
  u32 size, rate;
  Format fmt;
  bool linear, oldFx, fastVol;
  int nch, nord, restart, nsmp, nins, npat;
  u8* orders;
  Pattern* pats;
  Sample* smps;
  Inst* insts;
  const u8* chanPan0;
  int panInit[64];
  int cvolInit[64];
  bool chanOn[64];
  int initSpeed, initTempo, initGvol;
  u32 amount;
  float clock;  // period clock for the Amiga modes
  Cell* cells;  // the current pattern
  int cellsRows, curPatIdx;
  u32 cellsCap;

  // Playback state.
  Chan chans[64];
  Voice* voices;
  int nvoices;
  int speed, tempo, gvol;
  int order, row, tick, patDelay, patDelayCnt;
  int jumpOrder, breakRow, loopJump;
  bool rowDone;
  u32 tickLeft;
  float tickFrac;
  float pow2tab[258];
  s32* mixL;
  s32* mixR;
  u32 mixCap;
  float gain;
  char name[8];
  int played;
};

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Little endian reads.
inline u32 Le16(const u8* p) { return p[0] | (p[1] << 8); }
inline u32 Le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
inline u32 Be16(const u8* p) { return (p[0] << 8) | p[1]; }

float Pow2(Impl* m, float e) {
  int i = (int)e;
  if (e < 0 && (float)i != e) --i;
  float f = (e - (float)i) * 256.0f;
  int k = (int)f;
  float r = f - (float)k;
  float v = m->pow2tab[k] * (1.0f - r) + m->pow2tab[k + 1] * r;
  while (i > 0) { v *= 2.0f; --i; }
  while (i < 0) { v *= 0.5f; ++i; }
  return v;
}

inline s32 SmpAt(const Sample& s, u32 i) {
  return s.is16 ? (s32)(s16)(s.data[2 * i] | (s.data[2 * i + 1] << 8)) : ((s32)(s8)s.data[i]) << 8;
}

// ---------------------------------------------------------------------------------------------------------------
// Loaders.

struct Reader {
  const u8* d;
  u32 n;
  bool ok(u32 at, u32 len) const { return at <= n && len <= n - at; }
};

void ClearSample(Sample& s) {
  memset(&s, 0, sizeof(s));
  s.pan = -1;
  s.gvol = 64;
  s.c5spd = 8363;
}

void SetLoop(Sample& s, u32 start, u32 end, u8 type) {
  if (end > s.len) end = s.len;
  if (type && start < end) { s.loopStart = start; s.loopEnd = end; s.loop = type; }
  else s.loop = 0;
}

void TrivialInst(Impl* m, int i) {  // sample modes: instrument i is sample i
  Inst& in = m->insts[i];
  memset(&in, 0, sizeof(in));
  in.gv = 128;
  in.pan0 = -1;
  in.used = true;
  for (int n = 0; n < 120; ++n) { in.map[n] = (u16)(i + 1); in.noteMap[n] = (u8)n; }
}

// Amiga module.
bool LoadMod(Impl* m) {
  const u8* d = m->file;
  if (m->size < 1084) return false;
  int ch = 0;
  const u8* sig = d + 1080;
  if (!memcmp(sig, "M.K.", 4) || !memcmp(sig, "M!K!", 4) || !memcmp(sig, "FLT4", 4) || !memcmp(sig, "4CHN", 4)) ch = 4;
  else if (!memcmp(sig, "6CHN", 4)) ch = 6;
  else if (!memcmp(sig, "8CHN", 4) || !memcmp(sig, "OCTA", 4) || !memcmp(sig, "CD81", 4)) ch = 8;
  else if (sig[2] == 'C' && sig[3] == 'H' && sig[0] >= '1' && sig[0] <= '9' && sig[1] >= '0' && sig[1] <= '9') ch = (sig[0] - '0') * 10 + (sig[1] - '0');
  if (ch < 1 || ch > 32) return false;
  m->fmt = FmtMod;
  m->nch = ch;
  m->nord = d[950];
  m->restart = 0;
  m->orders = (u8*)d + 952;
  int maxpat = 0;
  for (int i = 0; i < 128; ++i)
    if (d[952 + i] > maxpat) maxpat = d[952 + i];
  m->npat = maxpat + 1;
  m->nsmp = 31;
  m->nins = 31;
  m->smps = (Sample*)calloc(32, sizeof(Sample));
  m->insts = (Inst*)calloc(32, sizeof(Inst));
  m->pats = (Pattern*)calloc(m->npat, sizeof(Pattern));
  if (!m->smps || !m->insts || !m->pats) return false;
  u32 at = 1084;
  const u32 patBytes = 64 * ch * 4;
  for (int i = 0; i < m->npat; ++i) {
    m->pats[i].data = d + at;
    m->pats[i].rows = 64;
    if (!(at + patBytes <= m->size)) { m->pats[i].data = NULL; }
    at += patBytes;
  }
  for (int i = 0; i < 31; ++i) {
    const u8* h = d + 20 + 30 * i;
    Sample& s = m->smps[i];
    ClearSample(s);
    u32 len = Be16(h + 22) * 2;
    int ft = h[24] & 15;
    if (ft > 7) ft -= 16;
    s.vol = h[25] > 64 ? 64 : h[25];
    u32 ls = Be16(h + 26) * 2, ll = Be16(h + 28) * 2;
    if (at > m->size) len = 0;
    else if (len > m->size - at) len = m->size - at;
    s.data = (u8*)d + at;
    s.len = len;
    s.finetune = (s8)ft;
    s.c5spd = (u32)(8363.0f * Pow2(m, (float)ft / 96.0f));
    SetLoop(s, ls, ls + ll, ll > 2 ? 1 : 0);
    at += len;
    TrivialInst(m, i);
  }
  m->clock = 14187580.0f;
  m->linear = false;
  m->initSpeed = 6;
  m->initTempo = 125;
  m->initGvol = 128;
  for (int c = 0; c < ch; ++c) { m->panInit[c] = ((c & 3) == 0 || (c & 3) == 3) ? 0x30 : 0xCF; m->cvolInit[c] = 64; m->chanOn[c] = true; }
  return true;
}

// ScreamTracker 3.
bool LoadS3m(Impl* m) {
  const u8* d = m->file;
  if (m->size < 0x60 || memcmp(d + 0x2C, "SCRM", 4)) return false;
  m->fmt = FmtS3m;
  const int ordnum = Le16(d + 0x20), insnum = Le16(d + 0x22), patnum = Le16(d + 0x24);
  const u32 flags = Le16(d + 0x26);
  if (ordnum > 256 || insnum > 255 || patnum > 255 || m->size < 0x60u + ordnum + 2u * (insnum + patnum)) return false;
  m->fastVol = (flags & 0x40) != 0 || Le16(d + 0x28) == 0x1300;
  m->initGvol = d[0x30] * 2 > 128 ? 128 : d[0x30] * 2;
  m->initSpeed = d[0x31] ? d[0x31] : 6;
  m->initTempo = d[0x32] >= 32 ? d[0x32] : 125;
  m->clock = 14317456.0f;
  m->linear = false;
  int ch = 0;
  for (int c = 0; c < 32; ++c) {
    const int v = d[0x40 + c];
    m->chanOn[c] = v < 16;
    if (v < 16) { ch = c + 1; m->panInit[c] = v < 8 ? 0x30 : 0xCF; }
    m->cvolInit[c] = 64;
  }
  if (ch == 0) ch = 1;
  m->nch = ch;
  m->orders = (u8*)malloc(ordnum + 1);
  memcpy(m->orders, d + 0x60, ordnum);
  m->nord = ordnum;
  m->restart = 0;
  const u8* para = d + 0x60 + ordnum;
  if (d[0x35] == 252 && m->size >= 0x60u + ordnum + 2u * (insnum + patnum) + 32) {
    const u8* pp = para + 2 * (insnum + patnum);
    for (int c = 0; c < ch; ++c)
      if (pp[c] & 0x20) m->panInit[c] = (pp[c] & 15) * 17;
  }
  m->nsmp = insnum;
  m->nins = insnum;
  m->npat = patnum;
  m->smps = (Sample*)calloc(insnum + 1, sizeof(Sample));
  m->insts = (Inst*)calloc(insnum + 1, sizeof(Inst));
  m->pats = (Pattern*)calloc(patnum + 1, sizeof(Pattern));
  if (!m->smps || !m->insts || !m->pats) return false;
  for (int i = 0; i < insnum; ++i) {
    Sample& s = m->smps[i];
    ClearSample(s);
    TrivialInst(m, i);
    const u32 off = Le16(para + 2 * i) * 16u;
    if (!off || off + 0x50 > m->size || d[off] != 1) continue;
    const u8* h = d + off;
    const u32 dat = (((u32)h[0x0D] << 16) | Le16(h + 0x0E)) * 16u;
    u32 len = Le32(h + 0x10), ls = Le32(h + 0x14), le = Le32(h + 0x18);
    const u32 f = h[0x1F];
    s.is16 = (f & 4) != 0;
    s.vol = h[0x1C] > 64 ? 64 : h[0x1C];
    s.c5spd = Le32(h + 0x20);
    if (s.c5spd < 100) s.c5spd = 8363;
    if (dat >= m->size) len = 0;
    else if (len * (s.is16 ? 2u : 1u) > m->size - dat) len = (m->size - dat) / (s.is16 ? 2u : 1u);
    s.len = len;
    s.data = (u8*)d + dat;
    if (h[0x1E] == 0 && len) {  // unsigned unless the file says otherwise
      if (Le16(d + 0x2A) != 1) {
        if (s.is16) for (u32 k = 0; k < len; ++k) s.data[2 * k + 1] ^= 0x80;
        else for (u32 k = 0; k < len; ++k) s.data[k] ^= 0x80;
      }
    }
    SetLoop(s, ls, le, (f & 1) ? 1 : 0);
  }
  const u32 pbase = 2u * insnum;
  for (int i = 0; i < patnum; ++i) {
    const u32 off = Le16(para + pbase + 2 * i) * 16u;
    m->pats[i].data = NULL;
    m->pats[i].rows = 64;
    if (!off || off + 2 > m->size) continue;
    u32 len = Le16(d + off);
    if (off + 2 + len > m->size) len = m->size - off - 2;
    m->pats[i].data = d + off + 2;
    m->pats[i].size = len;
  }
  return true;
}

// FastTracker 2.
bool LoadXm(Impl* m) {
  const u8* d = m->file;
  if (m->size < 336 || memcmp(d, "Extended Module: ", 17)) return false;
  m->fmt = FmtXm;
  const u32 hsize = Le32(d + 60);
  m->nord = Le16(d + 64);
  m->restart = Le16(d + 66);
  m->nch = Le16(d + 68);
  m->npat = Le16(d + 70);
  m->nins = Le16(d + 72);
  const u32 flags = Le16(d + 74);
  m->linear = (flags & 1) != 0;
  m->initSpeed = Le16(d + 76) ? Le16(d + 76) : 6;
  m->initTempo = Le16(d + 78) ? Le16(d + 78) : 125;
  m->initGvol = 128;
  m->clock = 14317456.0f;
  if (m->nch < 1 || m->nch > 32 || m->nord > 256 || m->npat > 256 || m->nins > 128 || 60 + hsize > m->size) return false;
  m->orders = (u8*)d + 80;
  m->pats = (Pattern*)calloc(m->npat + 1, sizeof(Pattern));
  m->insts = (Inst*)calloc(m->nins + 1, sizeof(Inst));
  m->smps = (Sample*)calloc(m->nins * 16 + 1, sizeof(Sample));
  if (!m->pats || !m->insts || !m->smps) return false;
  for (int c = 0; c < m->nch; ++c) { m->panInit[c] = 128; m->cvolInit[c] = 64; m->chanOn[c] = true; }
  u32 at = 60 + hsize;
  for (int i = 0; i < m->npat; ++i) {
    if (at + 9 > m->size) return false;
    const u32 hl = Le32(d + at);
    const u32 rows = Le16(d + at + 5);
    const u32 psz = Le16(d + at + 7);
    m->pats[i].rows = (u16)(rows ? rows : 64);
    m->pats[i].data = d + at + hl;
    m->pats[i].size = psz;
    if (at + hl + psz > m->size) return false;
    at += hl + psz;
  }
  int nsmp = 0;
  for (int i = 0; i < m->nins; ++i) {
    if (at + 29 > m->size) break;
    const u32 ihs = Le32(d + at);
    const u32 ns = Le16(d + at + 27);
    Inst& in = m->insts[i];
    memset(&in, 0, sizeof(in));
    in.gv = 128;
    in.pan0 = -1;
    in.used = true;
    if (ns == 0 || ns > 16 || ihs < 214 || at + ihs > m->size) { at += ihs < 29 ? 29 : ihs; continue; }
    const u8* h = d + at;
    const u32 shs = Le32(h + 29);
    const u8* map = h + 33;
    for (int n = 0; n < 96; ++n) {
      in.map[n + 12] = (u16)(nsmp + 1 + (map[n] < ns ? map[n] : 0));
      in.noteMap[n + 12] = (u8)(n + 12);
    }
    const u8* ve = h + 129;
    const u8* pe = h + 177;
    const int nv = h[225] > 12 ? 12 : h[225], np = h[226] > 12 ? 12 : h[226];
    in.vol.n = (u8)nv; in.pan.n = (u8)np;
    for (int k = 0; k < nv; ++k) { in.vol.x[k] = (u16)Le16(ve + 4 * k); in.vol.y[k] = (u8)(Le16(ve + 4 * k + 2) > 64 ? 64 : Le16(ve + 4 * k + 2)); }
    for (int k = 0; k < np; ++k) { in.pan.x[k] = (u16)Le16(pe + 4 * k); in.pan.y[k] = (u8)(Le16(pe + 4 * k + 2) > 64 ? 64 : Le16(pe + 4 * k + 2)); }
    in.vol.susS = in.vol.susE = h[227];
    in.vol.loopS = h[228]; in.vol.loopE = h[229];
    in.pan.susS = in.pan.susE = h[230];
    in.pan.loopS = h[231]; in.pan.loopE = h[232];
    in.vol.flags = h[233] & 7;
    in.pan.flags = h[234] & 7;
    if (in.vol.n < 1) in.vol.flags = 0;
    if (in.pan.n < 1) in.pan.flags = 0;
    const u8 vt = h[235], vs = h[236], vd = h[237], vr = h[238];
    in.fade = Le16(h + 239) * 2u;
    u32 sat = at + ihs;
    u32 dat = sat + shs * ns;
    for (u32 s = 0; s < ns; ++s) {
      if (sat + 40 > m->size) break;
      const u8* sh = d + sat;
      sat += shs;
      Sample& sm = m->smps[nsmp + s];
      ClearSample(sm);
      u32 len = Le32(sh), ls = Le32(sh + 4), ll = Le32(sh + 8);
      const u8 type = sh[14];
      sm.is16 = (type & 16) != 0;
      sm.vol = sh[12] > 64 ? 64 : sh[12];
      sm.finetune = (s8)sh[13];
      sm.pan = sh[15];
      sm.relnote = (s8)sh[16];
      sm.vibType = vt; sm.vibSweep = vs; sm.vibDepth = vd; sm.vibRate = vr;
      if (dat > m->size) len = 0;
      else if (len > m->size - dat) len = m->size - dat;
      if (sm.is16) { len &= ~1u; ls >>= 1; ll >>= 1; }
      sm.data = (u8*)d + dat;
      // Delta coding.
      if (sm.is16) {
        int old = 0;
        for (u32 k = 0; k < len / 2; ++k) {
          old = (s16)(old + (s16)(sm.data[2 * k] | (sm.data[2 * k + 1] << 8)));
          sm.data[2 * k] = (u8)old; sm.data[2 * k + 1] = (u8)(old >> 8);
        }
        sm.len = len / 2;
      } else {
        s8 old = 0;
        for (u32 k = 0; k < len; ++k) { old = (s8)(old + (s8)sm.data[k]); sm.data[k] = (u8)old; }
        sm.len = len;
      }
      SetLoop(sm, ls, ls + ll, (type & 3) == 0 ? 0 : (type & 3) == 1 ? 1 : 2);
      dat += len;
    }
    nsmp += ns;
    at = dat;
  }
  m->nsmp = nsmp;
  return true;
}

// IT214/215 sample decompression.
struct BitReader {
  const u8* p; const u8* end; u32 buf; int n;
  u32 Get(int bits) {
    u32 v = 0;
    for (int i = 0; i < bits; ++i) {
      if (!n) { buf = p < end ? *p++ : 0; n = 8; }
      v |= (buf & 1) << i;
      buf >>= 1; --n;
    }
    return v;
  }
};

u8* DecompressIt(const u8* src, const u8* end, u32 len, bool is16, bool it215) {
  const u32 bytes = len * (is16 ? 2 : 1);
  u8* out = (u8*)malloc(bytes + 2);
  if (!out) return NULL;
  u32 done = 0;
  const int width0 = is16 ? 17 : 9;
  const u32 blockMax = is16 ? 0x4000 : 0x8000;
  while (done < len && src + 2 <= end) {
    const u32 clen = Le16(src);
    src += 2;
    BitReader br = {src, src + clen < end ? src + clen : end, 0, 0};
    u32 blk = len - done < blockMax ? len - done : blockMax;
    int width = width0;
    int d1 = 0, d2 = 0;
    for (u32 k = 0; k < blk; ) {
      u32 v = br.Get(width);
      const u32 top = 1u << (width - 1);
      if (width < 7) {
        if (v == top) {
          u32 nw = br.Get(is16 ? 4 : 3) + 1;
          width = nw < (u32)width ? (int)nw : (int)nw + 1;
          continue;
        }
      } else if (width < width0) {
        const u32 border = (((1u << width0) - 1) >> (width0 - width)) - (is16 ? 8 : 4);
        if (v > border && v <= border + (is16 ? 16u : 8u)) {
          u32 nw = v - border;
          width = nw < (u32)width ? (int)nw : (int)nw + 1;
          continue;
        }
      } else if (v & top) {
        width = (int)(v & (top - 1)) + 1;
        continue;
      }
      int val;
      if (width < width0) {
        val = (int)(v << (32 - width)) >> (32 - width);
      } else {
        val = is16 ? (int)(s16)v : (int)(s8)v;
        if (width == width0 && !is16 && v & 0x100) val = (int)(s8)(v & 0xFF);
      }
      d1 += val;
      d2 += d1;
      const int outv = it215 ? d2 : d1;
      if (is16) { out[2 * (done + k)] = (u8)outv; out[2 * (done + k) + 1] = (u8)(outv >> 8); }
      else out[done + k] = (u8)outv;
      ++k;
    }
    done += blk;
    src += clen;
  }
  for (; done < len; ++done) { if (is16) { out[2 * done] = out[2 * done + 1] = 0; } else out[done] = 0; }
  return out;
}

// Impulse Tracker.
bool LoadIt(Impl* m) {
  const u8* d = m->file;
  if (m->size < 0xC0 || memcmp(d, "IMPM", 4)) return false;
  m->fmt = FmtIt;
  const int ordnum = Le16(d + 32), insnum = Le16(d + 34), smpnum = Le16(d + 36), patnum = Le16(d + 38);
  const u32 cmwt = Le16(d + 42), flags = Le16(d + 44);
  if (ordnum > 256 || insnum > 255 || smpnum > 255 || patnum > 255 || cmwt < 0x200) return false;
  const u32 tablesEnd = 0xC0 + ordnum + 4u * (insnum + smpnum + patnum);
  if (tablesEnd > m->size) return false;
  const bool useInst = (flags & 4) != 0;
  m->linear = (flags & 8) != 0;
  m->oldFx = (flags & 16) != 0;
  m->initGvol = d[48] > 128 ? 128 : d[48];
  m->initSpeed = d[50] ? d[50] : 6;
  m->initTempo = d[51] >= 32 ? d[51] : 125;
  m->clock = 14317456.0f;
  m->nch = 0;
  for (int c = 0; c < 64; ++c) {
    const int pan = d[64 + c];
    m->chanOn[c] = pan < 128;
    m->panInit[c] = pan == 100 ? 128 : (pan & 127) > 64 ? 128 : (pan & 127) * 255 / 64;
    m->cvolInit[c] = d[128 + c] > 64 ? 64 : d[128 + c];
  }
  m->orders = (u8*)malloc(ordnum + 1);
  memcpy(m->orders, d + 0xC0, ordnum);
  m->nord = ordnum;
  m->restart = 0;
  const u8* ioff = d + 0xC0 + ordnum;
  const u8* soff = ioff + 4 * insnum;
  const u8* poff = soff + 4 * smpnum;
  m->nsmp = smpnum;
  m->nins = useInst ? insnum : smpnum;
  m->npat = patnum;
  m->smps = (Sample*)calloc(smpnum + 1, sizeof(Sample));
  m->insts = (Inst*)calloc(m->nins + 1, sizeof(Inst));
  m->pats = (Pattern*)calloc(patnum + 1, sizeof(Pattern));
  if (!m->smps || !m->insts || !m->pats) return false;
  int maxch = 0;
  for (int i = 0; i < patnum; ++i) {
    const u32 off = Le32(poff + 4 * i);
    m->pats[i].data = NULL;
    m->pats[i].rows = 64;
    if (!off || off + 8 > m->size) continue;
    const u32 len = Le16(d + off), rows = Le16(d + off + 2);
    if (off + 8 + len > m->size) continue;
    m->pats[i].data = d + off + 8;
    m->pats[i].size = len;
    m->pats[i].rows = (u16)(rows ? rows : 64);
    // Highest channel used.
    const u8* p = d + off + 8;
    const u8* e = p + len;
    u8 lastMask[64];
    memset(lastMask, 0, sizeof(lastMask));
    while (p < e) {
      const u8 b = *p++;
      if (!b) continue;
      const int ch = (b - 1) & 63;
      if (ch + 1 > maxch) maxch = ch + 1;
      u8 mask = lastMask[ch];
      if (b & 128) { if (p >= e) break; mask = lastMask[ch] = *p++; }
      p += ((mask & 1) ? 1 : 0) + ((mask & 2) ? 1 : 0) + ((mask & 4) ? 1 : 0) + ((mask & 8) ? 2 : 0);
    }
  }
  m->nch = maxch < 1 ? 1 : maxch;
  for (int i = 0; i < smpnum; ++i) {
    Sample& s = m->smps[i];
    ClearSample(s);
    const u32 off = Le32(soff + 4 * i);
    if (!off || off + 80 > m->size || memcmp(d + off, "IMPS", 4)) continue;
    const u8* h = d + off;
    const u32 f = h[18];
    s.gvol = h[17] > 64 ? 64 : h[17];
    s.vol = h[19] > 64 ? 64 : h[19];
    s.is16 = (f & 2) != 0;
    u32 len = Le32(h + 48);
    s.c5spd = Le32(h + 60);
    if (s.c5spd < 100) s.c5spd = 8363;
    if (h[47] & 128) s.pan = (h[47] & 127) > 64 ? 128 : (h[47] & 127) * 255 / 64;
    const u32 sp = Le32(h + 72);
    if (!(f & 1) || !len || sp >= m->size) { s.len = 0; continue; }
    const u32 chn = (f & 4) ? 2 : 1;
    if (f & 8) {
      u8* dec = DecompressIt(d + sp, d + m->size, len, s.is16, (h[46] & 4) != 0);
      if (!dec) continue;
      s.data = dec;
      s.owned = true;
      // Owned: signed already.
    } else {
      const u32 bytes = len * (s.is16 ? 2u : 1u) * chn;
      if (bytes > m->size - sp) len = (m->size - sp) / ((s.is16 ? 2u : 1u) * chn);
      s.data = (u8*)d + sp;
      if (!(h[46] & 1)) {
        if (s.is16) for (u32 k = 0; k < len; ++k) s.data[2 * k + 1] ^= 0x80;
        else for (u32 k = 0; k < len; ++k) s.data[k] ^= 0x80;
      }
    }
    s.len = len;
    SetLoop(s, Le32(h + 52), Le32(h + 56), (f & 16) ? ((f & 64) ? 2 : 1) : 0);
    if (f & 32) {
      u32 a = Le32(h + 64), b = Le32(h + 68);
      if (b > len) b = len;
      if (a < b) { s.susStart = a; s.susEnd = b; s.susLoop = (f & 128) ? 2 : 1; }
    }
  }
  if (useInst) {
    for (int i = 0; i < insnum; ++i) {
      Inst& in = m->insts[i];
      memset(&in, 0, sizeof(in));
      in.gv = 128; in.pan0 = -1; in.used = true;
      const u32 off = Le32(ioff + 4 * i);
      if (!off || off + 554 > m->size || memcmp(d + off, "IMPI", 4)) continue;
      const u8* h = d + off;
      in.nna = h[17]; in.dct = h[18]; in.dca = h[19];
      in.fade = Le16(h + 20) * 32u;
      in.gv = h[24] > 128 ? 128 : h[24];
      if (!(h[25] & 128)) in.pan0 = (h[25] & 127) > 64 ? 128 : (h[25] & 127) * 255 / 64;
      for (int n = 0; n < 120; ++n) { in.noteMap[n] = h[64 + 2 * n]; in.map[n] = h[65 + 2 * n]; }
      for (int e = 0; e < 2; ++e) {
        const u8* eh = h + 304 + 82 * e;
        Env& env = e ? in.pan : in.vol;
        env.flags = eh[0] & 7;
        env.n = eh[1] > 25 ? 25 : eh[1];
        env.loopS = eh[2]; env.loopE = eh[3]; env.susS = eh[4]; env.susE = eh[5];
        for (int k = 0; k < env.n; ++k) {
          int y = (s8)eh[6 + 3 * k];
          if (e) y += 32; else if (y < 0) y = 0;
          env.y[k] = (u8)(y < 0 ? 0 : y > 64 ? 64 : y);
          env.x[k] = (u16)Le16(eh + 7 + 3 * k);
        }
        if (env.n < 1) env.flags = 0;
      }
    }
  } else {
    for (int i = 0; i < smpnum; ++i) TrivialInst(m, i);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Pattern unpacking into canonical cells.

int FindModNote(int period) {
  static const short table[36] = {856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320,
                                  302, 285, 269, 254, 240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113};
  int best = 0, bd = 1 << 30;
  for (int i = 0; i < 36; ++i) {
    int dd = table[i] > period ? table[i] - period : period - table[i];
    if (dd < bd) { bd = dd; best = i; }
  }
  return best;
}

void MapMod(Cell& c, int cmd, int par, bool xm) {
  c.fx = X_NONE; c.par = (u8)par;
  switch (cmd) {
    case 0: if (par) c.fx = X_ARPEG; break;
    case 1: c.fx = X_PORTA_UP; break;
    case 2: c.fx = X_PORTA_DN; break;
    case 3: c.fx = X_TONEPORTA; break;
    case 4: c.fx = X_VIBRATO; break;
    case 5: c.fx = X_TPVOL; break;
    case 6: c.fx = X_VIBVOL; break;
    case 7: c.fx = X_TREMOLO; break;
    case 8: c.fx = X_PAN; break;
    case 9: c.fx = X_OFFSET; break;
    case 10: c.fx = X_VSLIDE; break;
    case 11: c.fx = X_JUMP; break;
    case 12: c.fx = X_SETVOL; break;
    case 13: c.fx = X_BREAK; c.par = (u8)((par >> 4) * 10 + (par & 15)); break;
    case 14: {
      const int sub = par >> 4, x = par & 15;
      c.par = (u8)x;
      switch (sub) {
        case 1: c.fx = X_FPORTA_UP; break;
        case 2: c.fx = X_FPORTA_DN; break;
        case 4: c.fx = X_VIBWAVE; break;
        case 5: c.fx = X_FINETUNE; break;
        case 6: c.fx = X_LOOP; break;
        case 7: c.fx = X_TREMWAVE; break;
        case 8: c.fx = X_PAN; c.par = (u8)(x * 17); break;
        case 9: c.fx = X_RETRIG; c.par = (u8)x; break;
        case 10: c.fx = X_FVSLIDE_UP; break;
        case 11: c.fx = X_FVSLIDE_DN; break;
        case 12: c.fx = X_CUT; break;
        case 13: c.fx = X_DELAY; break;
        case 14: c.fx = X_PATDELAY; break;
      }
      break;
    }
    case 15:
      if (par == 0) break;
      c.fx = par < 32 ? X_SPEED : X_TEMPO;
      break;
  }
  (void)xm;
}

void UnpackMod(Impl* m, const Pattern& p, Cell* out) {
  const int ch = m->nch;
  if (!p.data) return;
  for (int r = 0; r < 64; ++r)
    for (int c = 0; c < ch; ++c) {
      const u8* b = p.data + (r * ch + c) * 4;
      Cell& cell = out[r * ch + c];
      const int period = ((b[0] & 15) << 8) | b[1];
      cell.ins = (u8)((b[0] & 0xF0) | (b[2] >> 4));
      if (period) cell.note = (u8)(FindModNote(period) + 48 + 1);
      MapMod(cell, b[2] & 15, b[3], false);
    }
}

void UnpackS3m(Impl* m, const Pattern& p, Cell* out) {
  const int ch = m->nch;
  if (!p.data) return;
  const u8* d = p.data;
  const u8* e = d + p.size;
  int row = 0;
  while (d < e && row < 64) {
    const u8 b = *d++;
    if (!b) { ++row; continue; }
    const int c = b & 31;
    Cell tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (b & 32) {
      if (d + 2 > e) break;
      const u8 n = d[0];
      tmp.ins = d[1];
      d += 2;
      if (n == 254) tmp.note = NoteCut;
      else if (n != 255) tmp.note = (u8)((n >> 4) * 12 + (n & 15) + 12 + 1);
    }
    if (b & 64) {
      if (d >= e) break;
      const u8 v = *d++;
      if (v <= 64) { tmp.vfx = X_SETVOL; tmp.vpar = v; }
    }
    if (b & 128) {
      if (d + 2 > e) break;
      const int cmd = d[0], par = d[1];
      d += 2;
      tmp.par = (u8)par;
      switch (cmd + 'A' - 1) {
        case 'A': tmp.fx = X_SPEED; break;
        case 'B': tmp.fx = X_JUMP; break;
        case 'C': tmp.fx = X_BREAK; tmp.par = (u8)((par >> 4) * 10 + (par & 15)); break;
        case 'D': {
          const int hi = par >> 4, lo = par & 15;
          if (lo == 15 && hi) { tmp.fx = X_FVSLIDE_UP; tmp.par = (u8)hi; }
          else if (hi == 15 && lo) { tmp.fx = X_FVSLIDE_DN; tmp.par = (u8)lo; }
          else tmp.fx = X_VSLIDE;
          break;
        }
        case 'E': if (par >= 0xF0) { tmp.fx = X_FPORTA_DN; tmp.par = (u8)(par & 15); } else if (par >= 0xE0) { tmp.fx = X_XFPORTA_DN; tmp.par = (u8)(par & 15); } else tmp.fx = X_PORTA_DN; break;
        case 'F': if (par >= 0xF0) { tmp.fx = X_FPORTA_UP; tmp.par = (u8)(par & 15); } else if (par >= 0xE0) { tmp.fx = X_XFPORTA_UP; tmp.par = (u8)(par & 15); } else tmp.fx = X_PORTA_UP; break;
        case 'G': tmp.fx = X_TONEPORTA; break;
        case 'H': tmp.fx = X_VIBRATO; break;
        case 'I': tmp.fx = X_TREMOR; break;
        case 'J': tmp.fx = X_ARPEG; break;
        case 'K': tmp.fx = X_VIBVOL; break;
        case 'L': tmp.fx = X_TPVOL; break;
        case 'O': tmp.fx = X_OFFSET; break;
        case 'Q': tmp.fx = X_RETRIG; break;
        case 'R': tmp.fx = X_TREMOLO; break;
        case 'S': {
          const int sub = par >> 4, x = par & 15;
          tmp.par = (u8)x;
          switch (sub) {
            case 3: tmp.fx = X_VIBWAVE; break;
            case 4: tmp.fx = X_TREMWAVE; break;
            case 8: tmp.fx = X_PAN; tmp.par = (u8)(x * 17); break;
            case 11: tmp.fx = X_LOOP; break;
            case 12: tmp.fx = X_CUT; break;
            case 13: tmp.fx = X_DELAY; break;
            case 14: tmp.fx = X_PATDELAY; break;
          }
          break;
        }
        case 'T': tmp.fx = X_TEMPO; break;
        case 'U': tmp.fx = X_FVIBRATO; break;
        case 'V': tmp.fx = X_GVOL; break;
        case 'X': tmp.fx = X_PAN; break;
      }
    }
    if (c < ch) out[row * ch + c] = tmp;
  }
}

void UnpackXm(Impl* m, const Pattern& p, Cell* out) {
  const int ch = m->nch;
  if (!p.data || !p.size) return;
  const u8* d = p.data;
  const u8* e = d + p.size;
  for (int r = 0; r < p.rows; ++r)
    for (int c = 0; c < ch; ++c) {
      if (d >= e) return;
      u8 note = 0, ins = 0, vol = 0, fx = 0, par = 0;
      const u8 b = *d++;
      if (b & 0x80) {
        if ((b & 1) && d < e) note = *d++;
        if ((b & 2) && d < e) ins = *d++;
        if ((b & 4) && d < e) vol = *d++;
        if ((b & 8) && d < e) fx = *d++;
        if ((b & 16) && d < e) par = *d++;
      } else {
        note = b;
        if (d + 4 <= e) { ins = d[0]; vol = d[1]; fx = d[2]; par = d[3]; d += 4; }
      }
      Cell& cell = out[r * ch + c];
      if (note == 97) cell.note = NoteOff;
      else if (note >= 1 && note <= 96) cell.note = (u8)(note + 12);
      cell.ins = ins;
      // Volume column.
      if (vol >= 0x10 && vol <= 0x50) { cell.vfx = X_SETVOL; cell.vpar = (u8)(vol - 0x10); }
      else {
        const int x = vol & 15;
        switch (vol >> 4) {
          case 6: cell.vfx = X_VSLIDE; cell.vpar = (u8)x; break;
          case 7: cell.vfx = X_VSLIDE; cell.vpar = (u8)(x << 4); break;
          case 8: cell.vfx = X_FVSLIDE_DN; cell.vpar = (u8)x; break;
          case 9: cell.vfx = X_FVSLIDE_UP; cell.vpar = (u8)x; break;
          case 10: cell.vfx = X_VIBRATO; cell.vpar = (u8)(x << 4); break;
          case 11: cell.vfx = X_VIBRATO; cell.vpar = (u8)x; break;
          case 12: cell.vfx = X_PAN; cell.vpar = (u8)(x * 17); break;
          case 13: cell.vfx = X_PANSLIDE; cell.vpar = (u8)x; break;
          case 14: cell.vfx = X_PANSLIDE; cell.vpar = (u8)(x << 4); break;
          case 15: cell.vfx = X_TONEPORTA; cell.vpar = (u8)(x << 4); break;
        }
      }
      // Effect column.
      if (fx <= 14 || fx == 15) MapMod(cell, fx, par, true);
      else {
        cell.par = par;
        switch (fx) {
          case 16: cell.fx = X_GVOL; break;       // G
          case 17: cell.fx = X_GVSLIDE; break;    // H
          case 20: cell.fx = X_KEYOFF; break;     // K
          case 21: cell.fx = X_ENVPOS; break;     // L
          case 25: cell.fx = X_PANSLIDE; break;   // P
          case 27: cell.fx = X_RETRIG; break;     // R
          case 29: cell.fx = X_TREMOR; break;     // T
          case 33: if ((par >> 4) == 1) { cell.fx = X_XFPORTA_UP; cell.par = (u8)(par & 15); } else if ((par >> 4) == 2) { cell.fx = X_XFPORTA_DN; cell.par = (u8)(par & 15); } break;
        }
      }
      if (cell.fx == X_PAN && fx == 8) cell.par = par;
      if (cell.fx == X_TEMPO && fx == 15) cell.par = par;
    }
}

void UnpackIt(Impl* m, const Pattern& p, Cell* out) {
  const int ch = m->nch;
  if (!p.data) return;
  const u8* d = p.data;
  const u8* e = d + p.size;
  u8 lastMask[64], lastNote[64], lastIns[64], lastVol[64], lastCmd[64], lastPar[64];
  memset(lastMask, 0, 64); memset(lastNote, 0, 64); memset(lastIns, 0, 64); memset(lastVol, 0, 64); memset(lastCmd, 0, 64); memset(lastPar, 0, 64);
  int row = 0;
  while (d < e && row < p.rows) {
    const u8 b = *d++;
    if (!b) { ++row; continue; }
    const int c = (b - 1) & 63;
    u8 mask = lastMask[c];
    if (b & 128) { if (d >= e) break; mask = lastMask[c] = *d++; }
    u8 note = 0, ins = 0, vol = 0, cmd = 0, par = 0;
    bool hasNote = false, hasIns = false, hasVol = false, hasCmd = false;
    if (mask & 1) { if (d >= e) break; note = lastNote[c] = *d++; hasNote = true; }
    if (mask & 2) { if (d >= e) break; ins = lastIns[c] = *d++; hasIns = true; }
    if (mask & 4) { if (d >= e) break; vol = lastVol[c] = *d++; hasVol = true; }
    if (mask & 8) { if (d + 2 > e) break; cmd = lastCmd[c] = d[0]; par = lastPar[c] = d[1]; d += 2; hasCmd = true; }
    if (mask & 16) { note = lastNote[c]; hasNote = true; }
    if (mask & 32) { ins = lastIns[c]; hasIns = true; }
    if (mask & 64) { vol = lastVol[c]; hasVol = true; }
    if (mask & 128) { cmd = lastCmd[c]; par = lastPar[c]; hasCmd = true; }
    if (c >= ch) continue;
    Cell& cell = out[row * ch + c];
    if (hasNote) {
      if (note < 120) cell.note = (u8)(note + 1);
      else if (note == 255) cell.note = NoteOff;
      else if (note == 254) cell.note = NoteCut;
      else cell.note = NoteFade;
    }
    if (hasIns) cell.ins = ins;
    if (hasVol) {
      if (vol <= 64) { cell.vfx = X_SETVOL; cell.vpar = vol; }
      else if (vol <= 74) { cell.vfx = X_FVSLIDE_UP; cell.vpar = (u8)(vol - 65); }
      else if (vol <= 84) { cell.vfx = X_FVSLIDE_DN; cell.vpar = (u8)(vol - 75); }
      else if (vol <= 94) { cell.vfx = X_VSLIDE; cell.vpar = (u8)((vol - 85) << 4); }
      else if (vol <= 104) { cell.vfx = X_VSLIDE; cell.vpar = (u8)(vol - 95); }
      else if (vol <= 114) { cell.vfx = X_PORTA_DN; cell.vpar = (u8)((vol - 105) * 4); }
      else if (vol <= 124) { cell.vfx = X_PORTA_UP; cell.vpar = (u8)((vol - 115) * 4); }
      else if (vol >= 128 && vol <= 192) { cell.vfx = X_PAN; cell.vpar = (u8)((vol - 128) * 255 / 64); }
      else if (vol >= 193 && vol <= 202) {
        static const u8 gt[10] = {0, 1, 4, 8, 16, 32, 64, 96, 128, 255};
        cell.vfx = X_TONEPORTA; cell.vpar = gt[vol - 193];
      }
      else if (vol >= 203 && vol <= 212) { cell.vfx = X_VIBRATO; cell.vpar = (u8)(vol - 203); }
    }
    if (hasCmd) {
      cell.par = par;
      switch (cmd + 'A' - 1) {
        case 'A': cell.fx = X_SPEED; break;
        case 'B': cell.fx = X_JUMP; break;
        case 'C': cell.fx = X_BREAK; cell.par = (u8)((par >> 4) * 10 + (par & 15)); break;
        case 'D': {
          const int hi = par >> 4, lo = par & 15;
          if (lo == 15 && hi) { cell.fx = X_FVSLIDE_UP; cell.par = (u8)hi; }
          else if (hi == 15 && lo) { cell.fx = X_FVSLIDE_DN; cell.par = (u8)lo; }
          else cell.fx = X_VSLIDE;
          break;
        }
        case 'E': if (par >= 0xF0) { cell.fx = X_FPORTA_DN; cell.par = (u8)(par & 15); } else if (par >= 0xE0) { cell.fx = X_XFPORTA_DN; cell.par = (u8)(par & 15); } else cell.fx = X_PORTA_DN; break;
        case 'F': if (par >= 0xF0) { cell.fx = X_FPORTA_UP; cell.par = (u8)(par & 15); } else if (par >= 0xE0) { cell.fx = X_XFPORTA_UP; cell.par = (u8)(par & 15); } else cell.fx = X_PORTA_UP; break;
        case 'G': cell.fx = X_TONEPORTA; break;
        case 'H': cell.fx = X_VIBRATO; break;
        case 'I': cell.fx = X_TREMOR; break;
        case 'J': cell.fx = X_ARPEG; break;
        case 'K': cell.fx = X_VIBVOL; break;
        case 'L': cell.fx = X_TPVOL; break;
        case 'M': cell.fx = X_CVOL; break;
        case 'N': cell.fx = X_CVSLIDE; break;
        case 'O': cell.fx = X_OFFSET; break;
        case 'P': {
          const int hi = par >> 4, lo = par & 15;
          if (lo == 15 && hi) { cell.fx = X_FPANSLIDE_L; cell.par = (u8)hi; }
          else if (hi == 15 && lo) { cell.fx = X_FPANSLIDE_R; cell.par = (u8)lo; }
          else { cell.fx = X_PANSLIDE; cell.par = (u8)(((par & 15) << 4) | (par >> 4)); }
          break;
        }
        case 'Q': cell.fx = X_RETRIG; break;
        case 'R': cell.fx = X_TREMOLO; break;
        case 'S': {
          const int sub = par >> 4, x = par & 15;
          cell.par = (u8)x;
          switch (sub) {
            case 3: cell.fx = X_VIBWAVE; break;
            case 4: cell.fx = X_TREMWAVE; break;
            case 7: cell.fx = X_NNA; break;
            case 8: cell.fx = X_PAN; cell.par = (u8)(x * 17); break;
            case 10: cell.fx = X_OFFSETHI; break;
            case 11: cell.fx = X_LOOP; break;
            case 12: cell.fx = X_CUT; break;
            case 13: cell.fx = X_DELAY; break;
            case 14: cell.fx = X_PATDELAY; break;
          }
          break;
        }
        case 'T': if (par >= 32) cell.fx = X_TEMPO; else cell.fx = X_TEMPOSLIDE; break;
        case 'U': cell.fx = X_FVIBRATO; break;
        case 'V': cell.fx = X_GVOL; break;
        case 'W': cell.fx = X_GVSLIDE; break;
        case 'X': cell.fx = X_PAN; break;
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The engine.

const u8 kSine[32] = {0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
                      255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24};
// IT/XM vibrato tables are the same shape as the MOD one at 64 steps.
int WaveForm(int type, int pos, int rnd) {
  pos &= 63;
  switch (type & 3) {
    case 0: return pos < 32 ? kSine[pos] : -kSine[pos - 32];
    case 1: return ((63 - pos) * 255) / 32 - 255 + 0 * rnd;          // ramp down
    case 2: return pos < 32 ? 255 : -255;                            // square
    default: return rnd;                                             // random
  }
}

float PeriodFor(Impl* m, int note, const Sample* s) {  // note: N, middle C = 60
  const float c5 = (float)s->c5spd;
  if (m->linear) return 7680.0f - (float)note * 64.0f;
  return (m->clock / c5) * Pow2(m, (float)(60 - note) / 12.0f);
}

float FreqFor(Impl* m, float per, const Sample* s) {
  if (m->linear) return (float)s->c5spd * Pow2(m, (3840.0f - per) / 768.0f);
  if (per < 1.0f) per = 1.0f;
  return m->clock / per;
}

float ClampPeriod(Impl* m, float per) {
  if (m->linear) return per < 0 ? 0 : per > 12 * 8 * 64 ? 12.0f * 8 * 64 : per;
  return per < 32 ? 32 : per > 65535 ? 65535 : per;
}

int EnvValue(const Env& e, u16 t) {
  if (e.n == 0) return 64;
  if (t >= e.x[e.n - 1]) return e.y[e.n - 1];
  int i = 0;
  while (i + 1 < e.n && e.x[i + 1] <= t) ++i;
  const int x0 = e.x[i], x1 = e.x[i + 1];
  if (x1 <= x0) return e.y[i];
  return e.y[i] + (e.y[i + 1] - e.y[i]) * (t - x0) / (x1 - x0);
}

void EnvAdvance(const Env& e, u16& t, bool keyoff) {
  if (!(e.flags & 1) || e.n == 0) return;
  ++t;
  if (!keyoff && (e.flags & 2)) {
    if (e.susE < e.n && t > e.x[e.susE]) t = e.x[e.susS < e.n ? e.susS : 0];
  } else if ((e.flags & 4) && e.loopE < e.n) {
    if (t > e.x[e.loopE]) t = e.x[e.loopS < e.n ? e.loopS : 0];
  }
  if (e.n && t > e.x[e.n - 1] + 1) t = (u16)(e.x[e.n - 1] + 1);
}

void LoadPattern(Impl* m, int patIdx) {
  Pattern empty = {NULL, 0, 64};
  const Pattern& p = (patIdx >= 0 && patIdx < m->npat) ? m->pats[patIdx] : empty;
  const int rows = p.rows ? p.rows : 64;
  const u32 need = (u32)rows * m->nch;
  if (need > m->cellsCap) {
    free(m->cells);
    m->cells = (Cell*)malloc(need * sizeof(Cell));
    m->cellsCap = need;
  }
  memset(m->cells, 0, need * sizeof(Cell));
  m->cellsRows = rows;
  m->curPatIdx = patIdx;
  switch (m->fmt) {
    case FmtMod: UnpackMod(m, p, m->cells); break;
    case FmtS3m: UnpackS3m(m, p, m->cells); break;
    case FmtXm: UnpackXm(m, p, m->cells); break;
    case FmtIt: UnpackIt(m, p, m->cells); break;
    default: break;
  }
}

void EnterOrder(Impl* m) {
  while (m->order < m->nord && m->orders[m->order] == 254) ++m->order;
  if (m->order >= m->nord || m->orders[m->order] == 255) {
    m->order = m->restart < m->nord ? m->restart : 0;
    while (m->order < m->nord && m->orders[m->order] >= 254) ++m->order;
    if (m->order >= m->nord) m->order = 0;
  }
  LoadPattern(m, m->nord ? m->orders[m->order] : 0);
  if (m->row >= m->cellsRows) m->row = 0;
}

int SampleOf(Impl* m, int ins, int note, const Sample** out, int* trans) {
  if (ins < 1 || ins > m->nins) return 0;
  const Inst& in = m->insts[ins - 1];
  if (note < 0 || note > 119) return 0;
  const int si = in.map[note];
  if (si < 1 || si > m->nsmp) return 0;
  *out = &m->smps[si - 1];
  *trans = in.noteMap[note];
  return si;
}

void SetupVoice(Impl* m, Chan& c, Voice* v) {
  v->per = c.per;
  v->vol = c.vol;
  v->pan = c.pan;
  v->cvol = c.cvol;
}

void CutVoice(Voice* v) { v->active = false; }

Voice* AllocBg(Impl* m) {
  for (int i = m->nch; i < m->nvoices; ++i)
    if (!m->voices[i].active) return &m->voices[i];
  // Steal the quietest.
  Voice* q = &m->voices[m->nch];
  for (int i = m->nch; i < m->nvoices; ++i)
    if (m->voices[i].fade < q->fade) q = &m->voices[i];
  return q;
}

void NnaApply(Impl* m, Chan& c, int hostIdx, const Sample* newSmp, int newNote, int newIns) {
  Voice* v = c.v;
  if (!v->active || !v->smp) return;
  // Duplicate check on background voices of this channel.
  if (v->ins && m->fmt == FmtIt) {
    const Inst& ni = m->insts[newIns - 1];
    for (int i = 0; i < m->nvoices; ++i) {
      Voice& o = m->voices[i];
      if (!o.active || o.host != hostIdx || !o.ins) continue;
      bool dup = false;
      if (ni.dct == 1) dup = (o.note == newNote && o.insNum == newIns);
      else if (ni.dct == 2) dup = (o.smp == newSmp);
      else if (ni.dct == 3) dup = (o.insNum == newIns);
      if (dup) {
        if (ni.dca == 0) o.active = false;
        else if (ni.dca == 1) o.keyoff = true;
        else o.fade = o.fade ? o.fade : 65535, o.keyoff = true, o.nnaAct = 3;
      }
    }
  }
  const u8 nna = v->ins ? v->ins->nna : 0;
  if (!nna || m->fmt != FmtIt) { return; }  // the foreground voice is simply restarted
  Voice* bg = AllocBg(m);
  if (bg != v) {
    *bg = *v;
    bg->bg = true;
    bg->host = hostIdx;
    bg->nnaAct = nna;
    if (nna == 2) bg->keyoff = true;
    if (nna == 3) { bg->keyoff = true; }
  }
}

void Trigger(Impl* m, int ch, int note, int ins, bool keepVol) {
  Chan& c = m->chans[ch];
  const bool hadVoiceIns = c.inst != 0;
  if (ins) c.inst = ins;
  const int useIns = c.inst;
  const Sample* smp = NULL;
  int trans = note;
  if (!SampleOf(m, useIns, note, &smp, &trans)) { (void)hadVoiceIns; return; }
  const Inst& in = m->insts[useIns - 1];
  const int N = trans + (m->fmt == FmtXm ? smp->relnote : 0);
  NnaApply(m, c, ch, smp, note, useIns);
  Voice* v = c.v;
  v->smp = smp;
  v->ins = &in;
  v->p = 0;
  v->dir = 1;
  v->active = smp->len > 0;
  v->bg = false;
  v->keyoff = false;
  v->nnaAct = 0;
  v->fade = 65536;
  v->vtick = v->ptick = 0;
  v->avPos = 0;
  v->avSweep = 0;
  v->host = ch;
  v->note = (u8)note;
  v->insNum = (u8)useIns;
  v->tremolo = 0;
  v->curL = v->curR = 0;
  c.note = N;
  float per = PeriodFor(m, N, smp);
  if (m->fmt == FmtXm) per -= m->linear ? (float)smp->finetune / 2.0f : 0;
  if (m->fmt == FmtXm && !m->linear && smp->finetune) per *= Pow2(m, -(float)smp->finetune / (12.0f * 128.0f));
  c.per = c.targetPer = per;
  if (!keepVol || m->fmt == FmtXm) {
    c.vol = smp->vol;
    if (m->fmt == FmtIt) c.vol = smp->vol;
  }
  if (smp->pan >= 0 && m->fmt != FmtIt) c.pan = smp->pan;
  else if (m->fmt == FmtIt) {
    if (in.pan0 >= 0) c.pan = in.pan0;
    else if (smp->pan >= 0) c.pan = smp->pan;
    else c.pan = m->panInit[ch];
  }
  if (c.vibWave < 4) c.vibPos = 0;
  if (c.tremWave < 4) c.tremPos = 0;
  c.sampleNo = (int)(smp - m->smps);
  c.arpTick = 0;
  c.hasNote = true;
  if (c.sampleOffset) {
    u32 off = (u32)c.sampleOffset;
    if (off >= smp->len) off = smp->len ? smp->len - 1 : 0;
    if (m->fmt == FmtMod && off >= smp->len) v->active = false;
    v->p = (long long)off << 16;
  }
  SetupVoice(m, c, v);
}

void KeyOff(Impl* m, Chan& c) {
  Voice* v = c.v;
  if (!v->active) return;
  v->keyoff = true;
  if (m->fmt == FmtXm || m->fmt == FmtIt) {
    const bool env = v->ins && (v->ins->vol.flags & 1);
    if (!env && (m->fmt == FmtXm || (v->ins && v->ins->fade == 0))) c.vol = 0;
  } else c.vol = 0;
  v->vol = c.vol;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void ApplyVolSlide(Impl* m, Chan& c, int par, bool tick0, bool fast) {
  (void)m;
  const int up = par >> 4, dn = par & 15;
  if (!tick0 || fast) {
    if (up) c.vol = Clamp(c.vol + up, 0, 64);
    else if (dn) c.vol = Clamp(c.vol - dn, 0, 64);
  }
}

void DoRetrig(Impl* m, int ch, Chan& c) {
  Voice* v = c.v;
  if (!v->smp) return;
  const int x = c.mRetrig >> 4;
  switch (x) {
    case 1: c.vol -= 1; break; case 2: c.vol -= 2; break; case 3: c.vol -= 4; break; case 4: c.vol -= 8; break;
    case 5: c.vol -= 16; break; case 6: c.vol = c.vol * 2 / 3; break; case 7: c.vol /= 2; break;
    case 9: c.vol += 1; break; case 10: c.vol += 2; break; case 11: c.vol += 4; break; case 12: c.vol += 8; break;
    case 13: c.vol += 16; break; case 14: c.vol = c.vol * 3 / 2; break; case 15: c.vol *= 2; break;
  }
  c.vol = Clamp(c.vol, 0, 64);
  v->p = 0;
  v->dir = 1;
  v->active = v->smp->len > 0;
  v->curL = v->curR = 0;
  v->vol = c.vol;
  (void)m; (void)ch;
}

// Effects run on the first tick of a row ("tick0") and on the others.
void RunEffect(Impl* m, int ch, Chan& c, int fx, int par, bool tick0, bool vcol) {
  Voice* v = c.v;
  switch (fx) {
    case X_ARPEG:
      if (tick0) { if (par) c.mArp = (u8)par; else if (m->fmt == FmtMod || m->fmt == FmtXm) c.mArp = 0; }
      break;
    case X_PORTA_UP:
    case X_PORTA_DN: {
      u8& mem = fx == X_PORTA_UP ? c.mPortaUp : c.mPortaDn;
      if (tick0) { if (par) mem = (u8)par; return; }
      const float amt = (float)mem * (m->linear ? 4.0f : (m->fmt == FmtMod ? 4.0f : 4.0f));
      c.per = ClampPeriod(m, c.per + (fx == X_PORTA_UP ? -amt : amt));
      c.targetPer = c.per;
      break;
    }
    case X_FPORTA_UP:
    case X_FPORTA_DN: {
      u8& mem = fx == X_FPORTA_UP ? c.mFPortaUp : c.mFPortaDn;
      if (!tick0) return;
      if (par) mem = (u8)par;
      const float amt = (float)mem * (m->linear ? 4.0f : 4.0f);
      c.per = ClampPeriod(m, c.per + (fx == X_FPORTA_UP ? -amt : amt));
      c.targetPer = c.per;
      break;
    }
    case X_XFPORTA_UP:
    case X_XFPORTA_DN: {
      if (!tick0) return;
      u8& mem = fx == X_XFPORTA_UP ? c.mFPortaUp : c.mFPortaDn;
      if (par) mem = (u8)par;
      const float amt = (float)mem;
      c.per = ClampPeriod(m, c.per + (fx == X_XFPORTA_UP ? -amt : amt));
      c.targetPer = c.per;
      break;
    }
    case X_TONEPORTA:
    case X_TPVOL: {
      if (fx == X_TPVOL) {
        // volume part below
      } else if (tick0) {
        if (par) c.mTone = (u8)par;
        return;
      }
      if (tick0) return;
      const float amt = (float)c.mTone * 4.0f;
      if (c.per < c.targetPer) { c.per += amt; if (c.per > c.targetPer) c.per = c.targetPer; }
      else if (c.per > c.targetPer) { c.per -= amt; if (c.per < c.targetPer) c.per = c.targetPer; }
      if (fx == X_TPVOL) { if (par) c.mVolSlide = (u8)par; ApplyVolSlide(m, c, c.mVolSlide, false, true); }
      break;
    }
    case X_VIBRATO:
    case X_FVIBRATO:
    case X_VIBVOL: {
      if (tick0) {
        if (fx == X_VIBVOL) { if (par) c.mVolSlide = (u8)par; return; }
        if (par >> 4) c.mVibSpd = (u8)(par >> 4);
        if (par & 15) c.mVibDep = (u8)(par & 15);
        if (vcol && !(par >> 4)) {}
      } else {
        if (fx == X_VIBVOL) ApplyVolSlide(m, c, c.mVolSlide, false, true);
      }
      break;  // the wave is applied when the voice is updated
    }
    case X_TREMOLO:
      if (tick0) { if (par >> 4) c.mTremSpd = (u8)(par >> 4); if (par & 15) c.mTremDep = (u8)(par & 15); }
      break;
    case X_PAN:
      if (tick0) c.pan = par > 255 ? 255 : par;
      break;
    case X_OFFSET:
      if (tick0) { if (par) c.mOffset = (u8)par; c.sampleOffset = c.mOffset * 256 + (c.sampleOffset & ~0xFFFF & 0); }
      break;
    case X_OFFSETHI:
      if (tick0) c.sampleOffset = (c.sampleOffset & 0xFFFF) | (par << 16);
      break;
    case X_VSLIDE:
      if (tick0) { if (par) c.mVolSlide = (u8)par; ApplyVolSlide(m, c, c.mVolSlide, true, m->fastVol); }
      else ApplyVolSlide(m, c, c.mVolSlide, false, true);
      break;
    case X_FVSLIDE_UP:
      if (tick0) { if (par) c.mFVol = (u8)par; c.vol = Clamp(c.vol + c.mFVol, 0, 64); }
      break;
    case X_FVSLIDE_DN:
      if (tick0) { if (par) c.mFVol = (u8)par; c.vol = Clamp(c.vol - c.mFVol, 0, 64); }
      break;
    case X_SETVOL:
      if (tick0) c.vol = Clamp(par, 0, 64);
      break;
    case X_JUMP:
      if (tick0) m->jumpOrder = par;
      break;
    case X_BREAK:
      if (tick0) m->breakRow = par;
      break;
    case X_SPEED:
      if (tick0 && par > 0) m->speed = par;
      break;
    case X_TEMPO:
      if (tick0 && par >= 32) m->tempo = par;
      break;
    case X_TEMPOSLIDE:
      if (!tick0) {
        if (par & 0x10) m->tempo = Clamp(m->tempo + (par & 15), 32, 255);
        else m->tempo = Clamp(m->tempo - (par & 15), 32, 255);
      }
      break;
    case X_VIBWAVE:
      if (tick0) c.vibWave = (u8)par;
      break;
    case X_TREMWAVE:
      if (tick0) c.tremWave = (u8)par;
      break;
    case X_FINETUNE:
      break;
    case X_LOOP:
      if (!tick0) break;
      if (par == 0) c.loopRow = m->row;
      else {
        if (c.loopCnt == 0) c.loopCnt = par; else --c.loopCnt;
        if (c.loopCnt > 0) m->loopJump = c.loopRow;
      }
      break;
    case X_RETRIG:
      if (tick0) {
        if (par) c.mRetrig = (u8)par;
        c.retrigTick = 0;
      } else {
        const int iv = c.mRetrig & 15;
        if (iv && ++c.retrigTick >= iv) { c.retrigTick = 0; DoRetrig(m, ch, c); }
      }
      break;
    case X_CUT:
      if (!tick0 && m->tick == par) c.vol = 0;
      if (tick0 && par == 0) c.vol = 0;
      break;
    case X_PATDELAY:
      if (tick0 && m->patDelay == 0 && m->patDelayCnt == 0) m->patDelay = par;
      break;
    case X_GVOL:
      if (tick0) m->gvol = Clamp(m->fmt == FmtIt ? par : par * 2, 0, 128);
      break;
    case X_GVSLIDE:
      if (tick0) { if (par) c.mGvol = (u8)par; }
      else {
        const int up = c.mGvol >> 4, dn = c.mGvol & 15;
        const int sc = m->fmt == FmtIt ? 1 : 2;
        if (up) m->gvol = Clamp(m->gvol + up * sc, 0, 128); else if (dn) m->gvol = Clamp(m->gvol - dn * sc, 0, 128);
      }
      break;
    case X_KEYOFF:
      if (m->tick == par && !(tick0 && par != 0)) KeyOff(m, c);
      else if (tick0 && par == 0) KeyOff(m, c);
      break;
    case X_ENVPOS:
      if (tick0 && v->smp) { v->vtick = (u16)par; v->ptick = (u16)par; }
      break;
    case X_PANSLIDE:
      if (tick0) { if (par) c.mPanSlide = (u8)par; }
      else {
        const int r = c.mPanSlide >> 4, l = c.mPanSlide & 15;
        c.pan = Clamp(c.pan + (r ? r : -l) * (m->fmt == FmtIt ? 4 : 1), 0, 255);
      }
      break;
    case X_FPANSLIDE_L:
      if (tick0) c.pan = Clamp(c.pan - par * 4, 0, 255);
      break;
    case X_FPANSLIDE_R:
      if (tick0) c.pan = Clamp(c.pan + par * 4, 0, 255);
      break;
    case X_TREMOR:
      if (tick0 && par) c.mTremor = (u8)par;
      break;
    case X_CVOL:
      if (tick0) c.cvol = Clamp(par, 0, 64);
      break;
    case X_CVSLIDE:
      if (tick0) { if (par) c.mCvol = (u8)par; }
      if (!tick0 || true) {
        const int up = c.mCvol >> 4, dn = c.mCvol & 15;
        if (tick0) {
          if (dn == 15 && up) c.cvol = Clamp(c.cvol + up, 0, 64);
          else if (up == 15 && dn) c.cvol = Clamp(c.cvol - dn, 0, 64);
        } else if (dn != 15 && up != 15) {
          if (up) c.cvol = Clamp(c.cvol + up, 0, 64); else c.cvol = Clamp(c.cvol - dn, 0, 64);
        }
      }
      break;
    case X_NNA:
      if (tick0 && v->smp) {
        if (par <= 2) { // S70..S72: past notes (apply to background voices of the channel)
          for (int i = 0; i < m->nvoices; ++i) {
            Voice& o = m->voices[i];
            if (o.active && o.bg && o.host == ch) { if (par == 0) o.active = false; else if (par == 1) o.keyoff = true; else { o.keyoff = true; o.nnaAct = 3; } }
          }
        }
      }
      break;
    default: break;
  }
}

void ProcessRow(Impl* m) {
  const int ch = m->nch;
  const Cell* row = m->cells + m->row * ch;
  for (int i = 0; i < ch; ++i) {
    Chan& c = m->chans[i];
    const Cell& cell = row[i];
    c.hasNote = false;
    c.sampleOffset = 0;
    // Note delay: the note triggers later.
    const bool delayed = cell.fx == X_DELAY && cell.par > 0;
    const bool tone = cell.fx == X_TONEPORTA || cell.fx == X_TPVOL || cell.vfx == X_TONEPORTA;
    if (delayed) {
      c.delayed = cell;
      c.delayTick = cell.par;
    } else {
      c.delayTick = -1;
    }
    // Instrument number without a note: reset the volume (and pan).
    if (!delayed) {
      if (cell.ins && !cell.note && c.v->smp && cell.ins <= m->nins) {
        const Sample* s = NULL; int tr;
        if (SampleOf(m, cell.ins, c.note < 0 ? 0 : (c.note > 119 ? 119 : c.note), &s, &tr) || true) {
          if (s) { c.vol = s->vol; }
        }
        c.inst = cell.ins;
      }
      if (cell.note >= 1 && cell.note <= 120) {
        const int N = cell.note - 1;
        if (tone && c.v->smp) {
          if (cell.ins) { c.inst = cell.ins; }
          const Sample* s = NULL; int tr = N;
          if (SampleOf(m, c.inst, N, &s, &tr)) {
            const int NN = tr + (m->fmt == FmtXm ? s->relnote : 0);
            c.targetPer = PeriodFor(m, NN, s);
            if (m->fmt == FmtXm && s->finetune) c.targetPer -= m->linear ? s->finetune / 2.0f : 0;
            if (cell.ins) c.vol = s->vol;
            if (!c.v->active && s) { Trigger(m, i, N, cell.ins, false); }
          }
        } else {
          Trigger(m, i, N, cell.ins, cell.ins == 0);
        }
      } else if (cell.note == NoteOff) {
        KeyOff(m, c);
      } else if (cell.note == NoteCut) {
        c.vol = 0; c.v->active = false;
      } else if (cell.note == NoteFade) {
        c.v->keyoff = true;
      }
    }
    // Volume column, then effects (tick 0).
    if (cell.vfx) RunEffect(m, i, c, cell.vfx, cell.vpar, true, true);
    RunEffect(m, i, c, delayed ? X_NONE : cell.fx, cell.par, true, false);
    if (delayed) RunEffect(m, i, c, X_NONE, 0, true, false);
    // Remember what the effect columns say for the per-tick processing.
    c.volColTone = cell.vfx == X_TONEPORTA;
    if (c.v->smp) SetupVoice(m, c, c.v);
  }
}

void TickEffects(Impl* m) {
  const int ch = m->nch;
  const Cell* row = m->cells + m->row * ch;
  for (int i = 0; i < ch; ++i) {
    Chan& c = m->chans[i];
    const Cell& cell = row[i];
    if (c.delayTick == m->tick && c.delayTick > 0) {
      const Cell& d = c.delayed;
      if (d.note >= 1 && d.note <= 120) Trigger(m, i, d.note - 1, d.ins, d.ins == 0);
      if (d.vfx == X_SETVOL) c.vol = d.vpar;
      c.delayTick = -1;
    }
    if (cell.vfx) {
      if (cell.vfx == X_VSLIDE || cell.vfx == X_PORTA_UP || cell.vfx == X_PORTA_DN || cell.vfx == X_PANSLIDE)
        RunEffect(m, i, c, cell.vfx, cell.vpar, false, true);
      else if (cell.vfx == X_TONEPORTA) { if (cell.vpar) c.mTone = cell.vpar; RunEffect(m, i, c, X_TONEPORTA, 0, false, true); }
    }
    RunEffect(m, i, c, cell.fx, cell.par, false, false);
  }
}

// Per tick: everything that follows from the channel state (envelopes, vibrato, final volume and pitch).
void UpdateVoice(Impl* m, Voice& v, Chan* c) {
  if (!v.active || !v.smp) return;
  const Sample& s = *v.smp;
  float per = v.per;
  int vol = v.vol, pan = v.pan;
  int cvol = v.cvol;
  if (c) {  // foreground: live channel state
    per = c->per; vol = c->vol; pan = c->pan; cvol = c->cvol;
    v.per = per;
    // Vibrato / arpeggio / tremolo.
    const int ch = (int)(c - m->chans);
    const Cell& cell = m->cells[m->row * m->nch + ch];
    const int vfx = cell.fx, vvfx = cell.vfx;
    if (vfx == X_VIBRATO || vfx == X_VIBVOL || vfx == X_FVIBRATO || vvfx == X_VIBRATO) {
      const int w = WaveForm(c->vibWave, c->vibPos, 0);
      const int depth = c->mVibDep;
      float d;
      if (m->linear) d = (float)(w * depth) / (float)(m->fmt == FmtXm ? 128 : (m->oldFx ? 64 : 32)) * (vfx == X_FVIBRATO ? 0.25f : 1.0f) * 2.0f;
      else d = (float)(w * depth) / 128.0f * 4.0f * (vfx == X_FVIBRATO ? 0.25f : 1.0f);
      if (m->fmt == FmtXm) d = (float)(w * depth) / 128.0f * (m->linear ? 4.0f : 4.0f);
      per += d;
    }
    if (vfx == X_ARPEG && c->mArp) {
      const int ph = m->tick % 3;
      const int semi = ph == 0 ? 0 : ph == 1 ? (c->mArp >> 4) : (c->mArp & 15);
      per = m->linear ? per - semi * 64.0f : per * Pow2(m, -(float)semi / 12.0f);
    }
    if (vfx == X_TREMOLO) {
      const int w = WaveForm(c->tremWave, c->tremPos, 0);
      vol = Clamp(vol + (w * c->mTremDep) / 64, 0, 64);
    }
    if (vfx == X_TREMOR) {
      const int on = (c->mTremor >> 4) + 1, off = (c->mTremor & 15) + 1;
      if (c->tremorPos % (on + off) >= on) vol = 0;
    }
  }
  // Auto vibrato.
  if (s.vibDepth && s.vibRate) {
    int depth = s.vibDepth;
    if (s.vibSweep && v.avSweep < s.vibSweep) depth = depth * v.avSweep / s.vibSweep;
    const int w = WaveForm(s.vibType == 1 ? 2 : s.vibType == 2 ? 1 : s.vibType == 3 ? 1 : 0, v.avPos >> 2, 0);
    per += (float)(w * depth) / (m->fmt == FmtIt ? 256.0f : 128.0f) * (m->linear ? 4.0f : 4.0f);
    v.avPos += s.vibRate;
    ++v.avSweep;
  }
  // Envelopes and fade-out.
  int env = 64;
  if (v.ins) {
    const Inst& in = *v.ins;
    if (in.vol.flags & 1) { env = EnvValue(in.vol, v.vtick); EnvAdvance(in.vol, v.vtick, v.keyoff); }
    if (in.pan.flags & 1) {
      const int pe = EnvValue(in.pan, v.ptick);
      EnvAdvance(in.pan, v.ptick, v.keyoff);
      pan = pan + (pe - 32) * (128 - (pan > 128 ? pan - 128 : 128 - pan)) / 32;
      pan = Clamp(pan, 0, 255);
    }
    if (v.keyoff && in.fade) {
      v.fade = v.fade > in.fade ? v.fade - in.fade : 0;
    } else if (v.keyoff && (m->fmt == FmtIt) && (in.vol.flags & 1) == 0 && in.fade == 0) {
      v.fade = 0;
    }
  }
  if (v.keyoff && v.fade == 0 && v.ins && (v.ins->fade || !(v.ins->vol.flags & 1))) v.active = false;
  if (v.bg && v.fade == 0) v.active = false;
  if (v.bg && v.nnaAct == 3 && v.ins) { v.keyoff = true; }
  if (!v.active) return;
  // Final volume (0..1) and gains.
  float volf = (float)vol / 64.0f * (float)cvol / 64.0f * (float)env / 64.0f * (float)v.fade / 65536.0f *
               (float)m->gvol / 128.0f * (float)s.gvol / 64.0f * (v.ins ? (float)v.ins->gv / 128.0f : 1.0f);
  if (v.bg && volf <= 0.0f && v.keyoff) { v.active = false; return; }
  const float pl = (float)(255 - pan) / 255.0f, pr = (float)pan / 255.0f;
  const float g = volf * m->gain * 4096.0f;
  v.tgtL = (s32)(g * (0.5f + pl * 0.5f));
  v.tgtR = (s32)(g * (0.5f + pr * 0.5f));
  float freq = FreqFor(m, ClampPeriod(m, per), v.smp);
  if (freq > 200000.0f) freq = 200000.0f;
  v.step = (u32)(freq * 65536.0f / (float)m->rate);
  if (v.step == 0) v.step = 1;
}

// Number of steps of `step` (16.16) that fit before `dist` (16.16) is used up, rounded up: no 64-bit division
// on the PowerPC.
long long Count(long long dist, u32 step) {
  const double q = (double)dist / (double)step;
  long long c = (long long)q;
  if ((double)c < q) ++c;
  return c;
}

// Mixes `n` frames of the voice into the accumulators, ramping the gains from the current to the target values.
void MixVoice(Impl* m, Voice& v, s32* L, s32* R, u32 n) {
  const Sample& s = *v.smp;
  s32 cl = v.curL, cr = v.curR;
  const s32 dl = (v.tgtL - cl) / (s32)n, dr = (v.tgtR - cr) / (s32)n;
  while (n && v.active) {
    // The loop in force.
    u32 ls = 0, le = s.len;
    u8 type = 0;
    if (s.susLoop && !v.keyoff) { ls = s.susStart; le = s.susEnd; type = s.susLoop; }
    else if (s.loop) { ls = s.loopStart; le = s.loopEnd; type = s.loop; }
    const long long end16 = (long long)le << 16, start16 = (long long)ls << 16;
    if (v.dir > 0) {
      if (v.p >= end16) {
        if (!type) { v.active = false; break; }
        const long long span = end16 - start16;
        if (type == 1) { while (v.p >= end16) v.p -= span; }
        else { v.p = 2 * end16 - v.p - 1; v.dir = -1; if (v.p < start16) v.p = start16; }
        continue;
      }
      long long cnt = Count(end16 - v.p, v.step);
      if (cnt > (long long)n) cnt = n;
      for (long long k = 0; k < cnt; ++k) {
        const u32 i = (u32)(v.p >> 16), f = (u32)(v.p & 0xFFFF);
        const s32 a = SmpAt(s, i);
        const s32 b = i + 1 < s.len ? SmpAt(s, i + 1) : a;
        const s32 x = a + (((b - a) * (s32)f) >> 16);
        *L++ += (x * (cl >> 4)) >> 8;
        *R++ += (x * (cr >> 4)) >> 8;
        cl += dl; cr += dr;
        v.p += v.step;
      }
      n -= (u32)cnt;
    } else {
      if (v.p < start16) {
        v.p = 2 * start16 - v.p;
        v.dir = 1;
        if (v.p >= end16) v.p = end16 - 1;
        continue;
      }
      long long cnt = (long long)((double)(v.p - start16) / (double)v.step) + 1;
      if (cnt > (long long)n) cnt = n;
      for (long long k = 0; k < cnt; ++k) {
        const u32 i = (u32)(v.p >> 16);
        const s32 x = SmpAt(s, i < s.len ? i : s.len - 1);
        *L++ += (x * (cl >> 4)) >> 8;
        *R++ += (x * (cr >> 4)) >> 8;
        cl += dl; cr += dr;
        v.p -= v.step;
      }
      n -= (u32)cnt;
    }
  }
  v.curL = v.tgtL;
  v.curR = v.tgtR;
  (void)m;
}

void AdvanceRow(Impl* m) {
  if (m->loopJump >= 0) { m->row = m->loopJump; m->loopJump = -1; m->jumpOrder = m->breakRow = -1; return; }
  if (m->jumpOrder >= 0 || m->breakRow >= 0) {
    m->order = m->jumpOrder >= 0 ? m->jumpOrder : m->order + 1;
    m->row = m->breakRow >= 0 ? m->breakRow : 0;
    m->jumpOrder = m->breakRow = -1;
    for (int c = 0; c < m->nch; ++c) m->chans[c].loopCnt = 0, m->chans[c].loopRow = 0;
    EnterOrder(m);
    if (m->row >= m->cellsRows) m->row = 0;
    return;
  }
  if (++m->row >= m->cellsRows) {
    m->row = 0;
    ++m->order;
    for (int c = 0; c < m->nch; ++c) m->chans[c].loopCnt = 0, m->chans[c].loopRow = 0;
    EnterOrder(m);
  }
}

void ProcessTick(Impl* m) {
  if (m->tick == 0 && m->patDelayCnt == 0) ProcessRow(m);
  else if (m->tick > 0) TickEffects(m);
  else if (m->tick == 0) TickEffects(m);  // a repeated row (pattern delay) only runs the continuous effects
  for (int i = 0; i < m->nch; ++i) {
    Chan& c = m->chans[i];
    c.vibPos = (u8)(c.vibPos + c.mVibSpd * (m->tick || true ? 1 : 0));
    c.tremPos = (u8)(c.tremPos + c.mTremSpd);
    c.tremorPos = (u8)(c.tremorPos + 1);
    UpdateVoice(m, *c.v, &c);
  }
  for (int i = m->nch; i < m->nvoices; ++i)
    if (m->voices[i].active) UpdateVoice(m, m->voices[i], NULL);
  if (++m->tick >= m->speed) {
    m->tick = 0;
    if (m->patDelay > 0) { --m->patDelay; ++m->patDelayCnt; }
    else { m->patDelayCnt = 0; AdvanceRow(m); }
  }
}

void ResetState(Impl* m, int order) {
  memset(m->chans, 0, sizeof(m->chans));
  memset(m->voices, 0, sizeof(Voice) * m->nvoices);
  for (int i = 0; i < m->nch; ++i) {
    Chan& c = m->chans[i];
    c.v = &m->voices[i];
    c.pan = m->panInit[i];
    c.cvol = m->cvolInit[i];
    c.vol = 64;
    c.delayTick = -1;
    c.vibWave = c.tremWave = 0;
    m->voices[i].host = i;
  }
  m->speed = m->initSpeed;
  m->tempo = m->initTempo;
  m->gvol = m->initGvol;
  m->order = order;
  m->row = 0;
  m->tick = 0;
  m->patDelay = m->patDelayCnt = 0;
  m->jumpOrder = m->breakRow = m->loopJump = -1;
  m->tickLeft = 0;
  m->tickFrac = 0;
  m->curPatIdx = -1;
  EnterOrder(m);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Public interface.

Player::Player() : p(NULL) {}

Player::~Player() {
  if (!p) return;
  for (int i = 0; i < p->nsmp; ++i)
    if (p->smps && p->smps[i].owned) free(p->smps[i].data);
  free(p->smps); free(p->insts); free(p->pats); free(p->cells); free(p->voices); free(p->mixL); free(p->mixR);
  if (p->fmt == FmtS3m || p->fmt == FmtIt) free(p->orders);
  free(p->file);
  free(p);
}

bool Player::Load(u8* data, u32 size, u32 rate) {
  p = (Impl*)calloc(1, sizeof(Impl));
  if (!p) { free(data); return false; }
  p->file = data;
  p->size = size;
  p->rate = rate;
  // 2^(i/256) for i in 0..257.
  {
    double v = 1.0;
    const double r = 1.0027112750502025;  // 2^(1/256)
    for (int i = 0; i < 258; ++i) { p->pow2tab[i] = (float)v; v *= r; }
  }
  bool ok = false;
  if (size > 1084 && !memcmp(data + 44, "SCRM", 4)) ok = LoadS3m(p);
  else if (size > 60 && !memcmp(data, "Extended Module: ", 17)) ok = LoadXm(p);
  else if (size > 4 && !memcmp(data, "IMPM", 4)) ok = LoadIt(p);
  else ok = LoadMod(p);
  if (!ok || p->nord < 1) { p->fmt = FmtNone; return false; }
  p->nvoices = p->nch + (p->fmt == FmtIt ? 32 : 0);
  p->voices = (Voice*)calloc(p->nvoices, sizeof(Voice));
  p->mixCap = 1024;
  p->mixL = (s32*)malloc(p->mixCap * sizeof(s32));
  p->mixR = (s32*)malloc(p->mixCap * sizeof(s32));
  if (!p->voices || !p->mixL || !p->mixR) return false;
  // Loudness: a few loud channels play at full scale, many are scaled down.
  p->gain = p->nch <= 4 ? 0.4f : p->nch <= 8 ? 0.32f : p->nch <= 16 ? 0.25f : 0.2f;
  static const char* const names[] = {"none", "MOD", "S3M", "XM", "IT"};
  strcpy(p->name, names[p->fmt]);
  ResetState(p, 0);
  return true;
}

void Player::Start(int order) {
  if (!p || p->fmt == FmtNone) return;
  ResetState(p, order < p->nord ? order : 0);
}

const char* Player::Format() const { return p ? p->name : "none"; }
int Player::Channels() const { return p ? p->nch : 0; }
int Player::Orders() const { return p ? p->nord : 0; }
unsigned Player::Bytes() const { return p ? p->size : 0; }

void Player::Render(s16* left, s16* right, u32 count) {
  Impl* m = p;
  if (!m || m->fmt == FmtNone) {
    for (u32 i = 0; i < count; ++i) left[i] = right[i] = 0;
    return;
  }
  u32 done = 0;
  while (done < count) {
    if (m->tickLeft == 0) {
      ProcessTick(m);
      const float len = (float)m->rate * 2.5f / (float)(m->tempo ? m->tempo : 125) + m->tickFrac;
      m->tickLeft = (u32)len;
      m->tickFrac = len - (float)m->tickLeft;
      if (m->tickLeft == 0) m->tickLeft = 1;
    }
    u32 n = count - done;
    if (n > m->tickLeft) n = m->tickLeft;
    if (n > m->mixCap) n = m->mixCap;
    memset(m->mixL, 0, n * sizeof(s32));
    memset(m->mixR, 0, n * sizeof(s32));
    for (int i = 0; i < m->nvoices; ++i)
      if (m->voices[i].active && m->voices[i].smp) MixVoice(m, m->voices[i], m->mixL, m->mixR, n);
    for (u32 i = 0; i < n; ++i) {
      s32 l = m->mixL[i], r = m->mixR[i];
      left[done + i] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
      right[done + i] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    m->tickLeft -= n;
    done += n;
  }
}

}  // namespace lionmod
