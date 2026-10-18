// Host test: renders a module with LionMod to a 16-bit stereo WAV.   modtest in.mod out.wav [seconds] [rate]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "../LionMod.h"
int main(int argc, char** argv) {
  if (argc < 3) return 1;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char* d = (unsigned char*)malloc(n);
  if (fread(d, 1, n, f) != (size_t)n) return 1;
  fclose(f);
  const int secs = argc > 3 ? atoi(argv[3]) : 10;
  const unsigned rate = argc > 4 ? atoi(argv[4]) : 44100;
  lionmod::Player p;
  if (!p.Load(d, n, rate)) { fprintf(stderr, "load failed\n"); return 2; }
  fprintf(stderr, "%s: %d channels, %d orders\n", p.Format(), p.Channels(), p.Orders());
  const unsigned total = secs * rate;
  std::vector<short> L(total), R(total);
  p.Render(L.data(), R.data(), total);
  FILE* o = fopen(argv[2], "wb");
  unsigned dl = total * 4, sr = rate, br = rate * 4;
  fwrite("RIFF", 1, 4, o); unsigned v = 36 + dl; fwrite(&v, 4, 1, o); fwrite("WAVEfmt ", 1, 8, o);
  v = 16; fwrite(&v, 4, 1, o); unsigned short s = 1; fwrite(&s, 2, 1, o); s = 2; fwrite(&s, 2, 1, o);
  fwrite(&sr, 4, 1, o); fwrite(&br, 4, 1, o); s = 4; fwrite(&s, 2, 1, o); s = 16; fwrite(&s, 2, 1, o);
  fwrite("data", 1, 4, o); fwrite(&dl, 4, 1, o);
  for (unsigned i = 0; i < total; ++i) { fwrite(&L[i], 2, 1, o); fwrite(&R[i], 2, 1, o); }
  fclose(o);
  return 0;
}
