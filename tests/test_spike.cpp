#include "test.h"

using lion::Lion;
using lion::Spike;

namespace {

constexpr u32 kFb = lion::map::kSgramBase;
constexpr u32 kZ = lion::map::kSgramBase + 0x10'0000;

void push(Lion& sys, std::initializer_list<u32> words) {
  for (u32 w : words) sys.bus.write32(lion::map::kSpike, w, lion::Master::Tuffy);
}
u32 fb(float f) { return f32_to_bits(f); }
u16 pixel(Lion& sys, int x, int y) { return load_be16(sys.bus.host_ptr(kFb + u32(y) * 640 * 2 + u32(x) * 2)); }

void setup(Lion& sys, u32 mode) {
  push(sys, {Spike::kSetTarget << 24 | 5, kFb, 640 * 2, 640, 480, kZ});
  push(sys, {Spike::kClear << 24 | 3, 3, 0x0000, 0xFFFFFF});
  push(sys, {Spike::kSetMode << 24 | 1, mode});
}

void tri(Lion& sys, float z, u32 argb, std::array<float, 6> xy) {
  push(sys, {Spike::kTri << 24 | 21});
  for (int i = 0; i < 3; ++i) push(sys, {fb(xy[i * 2]), fb(xy[i * 2 + 1]), fb(z), fb(1.0f), argb, fb(0), fb(0)});
}

}  // namespace

TEST(spike_flat_triangle_and_clear) {
  Lion sys;
  setup(sys, 0);
  tri(sys, 0.5f, 0xFFFF'0000, {100, 100, 300, 100, 100, 300});
  sys.spike.flush();
  CHECK_EQ(pixel(sys, 120, 120), 0xF800);  // red
  CHECK_EQ(pixel(sys, 290, 290), 0x0000);  // outside the hypotenuse
  CHECK_EQ(pixel(sys, 10, 10), 0x0000);
  CHECK(sys.spike.pixels() > 19000 && sys.spike.pixels() < 21000);  // ~ 200*200/2
}

TEST(spike_z_test) {
  Lion sys;
  setup(sys, Spike::kZTest | Spike::kZWrite);
  tri(sys, 0.2f, 0xFF00'FF00, {0, 0, 400, 0, 0, 400});  // near, green
  tri(sys, 0.8f, 0xFF00'00FF, {0, 0, 400, 0, 0, 400});  // far, blue: hidden
  sys.spike.flush();
  CHECK_EQ(pixel(sys, 50, 50), 0x07E0);
}

TEST(spike_backface_cull) {
  Lion sys;
  setup(sys, Spike::kCullBack);
  tri(sys, 0.5f, 0xFFFF'FFFF, {100, 100, 300, 100, 100, 300});  // clockwise on screen: back face
  sys.spike.flush();
  CHECK_EQ(pixel(sys, 120, 120), 0x0000);
  CHECK_EQ(sys.spike.pixels(), 0u);
  tri(sys, 0.5f, 0xFFFF'FFFF, {100, 100, 100, 300, 300, 100});  // counter-clockwise: front face
  sys.spike.flush();
  CHECK_EQ(pixel(sys, 120, 120), 0xFFFF);
}

TEST(spike_sync_raises_interrupt) {
  Lion sys;
  push(sys, {Spike::kSync << 24 | 1, 1234});
  sys.spike.flush();
  CHECK_EQ(sys.bus.read32(lion::map::kSpike + 0x08, lion::Master::Tuffy), 1234u);
  CHECK(sys.pic.pending() & lion::Pic::kSpike);
}

TEST(spike_timing_is_charged) {
  Lion sys;
  setup(sys, 0);
  sys.spike.flush();
  tri(sys, 0.5f, 0xFFFF'FFFF, {0, 0, 640, 0, 0, 480});
  // A big triangle cannot finish in 100 cycles at 2 pixels/cycle.
  sys.spike.tick(100);
  CHECK(!sys.spike.idle());
  sys.spike.flush();
  CHECK(sys.spike.idle());
}

TEST(spike_texture_sampling) {
  Lion sys;
  // 2x2 checker texture: red, green / blue, white.
  constexpr u32 kTex = lion::map::kSgramBase + 0x40'0000;
  const u16 texels[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  for (int i = 0; i < 4; ++i) store_be16(sys.bus.host_ptr(kTex + u32(i) * 2), texels[i]);
  setup(sys, Spike::kTexture);
  push(sys, {Spike::kSetTexture << 24 | 3, kTex, 1, 1});
  // Full-screen quad as two triangles, UV 0..1, white vertex colour.
  auto vtx = [&](float x, float y, float u, float v) { push(sys, {fb(x), fb(y), fb(0.5f), fb(1), 0xFFFF'FFFF, fb(u), fb(v)}); };
  push(sys, {Spike::kTri << 24 | 21});
  vtx(0, 0, 0, 0), vtx(0, 480, 0, 1), vtx(640, 0, 1, 0);
  push(sys, {Spike::kTri << 24 | 21});
  vtx(640, 0, 1, 0), vtx(0, 480, 0, 1), vtx(640, 480, 1, 1);
  sys.spike.flush();
  CHECK_EQ(pixel(sys, 100, 100), 0xF800);
  CHECK_EQ(pixel(sys, 500, 100), 0x07E0);
  CHECK_EQ(pixel(sys, 100, 400), 0x001F);
  CHECK_EQ(pixel(sys, 500, 400), 0xFFFF);
}

TEST(spike_mipmap_level_selection) {
  // 4x4 red level 0, 2x2 green level 1, 1x1 blue level 2.
  constexpr u32 kTex = lion::map::kSgramBase + 0x40'0000;
  auto fill = [&](Lion& sys, u32 addr, u32 n, u16 c) {
    for (u32 i = 0; i < n; ++i) store_be16(sys.bus.host_ptr(addr + i * 2), c);
  };
  auto draw = [&](u32 mode, float uv_span) {
    Lion sys;
    fill(sys, kTex, 16, 0xF800);
    fill(sys, kTex + 32, 4, 0x07E0);
    fill(sys, kTex + 40, 1, 0x001F);
    setup(sys, Spike::kTexture | mode);
    push(sys, {Spike::kSetTexture << 24 | 4, kTex, 2, 2, 3});
    auto vtx = [&](float x, float y, float u, float v) { push(sys, {fb(x), fb(y), fb(0.5f), fb(1), 0xFFFF'FFFF, fb(u), fb(v)}); };
    push(sys, {Spike::kTri << 24 | 21});
    vtx(0, 0, 0, 0), vtx(0, 480, 0, uv_span * 480 / 640), vtx(640, 0, uv_span, 0);
    sys.spike.flush();
    return pixel(sys, 50, 50);
  };
  CHECK_EQ(draw(0, 160), 0xF800);                 // no mipmapping: always level 0
  CHECK_EQ(draw(Spike::kMipmap, 0.5f), 0xF800);   // magnified: level 0
  CHECK_EQ(draw(Spike::kMipmap, 320), 0x07E0);    // 2 texels per pixel: level 1
  CHECK_EQ(draw(Spike::kMipmap, 640), 0x001F);    // 4 texels per pixel: level 2
  const u16 tri = draw(Spike::kMipmap | Spike::kTrilinear, 480);  // between levels 1 and 2
  CHECK((tri & 0x07E0) != 0 && (tri & 0x001F) != 0);
}

TEST(spike_additive_blend) {
  Lion sys;
  setup(sys, 0);
  tri(sys, 0.5f, 0xFF80'0000, {0, 0, 0, 400, 400, 0});  // dark red underneath
  push(sys, {Spike::kSetMode << 24 | 1, Spike::kBlendAdd});
  tri(sys, 0.5f, 0xFF00'0080, {0, 0, 0, 400, 400, 0});  // add blue
  tri(sys, 0.5f, 0xFFC0'0000, {0, 0, 0, 400, 400, 0});  // add red: saturates
  sys.spike.flush();
  const u16 p = pixel(sys, 50, 50);
  CHECK_EQ(u32(p >> 11), 31u);     // 0x80 + 0xC0 clamps to full red
  CHECK_EQ(u32(p & 31), 0x80u >> 3);  // blue added on top
  CHECK_EQ(pixel(sys, 300, 300), 0x0000);
}
