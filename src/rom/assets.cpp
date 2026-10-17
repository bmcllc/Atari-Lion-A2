#include "rom/assets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

#include "bus.h"
#include "rom/blobs.h"

namespace lion::rom {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- font

// 5x7 glyphs, one byte per row, bit 4 = leftmost pixel.
struct Glyph {
  char c;
  u8 rows[7];
};
constexpr Glyph kGlyphs[] = {
    {'!', {0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x04}}, {'"', {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}},
    {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}}, {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
    {'\'', {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'*', {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}, {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}}, {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}}, {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}}, {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}, {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {';', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}}, {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}}, {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}}, {'@', {0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E}},
    {'A', {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}}, {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
};

// ---------------------------------------------------------------- meshes

struct V3 {
  float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

class MeshBuilder {
 public:
  explicit MeshBuilder(u32 flags = 0) : flags_(flags) {}

  // Front faces are counter-clockwise seen from outside; `inside` is any point
  // inside the solid, used to orient the face.
  void tri(V3 a, V3 b, V3 c, u32 argb, V3 inside) {
    V3 n = cross(b - a, c - a);
    const float len = std::sqrt(dot(n, n));
    if (len < 1e-9f) return;
    n = n * (1.0f / len);
    const V3 centroid = (a + b + c) * (1.0f / 3.0f);
    if (dot(n, centroid - inside) < 0) {
      std::swap(b, c);
      n = n * -1.0f;
    }
    tris_.push_back({{vertex(a), vertex(b), vertex(c)}, argb, n});
  }
  void quad(V3 a, V3 b, V3 c, V3 d, u32 argb, V3 inside) {
    tri(a, b, c, argb, inside);
    tri(a, c, d, argb, inside);
  }
  // Unlit, unculled geometry (the grid) with a colour per corner: no orientation needed.
  void gouraud_quad(V3 a, V3 b, V3 c, V3 d, u32 ca, u32 cb, u32 cc, u32 cd) {
    tris_.push_back({{vertex(a), vertex(b), vertex(c)}, ca, {bits_to_f32(cb), bits_to_f32(cc), 0}});
    tris_.push_back({{vertex(a), vertex(c), vertex(d)}, ca, {bits_to_f32(cc), bits_to_f32(cd), 0}});
  }
  void box(V3 c, V3 h, u32 argb) {
    V3 p[8];
    for (int i = 0; i < 8; ++i) p[i] = {c.x + (i & 1 ? h.x : -h.x), c.y + (i & 2 ? h.y : -h.y), c.z + (i & 4 ? h.z : -h.z)};
    quad(p[0], p[1], p[3], p[2], argb, c);  // -z
    quad(p[4], p[5], p[7], p[6], argb, c);  // +z
    quad(p[0], p[2], p[6], p[4], argb, c);  // -x
    quad(p[1], p[3], p[7], p[5], argb, c);  // +x
    quad(p[0], p[1], p[5], p[4], argb, c);  // -y
    quad(p[2], p[3], p[7], p[6], argb, c);  // +y
  }
  // Tube swept along `path` with four-corner cross-sections from `section`.
  template <typename F>
  void sweep(const std::vector<V3>& path, F section, u32 argb, bool caps) {
    std::vector<std::array<V3, 4>> s;
    for (size_t i = 0; i < path.size(); ++i) s.push_back(section(i));
    for (size_t i = 0; i + 1 < path.size(); ++i) {
      const V3 mid = (path[i] + path[i + 1]) * 0.5f;
      for (int e = 0; e < 4; ++e) quad(s[i][e], s[i][(e + 1) % 4], s[i + 1][(e + 1) % 4], s[i + 1][e], argb, mid);
    }
    if (caps) {
      const size_t n = path.size() - 1;
      quad(s[0][0], s[0][1], s[0][2], s[0][3], argb, (path[0] + path[1]) * 0.5f);
      quad(s[n][0], s[n][1], s[n][2], s[n][3], argb, (path[n] + path[n - 1]) * 0.5f);
    }
  }

  std::vector<u32> words() const {
    std::vector<u32> w = {u32(verts_.size()), u32(tris_.size()), flags_, 0};
    for (const V3& v : verts_) {
      w.push_back(f32_to_bits(v.x));
      w.push_back(f32_to_bits(v.y));
      w.push_back(f32_to_bits(v.z));
      w.push_back(f32_to_bits(1.0f));
    }
    for (const Tri& t : tris_) {
      for (int k = 0; k < 3; ++k) w.push_back(u32(t.v[k]) * 16);
      w.push_back(t.argb);
      w.push_back(f32_to_bits(t.n.x));
      w.push_back(f32_to_bits(t.n.y));
      w.push_back(f32_to_bits(t.n.z));
    }
    return w;
  }

 private:
  struct Tri {
    int v[3];
    u32 argb;
    V3 n;
  };
  int vertex(V3 p) {
    const auto key = std::tuple{std::lround(p.x * 1e4f), std::lround(p.y * 1e4f), std::lround(p.z * 1e4f)};
    auto [it, fresh] = index_.try_emplace(key, int(verts_.size()));
    if (fresh) verts_.push_back(p);
    return it->second;
  }
  u32 flags_;
  std::vector<V3> verts_;
  std::vector<Tri> tris_;
  std::map<std::tuple<long, long, long>, int> index_;
};

u32 argb(double r, double g, double b) {
  auto c = [](double v) { return u32(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
  return 0xFF00'0000u | c(r) << 16 | c(g) << 8 | c(b);
}

// ---------------------------------------------------------------- audio

struct Pcm {
  std::vector<double> s;
};

void normalize(std::vector<double>& s, double peak) {
  double m = 1e-9;
  for (double v : s) m = std::max(m, std::fabs(v));
  for (double& v : s) v *= peak / m;
}

std::vector<double> bell(double hz, double seconds) {
  const size_t n = size_t(seconds * kAudioRate);
  std::vector<double> s(n);
  // Inharmonic partials of a struck bar: (ratio, amplitude, decay time).
  const double partials[][3] = {{1.0, 1.0, 0.9}, {2.0, 0.45, 0.6}, {2.76, 0.32, 0.4}, {5.40, 0.18, 0.2}, {8.93, 0.10, 0.12}};
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kAudioRate;
    double v = 0;
    for (auto& p : partials) v += p[1] * std::exp(-t / p[2]) * std::sin(2 * kPi * hz * p[0] * t);
    v *= std::min(1.0, t / 0.002);                                  // 2 ms attack
    v *= std::min(1.0, (seconds - t) / 0.05);                       // no click at the end
    s[i] = v;
  }
  normalize(s, 9000);
  return s;
}

// One cycle of a soft, slightly hollow saw: looped for pads and drones.
std::vector<double> pad_cycle(size_t n) {
  std::vector<double> s(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / double(n);
    double v = 0;
    for (int h = 1; h <= 10; ++h) v += std::sin(2 * kPi * h * t) / std::pow(h, h % 2 ? 1.2 : 1.6);
    s[i] = v;
  }
  normalize(s, 7000);
  return s;
}

// The lion's roar: swelling filtered noise with a low growl underneath.
std::vector<double> roar(double seconds) {
  const size_t n = size_t(seconds * kAudioRate);
  std::vector<double> s(n);
  u32 lcg = 0x1999'0A02;
  double lp1 = 0, lp2 = 0, phase = 0;
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kAudioRate, u = t / seconds;
    lcg = lcg * 1664525u + 1013904223u;
    const double noise = double(s32(lcg)) / 2147483648.0;
    const double cutoff = 250 + 2600 * std::sin(kPi * std::min(1.0, u * 1.3)) * (1 - 0.5 * u);
    const double a = 1 - std::exp(-2 * kPi * cutoff / kAudioRate);
    lp1 += a * (noise - lp1);
    lp2 += a * (lp1 - lp2);
    const double hz = 78 - 26 * u;
    phase += hz / kAudioRate;
    const double saw = 2 * (phase - std::floor(phase)) - 1;
    const double growl = 0.55 + 0.45 * std::sin(2 * kPi * 26 * t);
    const double env = std::min(1.0, u / 0.25) * std::pow(1 - u, 1.6);
    s[i] = env * (lp2 * 2.2 * growl + 0.35 * saw * growl);
  }
  normalize(s, 9500);
  return s;
}

std::vector<double> blip(std::initializer_list<std::pair<double, double>> steps, double decay) {
  std::vector<double> s;
  double phase = 0, t0 = 0;
  for (auto [hz, len] : steps) {
    const size_t n = size_t(len * kAudioRate);
    for (size_t i = 0; i < n; ++i) {
      const double t = t0 + double(i) / kAudioRate;
      phase += hz / kAudioRate;
      const double env = std::min(1.0, (t - t0) / 0.001) * std::exp(-(t - t0) / decay);
      s.push_back(env * (std::sin(2 * kPi * phase) + 0.25 * std::sin(6 * kPi * phase)));
    }
    t0 += len;
  }
  normalize(s, 6000);
  return s;
}

// Claws raking metal: band-passed noise sweeping down, broken into grains.
std::vector<double> claw(double seconds) {
  const size_t n = size_t(seconds * kAudioRate);
  std::vector<double> s(n);
  u32 lcg = 0xC1A3'5EED;
  double lp = 0, hp = 0, prev = 0;
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kAudioRate, u = t / seconds;
    lcg = lcg * 1664525u + 1013904223u;
    const double noise = double(s32(lcg)) / 2147483648.0;
    const double cutoff = 5200 * std::pow(0.18, u);
    const double a = 1 - std::exp(-2 * kPi * cutoff / kAudioRate);
    lp += a * (noise - lp);
    hp = 0.97 * (hp + lp - prev);  // drop the lows: a thin, metallic rasp
    prev = lp;
    const double grain = 0.55 + 0.45 * std::sin(2 * kPi * (90 + 160 * u) * t);
    const double env = std::min(1.0, t / 0.004) * std::pow(1 - u, 2.2);
    s[i] = env * grain * hp;
  }
  normalize(s, 10000);
  return s;
}

// Karplus-Strong plucked string.
std::vector<double> pluck(double hz, double seconds) {
  const size_t n = size_t(seconds * kAudioRate), period = size_t(kAudioRate / hz + 0.5);
  std::vector<double> line(period), s(n);
  u32 lcg = 0x0B0E'5EED;
  for (double& v : line) {
    lcg = lcg * 1664525u + 1013904223u;
    v = double(s32(lcg)) / 2147483648.0;
  }
  for (size_t i = 0; i < n; ++i) {
    const size_t k = i % period;
    s[i] = line[k];
    line[k] = 0.4985 * (line[k] + line[(k + 1) % period]);
  }
  for (size_t i = n - 400; i < n; ++i) s[i] *= double(n - i) / 400.0;
  normalize(s, 8000);
  return s;
}

std::vector<double> kick(double seconds) {
  const size_t n = size_t(seconds * kAudioRate);
  std::vector<double> s(n);
  double phase = 0;
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kAudioRate;
    phase += (45 + 95 * std::exp(-t / 0.03)) / kAudioRate;
    s[i] = std::sin(2 * kPi * phase) * std::exp(-t / 0.09) + (t < 0.002 ? 0.5 : 0.0);
  }
  normalize(s, 11000);
  return s;
}

std::vector<double> hat(double seconds) {
  const size_t n = size_t(seconds * kAudioRate);
  std::vector<double> s(n);
  u32 lcg = 0x4A75'0001;
  double prev = 0;
  for (size_t i = 0; i < n; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const double noise = double(s32(lcg)) / 2147483648.0;
    s[i] = (noise - prev) * std::exp(-double(i) / kAudioRate / 0.012);
    prev = noise;
  }
  normalize(s, 6000);
  return s;
}

void ease(double& v) { v = 1 - std::pow(1 - std::clamp(v, 0.0, 1.0), 3); }
double ramp(double f, double a, double b) { return std::clamp((f - a) / (b - a), 0.0, 1.0); }
double smooth(double f, double a, double b) {
  const double t = ramp(f, a, b);
  return t * t * (3 - 2 * t);
}

}  // namespace

std::vector<u8> font_texture() {
  constexpr int kW = 256, kH = 128;
  std::vector<u8> lum(kW * kH, 0);
  auto put = [&](int x, int y, u8 v) {
    if (x >= 0 && x < kW && y >= 0 && y < kH) lum[size_t(y * kW + x)] = std::max(lum[size_t(y * kW + x)], v);
  };
  for (int code = 0; code < 128; ++code) {
    char c = char(code);
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    const Glyph* g = nullptr;
    for (const Glyph& k : kGlyphs)
      if (k.c == c) g = &k;
    if (!g) continue;
    const int ox = (code % 16) * 16 + 1, oy = (code / 16) * 16 + 1;
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col) {
        if (!(g->rows[row] >> (4 - col) & 1)) continue;
        const int x = ox + col * 2, y = oy + row * 2;
        // Soft halo first, then the 2x2 core.
        for (int dy = -1; dy <= 2; ++dy)
          for (int dx = -1; dx <= 2; ++dx) put(x + dx, y + dy, 56);
        for (int dy = 0; dy < 2; ++dy)
          for (int dx = 0; dx < 2; ++dx) put(x + dx, y + dy, 255);
      }
  }
  std::vector<u8> out;
  out.reserve(lum.size() * 2);
  for (u8 v : lum) {
    const u16 p = u16((v >> 3) << 11 | (v >> 2) << 5 | (v >> 3));
    out.push_back(u8(p >> 8));
    out.push_back(u8(p));
  }
  return out;
}

std::vector<u32> mesh_emblem(bool small) {
  // The small version (dashboard header) has a coarser ring and bars.
  const int bar_segments = small ? 3 : 5, ring_segments = small ? 12 : 24;
  MeshBuilder m;
  const u32 red = argb(0.92, 0.10, 0.10), gold = argb(0.96, 0.66, 0.18), amber = argb(0.80, 0.48, 0.12);
  // Centre bar.
  m.box({0, 0, 0}, {0.16f, 1.0f, 0.16f}, red);
  // Side bars: they leave the apex and flare out towards the base.
  for (float side : {-1.0f, 1.0f}) {
    std::vector<V3> path;
    for (int k = 0; k <= bar_segments; ++k) {
      const float y = 1.0f - 2.0f * float(k) / float(bar_segments), u = (1 - y) / 2;
      path.push_back({side * (0.36f + 0.58f * u * u), y, 0});
    }
    const float w = 0.14f, d = 0.14f;
    m.sweep(path, [&](size_t i) {
      const V3 p = path[i];
      return std::array<V3, 4>{V3{p.x - w, p.y, -d}, V3{p.x + w, p.y, -d}, V3{p.x + w, p.y, d}, V3{p.x - w, p.y, d}};
    }, red, true);
  }
  // Ring.
  {
    const int kSeg = ring_segments;
    const float r = 1.22f, t = 0.05f;
    std::vector<V3> path;
    for (int k = 0; k <= kSeg; ++k) {
      const float a = float(2 * kPi * k / kSeg);
      path.push_back({r * std::cos(a), r * std::sin(a), 0});
    }
    m.sweep(path, [&](size_t i) {
      const float a = float(2 * kPi * double(i) / kSeg), cx = std::cos(a), cy = std::sin(a);
      return std::array<V3, 4>{V3{(r - t) * cx, (r - t) * cy, -t}, V3{(r + t) * cx, (r + t) * cy, -t},
                               V3{(r + t) * cx, (r + t) * cy, t}, V3{(r - t) * cx, (r - t) * cy, t}};
    }, amber, false);
  }
  // Mane: sixteen rays, long and short.
  for (int k = 0; k < 16; ++k) {
    const float th = float(2 * kPi * k / 16 + kPi / 16), a = 0.10f, r0 = 1.30f, r1 = k % 2 ? 1.50f : 1.74f, z = 0.07f;
    auto at = [](float r, float ang, float zz) { return V3{r * std::cos(ang), r * std::sin(ang), zz}; };
    const V3 b1 = at(r0, th - a, z), b2 = at(r0, th + a, z), b3 = at(r0, th + a, -z), b4 = at(r0, th - a, -z);
    const V3 tip = at(r1, th, 0), inside = at(r0 + (r1 - r0) * 0.25f, th, 0);
    m.tri(b1, b2, tip, gold, inside);
    m.tri(b2, b3, tip, gold, inside);
    m.tri(b3, b4, tip, gold, inside);
    m.tri(b4, b1, tip, gold, inside);
    m.quad(b1, b2, b3, b4, gold, inside);
  }
  return m.words();
}

std::vector<u32> mesh_disc() {
  MeshBuilder m;
  constexpr int kSeg = 24;
  const float ri = 0.20f, ro = 0.92f, t = 0.05f;
  for (int k = 0; k < kSeg; ++k) {
    const float a0 = float(2 * kPi * k / kSeg), a1 = float(2 * kPi * (k + 1) / kSeg), am = (a0 + a1) / 2;
    auto at = [](float r, float ang, float z) { return V3{r * std::cos(ang), r * std::sin(ang), z}; };
    const V3 inside = at((ri + ro) / 2, am, 0);
    // Iridescent face: silver with a hue that turns around the disc.
    const double h = double(k) / kSeg * 2 * kPi;
    const u32 face = argb(0.72 + 0.22 * std::cos(h), 0.74 + 0.22 * std::cos(h - 2.1), 0.80 + 0.18 * std::cos(h + 2.1));
    const u32 back = argb(0.55, 0.56, 0.62), rim = argb(0.70, 0.70, 0.76);
    m.quad(at(ri, a0, t), at(ro, a0, t), at(ro, a1, t), at(ri, a1, t), face, inside);
    m.quad(at(ri, a0, -t), at(ro, a0, -t), at(ro, a1, -t), at(ri, a1, -t), back, inside);
    m.quad(at(ro, a0, -t), at(ro, a1, -t), at(ro, a1, t), at(ro, a0, t), rim, inside);
    m.quad(at(ri, a0, -t), at(ri, a1, -t), at(ri, a1, t), at(ri, a0, t), rim, inside);
  }
  return m.words();
}

std::vector<u32> mesh_audio() {
  MeshBuilder m;
  const float heights[5] = {0.7f, 1.3f, 1.8f, 1.1f, 1.5f};
  for (int k = 0; k < 5; ++k) {
    const double u = k / 4.0;
    const float h = heights[k];
    m.box({-0.8f + 0.4f * float(k), -0.9f + h / 2, 0}, {0.14f, h / 2, 0.14f}, argb(1.0, 0.70 - 0.55 * u, 0.20 - 0.1 * u));
  }
  return m.words();
}

std::vector<u32> mesh_chip() {
  MeshBuilder m;
  m.box({0, 0, 0}, {0.62f, 0.62f, 0.10f}, argb(0.17, 0.17, 0.20));
  m.box({0, 0, 0.13f}, {0.30f, 0.30f, 0.035f}, argb(0.85, 0.12, 0.12));
  const u32 pin = argb(0.80, 0.80, 0.85);
  for (int k = 0; k < 5; ++k) {
    const float p = -0.44f + 0.22f * float(k);
    m.box({p, 0.72f, 0}, {0.05f, 0.10f, 0.025f}, pin);
    m.box({p, -0.72f, 0}, {0.05f, 0.10f, 0.025f}, pin);
    m.box({0.72f, p, 0}, {0.10f, 0.05f, 0.025f}, pin);
    m.box({-0.72f, p, 0}, {0.10f, 0.05f, 0.025f}, pin);
  }
  return m.words();
}

std::vector<u32> mesh_grid() {
  MeshBuilder m(kMeshUnlit | kMeshVertexColors);
  constexpr float kNear = 3.0f, kFar = 36.0f, kHalf = 14.0f;
  auto color = [&](float dist) {
    const double f = std::pow(std::clamp(1.0 - (dist - kNear) / (kFar - kNear), 0.0, 1.0), 1.4);
    return argb(1.0 * f, 0.16 * f, 0.22 * f);
  };
  // Lines across the view, thicker with distance so they stay about a pixel high.
  for (float z = kNear; z <= kFar; z += kGridStep) {
    const float dz = std::max(0.04f, z * z / 420.0f);
    const u32 c = color(z);
    m.gouraud_quad({-kHalf, 0, -z - dz / 2}, {kHalf, 0, -z - dz / 2}, {kHalf, 0, -z + dz / 2}, {-kHalf, 0, -z + dz / 2},
                   c, c, c, c);
  }
  // Lines towards the horizon: one trapezoid each, about 2 px wide all along,
  // fading with distance (Spike interpolates the colours with perspective).
  for (float x = -kHalf; x <= kHalf + 0.01f; x += 1.4f) {
    const float w0 = 0.0026f * kNear, w1 = 0.0026f * kFar;
    const u32 cn = color(kNear), cf = color(kFar);
    m.gouraud_quad({x - w0, 0, -kNear}, {x + w0, 0, -kNear}, {x + w1, 0, -kFar}, {x - w1, 0, -kFar}, cn, cn, cf, cf);
  }
  return m.words();
}

LowLayout low_layout() {
  LowLayout l{};
  u32 at = map::kRomBase;
  auto place = [&](size_t bytes) {
    const u32 addr = at;
    at += u32((bytes + 15) & ~size_t(15));
    return addr;
  };
  for (int t = 0; t < kStreamThemes; ++t) l.theme[t] = place(theme_ladp(t).size);
  l.roar = place(roar_pcm().size);
  l.claws = place(claws_pcm().size);
  l.end = at;
  return l;
}

SoundBank sound_bank() {
  SoundBank bank;
  const LowLayout low = low_layout();
  bank.lion_roar = {low.roar, u32(roar_pcm().size / 2), 0};
  bank.lion_claws = {low.claws, u32(claws_pcm().size / 2), 0};
  const u32 base = map::kRomVectors + kSampleOffset;
  auto add = [&](const std::vector<double>& s, double hz) {
    SampleRef r{base + u32(bank.pcm.size()), u32(s.size()), hz};
    for (double v : s) {
      const s16 x = s16(std::clamp(std::lround(v), -32768l, 32767l));
      bank.pcm.push_back(u8(u16(x) >> 8));
      bank.pcm.push_back(u8(x));
    }
    while (bank.pcm.size() % 4) bank.pcm.push_back(0);
    return r;
  };
  bank.bell = add(bell(440.0, 1.1), 440.0);
  bank.pad = add(pad_cycle(256), double(kAudioRate) / 256.0);
  bank.roar = add(roar(1.5), 0);
  bank.tick = add(blip({{1600, 0.045}}, 0.012), 0);
  bank.ok = add(blip({{880, 0.06}, {1320, 0.14}}, 0.05), 0);
  bank.back = add(blip({{660, 0.06}, {440, 0.11}}, 0.04), 0);
  bank.claw = add(claw(0.34), 0);
  bank.pluck = add(pluck(220.0, 0.8), 220.0);
  bank.kick = add(kick(0.3), 0);
  bank.hat = add(hat(0.06), 0);
  return bank;
}

std::vector<u32> sfx_record(const SampleRef& s, double pitch, u32 left, u32 right) {
  return {s.addr, s.length, u32(std::lround(pitch * 65536)), left << 16 | right};
}

std::vector<u8> jaguar_texture() {
  constexpr int kW = 256, kH = 64;
  std::vector<double> r(kW * kH), g(kW * kH), b(kW * kH);
  // Brushed metal with a bevelled frame.
  u32 lcg = 0x7A6A'0001;
  for (int y = 0; y < kH; ++y) {
    lcg = lcg * 1664525u + 1013904223u;
    const double streak = double(lcg >> 24) / 255.0 * 0.06;
    for (int x = 0; x < kW; ++x) {
      double v = 0.20 + 0.10 * (1 - double(y) / kH) + streak;
      if (y < 3 || x < 3) v += 0.30;
      if (y >= kH - 3 || x >= kW - 3) v -= 0.12;
      r[size_t(y * kW + x)] = g[size_t(y * kW + x)] = v;
      b[size_t(y * kW + x)] = v * 1.08;
    }
  }
  // Italic block letters, 6x6 per font pixel, sheared to the right.
  const char* word = "JAGUAR";
  auto letter_px = [&](int px, int py, double shear, bool shadow) {
    for (int k = 0; k < 6; ++k) {
      const int x = 20 + px + int(shear * (42 - (py + k))) + (shadow ? 3 : 0), y = 11 + py + k + (shadow ? 3 : 0);
      for (int j = 0; j < 6; ++j) {
        const int xx = x + j;
        if (xx < 0 || xx >= kW || y < 0 || y >= kH) continue;
        const size_t i = size_t(y * kW + xx);
        if (shadow) {
          r[i] *= 0.35, g[i] *= 0.35, b[i] *= 0.35;
        } else {
          const double t = double(py + k) / 42.0;  // red at the top, orange at the bottom
          r[i] = 0.95, g[i] = 0.10 + 0.45 * t, b[i] = 0.05;
        }
      }
    }
  };
  for (int pass = 0; pass < 2; ++pass)
    for (int c = 0; c < 6; ++c) {
      const Glyph* gl = nullptr;
      for (const Glyph& k : kGlyphs)
        if (k.c == word[c]) gl = &k;
      for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
          if (gl->rows[row] >> (4 - col) & 1) letter_px(c * 36 + col * 6, row * 6, 0.22, pass == 0);
    }
  std::vector<u8> out;
  for (size_t i = 0; i < r.size(); ++i) {
    auto q = [](double v, int bits) { return u32(std::clamp(v, 0.0, 1.0) * ((1 << bits) - 1) + 0.5); };
    const u16 p = u16(q(r[i], 5) << 11 | q(g[i], 6) << 5 | q(b[i], 5));
    out.push_back(u8(p >> 8));
    out.push_back(u8(p));
  }
  return out;
}

std::vector<u32> mesh_crown() {
  MeshBuilder m;
  const u32 gold = argb(0.98, 0.74, 0.22), ruby = argb(0.90, 0.08, 0.16);
  constexpr int kSeg = 16;
  const float r = 0.64f, t = 0.05f, y0 = 1.24f, y1 = 1.52f;
  auto at = [](float rad, float ang, float y) { return V3{rad * std::sin(ang), y, rad * std::cos(ang)}; };
  // Band: a short thick-walled tube.
  for (int k = 0; k < kSeg; ++k) {
    const float a0 = float(2 * kPi * k / kSeg), a1 = float(2 * kPi * (k + 1) / kSeg), am = (a0 + a1) / 2;
    const V3 inside = at(r, am, (y0 + y1) / 2);
    m.quad(at(r + t, a0, y0), at(r + t, a1, y0), at(r + t, a1, y1), at(r + t, a0, y1), gold, inside);
    m.quad(at(r - t, a0, y0), at(r - t, a1, y0), at(r - t, a1, y1), at(r - t, a0, y1), gold, inside);
    m.quad(at(r - t, a0, y1), at(r + t, a0, y1), at(r + t, a1, y1), at(r - t, a1, y1), gold, inside);
    m.quad(at(r - t, a0, y0), at(r + t, a0, y0), at(r + t, a1, y0), at(r - t, a1, y0), gold, inside);
  }
  // Eight points, tall and short, with rubies on the tall ones and on the band.
  for (int k = 0; k < 8; ++k) {
    const float a = float(2 * kPi * k / 8), w = 0.20f, h = k % 2 ? 0.42f : 0.70f;
    const V3 b1 = at(r + t, a - w, y1), b2 = at(r + t, a + w, y1), b3 = at(r - t, a + w, y1), b4 = at(r - t, a - w, y1);
    const V3 tip = at(r, a, y1 + h), inside = at(r, a, y1 + h * 0.25f);
    m.tri(b1, b2, tip, gold, inside);
    m.tri(b2, b3, tip, gold, inside);
    m.tri(b3, b4, tip, gold, inside);
    m.tri(b4, b1, tip, gold, inside);
    auto gem = [&](V3 c, float s) {
      const V3 px{s, 0, 0}, py{0, s, 0}, pz{0, 0, s};
      const V3 v[6] = {c + px, c - px, c + py, c - py, c + pz, c - pz};
      const int f[8][3] = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {0, 4, 3}, {4, 1, 3}, {1, 5, 3}, {5, 0, 3}};
      for (auto& tr : f) m.tri(v[tr[0]], v[tr[1]], v[tr[2]], ruby, c);
    };
    if (h > 0.5f) gem(at(r, a, y1 + h + 0.06f), 0.08f);
    if (k % 2 == 0) gem(at(r + t + 0.03f, a, (y0 + y1) / 2), 0.07f);
  }
  return m.words();
}

std::vector<u32> mesh_note() {
  MeshBuilder m;
  const u32 gold = argb(1.0, 0.72, 0.20);
  // Head: a tilted elliptic disc.
  constexpr int kSeg = 14;
  const float tilt = -0.45f, cx = -0.25f, cy = -0.62f;
  auto head = [&](float ang, float z) {
    const float x = 0.36f * std::cos(ang), y = 0.24f * std::sin(ang);
    return V3{cx + x * std::cos(tilt) - y * std::sin(tilt), cy + x * std::sin(tilt) + y * std::cos(tilt), z};
  };
  const V3 centre{cx, cy, 0};
  for (int k = 0; k < kSeg; ++k) {
    const float a0 = float(2 * kPi * k / kSeg), a1 = float(2 * kPi * (k + 1) / kSeg);
    m.tri(V3{cx, cy, 0.12f}, head(a0, 0.12f), head(a1, 0.12f), gold, centre);
    m.tri(V3{cx, cy, -0.12f}, head(a0, -0.12f), head(a1, -0.12f), gold, centre);
    m.quad(head(a0, -0.12f), head(a1, -0.12f), head(a1, 0.12f), head(a0, 0.12f), gold, centre);
  }
  // Stem and flag.
  m.box({0.07f, 0.25f, 0}, {0.05f, 0.85f, 0.06f}, gold);
  std::vector<V3> flag;
  for (int k = 0; k <= 4; ++k) {
    const float u = float(k) / 4;
    flag.push_back({0.12f + 0.42f * u, 1.08f - 0.75f * u * u - 0.2f * u, 0});
  }
  m.sweep(flag, [&](size_t i) {
    const V3 p = flag[i];
    return std::array<V3, 4>{V3{p.x, p.y - 0.09f, -0.05f}, V3{p.x, p.y + 0.09f, -0.05f}, V3{p.x, p.y + 0.09f, 0.05f},
                             V3{p.x, p.y - 0.09f, 0.05f}};
  }, gold, true);
  return m.words();
}

std::vector<u32> mesh_mixer() {
  MeshBuilder m;
  m.box({0, 0, 0}, {0.85f, 0.75f, 0.10f}, argb(0.20, 0.20, 0.24));
  const float level[3] = {0.30f, -0.25f, 0.10f};
  const u32 cap[3] = {argb(0.95, 0.15, 0.12), argb(1.0, 0.65, 0.18), argb(0.92, 0.90, 0.86)};
  for (int k = 0; k < 3; ++k) {
    const float x = -0.5f + 0.5f * float(k);
    m.box({x, 0, 0.12f}, {0.04f, 0.62f, 0.02f}, argb(0.05, 0.05, 0.06));  // track
    m.box({x, level[k], 0.20f}, {0.16f, 0.09f, 0.08f}, cap[k]);           // knob
  }
  return m.words();
}

std::vector<float> intro_table() {
  std::vector<float> t(kIntroFrames * kIntroColumns, 0.0f);
  for (u32 f = 0; f < kIntroFrames; ++f) {
    float* row = &t[f * kIntroColumns];
    const double fr = f;
    double e = ramp(fr, 132, 205);
    ease(e);
    // Two decelerating turns, then a slow sway.
    const double sway = fr > 205 ? 3.0 * std::sin((fr - 205) / 40.0) * smooth(fr, 205, 245) : 0.0;
    row[0] = float(512.0 * (1 - e) + sway);
    row[1] = float(10.0 * (1 - e));
    row[2] = float(0.05 + 0.95 * e);
    row[3] = 0.0f;
    row[4] = 0.42f;
    row[5] = -7.0f;
    row[6] = float(ramp(fr, 130, 150));
    const double pulse = fr > 215 ? 0.08 * std::sin((fr - 215) / 9.0) : 0.0;
    row[7] = float(0.95 * smooth(fr, 132, 190) - 0.25 * smooth(fr, 190, 220) + pulse);
    row[8] = float(smooth(fr, 214, 240));
    row[9] = float(smooth(fr, 224, 250));
    row[10] = float(0.75 * smooth(fr, 262, 290));
    row[11] = fr >= 128 && fr < 146 ? float(0.65 * std::pow(1 - (fr - 128) / 18.0, 2)) : 0.0f;
    row[12] = float(1.0 - ramp(fr, 0, 12) + ramp(fr, 326, 356));
    // The crown drops onto the mane and settles with a small bounce.
    const double c = ramp(fr, 196, 216);
    const double bounce = fr > 216 ? 0.10 * std::exp(-(fr - 216) / 7.0) * std::fabs(std::sin((fr - 216) / 3.0)) : 0.0;
    row[13] = float(7.0 * (1 - c) * (1 - c) + bounce);
    row[14] = float(smooth(fr, 238, 262));
    row[15] = float(0.8 * smooth(fr, 18, 40) * (1 - smooth(fr, 70, 112)) + 0.5 * smooth(fr, 112, 124) * (1 - smooth(fr, 128, 150)));
  }
  return t;
}

namespace {
// Plate geometry on screen and the three cuts (x at the plate's centre line, slanted).
constexpr float kPlateX0 = 128, kPlateY0 = 152, kPlateX1 = 512, kPlateY1 = 248, kPlateCy = 200, kSlant = 0.42f;
constexpr float kCutX[3] = {224, 320, 416};
float cut_x(int k, float y) { return kCutX[k] - kSlant * (y - kPlateCy); }
}  // namespace

std::vector<u32> plate_table() {
  std::vector<u32> out;
  for (u32 f = kPlateFirst; f < kPlateLast; ++f) {
    const double fr = f;
    // The roar shakes the plate.
    const double shake = 5.0 * smooth(fr, 22, 34) * (1 - smooth(fr, 70, 100));
    const float sx = float(shake * std::sin(fr * 1.7)), sy = float(shake * 0.6 * std::cos(fr * 2.3));
    for (u32 s = 0; s < kPlateStrips; ++s) {
      // Corners at rest: TL, TR, BR, BL.
      float x[4], y[4] = {kPlateY0, kPlateY0, kPlateY1, kPlateY1};
      x[0] = s == 0 ? kPlateX0 : cut_x(int(s) - 1, kPlateY0);
      x[1] = s == 3 ? kPlateX1 : cut_x(int(s), kPlateY0);
      x[2] = s == 3 ? kPlateX1 : cut_x(int(s), kPlateY1);
      x[3] = s == 0 ? kPlateX0 : cut_x(int(s) - 1, kPlateY1);
      float u[4], v[4];
      for (int k = 0; k < 4; ++k) u[k] = (x[k] - kPlateX0) / (kPlateX1 - kPlateX0), v[k] = (y[k] - kPlateY0) / (kPlateY1 - kPlateY0);
      // Torn apart: pieces fly outwards, spin and fall.
      float ox = sx, oy = sy, rot = 0, alpha = float(ramp(fr, kPlateFirst, kPlateFirst + 10));
      if (f >= kTearFrame) {
        const float n = float(f - kTearFrame), side = float(s) - 1.5f;
        ox += side * 2.4f * n;
        oy += -1.6f * n + 0.11f * n * n;
        rot = side * 0.014f * n;
        alpha *= float(1 - smooth(fr, 140, 176));
      }
      const float cx = (x[0] + x[1] + x[2] + x[3]) / 4, cy = (y[0] + y[1] + y[2] + y[3]) / 4;
      out.push_back(u32(std::lround(alpha * 255)) << 24 | 0xFFFFFF);
      for (int k = 0; k < 4; ++k) {
        const float dx = x[k] - cx, dy = y[k] - cy;
        out.push_back(f32_to_bits(cx + ox + dx * std::cos(rot) - dy * std::sin(rot)));
        out.push_back(f32_to_bits(cy + oy + dx * std::sin(rot) + dy * std::cos(rot)));
        out.push_back(f32_to_bits(u[k]));
        out.push_back(f32_to_bits(v[k]));
      }
    }
  }
  return out;
}

std::vector<u32> slash_table() {
  std::vector<u32> out;
  for (u32 f = kSlashFirst; f < kSlashLast; ++f) {
    for (u32 k = 0; k < kSlashes; ++k) {
      const double fr = f, s0 = kSlashFrame[k];
      // The streak runs from above the plate to below it in five frames, then fades.
      const double head = ramp(fr, s0, s0 + 5), tail = 0.25 * ramp(fr, s0 + 3, s0 + 12);
      const double glow = (1 - smooth(fr, s0 + 6, s0 + 32));
      const bool visible = fr >= s0 && glow > 0.0;
      const float ya = 128, yb = 272;
      auto point = [&](double t) {
        const float y = float(ya + (yb - ya) * t);
        return std::pair{cut_x(int(k), y), y};
      };
      const auto [hx, hy] = point(head);
      const auto [tx, ty] = point(tail);
      // Perpendicular to the cut for the width.
      const float len = std::sqrt(1 + kSlant * kSlant), nx = 1 / len, ny = kSlant / len, w = 6.5f;
      auto col = [&](double r, double g, double b) { return argb(r * glow, g * glow, b * glow); };
      const u32 hot = col(1.0, 0.95, 0.85), cool = col(1.0, 0.35, 0.10);
      out.push_back(visible ? 1 : 0);
      const float px[4] = {tx - nx * w, tx + nx * w, hx + nx * w, hx - nx * w};
      const float py[4] = {ty - ny * w, ty + ny * w, hy + ny * w, hy - ny * w};
      const u32 pc[4] = {cool, cool, hot, hot};
      for (int c = 0; c < 4; ++c) {
        out.push_back(f32_to_bits(px[c]));
        out.push_back(f32_to_bits(py[c]));
        out.push_back(pc[c]);
      }
    }
  }
  return out;
}
}  // namespace lion::rom
