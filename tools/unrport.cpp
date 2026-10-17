// Unreal-on-Lion packager.
//   unrport Map.unr client.bin out.iso [--wall P.utx:Name] [--floor P.utx:Name] [--ceiling P.utx:Name] [--gain N]
// Reads the BSP brush polygons of an Unreal 1 package (.unr, version >= 40) and, optionally, textures
// from .utx packages (8-bit palettized, with mipmaps). Packs them as the "ULVL" blob that
// tools/unrport/client.c draws and writes a bootable Lion disc.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "iso9660.h"
#include "loader.h"

namespace {

constexpr u32 kPackageTag = 0x9E2A83C1;
constexpr u32 kClientAddr = 0x0001'0000, kLevelAddr = 0x0020'0000, kTextureAddr = 0x1040'0000;
constexpr u32 kLevelMagic = 0x554C564C, kNoTexture = 0xFFFFFFFF;

struct Reader {
  const std::vector<u8>& d;
  size_t p;
  bool ok = true;
  u8 byte() { return p < d.size() ? d[p++] : (ok = false, u8(0)); }
  u32 u32le() {
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= u32(byte()) << (8 * i);
    return v;
  }
  float f32() { return std::bit_cast<float>(u32le()); }
  // Unreal compact index: sign/continuation in the first byte, then 7 bits per byte.
  int index() {
    u8 b = byte();
    const bool neg = b & 0x80;
    int v = b & 0x3F;
    if (b & 0x40)
      for (int s = 6; ok; s += 7) {
        b = byte();
        v |= (b & 0x7F) << s;
        if (!(b & 0x80)) break;
      }
    return neg ? -v : v;
  }
};

struct Export {
  int cls = 0, name = 0;
  u32 size = 0, offset = 0;
};

struct Package {
  std::vector<u8> d;
  std::vector<std::string> names, imports;  // imports: object names, index -(i+1)
  std::vector<Export> exports;              // object index i+1

  std::string name_of(int i) const { return i >= 0 && size_t(i) < names.size() ? names[size_t(i)] : std::string(); }
  std::string class_of(const Export& e) const {
    return e.cls < 0 && size_t(-e.cls - 1) < imports.size() ? imports[size_t(-e.cls - 1)] : std::string();
  }

  bool load(const std::string& path, std::string* err) {
    if (!lion::read_file(path, &d)) return *err = "cannot read " + path, false;
    Reader r{d, 0};
    if (r.u32le() != kPackageTag) return *err = path + ": not an Unreal package", false;
    if ((r.u32le() & 0xFFFF) < 40) return *err = path + ": package version too old", false;
    r.u32le();  // package flags
    const u32 nname = r.u32le(), noff = r.u32le(), nexp = r.u32le(), eoff = r.u32le(), nimp = r.u32le(), ioff = r.u32le();
    if (!r.ok || nname > 100000 || nexp > 100000 || nimp > 100000) return *err = path + ": bad header", false;
    r.p = noff;
    for (u32 i = 0; i < nname && r.ok; ++i) {
      std::string s;
      while (r.p < d.size() && d[r.p]) s += char(d[r.p++]);
      ++r.p;
      r.u32le();  // name flags
      names.push_back(s);
    }
    r.p = ioff;
    for (u32 i = 0; i < nimp && r.ok; ++i) {
      r.index();  // class package
      r.index();  // class name
      r.u32le();  // package
      imports.push_back(name_of(r.index()));
    }
    r.p = eoff;
    for (u32 i = 0; i < nexp && r.ok; ++i) {
      Export e;
      e.cls = r.index();
      r.index();  // super
      r.u32le();  // package
      e.name = r.index();
      r.u32le();  // object flags
      e.size = u32(r.index());
      if (e.size > 0) e.offset = u32(r.index());
      exports.push_back(e);
    }
    if (!r.ok) return *err = path + ": truncated tables", false;
    return true;
  }

  // Skips a tagged property list; returns the position after it and the raw bytes of each property.
  size_t properties(size_t pos, std::map<std::string, std::vector<u8>>* out) const {
    Reader r{d, pos};
    while (r.ok) {
      const std::string name = name_of(r.index());
      if (name == "None" || name.empty()) break;
      const u8 info = r.byte();
      if ((info & 15) == 10) r.index();  // struct name
      u32 size = 0;
      switch (info & 0x70) {
        case 0x00: size = 1; break;
        case 0x10: size = 2; break;
        case 0x20: size = 4; break;
        case 0x30: size = 12; break;
        case 0x40: size = 16; break;
        case 0x50: size = r.byte(); break;
        case 0x60: size = r.byte(), size |= u32(r.byte()) << 8; break;
        default: size = r.u32le(); break;
      }
      if ((info & 0x80) && (info & 15) != 3) {  // array index
        const u8 b = r.byte();
        if (b & 0x80) r.byte();
        if ((b & 0xC0) == 0xC0) r.byte(), r.byte();
      }
      if (r.p + size > d.size()) return r.ok = false, r.p;
      (*out)[name].assign(d.begin() + long(r.p), d.begin() + long(r.p + size));
      r.p += size;
    }
    return r.p;
  }

  const Export* find(const std::string& cls, const std::string& name) const {
    auto lower = [](std::string s) {
      for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
      return s;
    };
    for (const Export& e : exports)
      if (class_of(e) == cls && lower(name_of(e.name)) == lower(name)) return &e;
    return nullptr;
  }
};

struct Texture {
  struct Mip {
    u32 w, h;
    std::vector<u8> rgb565;  // big-endian
  };
  std::vector<Mip> mips;
  std::string label;
};

// "File.utx:Name" -> mip chain converted to RGB565.
bool load_texture(const std::string& spec, float gain, Texture* tex, std::string* err) {
  const size_t colon = spec.rfind(':');
  if (colon == std::string::npos) return *err = "texture must be File.utx:Name", false;
  Package pkg;
  if (!pkg.load(spec.substr(0, colon), err)) return false;
  const Export* te = pkg.find("Texture", spec.substr(colon + 1));
  if (!te) return *err = spec + ": texture not found", false;
  std::map<std::string, std::vector<u8>> props;
  const size_t pos = pkg.properties(te->offset, &props);
  const auto pal = props.find("Palette");
  if (pal == props.end()) return *err = spec + ": texture has no palette", false;
  Reader pr{pal->second, 0};
  const int pal_index = pr.index();
  if (pal_index < 1 || size_t(pal_index) > pkg.exports.size()) return *err = spec + ": bad palette reference", false;
  const Export& pe = pkg.exports[size_t(pal_index) - 1];
  std::map<std::string, std::vector<u8>> pprops;
  Reader cr{pkg.d, pkg.properties(pe.offset, &pprops)};
  const int ncolors = cr.index();
  if (ncolors != 256) return *err = spec + ": palette is not 256 colours", false;
  u16 colors[256];
  for (u16& c : colors) {
    auto lit = [&](u8 v) { return u8(std::min(255.0f, float(v) * gain)); };  // stands in for the lightmap
    const u8 r = lit(cr.byte()), g = lit(cr.byte()), b = lit(cr.byte());
    cr.byte();
    c = u16((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
  }
  Reader mr{pkg.d, pos};
  const int nmips = mr.index();
  for (int m = 0; m < nmips && mr.ok; ++m) {
    const int len = mr.index();
    const size_t at = mr.p;
    mr.p += size_t(len);
    const u32 w = mr.u32le(), h = mr.u32le();
    mr.byte(), mr.byte();
    if (!mr.ok || at + size_t(len) > pkg.d.size() || u64(w) * h != u64(len) || w > 1024 || h > 1024)
      return *err = spec + ": bad mipmap", false;
    Texture::Mip mip{w, h, {}};
    for (int i = 0; i < len; ++i) {
      const u16 c = colors[pkg.d[at + size_t(i)]];
      mip.rgb565.push_back(u8(c >> 8)), mip.rgb565.push_back(u8(c));
    }
    tex->mips.push_back(std::move(mip));
  }
  if (tex->mips.empty()) return *err = spec + ": texture has no pixel data", false;
  tex->label = spec;
  return true;
}

struct Poly {
  std::vector<float> v;  // xyz triplets
  float base[3], normal[3], tex_u[3], tex_v[3];
};

bool load_level(const std::string& path, std::vector<Poly>* polys, std::string* err) {
  Package pkg;
  if (!pkg.load(path, err)) return false;
  for (const Export& e : pkg.exports) {
    if (pkg.class_of(e) != "Polys" || size_t(e.offset) + e.size > pkg.d.size()) continue;
    Reader p{pkg.d, e.offset + 1};  // skip the property list terminator
    const u32 count = p.u32le();
    p.u32le();  // DbMax
    for (u32 k = 0; k < count && p.ok; ++k) {
      const int nv = p.index();
      if (nv < 3 || nv > 16) return *err = "unsupported polygon", false;
      Poly poly;
      for (float& f : poly.base) f = p.f32();
      for (float& f : poly.normal) f = p.f32();
      for (float& f : poly.tex_u) f = p.f32();
      for (float& f : poly.tex_v) f = p.f32();
      for (int i = 0; i < nv * 3; ++i) poly.v.push_back(p.f32());
      p.u32le();                         // poly flags
      p.index(), p.index(), p.index();   // actor, texture, item name
      p.index(), p.index();              // iLink, iBrushPoly
      p.u32le();                         // pan U, V
      if (!p.ok) return *err = "truncated polygons", false;
      bool dup = false;  // the editor stores the same brush more than once
      for (const Poly& q : *polys) dup |= q.v == poly.v;
      if (!dup) polys->push_back(std::move(poly));
    }
  }
  return !polys->empty() || (*err = "no polygons found", false);
}

u32 color_for(const float* n, bool textured) {
  if (textured) return 0xFFFFFFFF;
  const float ax = std::fabs(n[0]), ay = std::fabs(n[1]), az = std::fabs(n[2]);
  if (az >= ax && az >= ay) return n[2] > 0 ? 0xFF3A4660 : 0xFF6B5A3C;  // ceiling slate, floor earth
  if (ax >= ay) return n[0] > 0 ? 0xFFB05A4A : 0xFF4F8F6A;
  return n[1] > 0 ? 0xFF4A6FB0 : 0xFFB0A04A;
}

u32 log2_of(u32 v) {
  u32 n = 0;
  while ((1u << n) < v) ++n;
  return n;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> pos;
  std::string specs[3];  // wall, floor, ceiling
  float gain = 1.0f;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const int slot = a == "--wall" ? 0 : a == "--floor" ? 1 : a == "--ceiling" ? 2 : -1;
    if (slot >= 0 && i + 1 < argc) specs[slot] = argv[++i];
    else if (a == "--gain" && i + 1 < argc) gain = std::strtof(argv[++i], nullptr);
    else pos.push_back(a);
  }
  if (pos.size() != 3) {
    std::fprintf(stderr,
                 "usage: unrport Map.unr client.bin out.iso [--wall P.utx:Name] [--floor P.utx:Name] "
                 "[--ceiling P.utx:Name] [--gain N]\n");
    return 2;
  }
  std::vector<u8> client;
  if (!lion::read_file(pos[1], &client)) {
    std::fprintf(stderr, "unrport: cannot read %s\n", pos[1].c_str());
    return 1;
  }
  std::vector<Poly> polys;
  std::string err;
  if (!load_level(pos[0], &polys, &err)) {
    std::fprintf(stderr, "unrport: %s: %s\n", pos[0].c_str(), err.c_str());
    return 1;
  }

  // Textures: identical specs share one copy in SGRAM.
  std::vector<Texture> textures;
  std::vector<u32> slot_tex(3, kNoTexture);
  for (int s = 0; s < 3; ++s) {
    if (specs[s].empty()) continue;
    for (size_t t = 0; t < textures.size(); ++t)
      if (textures[t].label == specs[s]) slot_tex[size_t(s)] = u32(t);
    if (slot_tex[size_t(s)] != kNoTexture) continue;
    Texture tex;
    if (!load_texture(specs[s], gain, &tex, &err)) {
      std::fprintf(stderr, "unrport: %s\n", err.c_str());
      return 1;
    }
    slot_tex[size_t(s)] = u32(textures.size());
    textures.push_back(std::move(tex));
  }
  std::vector<u8> texdata;
  struct Placed { u32 addr, log2w, log2h, levels; };
  std::vector<Placed> placed;
  for (const Texture& t : textures) {
    placed.push_back({kTextureAddr + u32(texdata.size()), log2_of(t.mips[0].w), log2_of(t.mips[0].h), u32(t.mips.size())});
    for (const auto& m : t.mips) texdata.insert(texdata.end(), m.rgb565.begin(), m.rgb565.end());
    while (texdata.size() % 32) texdata.push_back(0);
  }

  std::vector<u8> level;
  auto put32 = [&](u32 v) {
    for (int i = 24; i >= 0; i -= 8) level.push_back(u8(v >> i));
  };
  auto putf = [&](float f) { put32(std::bit_cast<u32>(f)); };
  put32(kLevelMagic);
  put32(u32(polys.size()));
  put32(u32(placed.size()));
  for (const Placed& p : placed) put32(p.addr), put32(p.log2w), put32(p.log2h), put32(p.levels);
  for (const Poly& p : polys) {
    const float* n = p.normal;
    const int slot = std::fabs(n[2]) > 0.7f ? (n[2] > 0 ? 2 : 1) : 0;
    const u32 tex = slot_tex[size_t(slot)];
    put32(u32(p.v.size() / 3));
    put32(color_for(n, tex != kNoTexture));
    put32(tex);
    for (int i = 0; i < 3; ++i) putf(n[i]);
    const float tw = tex == kNoTexture ? 1.0f : float(1u << placed[tex].log2w);
    const float th = tex == kNoTexture ? 1.0f : float(1u << placed[tex].log2h);
    for (size_t i = 0; i + 2 < p.v.size(); i += 3) {
      float d[3], u = 0, v = 0;
      for (int k = 0; k < 3; ++k) d[k] = p.v[i + size_t(k)] - p.base[k];
      for (int k = 0; k < 3; ++k) u += d[k] * p.tex_u[k], v += d[k] * p.tex_v[k];
      for (int k = 0; k < 3; ++k) putf(p.v[i + size_t(k)]);
      putf(u / tw), putf(v / th);  // texels -> normalized
    }
  }

  lion::LionImage img;
  img.entry = kClientAddr;
  img.segments.push_back({kClientAddr, client});
  img.segments.push_back({kLevelAddr, level});
  if (!texdata.empty()) img.segments.push_back({kTextureAddr, texdata});
  const auto disc = iso9660::build("UNREAL", {{"BOOT.LION", img.serialize()}});
  FILE* f = std::fopen(pos[2].c_str(), "wb");
  if (!f || std::fwrite(disc.data(), 1, disc.size(), f) != disc.size()) {
    std::fprintf(stderr, "unrport: cannot write %s\n", pos[2].c_str());
    return 1;
  }
  std::fclose(f);
  std::printf("%zu polygons, %zu textures (%zu bytes) -> %s (%zu bytes of code)\n", polys.size(), textures.size(),
              texdata.size(), pos[2].c_str(), client.size());
  return 0;
}
