#pragma once
// Audio discs for the dashboard's CD player: ISO9660 images with .WAV tracks
// (16-bit PCM) in the root directory, plus three short demo tracks.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "iso9660.h"

namespace music {

// RIFF/WAVE, 16-bit PCM, interleaved when stereo.
inline std::vector<uint8_t> wav(const std::vector<int16_t>& samples, uint32_t rate, uint16_t channels) {
  std::vector<uint8_t> out;
  auto u32le = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
  };
  auto u16le = [&](uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
  };
  auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
  const uint32_t bytes = uint32_t(samples.size() * 2);
  tag("RIFF"), u32le(36 + bytes), tag("WAVE");
  tag("fmt "), u32le(16), u16le(1), u16le(channels), u32le(rate), u32le(rate * channels * 2), u16le(channels * 2), u16le(16);
  tag("data"), u32le(bytes);
  for (int16_t s : samples) u16le(uint16_t(s));
  return out;
}

namespace detail {
constexpr double kPi = 3.14159265358979323846;
inline double hz(int semis_from_a4) { return 440.0 * std::pow(2.0, semis_from_a4 / 12.0); }
inline int16_t clip(double v) { return int16_t(std::clamp(std::lround(v * 32767.0), -32767l, 32767l)); }
inline double saw(double phase) { return 2.0 * (phase - std::floor(phase)) - 1.0; }
}  // namespace detail

// 16 s of synthwave: kick, gliding bass, plucked arpeggio and a pad (44.1 kHz stereo).
inline std::vector<uint8_t> track_pride() {
  using namespace detail;
  constexpr uint32_t kRate = 44100;
  constexpr double kBeat = 0.5;
  const int roots[4] = {-36, -40, -33, -38}, thirds[4] = {3, 4, 4, 4};
  const size_t n = size_t(16.0 * kRate);
  std::vector<int16_t> out;
  double bass_phase = 0, pad_phase[3] = {}, lp = 0;
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kRate;
    const int bar = int(t / (4 * kBeat)) % 4;
    const double in_beat = std::fmod(t, kBeat), in_eighth = std::fmod(t, kBeat / 2), in_16th = std::fmod(t, kBeat / 4);
    const double kick = std::sin(2 * kPi * (50 * in_beat + 6 * (1 - std::exp(-in_beat / 0.03)))) * std::exp(-in_beat / 0.12);
    bass_phase += hz(roots[bar] + (int(t / (kBeat / 2)) % 2 ? 12 : 0)) / kRate;
    const double bass_raw = saw(bass_phase) * std::exp(-in_eighth / 0.15);
    lp += 0.08 * (bass_raw - lp);
    const int step = int(t / (kBeat / 4)) % 8;
    const int arp[8] = {0, 7, 12, thirds[bar] + 12, 19, thirds[bar] + 12, 12, 7};
    const double a_hz = hz(roots[bar] + 36 + arp[step]);
    const double pluck = (saw(a_hz * in_16th) * 0.6 + std::sin(2 * kPi * a_hz * in_16th)) * std::exp(-in_16th / 0.07);
    double pad = 0;
    const int chord[3] = {0, thirds[bar], 7};
    for (int k = 0; k < 3; ++k) {
      pad_phase[k] += hz(roots[bar] + 24 + chord[k]) * (k == 1 ? 1.002 : 1.0) / kRate;
      pad += saw(pad_phase[k]);
    }
    const double fade = std::min(1.0, std::min(t / 0.5, (16.0 - t) / 1.0));
    const double l = fade * (0.55 * kick + 0.45 * lp * 2 + 0.22 * pluck * (step % 2 ? 0.6 : 1.0) + 0.05 * pad);
    const double r = fade * (0.55 * kick + 0.45 * lp * 2 + 0.22 * pluck * (step % 2 ? 1.0 : 0.6) + 0.05 * pad);
    out.push_back(clip(l * 0.55));
    out.push_back(clip(r * 0.55));
  }
  return wav(out, kRate, 2);
}

// 12 s of bells over a drone, mono at 22.05 kHz.
inline std::vector<uint8_t> track_savanna() {
  using namespace detail;
  constexpr uint32_t kRate = 22050;
  const size_t n = size_t(12.0 * kRate);
  const int notes[8] = {0, 3, 7, 10, 12, 10, 7, 5};
  std::vector<int16_t> out;
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kRate;
    double v = 0.15 * std::sin(2 * kPi * 55 * t) + 0.08 * std::sin(2 * kPi * 82.5 * t);
    const int k = int(t / 1.5);
    const double dt = std::fmod(t, 1.5);
    const double f = hz(notes[k % 8]);
    v += 0.35 * std::exp(-dt / 0.6) * (std::sin(2 * kPi * f * dt) + 0.4 * std::sin(2 * kPi * f * 2.76 * dt));
    out.push_back(clip(v * std::min(1.0, (12.0 - t) / 1.0)));
  }
  return wav(out, kRate, 1);
}

// 10 s brass-like fanfare in A major, stereo at 32 kHz.
inline std::vector<uint8_t> track_crown() {
  using namespace detail;
  constexpr uint32_t kRate = 32000;
  const size_t n = size_t(10.0 * kRate);
  const int chords[5][3] = {{-12, -8, -5}, {-7, -3, 0}, {-10, -5, -2}, {-12, -8, -5}, {-12, -8, -5}};
  std::vector<int16_t> out;
  double ph[3] = {}, lp[3] = {};
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kRate;
    const int c = std::min(4, int(t / 2.0));
    const double dt = std::fmod(t, 2.0);
    const double env = std::min(1.0, dt / 0.08) * (0.7 + 0.3 * std::exp(-dt / 0.3));
    double l = 0, r = 0;
    for (int k = 0; k < 3; ++k) {
      ph[k] += hz(chords[c][k]) / kRate;
      lp[k] += 0.2 * (saw(ph[k]) - lp[k]);
      l += lp[k] * (k == 2 ? 0.5 : 1.0);
      r += lp[k] * (k == 0 ? 0.5 : 1.0);
    }
    const double fade = std::min(1.0, (10.0 - t) / 1.5);
    out.push_back(clip(0.3 * env * fade * l));
    out.push_back(clip(0.3 * env * fade * r));
  }
  return wav(out, kRate, 2);
}

// An audio disc; with no tracks given, the three demo tracks.
inline std::vector<uint8_t> build_disc(std::vector<iso9660::File> tracks = {}) {
  if (tracks.empty())
    tracks = {{"PRIDE.WAV", track_pride()}, {"SAVANNA.WAV", track_savanna()}, {"CROWN.WAV", track_crown()}};
  return iso9660::build("LION_MUSIC", tracks);
}

}  // namespace music
