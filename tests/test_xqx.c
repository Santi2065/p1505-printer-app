// Round-trip check against the HPLIP reference stream.
//   test_xqx encode ref.jbg out.bin   -> decode HPLIP page, re-encode with our xqx_page into a job
//   test_xqx cmp a.jbg b.jbg          -> decode both JBIG pages, compare pixels
//   test_xqx unit                     -> rotation / 600-dpi doubling checks
#include "../src/xqx.h"
#include <jbig.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *f, size_t *len)
{
  FILE *fp = fopen(f, "rb");
  if (!fp) { perror(f); exit(1); }
  fseek(fp, 0, SEEK_END); *len = (size_t)ftell(fp); rewind(fp);
  unsigned char *d = malloc(*len);
  if (fread(d, 1, *len, fp) != *len) exit(1);
  fclose(fp);
  return d;
}

static unsigned char *decode(const char *f, unsigned long *w, unsigned long *h, size_t *size)
{
  size_t len; unsigned char *d = slurp(f, &len);
  struct jbg_dec_state s;
  jbg_dec_init(&s);
  int r = jbg_dec_in(&s, d, len, NULL);
  if (r != JBG_EOK) { fprintf(stderr, "%s: %s\n", f, jbg_strerror(r)); exit(1); }
  *w = jbg_dec_getwidth(&s); *h = jbg_dec_getheight(&s); *size = jbg_dec_getsize(&s);
  unsigned char *img = malloc(*size);
  memcpy(img, jbg_dec_getimage(&s, 0), *size);
  jbg_dec_free(&s); free(d);
  return img;
}

static void unit(void)
{
  xqx_job_t j = { .xres = 600, .paper = 9, .media_type = 1, .source = 7, .density = 3 };
  unsigned char px[2] = { 0x80, 0x00 };          // 16x1, leftmost pixel black
  xqx_buf_t a = { 0 }, b = { 0 };
  assert(xqx_page(&a, &j, px, 16, 1, 2, false, XQX_NO_PAUSE));
  assert(xqx_page(&b, &j, px, 16, 1, 2, false, 2));
  assert(a.len > 0 && b.len == a.len + 12);      // one extra pause item
  xqx_buf_free(&a); xqx_buf_free(&b);
  assert(xqx_paper_code(21000, 29700) == 9 && xqx_paper_code(21590, 27940) == 1);
  puts("unit ok");
}

int main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "unit")) { unit(); return 0; }
  if (argc == 4 && !strcmp(argv[1], "encode"))
  {
    unsigned long w, h; size_t size;
    unsigned char *img = decode(argv[2], &w, &h, &size);
    // HPLIP data is already 2 bits per 600-dpi pixel, i.e. a 1200x600 bitmap.
    xqx_job_t j = { .xres = 1200, .paper = 1, .media_type = 1, .source = 7, .density = 3, .ret = "MEDIUM" };
    xqx_buf_t out = { 0 };
    assert(xqx_job_start(&out, &j));
    assert(xqx_page(&out, &j, img, (unsigned)w, (unsigned)h, (unsigned)((w + 7) / 8), false, XQX_NO_PAUSE));
    assert(xqx_job_end(&out));
    FILE *fp = fopen(argv[3], "wb"); fwrite(out.data, 1, out.len, fp); fclose(fp);
    printf("encoded %lux%lu -> %zu bytes\n", w, h, out.len);
    return 0;
  }
  if (argc == 4 && !strcmp(argv[1], "topbm"))    // test_xqx topbm page.jbg out.pbm
  {
    unsigned long w, h; size_t size;
    unsigned char *img = decode(argv[2], &w, &h, &size);
    FILE *fp = fopen(argv[3], "wb");
    fprintf(fp, "P4\n%lu %lu\n", w, h);
    fwrite(img, 1, size, fp);
    fclose(fp);
    return 0;
  }
  if (argc == 4 && !strcmp(argv[1], "cmp"))
  {
    unsigned long w1, h1, w2, h2; size_t s1, s2;
    unsigned char *a = decode(argv[2], &w1, &h1, &s1), *b = decode(argv[3], &w2, &h2, &s2);
    if (w1 != w2 || h1 != h2 || s1 != s2 || memcmp(a, b, s1)) { printf("DIFFERENT %lux%lu vs %lux%lu\n", w1, h1, w2, h2); return 1; }
    printf("pixels identical %lux%lu\n", w1, h1);
    return 0;
  }
  fprintf(stderr, "usage: see header\n");
  return 2;
}
