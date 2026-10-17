#pragma once
// Minimal ISO9660 (level 1, single root directory) image writer.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace iso9660 {

constexpr uint32_t kSector = 2048;
constexpr uint32_t kFirstFileLba = 21;  // 16 PVD, 17 terminator, 18/19 path tables, 20 root

struct File {
  std::string name;  // 8.3-style, upper case, e.g. "BOOT.LION" (written as "BOOT.LION;1")
  std::vector<uint8_t> data;
};

inline uint32_t sectors(size_t bytes) { return uint32_t((bytes + kSector - 1) / kSector); }

// LBA each file will get, in order.
inline std::vector<uint32_t> layout(const std::vector<File>& files) {
  std::vector<uint32_t> lba;
  uint32_t next = kFirstFileLba;
  for (const File& f : files) {
    lba.push_back(next);
    next += std::max<uint32_t>(1, sectors(f.data.size()));
  }
  return lba;
}

namespace detail {
inline void le16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
inline void be16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
inline void le32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
inline void be32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
inline void both16(uint8_t* p, uint32_t v) { le16(p, v), be16(p + 2, v); }
inline void both32(uint8_t* p, uint32_t v) { le32(p, v), be32(p + 4, v); }
inline void text(uint8_t* p, size_t n, const std::string& s) {
  std::memset(p, ' ', n);
  std::memcpy(p, s.data(), std::min(n, s.size()));
}
// Directory record; returns its length.
inline uint32_t record(uint8_t* p, uint32_t lba, uint32_t size, bool dir, const std::string& name) {
  const uint32_t len = 33 + uint32_t(name.size()) + ((name.size() & 1) ? 0 : 1);
  std::memset(p, 0, len);
  p[0] = uint8_t(len);
  both32(p + 2, lba);
  both32(p + 10, size);
  p[18] = 99, p[19] = 1, p[20] = 1;  // 1999-01-01 00:00:00
  p[25] = dir ? 2 : 0;
  both16(p + 28, 1);
  p[32] = uint8_t(name.size());
  std::memcpy(p + 33, name.data(), name.size());
  return len;
}
}  // namespace detail

inline std::vector<uint8_t> build(const std::string& volume, const std::vector<File>& files) {
  using namespace detail;
  const auto lba = layout(files);
  uint32_t total = kFirstFileLba;
  for (const File& f : files) total += std::max<uint32_t>(1, sectors(f.data.size()));
  std::vector<uint8_t> img(size_t(total) * kSector, 0);
  auto sec = [&](uint32_t n) { return &img[size_t(n) * kSector]; };

  // Primary volume descriptor.
  uint8_t* pvd = sec(16);
  pvd[0] = 1;
  std::memcpy(pvd + 1, "CD001", 5);
  pvd[6] = 1;
  text(pvd + 8, 32, "ATARI LION");
  text(pvd + 40, 32, volume);
  both32(pvd + 80, total);
  both16(pvd + 120, 1);
  both16(pvd + 124, 1);
  both16(pvd + 128, kSector);
  both32(pvd + 132, 10);  // path table: one 10-byte entry
  le32(pvd + 140, 18);
  be32(pvd + 148, 19);
  record(pvd + 156, 20, kSector, true, std::string(1, '\0'));
  text(pvd + 190, 128, volume);
  text(pvd + 318, 128, "ATARI");
  text(pvd + 446, 128, "ATARI LION MKDEMO");
  text(pvd + 574, 128, "ATARI LION");
  for (int f = 0; f < 4; ++f) {
    uint8_t* d = pvd + 813 + f * 17;
    std::memcpy(d, f < 2 ? "1999010100000000" : "0000000000000000", 16);
    d[16] = 0;
  }
  pvd[881] = 1;

  // Terminator.
  uint8_t* term = sec(17);
  term[0] = 255;
  std::memcpy(term + 1, "CD001", 5);
  term[6] = 1;

  // Path tables (root only).
  for (int t = 0; t < 2; ++t) {
    uint8_t* pt = sec(18 + uint32_t(t));
    pt[0] = 1;
    if (t == 0) le32(pt + 2, 20), le16(pt + 6, 1);
    else be32(pt + 2, 20), be16(pt + 6, 1);
  }

  // Root directory.
  uint8_t* dir = sec(20);
  uint32_t off = record(dir, 20, kSector, true, std::string(1, '\0'));
  off += record(dir + off, 20, kSector, true, std::string(1, '\1'));
  for (size_t i = 0; i < files.size(); ++i) {
    off += record(dir + off, lba[i], uint32_t(files[i].data.size()), false, files[i].name + ";1");
    std::copy(files[i].data.begin(), files[i].data.end(), sec(lba[i]));
  }
  return img;
}

}  // namespace iso9660
