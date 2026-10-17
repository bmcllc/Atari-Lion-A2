#include "loader.h"

#include <fstream>
#include <iterator>

namespace lion {

bool LionImage::parse(const std::vector<u8>& file, std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    return false;
  };
  if (file.size() < 16) return fail("file too small");
  if (load_be32(&file[0]) != kMagic) return fail("bad magic (expected \"LION\")");
  if (load_be32(&file[4]) != kVersion) return fail("unsupported version");
  entry = load_be32(&file[8]);
  const u32 count = load_be32(&file[12]);
  if (count > 256 || 16 + size_t(count) * 12 > file.size()) return fail("bad segment table");
  segments.clear();
  for (u32 i = 0; i < count; ++i) {
    const u8* s = &file[16 + i * 12];
    const u32 addr = load_be32(s), size = load_be32(s + 4), offset = load_be32(s + 8);
    if (size_t(offset) + size > file.size()) return fail("segment outside file");
    segments.push_back({addr, std::vector<u8>(file.begin() + offset, file.begin() + offset + size)});
  }
  return true;
}

std::vector<u8> LionImage::serialize() const {
  std::vector<u8> out(16 + segments.size() * 12);
  store_be32(&out[0], kMagic);
  store_be32(&out[4], kVersion);
  store_be32(&out[8], entry);
  store_be32(&out[12], u32(segments.size()));
  for (size_t i = 0; i < segments.size(); ++i) {
    u8* s = &out[16 + i * 12];
    store_be32(s, segments[i].addr);
    store_be32(s + 4, u32(segments[i].data.size()));
    store_be32(s + 8, u32(out.size()));
    out.insert(out.end(), segments[i].data.begin(), segments[i].data.end());
  }
  return out;
}

bool read_file(const std::string& path, std::vector<u8>* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace lion
