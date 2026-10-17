// The intro jingle and the dashboard's sound themes, as Tyke register writes
// scheduled by frame (played by the boot ROM's sequencer).
#include <algorithm>
#include <cmath>

#include "rom/assets.h"

namespace lion::rom {

const char* const kThemeNames[kThemeCount] = {"LION A2 THEME", "LION A2 THEME II", "PRIDE",
                                              "NEON SAVANNA",  "NIGHT PLAINS",     "SILENCE"};
const char* const kThemeBlurbs[kThemeCount] = {"DASHBOARD MUSIC  -  2:50",     "DASHBOARD MUSIC  -  1:00",
                                               "WARM PADS AND BELLS",          "SYNTHWAVE AT 112 BPM",
                                               "WIND, DRONE AND DISTANT BELLS", "NO MUSIC"};

int theme_stream(u32 theme) { return theme < 2 ? int(theme) : -1; }

namespace {

enum Reg { kStart, kLength, kLoop, kPitch, kVolume, kCtrl };
u32 reg(int ch, int r) { return u32(0x100 + 32 * ch + 4 * r); }
u32 pitch(double ratio) { return u32(std::lround(ratio * 65536)); }
u32 vol(double l, double r) {
  auto q = [](double v) { return u32(std::lround(std::clamp(v, 0.0, 1.0) * 256)); };
  return q(l) << 16 | q(r);
}

class Score {
 public:
  Score() = default;

  void poke(u32 frame, u32 offset, u32 value) { ev_.push_back({frame, offset, value}); }
  void off(u32 frame, int ch) { poke(frame, reg(ch, kCtrl), 0); }
  // Starts `s` on voice `ch`. `pan` is -1 (left) .. 1 (right).
  void voice(u32 frame, int ch, const SampleRef& s, double ratio, double level, double pan, bool loop,
             u32 loop_start = 0) {
    poke(frame, reg(ch, kStart), s.addr);
    poke(frame, reg(ch, kLength), s.length);
    poke(frame, reg(ch, kLoop), loop ? loop_start : s.length);
    poke(frame, reg(ch, kPitch), pitch(ratio));
    level_(frame, ch, level, pan);
    poke(frame, reg(ch, kCtrl), loop ? 3u : 1u);
  }
  // Pitched to `hz` from the sample's base frequency.
  void note(u32 frame, int ch, const SampleRef& s, double hz, double level, double pan) {
    voice(frame, ch, s, hz / s.base_hz, level, pan, false);
  }
  void retune(u32 frame, int ch, const SampleRef& s, double hz) { poke(frame, reg(ch, kPitch), pitch(hz / s.base_hz)); }
  // Linear volume segments, sampled every other frame.
  void envelope(int ch, std::initializer_list<std::pair<u32, double>> points, double pan) {
    auto p = points.begin();
    for (auto q = std::next(p); q != points.end(); ++p, ++q)
      for (u32 f = p->first; f < q->first; f += 2)
        level_(f, ch, p->second + (q->second - p->second) * double(f - p->first) / double(q->first - p->first), pan);
    level_(p->first, ch, p->second, pan);
  }
  std::vector<u32> finish(bool loop, u32 length) {
    std::stable_sort(ev_.begin(), ev_.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
    std::vector<u32> out;
    for (const Ev& e : ev_) out.insert(out.end(), {e.frame, e.offset, e.value});
    if (loop) out.insert(out.end(), {length, kSeqLoop, 0});
    out.insert(out.end(), {kSeqEnd, 0, 0});
    return out;
  }

 private:
  struct Ev {
    u32 frame, offset, value;
  };
  void level_(u32 frame, int ch, double level, double pan) {
    poke(frame, reg(ch, kVolume), vol(level * std::min(1.0, 1 - pan), level * std::min(1.0, 1 + pan)));
  }
  std::vector<Ev> ev_;
};

constexpr double kA1 = 55.0;
double note_hz(int semitones_from_a4) { return 440.0 * std::pow(2.0, semitones_from_a4 / 12.0); }

}  // namespace

std::vector<u32> jingle_events(const SoundBank& bank) {
  Score s;
  s.poke(0, 0, 0x100);  // master volume
  for (int ch = 0; ch < 16; ++ch) s.off(0, ch);

  // The roar over a drone on A1.
  s.voice(16, 0, bank.lion_roar, 1.0, 0.95, 0.0, false);
  s.voice(18, 2, bank.pad, kA1 / bank.pad.base_hz, 0.0, 0.0, true);
  s.envelope(2, {{18, 0.0}, {80, 0.34}, {300, 0.34}, {350, 0.0}}, 0.0);
  s.off(352, 2);

  // The claws, as the slashes cross the plate.
  s.voice(kSlashFrame[0] - 4, 3, bank.lion_claws, 1.0, 1.0, 0.0, false);

  // The reveal: a low and a high bell on the flash, then an A major arpeggio and a pad.
  s.voice(130, 6, bank.bell, 0.25, 0.55, 0.0, false);
  s.voice(130, 7, bank.bell, 2.0, 0.16, 0.1, false);
  const double arp[5] = {220.0, 329.63, 440.0, 554.37, 659.26}, arp_pan[5] = {-0.4, 0.4, 0.0, -0.5, 0.5};
  for (int k = 0; k < 5; ++k) s.note(136 + 6 * u32(k), 8 + k, bank.bell, arp[k], 0.42, arp_pan[k]);
  const double chord[3] = {220.0, 277.18, 329.63};
  for (int k = 0; k < 3; ++k) {
    const int ch = 13 + k;
    s.voice(130, ch, bank.pad, chord[k] / bank.pad.base_hz * (k % 2 ? 1.003 : 0.997), 0.0, 0.0, true);
    s.envelope(ch, {{130, 0.0}, {165, 0.16}, {280, 0.16}, {345, 0.0}}, k == 0 ? 0.0 : k == 1 ? -0.35 : 0.35);
    s.off(348, ch);
  }
  // The crown lands: two high bells.
  s.note(218, 7, bank.bell, 1108.73, 0.30, -0.2);
  s.note(224, 4, bank.bell, 1318.51, 0.22, 0.2);
  return s.finish(false, 0);
}

std::vector<u32> theme_events(const SoundBank& bank, u32 theme) {
  Score s;
  for (int ch = 0; ch < 16; ++ch) s.off(0, ch);
  switch (theme) {
    case 2: {  // PRIDE: Am F C G, two seconds each, pads under a bell melody.
      const int chords[4][4] = {{-24, -17, -12, -9}, {-28, -21, -16, -12}, {-21, -14, -9, -5}, {-26, -19, -14, -10}};
      const int melody[4][3] = {{7, 3, 0}, {3, 0, 8}, {7, 10, 3}, {5, 2, -2}};
      for (int v = 0; v < 4; ++v) s.voice(0, v, bank.pad, note_hz(chords[0][v]) / bank.pad.base_hz, 0.0, 0.0, true);
      s.voice(0, 6, bank.pad, note_hz(chords[0][0] - 12) / bank.pad.base_hz, 0.0, 0.0, true);
      for (u32 c = 0; c < 4; ++c) {
        const u32 t = c * 120;
        for (int v = 0; v < 4; ++v) {
          s.retune(t, v, bank.pad, note_hz(chords[c][v]));
          s.envelope(v, {{t, 0.05}, {t + 24, 0.11}, {t + 108, 0.10}, {t + 120, 0.05}}, v % 2 ? 0.3 : -0.3);
        }
        s.retune(t, 6, bank.pad, note_hz(chords[c][0] - 12));
        s.envelope(6, {{t, 0.10}, {t + 30, 0.17}, {t + 120, 0.10}}, 0.0);
        for (int n = 0; n < 3; ++n)
          s.note(t + 10 + 36 * u32(n), 4 + n % 2, bank.bell, note_hz(melody[c][n]), 0.17, n % 2 ? 0.35 : -0.35);
      }
      return s.finish(true, 480);
    }
    case 3: {  // NEON SAVANNA: kick, hats, a gliding bass and a plucked arpeggio; beat = 32 frames.
      const int roots[4] = {-36, -40, -33, -38};  // A1 F1 C2 G1
      const int arp[8] = {0, 7, 12, 15, 19, 15, 12, 7};
      const int third[4] = {3, 4, 4, 4};
      s.voice(0, 2, bank.pad, note_hz(roots[0]) / bank.pad.base_hz, 0.0, 0.0, true);
      for (int v = 0; v < 2; ++v) s.voice(0, 5 + v, bank.pad, note_hz(roots[0] + 24) / bank.pad.base_hz, 0.0, 0.0, true);
      for (u32 bar = 0; bar < 4; ++bar) {
        const u32 t0 = bar * 128;
        for (int v = 0; v < 2; ++v) {
          s.retune(t0, 5 + v, bank.pad, note_hz(roots[bar] + 24 + (v ? 7 : third[bar])));
          s.envelope(5 + v, {{t0, 0.04}, {t0 + 40, 0.08}, {t0 + 128, 0.04}}, v ? 0.4 : -0.4);
        }
        for (u32 beat = 0; beat < 4; ++beat) {
          const u32 t = t0 + beat * 32;
          s.voice(t, 0, bank.kick, 1.0, 0.60, 0.0, false);
          s.voice(t + 16, 1, bank.hat, 1.0, 0.22, 0.25, false);
          for (u32 e = 0; e < 2; ++e) {  // bass eighths
            const u32 tb = t + e * 16;
            s.retune(tb, 2, bank.pad, note_hz(roots[bar] + (e ? 12 : 0)));
            s.envelope(2, {{tb, 0.42}, {tb + 12, 0.12}, {tb + 16, 0.12}}, 0.0);
          }
          for (u32 q = 0; q < 4; ++q) {  // sixteenth arpeggio, two voices so notes ring
            const int idx = int((beat * 4 + q) % 8);
            const int semis = roots[bar] + 36 + (arp[idx] == 3 || arp[idx] == 15 ? arp[idx] + third[bar] - 3 : arp[idx]);
            s.note(t + q * 8, 3 + int(q % 2), bank.pluck, note_hz(semis), 0.16, q % 2 ? 0.45 : -0.45);
          }
        }
      }
      return s.finish(true, 512);
    }
    case 4: {  // NIGHT PLAINS: wind from the roar slowed right down, a drone and sparse bells.
      s.voice(0, 0, bank.roar, 0.22, 0.10, -0.3, true, bank.roar.length / 3);
      s.voice(0, 1, bank.roar, 0.19, 0.08, 0.3, true, bank.roar.length / 2);
      s.voice(0, 2, bank.pad, kA1 / bank.pad.base_hz, 0.0, 0.0, true);
      s.voice(0, 3, bank.pad, kA1 * 1.5 / bank.pad.base_hz, 0.0, 0.0, true);
      s.envelope(2, {{0, 0.10}, {360, 0.16}, {720, 0.10}}, 0.0);
      s.envelope(3, {{0, 0.08}, {360, 0.04}, {720, 0.08}}, 0.0);
      // A minor pentatonic, far away: each note followed by a softer repeat.
      const int notes[8] = {12, 7, 3, 10, 5, 0, 7, 15};
      const u32 when[8] = {20, 110, 170, 300, 380, 470, 560, 640};
      for (int n = 0; n < 8; ++n) {
        const double pan = (n % 3 - 1) * 0.5;
        s.note(when[n], 4 + n % 4, bank.bell, note_hz(notes[n]), 0.15, pan);
        s.note(when[n] + 22, 8 + n % 4, bank.bell, note_hz(notes[n]), 0.06, -pan);
      }
      return s.finish(true, 720);
    }
    default:  // SILENCE, and the streamed themes (the sequencer only clears the voices)
      return s.finish(false, 0);
  }
}

}  // namespace lion::rom
