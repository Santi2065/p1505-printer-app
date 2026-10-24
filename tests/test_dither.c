// Halftone check: coverage must fall as gray rises, and pure black/white
// (text) must come out solid, for both the screen and green noise. Builds main.c with its main() renamed.
#define main app_main
#include "../src/main.c"
#undef main
#include <assert.h>

// test_dither in.pgm out.pbm screens a 1200x600 dpi P5 image for a look at the result.
static int screen_file(const char *in, const char *out)
{
  FILE *f = fopen(in, "rb");
  unsigned w, h;
  assert(f && fscanf(f, "P5 %u %u 255", &w, &h) == 2 && fgetc(f) != EOF);
  unsigned bpl = (w + 7) / 8;
  unsigned char *g = malloc((size_t)w * h), *b = calloc(bpl, h);
  assert(g && b && fread(g, 1, (size_t)w * h, f) == (size_t)w * h);
  (getenv("P1505_HALFTONE") ? dither_green : dither_screen)(g, w, 0, 0, w, h, b, bpl, 1200);
  FILE *o = fopen(out, "wb");
  fprintf(o, "P4 %u %u\n", w, h);
  fwrite(b, bpl, h, o);
  return fclose(o);
}

int main(int argc, char *argv[])
{
  if (argc == 3) return screen_file(argv[1], argv[2]);
  enum { W = 1200, H = 600, BPL = W / 8 };
  static unsigned char gray[W * H], bits[BPL * H];
  int xres[] = { 600, 1200 };

  void (*fn[])(const unsigned char *, unsigned, unsigned, unsigned, unsigned, unsigned,
               unsigned char *, unsigned, int) = { dither_screen, dither_green };
  for (int f = 0; f < 2; f ++)
  for (int r = 0; r < 2; r ++)
  {
    long prev = (long)W * H + 1;
    for (int g = 0; g <= 255; g += 15)
    {
      memset(gray, g, sizeof(gray));
      memset(bits, 0, sizeof(bits));
      fn[f](gray, W, 0, 0, W, H, bits, BPL, xres[r]);
      long black = 0;
      for (size_t i = 0; i < sizeof(bits); i ++) black += __builtin_popcount(bits[i]);
      if (g == 0)   assert(black == (long)W * H);
      if (g == 255) assert(black == 0);
      assert(black <= prev);
      printf("%s xres %4d gray %3d black %5.1f%%\n", f ? "green " : "screen", xres[r], g, 100.0 * black / (W * H));
      prev = black;
    }
  }
  puts("dither ok");
  return 0;
}
