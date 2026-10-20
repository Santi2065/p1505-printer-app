// XQX stream encoder for the HP LaserJet P1505 (also P1005/P1006 family).
// Protocol notes: docs/xqx-spec.md.
#ifndef XQX_H
#define XQX_H
#include <stdbool.h>
#include <stddef.h>

typedef struct
{
  unsigned char *data;
  size_t        len, alloc;
} xqx_buf_t;

typedef struct
{
  int  xres;                // 600 or 1200 (FastRes); vertical is always 600
  int  paper;               // DMPAPER code, see xqx_paper_code()
  int  media_type;          // XQX media type code (1 = plain)
  int  source;              // 1 = tray 1, 4 = manual, 7 = auto
  bool economode;
  int  density;             // 1..5
  const char *ret;          // OFF, LIGHT, MEDIUM, DARK
  bool duplex;              // manual duplex: printer pauses before the backs
  bool job_status;          // ask the printer to report job START/END on bulk IN
} xqx_job_t;

enum { XQX_NO_PAUSE = -1 };

bool xqx_buf_add(xqx_buf_t *b, const void *data, size_t len);
void xqx_buf_free(xqx_buf_t *b);

int  xqx_paper_code(int width_mm100, int length_mm100);

bool xqx_job_start(xqx_buf_t *out, const xqx_job_t *job);
bool xqx_job_end(xqx_buf_t *out);

// Encode one page. bits: 1-bit printable area at job->xres x 600 dpi, 1 = black,
// MSB = leftmost, bpl bytes per line. rotate = 180 degree rotation (long-edge backs).
// pause: XQX_NO_PAUSE for simplex, else 0..3 (see spec section 6).
bool xqx_page(xqx_buf_t *out, const xqx_job_t *job, const unsigned char *bits,
              unsigned width, unsigned height, unsigned bpl, bool rotate, int pause);

#endif
