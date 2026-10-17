#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <bit>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// Guest memory is stored big-endian, exactly as the hardware sees it.
inline u16 load_be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
inline u32 load_be32(const u8* p) {
  u32 v;
  std::memcpy(&v, p, 4);
  return std::byteswap(v);
}
inline u64 load_be64(const u8* p) { return u64(load_be32(p)) << 32 | load_be32(p + 4); }
inline void store_be16(u8* p, u16 v) {
  p[0] = u8(v >> 8);
  p[1] = u8(v);
}
inline void store_be32(u8* p, u32 v) {
  v = std::byteswap(v);
  std::memcpy(p, &v, 4);
}
inline void store_be64(u8* p, u64 v) {
  store_be32(p, u32(v >> 32));
  store_be32(p + 4, u32(v));
}

inline float bits_to_f32(u32 v) { return std::bit_cast<float>(v); }
inline u32 f32_to_bits(float f) { return std::bit_cast<u32>(f); }
inline double bits_to_f64(u64 v) { return std::bit_cast<double>(v); }
inline u64 f64_to_bits(double d) { return std::bit_cast<u64>(d); }

inline u32 rotl32(u32 v, unsigned n) { return std::rotl(v, int(n & 31)); }

namespace lion {

// Clocks from the A2 spec.
constexpr u64 kTuffyHz = 500'000'000;
constexpr u64 kQuackerHz = 100'000'000;
constexpr u64 kBusHz = 100'000'000;
constexpr u64 kSpikeHz = 90'000'000;
constexpr int kFrameRate = 60;
constexpr int kAudioRate = 44100;

extern bool g_verbose;

}  // namespace lion

#define LION_LOG(...)                    \
  do {                                   \
    if (::lion::g_verbose) {             \
      std::fprintf(stderr, __VA_ARGS__); \
      std::fputc('\n', stderr);          \
    }                                    \
  } while (0)

#define LION_WARN(...)                 \
  do {                                 \
    std::fprintf(stderr, "[lion] ");   \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fputc('\n', stderr);          \
  } while (0)
