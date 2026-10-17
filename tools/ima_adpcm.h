#pragma once
// IMA ADPCM as the Lion boot ROM streams its dashboard music ("LADP" files).
//
//   header  "LADP", frames, blocks, sample rate        (4 big-endian words)
//   blocks  of kBlockFrames stereo frames, kBlockBytes each:
//           left predictor (s16), left index (u8), 0, right predictor, right index, 0,
//           then one byte per frame: left nibble high, right nibble low.
// Each block restarts both predictors, so a block decodes on its own (and loops cleanly).

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ima {

constexpr uint32_t kMagic = 0x4C41'4450;  // "LADP"
constexpr uint32_t kBlockFrames = 1024, kBlockBytes = 8 + kBlockFrames, kHeaderBytes = 16;

constexpr int16_t kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct State {
  int pred = 0, index = 0;
};

// Exactly what the boot ROM's decoder does.
inline int16_t decode(State& s, unsigned nibble) {
  const int step = kStep[s.index];
  int diff = step >> 3;
  if (nibble & 4) diff += step;
  if (nibble & 2) diff += step >> 1;
  if (nibble & 1) diff += step >> 2;
  if (nibble & 8) diff = -diff;
  s.pred = std::clamp(s.pred + diff, -32768, 32767);
  s.index = std::clamp(s.index + kIndex[nibble & 15], 0, 88);
  return int16_t(s.pred);
}

// Picks the nibble whose decoded value lands closest to `sample`.
inline unsigned encode(State& s, int sample) {
  unsigned best = 0;
  int best_err = 1 << 30;
  for (unsigned n = 0; n < 16; ++n) {
    State t = s;
    const int err = std::abs(decode(t, n) - sample);
    if (err < best_err) best_err = err, best = n;
  }
  decode(s, best);
  return best;
}

inline void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

// Interleaved stereo PCM -> LADP.
inline std::vector<uint8_t> encode_stereo(const std::vector<int16_t>& pcm, uint32_t rate) {
  const uint32_t frames = uint32_t(pcm.size() / 2);
  const uint32_t blocks = (frames + kBlockFrames - 1) / kBlockFrames;
  std::vector<uint8_t> out;
  put32(out, kMagic), put32(out, frames), put32(out, blocks), put32(out, rate);
  State st[2];
  for (uint32_t b = 0; b < blocks; ++b) {
    for (int c = 0; c < 2; ++c) {
      // Start each block from the true sample, with the step the encoder had reached.
      const size_t first = size_t(b) * kBlockFrames;
      st[c].pred = first < frames ? pcm[first * 2 + size_t(c)] : 0;
      out.push_back(uint8_t(uint16_t(st[c].pred) >> 8));
      out.push_back(uint8_t(st[c].pred));
      out.push_back(uint8_t(st[c].index));
      out.push_back(0);
    }
    for (uint32_t f = 0; f < kBlockFrames; ++f) {
      const size_t i = size_t(b) * kBlockFrames + f;
      const int l = i < frames ? pcm[i * 2] : 0, r = i < frames ? pcm[i * 2 + 1] : 0;
      out.push_back(uint8_t(encode(st[0], l) << 4 | encode(st[1], r)));
    }
  }
  return out;
}

// LADP -> interleaved stereo PCM (reference decoder, for tests).
inline std::vector<int16_t> decode_stereo(const uint8_t* data) {
  auto get32 = [&](size_t at) {
    return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 | data[at + 3];
  };
  const uint32_t blocks = get32(8);
  std::vector<int16_t> out;
  for (uint32_t b = 0; b < blocks; ++b) {
    const uint8_t* p = data + kHeaderBytes + size_t(b) * kBlockBytes;
    State st[2];
    for (int c = 0; c < 2; ++c) st[c] = {int16_t(p[4 * c] << 8 | p[4 * c + 1]), p[4 * c + 2]};
    for (uint32_t f = 0; f < kBlockFrames; ++f) {
      out.push_back(decode(st[0], p[8 + f] >> 4));
      out.push_back(decode(st[1], p[8 + f] & 15));
    }
  }
  return out;
}

}  // namespace ima
