#pragma once

#include "bus.h"
#include "fifo.h"
#include "pic.h"
#include "spike/raster_pool.h"

#include <memory>

namespace lion {

// Spike ASIC: command processor + 2-pipeline rasterizer (software model).
class Spike : public MmioDevice {
 public:
  enum Cmd : u32 { kNop = 0, kSetTarget = 1, kClear = 2, kSetMode = 3, kSetTexture = 4, kTri = 5, kSync = 6 };
  enum ModeBits : u32 {
    kZTest = 1u << 0,
    kZWrite = 1u << 1,
    kGouraud = 1u << 2,
    kTexture = 1u << 3,
    kBilinear = 1u << 4,
    kPerspective = 1u << 5,
    kBlend = 1u << 6,
    kCullBack = 1u << 7,
    kMipmap = 1u << 8,     // per-pixel level of detail
    kTrilinear = 1u << 9,  // blend the two nearest levels
  };
  static constexpr size_t kFifoWords = 4096;
  static constexpr u32 kPixelsPerCycle = 2;
  static constexpr u32 kTriSetupCycles = 40;
  static constexpr u32 kVertexWords = 7;

  struct Vertex {
    float x, y, z, inv_w;
    u32 argb;
    float u, v;
  };

  static constexpr u64 kParallelArea = 16 * 1024;  // bounding boxes worth splitting

  Spike(Bus& bus, Pic& pic) : bus_(bus), pic_(pic), fifo_(kFifoWords) {
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw > 1) pool_ = std::make_unique<RasterPool>(std::min(hw - 1, 7u));
  }
  // Rasterize on the calling thread only (identical output; for debugging).
  void set_threads(bool enabled) {
    if (!enabled) pool_.reset();
  }

  // Advances the command processor by `cycles` Spike cycles.
  void tick(u64 cycles);
  // Runs every queued command regardless of timing (tests, end of frame).
  void flush() { tick(~0ull >> 1); }

  bool idle() const { return fifo_.empty() && debt_ == 0; }
  u64 pixels() const { return pixels_; }
  u64 triangles() const { return triangles_; }

  // Direct entry point used by the FIFO parser and by tests.
  void draw_triangle(const Vertex v[3]);

  u32 read32(u32 offset, Master who, bool* stall) override;
  void write32(u32 offset, u32 value, Master who, bool* stall) override;

 private:
  bool execute_next(u64* cost);
  void clear(u32 flags, u32 color, u32 z);
  u32 sample(u32 level, float u, float v) const;  // returns XRGB8888
  u32 sample_lod(float u, float v, float lod) const;

  Bus& bus_;
  Pic& pic_;
  WordFifo fifo_;
  u64 debt_ = 0;  // cycles still owed by the command in progress

  // Render state.
  u32 color_addr_ = 0, stride_ = 0, width_ = 0, height_ = 0, z_addr_ = 0;
  u32 mode_ = 0;
  u32 tex_addr_ = 0, tex_w_log2_ = 0, tex_h_log2_ = 0, tex_levels_ = 1;
  // Per-triangle cache of the mip chain: host pointer and size of each level.
  struct Level {
    const u8* ptr;
    u32 w, h;
  };
  Level levels_[11] = {};
  void resolve_levels();
  u32 sync_ = 0;
  u64 pixels_ = 0, triangles_ = 0;
  std::unique_ptr<RasterPool> pool_;
};

}  // namespace lion
