#include <set>

#include "demo_rom.h"
#include "test.h"

using lion::Lion;

TEST(image_roundtrip) {
  lion::LionImage img = demo::build();
  lion::LionImage back;
  std::string err;
  CHECK(back.parse(img.serialize(), &err));
  CHECK_EQ(back.entry, img.entry);
  CHECK_EQ(back.segments.size(), img.segments.size());
  CHECK(back.segments[1].data == img.segments[1].data);
  std::vector<u8> junk = {'N', 'O', 'P', 'E', 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(!back.parse(junk, &err));
}

TEST(qve_projects_vertices) {
  Lion sys;
  // Identity matrix, viewport (100, 100, 10, 20).
  for (int i = 0; i < 16; ++i)
    sys.bus.write32(lion::map::kQve + 4 * u32(i), f32_to_bits(i % 5 == 0 ? 1.0f : 0.0f), lion::Master::Quacker);
  const float vp[4] = {100, 100, 10, 20};
  for (int i = 0; i < 4; ++i) sys.bus.write32(lion::map::kQve + 0x40 + 4 * u32(i), f32_to_bits(vp[i]), lion::Master::Quacker);
  u8* src = sys.bus.host_ptr(0x7000);
  const float v[4] = {1, -1, 0.5f, 2};
  for (int i = 0; i < 4; ++i) store_be32(src + 4 * i, f32_to_bits(v[i]));
  sys.bus.write32(lion::map::kQve + 0x60, 0x7000, lion::Master::Quacker);
  sys.bus.write32(lion::map::kQve + 0x64, 0x7100, lion::Master::Quacker);
  sys.bus.write32(lion::map::kQve + 0x68, 1, lion::Master::Quacker);
  sys.bus.write32(lion::map::kQve + 0x74, lion::Qve::kXformProject, lion::Master::Quacker);
  CHECK(sys.qve.busy());
  const u8* out = sys.bus.host_ptr(0x7100);
  CHECK(bits_to_f32(load_be32(out)) == 60.0f);       // 1/2 * 100 + 10
  CHECK(bits_to_f32(load_be32(out + 4)) == -30.0f);  // -1/2 * 100 + 20
  CHECK(bits_to_f32(load_be32(out + 8)) == 0.25f);
  CHECK(bits_to_f32(load_be32(out + 12)) == 0.5f);
  sys.qve.tick(lion::Qve::kCyclesPerVector);
  CHECK(!sys.qve.busy());
}

TEST(demo_renders_cube) {
  Lion sys;
  sys.boot(demo::build());
  for (int i = 0; i < 5; ++i) sys.run_frame();
  CHECK(sys.quacker.running);
  CHECK_EQ(sys.tuffy.illegal_count, 0u);
  CHECK_EQ(sys.quacker.illegal_count, 0u);
  CHECK(sys.vi.frame() >= 5);
  CHECK(sys.spike.triangles() >= 12 * 3);
  CHECK(sys.qve.vectors_processed() >= 8 * 3);
  // Tuffy spends most of the frame dozing.
  CHECK(sys.tuffy.instructions < sys.tuffy.total_cycles / 10);

  std::vector<u32> px;
  CHECK(sys.vi.scanout(px));
  CHECK_EQ(px.size(), size_t(640 * 480));
  // The cube covers the screen centre; the corner shows the background.
  CHECK(px[240 * 640 + 320] != px[5 * 640 + 5]);
  // The frame advanced: two different rotations give different images.
  std::vector<u32> px2;
  for (int i = 0; i < 3; ++i) sys.run_frame();
  sys.vi.scanout(px2);
  CHECK(px != px2);
}

TEST(demo_disc_textured_cube) {
  Lion sys;
  std::string err;
  CHECK(sys.boot_disc(demo::build_disc(), &err));
  // Boot ROM load, then the game's own texture read: give it some frames.
  for (int i = 0; i < 60; ++i) sys.run_frame();
  CHECK_EQ(sys.tuffy.illegal_count, 0u);
  CHECK_EQ(sys.quacker.illegal_count, 0u);
  // The texture arrived in SGRAM and matches TEX.BIN.
  const auto tex = demo::make_texture();
  CHECK(std::equal(tex.begin(), tex.end(), sys.bus.host_ptr(demo::kTexture)));
  CHECK_EQ(sys.tuffy.gpr[19], demo::kTexturedMode);
  // The checker gives the cube many distinct colours.
  std::vector<u32> px;
  CHECK(sys.vi.scanout(px));
  std::set<u32> colors(px.begin() + 200 * 640, px.begin() + 280 * 640);
  CHECK(colors.size() > 50);
}

TEST(bus_contention_slows_masters) {
  // Tuffy streams through 4 MB (every access misses); optionally Quacker does the same elsewhere.
  auto run = [](bool with_quacker) {
    Lion sys;
    ppcasm::Asm t(0x2000);
    t.li32(3, 0x0040'0000);
    t.li32(4, (4u << 20) / 32);
    t << ppcasm::mtctr(4);
    t.label("loop");
    t << ppcasm::lwz(5, 0, 3) << ppcasm::addi(3, 3, 32);
    t.bdnz("loop");
    t.label("halt");
    t.b("halt");
    auto tc = t.finish();
    std::copy(tc.begin(), tc.end(), sys.bus.host_ptr(0x2000));
    sys.tuffy.reset(0x2000, Lion::kBootMsr);
    if (with_quacker) {
      using namespace m88kasm;
      Asm q(0x3000);
      q.label("start");
      q.li32(2, 0x0090'0000);
      q.label("loop");
      q << ld(4, 2, 0) << addu_i(2, 2, 32);
      q.br("loop");
      auto qc = q.finish();
      std::copy(qc.begin(), qc.end(), sys.bus.host_ptr(0x3000));
      sys.quacker.reset(0x3000);
      sys.quacker.running = true;
    }
    sys.run_frame();
    return std::pair{sys.tuffy.instructions, sys.bus.contention[0]};
  };
  auto [solo, solo_wait] = run(false);
  auto [shared, shared_wait] = run(true);
  CHECK_EQ(solo_wait, 0u);
  CHECK(shared_wait > 0);
  CHECK(shared < solo * 9 / 10);  // at least 10% slower with a competing master
}

namespace {
// Cycles Tuffy needs for `body` repeated 1000 times (CTR loop).
u64 tuffy_cycles(const std::vector<u32>& body) {
  Lion sys;
  ppcasm::Asm a(0x2000);
  a.li32(9, 1000);
  a << ppcasm::mtctr(9);
  a.label("loop");
  for (u32 w : body) a << w;
  a.bdnz("loop");
  a.label("halt");
  a.b("halt");
  auto code = a.finish();
  std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x2000));
  sys.tuffy.reset(0x2000, Lion::kBootMsr);
  u64 cycles = 0;
  while (sys.tuffy.pc != a.addr("halt") && cycles < 10'000'000) cycles += sys.tuffy.run(1);
  return cycles;
}
}  // namespace

TEST(tuffy_dual_dispatch) {
  using namespace ppcasm;
  // Eight independent adds pair up; a dependent chain cannot.
  const u64 independent = tuffy_cycles({addi(3, 3, 1), addi(4, 4, 1), addi(5, 5, 1), addi(6, 6, 1),
                                        addi(7, 7, 1), addi(8, 8, 1), addi(10, 10, 1), addi(11, 11, 1)});
  const u64 dependent = tuffy_cycles({addi(3, 3, 1), addi(3, 3, 1), addi(3, 3, 1), addi(3, 3, 1),
                                      addi(3, 3, 1), addi(3, 3, 1), addi(3, 3, 1), addi(3, 3, 1)});
  CHECK(independent < dependent * 7 / 10);
  // An integer op pairs with a load (different units) but two loads do not pair.
  const u64 mixed = tuffy_cycles({lwz(3, 0, 1), addi(4, 4, 1), lwz(5, 4, 1), addi(6, 6, 1)});
  const u64 loads = tuffy_cycles({lwz(3, 0, 1), lwz(4, 4, 1), lwz(5, 8, 1), lwz(6, 12, 1)});
  CHECK(mixed < loads);
}

TEST(tuffy_branch_prediction) {
  using namespace ppcasm;
  Lion sys;
  Asm a(0x2000);
  // A loop branch is learnt; an alternating branch keeps missing with 2-bit counters... mostly.
  a.li32(9, 1000);
  a << mtctr(9) << li(3, 0);
  a.label("loop");
  a << xori(3, 3, 1) << cmpwi(0, 3, 0);
  a.beq("skip");
  a << nop();
  a.label("skip");
  a.bdnz("loop");
  a.label("halt");
  a.b("halt");
  auto code = a.finish();
  std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x2000));
  sys.tuffy.reset(0x2000, Lion::kBootMsr);
  for (int i = 0; i < 100000 && sys.tuffy.pc != a.addr("halt"); ++i) sys.tuffy.run(1);
  CHECK(sys.tuffy.bht.hits > 1000);   // the loop-closing bdnz is predicted
  CHECK(sys.tuffy.bht.misses > 300);  // the alternating beq is not
  CHECK(sys.tuffy.stall_cycles >= sys.tuffy.bht.misses * lion::Tuffy::kMispredictPenalty);
}

TEST(quacker_dual_issue_and_tic) {
  using namespace m88kasm;
  auto run = [](const std::vector<u32>& body) {
    Lion sys;
    Asm a(0x4000);
    a.label("start");
    a << or_i(9, 0, 1000);
    a.label("loop");
    for (u32 w : body) a << w;
    a << subu_i(9, 9, 1);
    a.bcnd(kNe0, 9, "loop");
    a.label("halt");
    a.br("halt");
    auto code = a.finish();
    std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x4000));
    sys.quacker.reset(0x4000);
    sys.quacker.running = true;
    u64 cycles = 0;
    while (sys.quacker.pc != a.addr("halt") && cycles < 1'000'000) cycles += sys.quacker.run(1);
    return std::pair{cycles, sys.quacker.tic_hits};
  };
  auto [indep, tic] = run({addu_i(2, 2, 1), addu_i(3, 3, 1), addu_i(4, 4, 1), addu_i(5, 5, 1)});
  auto [dep, unused] = run({addu_i(2, 2, 1), addu_i(2, 2, 1), addu_i(2, 2, 1), addu_i(2, 2, 1)});
  CHECK(indep < dep * 8 / 10);
  CHECK(tic >= 990);  // the loop branch hits the target instruction cache
}

TEST(threaded_rasterizer_is_deterministic) {
  auto render = [](bool threads) {
    Lion sys;
    sys.spike.set_threads(threads);
    std::string err;
    sys.boot_disc(demo::build_disc(), &err);
    for (int i = 0; i < 90; ++i) sys.run_frame();
    std::vector<u32> px;
    sys.vi.scanout(px);
    return px;
  };
  const auto a = render(true), b = render(false);
  CHECK(!a.empty());
  CHECK(a == b);
}
