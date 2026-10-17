// Builds the demo discs.
//   mkdemo [output.lion|output.iso]          the "Apex Cube" demo; .iso is a bootable disc
//                                            (textured cube), .lion a bare image (Gouraud only)
//   mkdemo --music output.iso [a.wav ...]    an audio disc for the dashboard's CD player:
//                                            the given 16-bit PCM .wav files, or three demo tracks
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>

#include "demo_rom.h"
#include "music_disc.h"

namespace {

bool write(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream f(path, std::ios::binary);
  if (!f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) {
    std::fprintf(stderr, "mkdemo: cannot write %s\n", path.c_str());
    return false;
  }
  std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
  return true;
}

// "dir/My Song.wav" -> "MY_SONG.WAV" (ISO9660 level 1: 8.3, upper case, A-Z 0-9 _).
std::string iso_name(const std::string& path) {
  std::string base = path.substr(path.find_last_of('/') + 1);
  if (const size_t dot = base.find_last_of('.'); dot != std::string::npos) base.resize(dot);
  std::string out;
  for (char c : base) {
    if (out.size() == 8) break;
    out += std::isalnum(static_cast<unsigned char>(c)) ? char(std::toupper(static_cast<unsigned char>(c))) : '_';
  }
  return (out.empty() ? "TRACK" : out) + ".WAV";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--music") {
    if (argc < 3) {
      std::fprintf(stderr, "usage: mkdemo --music output.iso [track.wav ...]\n");
      return 2;
    }
    std::vector<iso9660::File> tracks;
    for (int i = 3; i < argc; ++i) {
      std::ifstream f(argv[i], std::ios::binary);
      std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (data.size() < 44) {
        std::fprintf(stderr, "mkdemo: cannot read %s\n", argv[i]);
        return 1;
      }
      tracks.push_back({iso_name(argv[i]), std::move(data)});
    }
    return write(argv[2], music::build_disc(std::move(tracks))) ? 0 : 1;
  }
  const std::string path = argc > 1 ? argv[1] : "apex_cube.iso";
  const bool iso = path.size() >= 4 && path.compare(path.size() - 4, 4, ".iso") == 0;
  return write(path, iso ? demo::build_disc() : demo::build().serialize()) ? 0 : 1;
}
