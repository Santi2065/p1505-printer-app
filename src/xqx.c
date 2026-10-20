// XQX stream encoder. Layout and constants: docs/xqx-spec.md.
#include "xqx.h"
#include <jbig.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { C_START_DOC = 1, C_END_DOC, C_START_PAGE, C_END_PAGE, C_START_PLANE, C_END_PLANE, C_JBIG };
#define I_LEN   0x80000000u
#define I_END   0x80000001u
#define SEGMENT 65536

bool xqx_buf_add(xqx_buf_t *b, const void *data, size_t len)
{
  if (b->len + len > b->alloc)
  {
    size_t n = b->alloc ? b->alloc : 65536;
    while (n < b->len + len) n *= 2;
    unsigned char *p = realloc(b->data, n);
    if (!p) return false;
    b->data = p; b->alloc = n;
  }
  memcpy(b->data + b->len, data, len);
  b->len += len;
  return true;
}

void xqx_buf_free(xqx_buf_t *b)
{
  free(b->data);
  memset(b, 0, sizeof(*b));
}

static bool put32(xqx_buf_t *b, uint32_t v)
{
  unsigned char x[4] = { v >> 24, v >> 16, v >> 8, v };
  return xqx_buf_add(b, x, 4);
}

static bool chunk(xqx_buf_t *b, uint32_t type, uint32_t count)
{
  return put32(b, type) && put32(b, count);
}

static bool item(xqx_buf_t *b, uint32_t id, uint32_t v)
{
  return put32(b, id) && put32(b, 4) && put32(b, v);
}

// Item list: n = number of items including the length and end items.
static bool list_begin(xqx_buf_t *b, uint32_t type, uint32_t n, uint32_t bytes)
{
  return chunk(b, type, n) && item(b, I_LEN, bytes);
}

static bool list_end(xqx_buf_t *b)
{
  return item(b, I_END, 0xDEADBEEF);
}

int xqx_paper_code(int w, int l)
{
  static const struct { int w, l, code; } sizes[] =
  {
    { 21590, 27940, 1 },   // Letter
    { 21590, 35560, 5 },   // Legal
    { 18415, 26670, 7 },   // Executive
    { 21000, 29700, 9 },   // A4
    { 14800, 21000, 11 },  // A5
    { 18200, 25700, 13 },  // B5 JIS
    { 10477, 24130, 20 },  // Env #10
    { 11000, 22000, 27 },  // Env DL
    { 16200, 22900, 28 },  // Env C5
    { 17600, 25000, 34 },  // Env B5
    { 9842,  19050, 37 },  // Env Monarch
  };
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i ++)
    if (abs(sizes[i].w - w) < 200 && abs(sizes[i].l - l) < 200)
      return sizes[i].code;
  return 96;               // HPLIP's code for anything else
}

bool xqx_job_start(xqx_buf_t *out, const xqx_job_t *job)
{
  char pjl[512];
  int n = snprintf(pjl, sizeof(pjl),
    "\033E\033%%-12345X\033%%-12345X@PJL JOB\r\n"
    "@PJL SET JAMRECOVERY=OFF\r\n"
    "@PJL SET DENSITY=%d\r\n"
    "@PJL SET RET=%s\r\n"
    "@PJL SET ECONOMODE=%s\r\n"
    "%s"
    "\033%%-12345X,XQX",
    job->density, job->ret ? job->ret : "MEDIUM", job->economode ? "ON" : "OFF",
    job->job_status ? "@PJL USTATUS JOB = ON\r\n" : "");

  return xqx_buf_add(out, pjl, (size_t)n)
      && list_begin(out, C_START_DOC, 7, 7 * 12)
      && item(out, 0x10000005, 1)
      && item(out, 0x10000001, 0)
      && item(out, 0x10000002, job->duplex ? 2 : 0)
      && item(out, 0x10000000, 0)
      && item(out, 0x10000003, 1)
      && list_end(out);
}

bool xqx_job_end(xqx_buf_t *out)
{
  static const char eoj[] = "\033%-12345X@PJL EOJ\r\n\033%-12345X";
  return chunk(out, C_END_DOC, 0) && xqx_buf_add(out, eoj, sizeof(eoj) - 1);
}

static unsigned char mirror(unsigned char c)
{
  c = (unsigned char)((c & 0xF0) >> 4 | (c & 0x0F) << 4);
  c = (unsigned char)((c & 0xCC) >> 2 | (c & 0x33) << 2);
  return (unsigned char)((c & 0xAA) >> 1 | (c & 0x55) << 1);
}

static void jbig_out(unsigned char *data, size_t len, void *ctx)
{
  xqx_buf_add(ctx, data, len);   // ponytail: OOM here truncates the page; caller checks size
}

bool xqx_page(xqx_buf_t *out, const xqx_job_t *job, const unsigned char *bits,
              unsigned width, unsigned height, unsigned bpl, bool rotate, int pause)
{
  // The printer always takes 2 bits per 600-dpi pixel. 600-dpi input doubles
  // each pixel (as HPLIP does); 1200-dpi input already has two bits per pixel.
  // 1200 dpi input carries 2 bits per 600-dpi pixel directly. 4 bpp (INFO CONFIG
  // lists it) printed garbage on 2026-10-20, so it is not offered.
  unsigned bpp      = 2;
  bool     direct   = job->xres == 1200;
  unsigned realbits = direct ? width & ~1u : width * 2;
  unsigned xd       = (realbits + 63) & ~63u;     // HPLIP pads to 64 bits on this printer
  size_t   obpl     = xd / 8;
  unsigned char *img = calloc(obpl, height ? height : 1);
  if (!img) return false;

  for (unsigned y = 0; y < height; y ++)
  {
    const unsigned char *src = bits + (size_t)y * bpl;
    unsigned char *dst = img + (size_t)y * obpl;
    if (direct)
      memcpy(dst, src, realbits / 8 + ((realbits & 7) != 0));
    else
      for (unsigned x = 0; x < width; x ++)
        if (src[x >> 3] & (0x80 >> (x & 7)))
          dst[x >> 2] |= (unsigned char)(0xC0 >> ((x & 3) * 2));
  }
  if (direct && (realbits & 7))   // clear bits past realbits
    for (unsigned y = 0; y < height; y ++)
      img[(size_t)y * obpl + realbits / 8] &= (unsigned char)(0xFF << (8 - (realbits & 7)));

  if (rotate)                               // 180 degrees: reverse bytes, mirror bits
  {
    size_t total = obpl * height;
    for (size_t i = 0, j = total - 1; i < j; i ++, j --)
    {
      unsigned char t = mirror(img[i]);
      img[i] = mirror(img[j]);
      img[j] = t;
    }
    if (total & 1) img[total / 2] = mirror(img[total / 2]);
  }

  xqx_buf_t jb = { 0 };
  struct jbg_enc_state s;
  unsigned char *planes[1] = { img };
  jbg_enc_init(&s, xd, height, 1, planes, jbig_out, &jb);
  jbg_enc_layers(&s, 0);
  // Same BIH as HPLIP sends to this printer: L0=128, MX=0, order 0x03, options 0x5C.
  jbg_enc_options(&s, JBG_ILEAVE | JBG_SMID, JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON, 128, 0, 0);
  jbg_enc_out(&s);
  jbg_enc_free(&s);
  free(img);
  if (jb.len < 20) { xqx_buf_free(&jb); return false; }

  uint32_t n = pause == XQX_NO_PAUSE ? 15 : 16;
  bool ok = list_begin(out, C_START_PAGE, n, n * 12)
         && item(out, 0x20000005, 1)
         && item(out, 0x20000006, (uint32_t)job->source)
         && item(out, 0x20000000, (uint32_t)job->media_type)
         && item(out, 0x20000007, 1)
         && item(out, 0x20000008, 600)
         && item(out, 0x20000009, 600)
         && item(out, 0x2000000D, xd)
         && item(out, 0x2000000E, height)
         && item(out, 0x2000000A, bpp)
         && item(out, 0x2000000F, realbits / bpp)
         && item(out, 0x20000010, height)
         && item(out, 0x20000011, 1)     // HPLIP always sends 1; EconoMode goes via PJL
         && (pause == XQX_NO_PAUSE || item(out, 0x20000012, (uint32_t)pause))
         && item(out, 0x20000001, (uint32_t)job->paper)
         && list_end(out);

  for (size_t off = 20, k = 0; ok && off < jb.len; off += SEGMENT, k ++)
  {
    size_t seg = jb.len - off < SEGMENT ? jb.len - off : SEGMENT;
    ok = list_begin(out, C_START_PLANE, 4, k ? 48 : 64)
      && item(out, 0x40000000, 0)
      && (k ? item(out, 0x40000003, 1)
            : put32(out, 0x40000002) && put32(out, 20) && xqx_buf_add(out, jb.data, 20))
      && list_end(out)
      && chunk(out, C_JBIG, (uint32_t)seg)
      && xqx_buf_add(out, jb.data + off, seg)
      && chunk(out, C_END_PLANE, 0);
  }
  xqx_buf_free(&jb);
  return ok && chunk(out, C_END_PAGE, 0);
}
