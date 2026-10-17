// Builds the "Apex Cube" demo: mkdemo [output.lion|output.iso]
// A .iso is a bootable disc (textured cube); a .lion is a bare image (Gouraud only).
#include <fstream>
#include <string>

#include "demo_rom.h"

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "apex_cube.iso";
  const bool iso = path.size() >= 4 && path.compare(path.size() - 4, 4, ".iso") == 0;
  const auto bytes = iso ? demo::build_disc() : demo::build().serialize();
  std::ofstream f(path, std::ios::binary);
  if (!f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) {
    std::fprintf(stderr, "mkdemo: cannot write %s\n", path.c_str());
    return 1;
  }
  std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
  return 0;
}
