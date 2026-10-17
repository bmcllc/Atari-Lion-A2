// Converts raw audio (16-bit little-endian PCM, as `ffmpeg -f s16le` writes it)
// into the boot ROM's formats. tools/convert_assets.sh drives it.
//   lionasset pcm   in.raw out.pcm             mono -> big-endian PCM16 (sound effects)
//   lionasset adpcm in.raw RATE out.ladp       interleaved stereo -> LADP (music)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "ima_adpcm.h"

namespace {

std::vector<int16_t> read_raw(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<int16_t> out(bytes.size() / 2);
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = int16_t(uint8_t(bytes[2 * i]) | uint8_t(bytes[2 * i + 1]) << 8);
  return out;
}

bool write(const char* path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary);
  return bool(f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size())));
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "pcm" && argc == 4) {
    std::vector<uint8_t> out;
    for (int16_t s : read_raw(argv[2])) out.push_back(uint8_t(uint16_t(s) >> 8)), out.push_back(uint8_t(s));
    return write(argv[3], out) ? 0 : 1;
  }
  if (mode == "adpcm" && argc == 5) {
    const auto pcm = read_raw(argv[2]);
    if (pcm.empty()) return 1;
    return write(argv[4], ima::encode_stereo(pcm, uint32_t(std::atoi(argv[3])))) ? 0 : 1;
  }
  std::fprintf(stderr, "usage: lionasset pcm in.raw out.pcm | lionasset adpcm in.raw RATE out.ladp\n");
  return 2;
}
