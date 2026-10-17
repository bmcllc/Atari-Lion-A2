#include "rom/assets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

#include "bus.h"

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

SoundBank sound_bank() {
  SoundBank bank;
  const u32 base = map::kRomBase + kSampleOffset;
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
  return bank;
}

std::vector<u32> sfx_record(const SampleRef& s, double pitch, u32 left, u32 right) {
  return {s.addr, s.length, u32(std::lround(pitch * 65536)), left << 16 | right};
}

std::vector<u32> jingle_events(const SoundBank& bank) {
  struct Ev {
    u32 frame, offset, value;
  };
  std::vector<Ev> ev;
  auto reg = [](int ch, int r) { return u32(0x100 + 32 * ch + 4 * r); };
  enum { kStart, kLength, kLoop, kPitch, kVolume, kCtrl };
  auto vol = [](double l, double r) { return u32(std::lround(l * 256)) << 16 | u32(std::lround(r * 256)); };
  auto pitch = [](double ratio) { return u32(std::lround(ratio * 65536)); };
  auto voice = [&](u32 frame, int ch, const SampleRef& s, double ratio, double l, double r, bool loop) {
    ev.push_back({frame, reg(ch, kStart), s.addr});
    ev.push_back({frame, reg(ch, kLength), s.length});
    ev.push_back({frame, reg(ch, kLoop), loop ? 0 : s.length});
    ev.push_back({frame, reg(ch, kPitch), pitch(ratio)});
    ev.push_back({frame, reg(ch, kVolume), vol(l, r)});
    ev.push_back({frame, reg(ch, kCtrl), loop ? 3u : 1u});
  };
  // Volume envelope: linear segments sampled every other frame.
  auto envelope = [&](int ch, std::initializer_list<std::pair<u32, double>> points, double pan) {
    auto p = points.begin();
    for (auto q = std::next(p); q != points.end(); ++p, ++q)
      for (u32 f = p->first; f < q->first; f += 2) {
        const double v = p->second + (q->second - p->second) * double(f - p->first) / double(q->first - p->first);
        ev.push_back({f, reg(ch, kVolume), vol(v * (1 - pan), v * (1 + pan))});
      }
    ev.push_back({p->first, reg(ch, kVolume), vol(p->second * (1 - pan), p->second * (1 + pan))});
  };

  ev.push_back({0, 0, 0x100});  // master volume
  for (int ch = 0; ch < 16; ++ch) ev.push_back({0, reg(ch, kCtrl), 0});

  // The roar, doubled an octave down.
  voice(3, 0, bank.roar, 1.0, 0.70, 0.70, false);
  voice(5, 1, bank.roar, 0.72, 0.42, 0.30, false);
  // Sub drone on A1 under the whole intro.
  voice(3, 2, bank.pad, 55.0 / bank.pad.base_hz, 0, 0, true);
  envelope(2, {{3, 0.0}, {70, 0.42}, {170, 0.42}, {232, 0.0}}, 0.0);
  ev.push_back({234, reg(2, kCtrl), 0});

  // Frame 98: the hit, timed with the flash. A2 and A5 bells under an A major arpeggio.
  voice(98, 3, bank.bell, 0.25, 0.55, 0.55, false);
  voice(98, 4, bank.bell, 2.0, 0.14, 0.18, false);
  const double arp[5] = {220.0, 329.63, 440.0, 554.37, 659.26};
  const double pans[5][2] = {{0.50, 0.30}, {0.30, 0.50}, {0.42, 0.42}, {0.50, 0.22}, {0.22, 0.50}};
  for (int k = 0; k < 5; ++k) voice(98 + 6 * u32(k), 5 + k, bank.bell, arp[k] / 440.0, pans[k][0], pans[k][1], false);
  // Warm pad chord, slightly detuned against itself.
  const double chord[6] = {110.0, 164.81, 220.0, 277.18, 329.63, 440.0};
  for (int k = 0; k < 6; ++k) {
    const int ch = 10 + k;
    voice(98, ch, bank.pad, chord[k] / bank.pad.base_hz * (k % 2 ? 1.003 : 0.997), 0, 0, true);
    envelope(ch, {{98, 0.0}, {130, 0.14}, {185, 0.14}, {236, 0.0}}, k % 2 ? 0.35 : -0.35);
    ev.push_back({238, reg(ch, kCtrl), 0});
  }

  std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
  std::vector<u32> out;
  for (const Ev& e : ev) out.insert(out.end(), {e.frame, e.offset, e.value});
  out.insert(out.end(), {0xFFFF'FFFFu, 0, 0});
  return out;
}

std::vector<float> intro_table() {
  std::vector<float> t(kIntroFrames * kIntroColumns, 0.0f);
  for (u32 f = 0; f < kIntroFrames; ++f) {
    float* row = &t[f * kIntroColumns];
    const double fr = f;
    double e = ramp(fr, 8, 100);
    ease(e);
    // Two decelerating turns, then a slow sway.
    const double sway = fr > 100 ? 3.0 * std::sin((fr - 100) / 40.0) * smooth(fr, 100, 140) : 0.0;
    row[0] = float(512.0 * (1 - e) + sway);
    row[1] = float(10.0 * (1 - e));
    row[2] = float(0.05 + 0.95 * e);
    row[3] = 0.0f;
    row[4] = 0.55f;
    row[5] = -4.6f;
    row[6] = float(ramp(fr, 8, 40));
    const double pulse = fr > 110 ? 0.08 * std::sin((fr - 110) / 9.0) : 0.0;
    row[7] = float(0.95 * smooth(fr, 40, 100) - 0.25 * smooth(fr, 100, 130) + pulse);
    row[8] = float(smooth(fr, 104, 134));
    row[9] = float(smooth(fr, 118, 148));
    row[10] = float(0.75 * smooth(fr, 145, 175));
    row[11] = fr >= 98 && fr < 116 ? float(0.55 * std::pow(1 - (fr - 98) / 18.0, 2)) : 0.0f;
    row[12] = float(1.0 - ramp(fr, 0, 10) + ramp(fr, 206, 236));
  }
  return t;
}

}  // namespace lion::rom
