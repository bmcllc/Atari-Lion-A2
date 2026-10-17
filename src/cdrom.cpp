#include "cdrom.h"

#include <algorithm>
#include <cctype>

namespace lion {

namespace {

constexpr u64 kSectorCycles = kBusHz * Cdrom::kSectorSize / Cdrom::kBytesPerSecond;

inline u32 le32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }

std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

void Cdrom::insert(std::vector<u8> image) {
  image.resize((image.size() + kSectorSize - 1) / kSectorSize * kSectorSize);
  image_ = std::move(image);
  status_ = kPresent;
  head_ = 0;
}

void Cdrom::eject() {
  image_.clear();
  status_ = 0;
}

void Cdrom::finish(u32 status_bit) {
  status_ = (status_ & ~kBusy) | status_bit;
  pic_.raise(Pic::kCd);
}

void Cdrom::start_read() {
  if (!present() || count_ == 0 || u64(lba_) + count_ > capacity()) {
    finish(kError | kDone);
    return;
  }
  status_ = (status_ | kBusy) & ~(kDone | kError);
  done_ = 0;
  // Seek time grows with the distance the sled travels.
  const double dist = double(lba_ > head_ ? lba_ - head_ : head_ - lba_) / double(kFullDiscSectors);
  wait_ = u64((kSeekBaseSeconds + kSeekFullSeconds * dist) * double(kBusHz));
  seeking_ = true;
}

void Cdrom::tick(u64 cycles) {
  const u64 start = clock_;
  clock_ += cycles;
  const u64 total = cycles;
  while ((status_ & kBusy) && cycles) {
    const u64 step = std::min(cycles, wait_);
    cycles -= step;
    wait_ -= step;
    if (wait_) break;
    if (seeking_) {
      seeking_ = false;
      head_ = lba_;
      wait_ = kSectorCycles;
      continue;
    }
    // One sector arrived: DMA it to memory (256 bus cycles at 8 bytes per cycle).
    bus_.acquire(Master::Dma, start + (total - cycles), kSectorSize / 8);
    const u32 dst = dma_ + done_ * kSectorSize;
    const u8* src = sector(lba_ + done_);
    if (u8* p = bus_.host_range(dst, kSectorSize)) {
      std::copy(src, src + kSectorSize, p);
    } else {
      for (u32 i = 0; i < kSectorSize; ++i) bus_.write8(dst + i, src[i], Master::Dma);
    }
    ++done_;
    head_ = lba_ + done_;
    if (done_ == count_) {
      finish(kDone);
    } else {
      wait_ = kSectorCycles;
    }
  }
}

u32 Cdrom::read32(u32 offset, Master, bool*) {
  switch (offset) {
    case 0x00: return status_ | (present() ? u32(kPresent) : 0u);
    case 0x08: return lba_;
    case 0x0C: return count_;
    case 0x10: return dma_;
    case 0x14: return capacity();
    case 0x18: return done_;
  }
  return 0;
}

void Cdrom::write32(u32 offset, u32 value, Master, bool*) {
  switch (offset) {
    case 0x04:
      if (value == kRead) start_read();
      else if (value == kStop) status_ &= ~kBusy;
      else if (value == kAck) status_ &= ~(kDone | kError);
      break;
    case 0x08: lba_ = value; break;
    case 0x0C: count_ = value; break;
    case 0x10: dma_ = value; break;
  }
}

bool Cdrom::find_file(const std::string& path, u32* lba, u32* size) const {
  if (capacity() < 17) return false;
  const u8* pvd = sector(16);
  if (pvd[0] != 1 || std::string(reinterpret_cast<const char*>(pvd + 1), 5) != "CD001") return false;
  u32 dir_lba = le32(pvd + 156 + 2), dir_size = le32(pvd + 156 + 10);

  std::string rest = upper(path);
  while (!rest.empty()) {
    const size_t slash = rest.find('/');
    const std::string part = rest.substr(0, slash);
    rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
    bool found = false;
    for (u32 off = 0; off < dir_size && !found;) {
      const u32 abs = dir_lba * kSectorSize + off;
      if (abs >= image_.size()) return false;
      const u8* rec = &image_[abs];
      if (rec[0] == 0) {  // records never straddle sectors: skip to the next one
        off = (off / kSectorSize + 1) * kSectorSize;
        continue;
      }
      std::string name(reinterpret_cast<const char*>(rec + 33), rec[32]);
      if (const size_t semi = name.find(';'); semi != std::string::npos) name.resize(semi);
      if (!name.empty() && name.back() == '.') name.pop_back();
      if (upper(name) == part) {
        const bool is_dir = rec[25] & 2;
        if (rest.empty() == is_dir) return false;
        dir_lba = le32(rec + 2);
        dir_size = le32(rec + 10);
        found = true;
      }
      off += rec[0];
    }
    if (!found) return false;
  }
  *lba = dir_lba;
  *size = dir_size;
  return true;
}

bool Cdrom::read_file(const std::string& path, std::vector<u8>* out) const {
  u32 lba, size;
  if (!find_file(path, &lba, &size)) return false;
  if (size_t(lba) * kSectorSize + size > image_.size()) return false;
  out->assign(image_.begin() + std::ptrdiff_t(size_t(lba) * kSectorSize),
              image_.begin() + std::ptrdiff_t(size_t(lba) * kSectorSize + size));
  return true;
}

}  // namespace lion
