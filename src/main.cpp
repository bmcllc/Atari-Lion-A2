// Atari Lion A2 emulator — SDL2 frontend.
//
//   lion [image.lion | disc.iso] [--frames N] [--dump frame.ppm] [--headless] [--verbose]
//                     [--trace-tuffy] [--trace-quacker] [--scale N] [--fast-boot]
//
// Keyboard (port 1): arrows, Z/X/C = A/B/C, A/S/D = X/Y/Z, Q/W = L/R,
// Enter = Start, Right Shift = Select. Game controllers fill ports 1-4.
// Drop a disc image on the window to insert it; F9 ejects.
#include <SDL.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "system.h"

namespace {

struct Options {
  std::string image;
  long frames = -1;
  std::string dump;
  std::string dump_ram;  // file,hexaddr,hexlen
  std::vector<std::pair<long, u32>> holds;  // --hold FRAME:HEXMASK[:LX:LY]: controller state from that frame on
  std::vector<std::array<u8, 2>> hold_sticks;
  std::string wav;       // --wav file: headless runs write the Tyke's output as a 16-bit stereo WAV
  std::string until_log;  // --until-log A|B: headless run ends when the guest log contains A or B
  bool headless = false;
  int scale = 2;
  bool fast_boot = false;
};

void usage() {
  std::fprintf(stderr,
               "usage: lion [image.lion | disc.iso] [--frames N] [--dump out.ppm] [--dump-ram file,addr,len] [--hold frame:mask[:lx:ly]] [--headless]\n"
               "            [--verbose] [--trace-tuffy] [--trace-quacker] [--scale N] [--strict-7400]\n"
               "            [--fast-boot] [--until-log TEXT|TEXT] [--wav out.wav]\n"
               "With no image, the console starts its dashboard with an empty CD drive.\n"
               "--fast-boot straps the board as a development unit: no intro, discs boot at once.\n");
}

bool write_ppm(const std::string& path, const std::vector<u32>& px, int w, int h) {
  std::ofstream f(path, std::ios::binary);
  f << "P6\n" << w << " " << h << "\n255\n";
  for (u32 c : px) {
    const char rgb[3] = {char(c >> 16), char(c >> 8), char(c)};
    f.write(rgb, 3);
  }
  return bool(f);
}

u32 keyboard_buttons(const Uint8* k) {
  using P = lion::Pads;
  u32 b = 0;
  auto map = [&](SDL_Scancode sc, u32 bit) {
    if (k[sc]) b |= bit;
  };
  map(SDL_SCANCODE_UP, P::kUp);
  map(SDL_SCANCODE_DOWN, P::kDown);
  map(SDL_SCANCODE_LEFT, P::kLeft);
  map(SDL_SCANCODE_RIGHT, P::kRight);
  map(SDL_SCANCODE_Z, P::kA);
  map(SDL_SCANCODE_X, P::kB);
  map(SDL_SCANCODE_C, P::kC);
  map(SDL_SCANCODE_A, P::kX);
  map(SDL_SCANCODE_S, P::kY);
  map(SDL_SCANCODE_D, P::kZ);
  map(SDL_SCANCODE_Q, P::kL);
  map(SDL_SCANCODE_W, P::kR);
  map(SDL_SCANCODE_RETURN, P::kStart);
  map(SDL_SCANCODE_RSHIFT, P::kSelect);
  return b;
}

void poll_controller(SDL_GameController* gc, lion::Pads::State& s) {
  using P = lion::Pads;
  auto btn = [&](SDL_GameControllerButton b, u32 bit) {
    if (SDL_GameControllerGetButton(gc, b)) s.buttons |= bit;
  };
  btn(SDL_CONTROLLER_BUTTON_DPAD_UP, P::kUp);
  btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, P::kDown);
  btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, P::kLeft);
  btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, P::kRight);
  btn(SDL_CONTROLLER_BUTTON_A, P::kA);
  btn(SDL_CONTROLLER_BUTTON_B, P::kB);
  btn(SDL_CONTROLLER_BUTTON_X, P::kX);
  btn(SDL_CONTROLLER_BUTTON_Y, P::kY);
  btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, P::kL);
  btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, P::kR);
  btn(SDL_CONTROLLER_BUTTON_START, P::kStart);
  btn(SDL_CONTROLLER_BUTTON_BACK, P::kSelect);
  if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) s.buttons |= P::kC;
  if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) s.buttons |= P::kZ;
  auto axis = [&](SDL_GameControllerAxis a) { return u8((SDL_GameControllerGetAxis(gc, a) + 32768) >> 8); };
  s.lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
  s.ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
  s.rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
  s.ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
}

int run_headless(lion::Lion& sys, const Options& opt) {
  const long frames = opt.frames < 0 ? 60 : opt.frames;
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<int16_t> wav;
  for (long i = 0; i < frames; ++i) {
    for (size_t h = 0; h < opt.holds.size(); ++h)
      if (opt.holds[h].first == i) {
        auto& p = sys.pads.port[0];
        p.connected = true;
        p.buttons = opt.holds[h].second;
        p.lx = opt.hold_sticks[h][0];
        p.ly = opt.hold_sticks[h][1];
      }
    sys.run_frame();
    if (!opt.wav.empty()) wav.insert(wav.end(), sys.audio.begin(), sys.audio.end());
    sys.audio.clear();
    if (!opt.until_log.empty() && sys.tuffy.illegal_count > 0) {  // a guest crash ends the run too
      std::printf("until-log: Tuffy fault after %ld frames: first illegal opcode at PC=%08X LR=%08X SP=%08X\n", i + 1,
                  sys.tuffy.first_illegal_pc, sys.tuffy.first_illegal_lr, sys.tuffy.first_illegal_sp);
      break;
    }
    // --until-log: stop once the guest's log ring (0x1F0000, see unreal/) contains the text.
    if (!opt.until_log.empty() && i % 30 == 29)
      if (const u8* p = sys.bus.host_range(0x1F0000, 0x20000)) {
        const std::string log(reinterpret_cast<const char*>(p) + 4, 0x20000 - 4);
        bool hit = false;
        for (size_t at = 0; at <= opt.until_log.size() && !hit;) {
          size_t bar = opt.until_log.find('|', at);
          if (bar == std::string::npos) bar = opt.until_log.size();
          hit = bar > at && log.find(opt.until_log.substr(at, bar - at)) != std::string::npos;
          at = bar + 1;
        }
        if (hit) {
          std::printf("until-log: found after %ld frames\n", i + 1);
          break;
        }
      }
  }
  if (!opt.wav.empty()) {
    if (FILE* f = std::fopen(opt.wav.c_str(), "wb")) {
      const uint32_t bytes = uint32_t(wav.size() * 2), rate = lion::kAudioRate, byte_rate = rate * 4, riff = 36 + bytes, fmt = 16;
      const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
      std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
      std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f);
      std::fwrite(&byte_rate, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
      std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
      std::fwrite(wav.data(), 2, wav.size(), f);
      std::fclose(f);
      std::printf("wrote %s (%zu stereo frames)\n", opt.wav.c_str(), wav.size() / 2);
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("%ld frames in %.2fs (%.0f%% of real time)\n", frames, secs,
              100.0 * (double(frames) / lion::kFrameRate) / secs);
  std::printf("Tuffy: %llu instr / %llu cycles   Quacker: %llu instr   QVE: %llu vectors\n",
              (unsigned long long)sys.tuffy.instructions, (unsigned long long)sys.tuffy.total_cycles,
              (unsigned long long)sys.quacker.instructions, (unsigned long long)sys.qve.vectors_processed());
  std::printf("Tuffy state: PC=%08X LR=%08X SP=%08X CTR=%08X\n", sys.tuffy.pc, sys.tuffy.lr, sys.tuffy.gpr[1], sys.tuffy.ctr);
  std::printf("Spike: %llu triangles, %llu pixels\n", (unsigned long long)sys.spike.triangles(),
              (unsigned long long)sys.spike.pixels());
  if (!opt.dump_ram.empty()) {  // debugging aid: raw memory after the run
    char path[512];
    unsigned addr = 0, len = 0;
    if (std::sscanf(opt.dump_ram.c_str(), "%511[^,],%x,%x", path, &addr, &len) == 3) {
      if (const u8* p = sys.bus.host_range(addr, len)) {
        if (FILE* f = std::fopen(path, "wb")) {
          std::fwrite(p, 1, len, f);
          std::fclose(f);
          std::printf("wrote %s (%u bytes at %08X)\n", path, len, addr);
        }
      } else {
        std::fprintf(stderr, "dump-ram: range not backed by memory\n");
      }
    }
  }
  if (!opt.dump.empty()) {
    std::vector<u32> px;
    if (!sys.vi.scanout(px)) {
      std::fprintf(stderr, "video is off, nothing to dump\n");
      return 1;
    }
    if (!write_ppm(opt.dump, px, sys.vi.width(), sys.vi.height())) return 1;
    std::printf("wrote %s\n", opt.dump.c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--frames") opt.frames = std::atol(next());
    else if (a == "--dump") opt.dump = next(), opt.headless = true;
    else if (a == "--dump-ram") opt.dump_ram = next(), opt.headless = true;
    else if (a == "--hold") {  // FRAME:HEXMASK[:LX:LY] (headless tests)
      long frame = 0;
      unsigned mask = 0, lx = 0x80, ly = 0x80;
      std::sscanf(next(), "%ld:%x:%u:%u", &frame, &mask, &lx, &ly);
      opt.holds.push_back({frame, mask});
      opt.hold_sticks.push_back({u8(lx), u8(ly)});
      opt.headless = true;
    }
    else if (a == "--until-log") opt.until_log = next(), opt.headless = true;
    else if (a == "--wav") opt.wav = next(), opt.headless = true;
    else if (a == "--headless") opt.headless = true;
    else if (a == "--verbose") lion::g_verbose = true;
    else if (a == "--scale") opt.scale = std::max(1, std::atoi(next()));
    else if (a == "--fast-boot") opt.fast_boot = true;
    else if (a == "--trace-tuffy" || a == "--trace-quacker" || a == "--strict-7400") continue;  // after boot
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (opt.image.empty() && a[0] != '-') opt.image = a;
    else { usage(); return 2; }
  }
  auto sys = std::make_unique<lion::Lion>();
  if (opt.fast_boot) sys->pic.straps |= lion::Pic::kStrapFastBoot;
  std::string err;
  std::vector<u8> file;
  if (opt.image.empty()) {
    sys->power_on();  // empty drive: the boot ROM runs its dashboard
  } else if (!lion::read_file(opt.image, &file)) {
    std::fprintf(stderr, "cannot read %s\n", opt.image.c_str());
    return 1;
  }
  // ISO9660 discs carry "CD001" in the volume descriptor at sector 16.
  const bool is_disc = file.size() > 0x8006 && std::memcmp(&file[0x8001], "CD001", 5) == 0;
  if (opt.image.empty()) {
  } else if (is_disc) {
    if (!sys->boot_disc(std::move(file), &err)) {
      std::fprintf(stderr, "%s: %s\n", opt.image.c_str(), err.c_str());
      return 1;
    }
  } else {
    lion::LionImage image;
    if (!image.parse(file, &err)) {
      std::fprintf(stderr, "%s: %s\n", opt.image.c_str(), err.c_str());
      return 1;
    }
    sys->boot(image);
  }
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--trace-tuffy") sys->tuffy.trace = true;
    if (std::string(argv[i]) == "--trace-quacker") sys->quacker.trace = true;
    if (std::string(argv[i]) == "--strict-7400") sys->tuffy.strict_7400 = true;
  }
  if (opt.headless) return run_headless(*sys, opt);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* win = SDL_CreateWindow("Atari Lion A2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640 * opt.scale,
                                     480 * opt.scale, SDL_WINDOW_RESIZABLE);
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!win || !ren) {
    std::fprintf(stderr, "SDL window: %s\n", SDL_GetError());
    return 1;
  }
  SDL_RenderSetLogicalSize(ren, 640, 480);
  SDL_Texture* tex640 = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);
  SDL_Texture* tex320 = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240);

  SDL_AudioSpec want{}, have{};
  want.freq = lion::kAudioRate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (audio) SDL_PauseAudioDevice(audio, 0);

  SDL_GameController* pads[4] = {};
  std::vector<u32> px;
  bool quit = false;
  long frame = 0;
  auto title_t = std::chrono::steady_clock::now();
  long title_frames = 0;
  const auto frame_period = std::chrono::nanoseconds(1'000'000'000 / lion::kFrameRate);
  auto next_frame = std::chrono::steady_clock::now();

  while (!quit && (opt.frames < 0 || frame < opt.frames)) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) quit = true;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = true;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F9) sys->cd.eject();
      if (e.type == SDL_DROPFILE) {
        // Hot-insert a disc; the dashboard notices and reads its label.
        std::vector<u8> disc;
        if (lion::read_file(e.drop.file, &disc) && disc.size() > 0x8006 && std::memcmp(&disc[0x8001], "CD001", 5) == 0)
          sys->cd.insert(std::move(disc));
        else
          std::fprintf(stderr, "%s: not an ISO9660 disc image\n", e.drop.file);
        SDL_free(e.drop.file);
      }
      if (e.type == SDL_CONTROLLERDEVICEADDED) {
        for (auto& p : pads)
          if (!p) {
            p = SDL_GameControllerOpen(e.cdevice.which);
            break;
          }
      }
      if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
        for (auto& p : pads)
          if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == e.cdevice.which) {
            SDL_GameControllerClose(p);
            p = nullptr;
          }
      }
    }

    for (int i = 0; i < 4; ++i) {
      auto& s = sys->pads.port[size_t(i)];
      s.buttons = 0;
      s.connected = i == 0 || pads[i];
      if (pads[i]) poll_controller(pads[i], s);
    }
    sys->pads.port[0].buttons |= keyboard_buttons(SDL_GetKeyboardState(nullptr));

    sys->run_frame();
    ++frame;
    ++title_frames;

    if (audio) {
      // Keep the queue short so audio stays in sync with video.
      if (SDL_GetQueuedAudioSize(audio) < 44100 * 4 / 5)
        SDL_QueueAudio(audio, sys->audio.data(), Uint32(sys->audio.size() * sizeof(s16)));
    }
    sys->audio.clear();

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (sys->vi.scanout(px)) {
      SDL_Texture* t = sys->vi.width() == 320 ? tex320 : tex640;
      SDL_UpdateTexture(t, nullptr, px.data(), sys->vi.width() * 4);
      SDL_RenderCopy(ren, t, nullptr, nullptr);
    }
    SDL_RenderPresent(ren);

    const auto now = std::chrono::steady_clock::now();
    if (now - title_t >= std::chrono::seconds(1)) {
      const double secs = std::chrono::duration<double>(now - title_t).count();
      char title[128];
      std::snprintf(title, sizeof title, "Atari Lion A2 — %.0f fps (%.0f%%)", title_frames / secs,
                    100.0 * title_frames / secs / lion::kFrameRate);
      SDL_SetWindowTitle(win, title);
      title_t = now;
      title_frames = 0;
    }
    // Pace to 60 Hz when vsync is not doing it.
    next_frame += frame_period;
    if (next_frame > std::chrono::steady_clock::now()) {
      std::this_thread::sleep_until(next_frame);
    } else {
      next_frame = std::chrono::steady_clock::now();
    }
  }

  for (auto& p : pads)
    if (p) SDL_GameControllerClose(p);
  if (audio) SDL_CloseAudioDevice(audio);
  SDL_DestroyTexture(tex640);
  SDL_DestroyTexture(tex320);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
