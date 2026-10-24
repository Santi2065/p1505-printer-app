// HP LaserJet P1505 Printer Application (PAPPL, CUPS 3 driver model).
// Windows-driver features: manual duplex with the printer's Go button, FastRes 1200,
// EconoMode, density, resolution enhancement (RET), media type and tray.
#include "xqx.h"
#include <pappl/pappl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DRIVER    "hp_laserjet_p1505"
// HP firmware (proprietary, from the HPLIP plugin); a copy lives next to the app state
// so the driver keeps working without HPLIP installed.
#define FIRMWARE_NAME  "hp_laserjet_p1505.fw.gz"
#define FIRMWARE_HPLIP "/usr/share/hplip/data/firmware/" FIRMWARE_NAME
#define MARGIN_LR 406     // hundredths of mm; HPLIP's 96 px @ 600 dpi
#define MARGIN_TB 460     // 108 px @ 600 dpi
// Paper-feed calibration default, in tenths of mm (+x right, +y down). Each unit
// differs slightly: measure with ref/calibration.pdf and set print-offset-x/y.
#define OFFSET_X  0
#define OFFSET_Y  0
#define OFFSET_MAX 40     // must stay below the 4 mm crop margin

static pappl_pr_driver_t drivers[] =
{
  { DRIVER, "HP LaserJet P1505", "MFG:Hewlett-Packard;MDL:HP LaserJet P1505;", NULL }
};

static const char *media[] =
{
  "iso_a4_210x297mm", "na_letter_8.5x11in", "na_legal_8.5x14in", "na_executive_7.25x10.5in",
  "iso_a5_148x210mm", "jis_b5_182x257mm", "iso_b5_176x250mm", "na_number-10_4.125x9.5in",
  "iso_dl_110x220mm", "iso_c5_162x229mm", "na_monarch_3.875x7.5in"
};

// User-facing text follows the service's locale: Spanish for es_*, English otherwise.
static bool spanish(void)
{
  const char *l = getenv("LC_ALL");
  if (!l || !*l) l = getenv("LC_MESSAGES");
  if (!l || !*l) l = getenv("LANG");
  return l && !strncmp(l, "es", 2);
}
#define TR(en, es) (spanish() ? (es) : (en))

typedef struct { const char *pwg; int code; } code_t;

static const code_t types[] =
{
  { "stationery", 1 }, { "stationery-lightweight", 258 }, { "stationery-bond", 260 },
  { "cardstock", 261 }, { "stationery-heavyweight", 262 }, { "labels", 265 },
  { "envelope", 267 }, { "transparency", 2 }, { "stationery-letterhead", 513 },
  { "stationery-preprinted", 514 }, { "stationery-prepunched", 515 },
  { "stationery-recycled", 516 }, { "stationery-colored", 512 }
};

static const code_t sources[] =
{
  { "auto", 7 }, { "by-pass-tray", 1 }, { "manual", 4 }
};

typedef struct
{
  xqx_job_t     xj;
  bool          duplex, rotate_backs;
  unsigned      crop_x, crop_y;         // pixels cut from each side
  int           off_x, off_y;           // calibration in tenths of mm (+x right, +y down)
  unsigned      w, h;                   // incoming full page
  unsigned char *gray;                  // current page, 8-bit, 255 = white
  unsigned      num_pages;
  xqx_buf_t     *pages;                 // encoded pages in input order (duplex only)
  size_t        apages;
  bool          started;                // PJL/START_DOC already sent
} p15_job_t;


static int lookup(const char *name, const code_t *t, size_t n, int def)
{
  for (size_t i = 0; name && i < n; i ++)
    if (!strcmp(t[i].pwg, name))
      return t[i].code;
  return def;
}
#define LOOKUP(name, t, def) lookup(name, t, sizeof(t) / sizeof(t[0]), def)


// PAPPL 1.4.9 only looks for "<name>-default" among the job's own attributes, so
// printer defaults set in the web UI or with `modify` need this fallback.
static const char *vendor_opt(pappl_pr_options_t *o, ipp_t *pattrs, const char *name, char *buf, size_t bufsize)
{
  const char *v = cupsGetOption(name, o->num_vendor, o->vendor);
  if (v || !pattrs)
    return v;
  char defname[128];
  snprintf(defname, sizeof(defname), "%s-default", name);
  ipp_attribute_t *a = ippFindAttribute(pattrs, defname, IPP_TAG_ZERO);
  if (!a)
    return NULL;
  ippAttributeString(a, buf, bufsize);
  return buf;
}

static p15_job_t *job_new(pappl_job_t *job, pappl_pr_options_t *o)
{
  p15_job_t *j = calloc(1, sizeof(p15_job_t));
  if (!j) return NULL;

  ipp_t *pattrs = papplPrinterGetDriverAttributes(papplJobGetPrinter(job));
  char b1[64], b2[64], b3[64], b4[64];
  const char *econo = vendor_opt(o, pattrs, "economode", b1, sizeof(b1));
  const char *ret   = vendor_opt(o, pattrs, "resolution-enhancement", b2, sizeof(b2));
  const char *ox    = vendor_opt(o, pattrs, "print-offset-x", b3, sizeof(b3));
  const char *oy    = vendor_opt(o, pattrs, "print-offset-y", b4, sizeof(b4));
  int dark = o->darkness_configured + o->print_darkness;      // 0..100, 50 = normal
  dark = dark < 0 ? 0 : dark > 100 ? 100 : dark;

  j->xj.xres       = o->printer_resolution[0] == 600 ? 600 : 1200;
  j->xj.paper      = xqx_paper_code(o->media.size_width, o->media.size_length);
  j->xj.media_type = LOOKUP(o->media.type, types, 1);
  j->xj.source     = LOOKUP(o->media.source, sources, 7);
  j->xj.economode  = (econo && !strcmp(econo, "on")) || o->print_quality == IPP_QUALITY_DRAFT;
  j->xj.density    = 1 + (dark + 12) / 25;
  j->xj.ret        = !ret ? "MEDIUM" : !strcmp(ret, "off") ? "OFF" : !strcmp(ret, "light") ? "LIGHT" : !strcmp(ret, "dark") ? "DARK" : "MEDIUM";
  j->duplex        = o->sides != PAPPL_SIDES_ONE_SIDED;
  j->rotate_backs  = o->sides == PAPPL_SIDES_TWO_SIDED_LONG_EDGE;
  j->xj.duplex     = j->duplex;
  j->crop_x        = (unsigned)(MARGIN_LR * j->xj.xres / 2540);
  j->crop_y        = (unsigned)(MARGIN_TB * 600 / 2540);
  j->off_x = ox ? atoi(ox) : OFFSET_X;
  j->off_y = oy ? atoi(oy) : OFFSET_Y;
  ippDelete(pattrs);

  papplLogJob(job, PAPPL_LOGLEVEL_INFO, "XQX job: %dx600 paper=%d type=%d source=%d econo=%d density=%d ret=%s sides=%s offset=%d,%d",
              j->xj.xres, j->xj.paper, j->xj.media_type, j->xj.source, j->xj.economode, j->xj.density, j->xj.ret,
              j->duplex ? (j->rotate_backs ? "long" : "short") : "one", j->off_x, j->off_y);
  return j;
}

static void job_free(p15_job_t *j)
{
  if (!j) return;
  for (unsigned i = 0; i < j->num_pages; i ++)
    xqx_buf_free(j->pages + i);
  free(j->pages);
  free(j->gray);
  free(j);
}

static bool page_begin(p15_job_t *j, unsigned w, unsigned h)
{
  free(j->gray);
  j->w = w; j->h = h;
  j->gray = malloc((size_t)w * (h ? h : 1));
  if (j->gray) memset(j->gray, 255, (size_t)w * h);
  return j->gray != NULL;
}

// Clustered-dot halftone from 8-bit gray to 1-bit (1 = black). Error diffusion
// printed photos as grain: the engine cannot place isolated half-dots evenly,
// so grays grow round dots on a SCREEN_LPI grid instead. Pure black and white
// (text, lines) are never screened. A 45 degree screen (106 lpi) was tried on
// 2026-10-24 and printed white blotches and artifacts on this engine.
#define SCREEN_LPI 100      // calibration knob: lower = smoother but coarser
#define SCREEN_MAX 12       // cell width at 1200 dpi

static void dither_screen(const unsigned char *src, unsigned sw, unsigned cx, unsigned cy,
                          unsigned w, unsigned h, unsigned char *dst, unsigned bpl, int xres)
{
  unsigned cw = (unsigned)xres / SCREEN_LPI, ch = 600 / SCREEN_LPI, n = cw * ch;
  double spot[SCREEN_MAX * SCREEN_MAX / 2];
  unsigned char th[SCREEN_MAX * SCREEN_MAX / 2];
  for (unsigned i = 0; i < n; i ++)        // dot centre at the cell corner
    spot[i] = cos(2 * M_PI * ((i % cw) + 0.5) / cw) + cos(2 * M_PI * ((i / cw) + 0.5) / ch);
  for (unsigned i = 0; i < n; i ++)        // rank = how early the pixel turns black
  {
    unsigned r = 0;
    for (unsigned k = 0; k < n; k ++)
      r += spot[k] > spot[i] || (spot[k] == spot[i] && k < i);
    th[i] = (unsigned char)((r * 2 + 1) * 255 / (2 * n));
  }

  for (unsigned y = 0; y < h; y ++)
  {
    const unsigned char *row = src + (size_t)(y + cy) * sw + cx;
    const unsigned char *t = th + ((y + cy) % ch) * cw;
    unsigned char *out = dst + (size_t)y * bpl;
    for (unsigned x = 0; x < w; x ++)
      if (255 - row[x] > t[(x + cx) % cw])
        out[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
  }
}

// Green-noise halftone: Levien's output-dependent feedback (Floyd-Steinberg
// whose threshold is pulled by already printed neighbours) on square 600-dpi
// cells, plus threshold jitter (Lau & Arce) to break up worms. Black clumps
// into random clusters the engine can print. Clusters have more edge than round
// dots, so toner spread darkens them: GREEN_GAMMA lightens the midtones back.
#ifndef GREEN_H
#define GREEN_H 1.0         // calibration knob: 0 = plain FS, higher = bigger clusters
#endif
#define GREEN_JITTER 40     // threshold noise, 0..255
#define GREEN_GAMMA 1.4     // calibration knob: picked on paper 2026-10-24 (1.7 = more contrast)

static void dither_green(const unsigned char *src, unsigned sw, unsigned cx, unsigned cy,
                         unsigned w, unsigned h, unsigned char *dst, unsigned bpl, int xres)
{
  unsigned s = xres == 1200 ? 2 : 1, wc = (w + s - 1) / s;          // cells per row
  float *err = calloc(2 * (wc + 4), sizeof(float));
  unsigned char *prev = calloc(wc + 4, 1), *cur = calloc(wc + 4, 1); // 1 = black
  if (!err || !prev || !cur) { free(err); free(prev); free(cur); return; }
  float *e0 = err + 2, *e1 = err + wc + 6;
  uint32_t rnd = 2463534242u;
  float tone[256];                                   // gray -> corrected darkness
  for (int i = 0; i < 256; i ++)
    tone[i] = 255 * powf((255 - i) / 255.0f, GREEN_GAMMA);

  for (unsigned y = 0; y < h; y ++)
  {
    const unsigned char *row = src + (size_t)(y + cy) * sw + cx;
    unsigned char *out = dst + (size_t)y * bpl;
    int dir = (y & 1) ? -1 : 1;
    memset(e1 - 2, 0, (wc + 4) * sizeof(float));
    memset(cur, 0, wc + 4);
    for (unsigned k = 0; k < wc; k ++)
    {
      int c = (int)((y & 1) ? wc - 1 - k : k);
      unsigned x0 = (unsigned)c * s, n = x0 + s <= w ? s : w - x0, sum = 0, hard = 1;
      for (unsigned i = 0; i < n; i ++)
      {
        sum += row[x0 + i];
        hard &= row[x0 + i] == 0 || row[x0 + i] == 255;
      }
      if (hard)                                        // text and lines stay crisp
      {
        for (unsigned i = 0; i < n; i ++)
          if (!row[x0 + i]) out[(x0 + i) >> 3] |= (unsigned char)(0x80 >> ((x0 + i) & 7));
        cur[c + 2] = sum * 2 < 255 * n;
        continue;
      }
      rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
      float jitter = GREEN_JITTER * ((float)(rnd & 0xffff) / 65535 - 0.5f);
      float fb = (cur[c + 2 - dir] + prev[c + 1] + prev[c + 2] + prev[c + 3]) / 4.0f;
      float v = tone[(sum + n / 2) / n] + e0[c];
      bool black = v + GREEN_H * 255 * (fb - 0.5f) + jitter >= 128;
      float e = v - (black ? 255 : 0);
      if (black)
      {
        cur[c + 2] = 1;
        for (unsigned i = 0; i < n; i ++) out[(x0 + i) >> 3] |= (unsigned char)(0x80 >> ((x0 + i) & 7));
      }
      e0[c + dir] += e * 7 / 16;
      e1[c - dir] += e * 3 / 16;
      e1[c]       += e * 5 / 16;
      e1[c + dir] += e / 16;
    }
    float *t = e0; e0 = e1; e1 = t;
    unsigned char *u = prev; prev = cur; cur = u;
  }
  free(err); free(prev); free(cur);
}

static bool load_firmware(pappl_job_t *job, pappl_device_t *device);
static bool dev_write(pappl_device_t *device, const xqx_buf_t *b);

static bool job_begin(p15_job_t *j, pappl_job_t *job, pappl_device_t *device)
{
  if (j->started) return true;
  xqx_buf_t head = { 0 };
  bool ok = load_firmware(job, device) && xqx_job_start(&head, &j->xj) && dev_write(device, &head);
  xqx_buf_free(&head);
  return j->started = ok;
}

// Crop to the printable area, dither and encode. Simplex pages go out right away;
// duplex pages are kept until the end because the backs print in reverse order.
static bool page_end(p15_job_t *j, pappl_job_t *job, pappl_device_t *device)
{
  if (j->num_pages == j->apages)
  {
    size_t n = j->apages ? j->apages * 2 : 16;
    xqx_buf_t *p = realloc(j->pages, n * sizeof(xqx_buf_t));
    if (!p) return false;
    memset(p + j->apages, 0, (n - j->apages) * sizeof(xqx_buf_t));
    j->pages = p; j->apages = n;
  }

  unsigned pageno = j->num_pages + 1;
  bool back = j->duplex && (pageno % 2) == 0;
  int pause = !j->duplex ? XQX_NO_PAUSE : back ? 3 : pageno == 1 ? 0 : 1;

  unsigned cx = j->crop_x < j->w / 4 ? j->crop_x : 0;
  unsigned cy = j->crop_y < j->h / 4 ? j->crop_y : 0;
  unsigned w = j->w - 2 * cx, h = j->h - 2 * cy, bpl = (w + 7) / 8;

  // Shift the crop window to move the image on the sheet. Backs are rotated
  // 180 degrees after this, so the same physical shift is the opposite here.
  bool rot = back && j->rotate_backs;
  int dx = j->off_x * j->xj.xres / 254, dy = j->off_y * 600 / 254;
  if (rot) { dx = -dx; dy = -dy; }
  int sx = (int)cx - dx, sy = (int)cy - dy;
  sx = sx < 0 ? 0 : sx > (int)(2 * cx) ? (int)(2 * cx) : sx;
  sy = sy < 0 ? 0 : sy > (int)(2 * cy) ? (int)(2 * cy) : sy;

  unsigned char *bits = calloc(bpl, h ? h : 1);
  if (!bits) return false;
  // Green noise is the default (smoother on paper); P1505_HALFTONE=screen in the
  // service environment brings back the ordered 0 degree screen.
  const char *ht = getenv("P1505_HALFTONE");
  (ht && !strcmp(ht, "screen") ? dither_screen : dither_green)(j->gray, j->w, (unsigned)sx, (unsigned)sy, w, h, bits, bpl, j->xj.xres);

  xqx_buf_t *pg = j->pages + j->num_pages;
  bool ok = xqx_page(pg, &j->xj, bits, w, h, bpl, rot, pause);
  free(bits);
  if (ok && !j->duplex)
  {
    ok = job_begin(j, job, device) && dev_write(device, pg);
    xqx_buf_free(pg);
  }
  if (ok) j->num_pages ++;
  papplJobSetImpressionsCompleted(job, 1);
  return ok;
}

// The pause item is the 14th item of START_PAGE: 8 (chunk) + 13 * 12 + 8 (id, size).
#define PAUSE_OFFSET 172

static bool load_firmware(pappl_job_t *job, pappl_device_t *device)
{
  char id[1024];
  if (!papplDeviceGetID(device, id, sizeof(id)) || !*id || strstr(id, "FWVER:"))
    return true;    // no device ID (file/network output) or firmware already loaded

  char path[1024], cmd[1100];
  const char *home = getenv("HOME");
  snprintf(path, sizeof(path), "%s/.local/share/p1505-printer-app/" FIRMWARE_NAME, home ? home : "");
  if (access(path, R_OK))
    snprintf(path, sizeof(path), "%s", FIRMWARE_HPLIP);
  if (job)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_INFO, "Firmware not loaded, uploading %s", path);
    papplJobSetMessage(job, "%s", TR("Loading printer firmware...", "Cargando firmware en la impresora..."));
  }
  snprintf(cmd, sizeof(cmd), "gzip -dc '%s'", path);
  FILE *fp = popen(cmd, "r");
  if (!fp) return false;
  char buf[65536];
  size_t n;
  bool ok = true;
  while (ok && (n = fread(buf, 1, sizeof(buf), fp)) > 0)
    ok = papplDeviceWrite(device, buf, n) == (ssize_t)n;
  if (pclose(fp) != 0) ok = false;
  papplDeviceFlush(device);

  for (int i = 0; ok && i < 30; i ++)
  {
    sleep(1);
    if (papplDeviceGetID(device, id, sizeof(id)) && strstr(id, "FWVER:"))
      return true;
  }
  if (job)
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Firmware upload failed or printer did not report FWVER.");
  return false;
}

// Desktop notification; returns its id (0 if none) so it can be closed later.
static unsigned notify_user(const char *msg)
{
  char cmd[512];
  unsigned id = 0;
  snprintf(cmd, sizeof(cmd), "notify-send --print-id -i printer -u critical 'HP LaserJet P1505' '%s'", msg);
  FILE *fp = popen(cmd, "r");     // msg is a fixed string from this file, no quotes in it
  if (fp)
  {
    if (fscanf(fp, "%u", &id) != 1) id = 0;
    pclose(fp);
  }
  return id;
}

static void notify_close(unsigned id)
{
  char arg[32];
  snprintf(arg, sizeof(arg), "%u", id);
  // posix_spawn, not fork: this process is multithreaded (cf. pappl-retrofit issue #35).
  char *argv[] = { "gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications",
                   "--object-path", "/org/freedesktop/Notifications",
                   "--method", "org.freedesktop.Notifications.CloseNotification", arg, NULL };
  pid_t pid;
  if (id && posix_spawnp(&pid, "gdbus", NULL, NULL, argv, environ) == 0)
    waitpid(pid, NULL, 0);
}

// Wait for the printer's "@PJL USTATUS JOB ... END" (sent after the last sheet).
// papplDeviceRead times out every 10 s; give the user 30 min to reload the stack.
static bool wait_printed(pappl_job_t *job, pappl_device_t *device)
{
  char acc[4096];
  size_t len = 0;
  time_t deadline = time(NULL) + 30 * 60;
  while (time(NULL) < deadline && !papplJobIsCanceled(job))
  {
    ssize_t n = papplDeviceRead(device, acc + len, sizeof(acc) - len);
    if (n <= 0) { sleep(1); continue; }
    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Printer status: %.*s", (int)n, acc + len);
    len += (size_t)n;
    if (memmem(acc, len, "USTATUS JOB", 11) && memmem(acc, len, "\nEND", 4))
      return true;
    if (len > sizeof(acc) / 2)       // keep the tail only
    {
      memmove(acc, acc + len - 1024, 1024);
      len = 1024;
    }
  }
  return false;
}

static bool dev_write(pappl_device_t *device, const xqx_buf_t *b)
{
  return papplDeviceWrite(device, b->data, b->len) == (ssize_t)b->len;
}

// Fronts in order, then backs in reverse; the first back carries pause 2 (printer waits for Go).
static bool job_send(p15_job_t *j, pappl_job_t *job, pappl_device_t *device)
{
  char id[1024];   // only a real USB printer reports status; network/test sinks don't
  j->xj.job_status = j->duplex && j->num_pages > 1 && papplDeviceGetID(device, id, sizeof(id)) && *id;
  if (!j->num_pages || !job_begin(j, job, device))
    return false;

  xqx_buf_t tail = { 0 }, blank = { 0 };
  bool ok = xqx_job_end(&tail);

  if (j->duplex && j->num_pages == 1)     // nothing on the back: no pause, no blank sheet
    ok = ok && dev_write(device, j->pages);
  else if (j->duplex)
  {
    for (unsigned i = 0; ok && i < j->num_pages; i += 2)
      ok = dev_write(device, j->pages + i);

    bool first = true;
    if (j->num_pages & 1)   // odd count: blank back for the last sheet
    {
      unsigned w = j->w - 2 * j->crop_x, h = j->h - 2 * j->crop_y, bpl = (w + 7) / 8;
      unsigned char *z = calloc(bpl, h ? h : 1);   // full-size blank, like a real page
      ok = ok && z && xqx_page(&blank, &j->xj, z, w, h, bpl, false, 2) && dev_write(device, &blank);
      free(z);
      first = false;
    }
    for (int i = (int)(j->num_pages & ~1u) - 1; ok && i >= 1; i -= 2, first = false)
    {
      j->pages[i].data[PAUSE_OFFSET + 3] = first ? 2 : 3;
      ok = dev_write(device, j->pages + i);
    }

    papplJobSetMessage(job, "%s", TR("Manual duplex: when the printer stops, put the stack back in the tray without rotating it and press the Go button.",
                                     "Doble faz: cuando la impresora se detenga, poné la pila en la bandeja sin rotarla y apretá el botón."));
  }

  ok = ok && dev_write(device, &tail);
  papplDeviceFlush(device);
  if (ok && j->xj.job_status)
  {
    unsigned id = notify_user(TR("Manual duplex: when the first side is done, take the stack, put it in the tray without flipping or rotating it, and press the Go button on the printer.",
                                 "Doble faz: cuando termine la primera cara, sacá la pila, ponela en la bandeja sin dar vuelta ni rotar, y apretá el botón de la impresora."));
    if (wait_printed(job, device))
      notify_close(id);
    else
      papplLogJob(job, PAPPL_LOGLEVEL_WARN, "No job END status from printer; leaving notification open.");
    papplDevicePrintf(device, "\033%%-12345X@PJL USTATUS JOB = OFF\r\n\033%%-12345X");   // stop unsolicited status
    papplDeviceFlush(device);
  }
  xqx_buf_free(&tail); xqx_buf_free(&blank);
  return ok;
}


// ---- PAPPL raster callbacks (PWG raster from CUPS / IPP clients) ----

static bool cb_rstartjob(pappl_job_t *job, pappl_pr_options_t *o, pappl_device_t *device)
{
  (void)device;
  p15_job_t *j = job_new(job, o);
  papplJobSetData(job, j);
  return j != NULL;
}

static bool cb_rstartpage(pappl_job_t *job, pappl_pr_options_t *o, pappl_device_t *device, unsigned page)
{
  (void)device; (void)page;
  p15_job_t *j = papplJobGetData(job);
  unsigned bpp = o->header.cupsBitsPerPixel;
  if (bpp != 1 && bpp != 8)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unsupported raster: %u bpp", bpp);
    return false;
  }
  if ((int)o->header.HWResolution[0] != j->xj.xres)   // client picked the resolution
  {
    j->xj.xres = o->header.HWResolution[0] >= 1200 ? 1200 : 600;
    j->crop_x  = (unsigned)(MARGIN_LR * j->xj.xres / 2540);
  }
  return page_begin(j, o->header.cupsWidth, o->header.cupsHeight);
}

static bool cb_rwriteline(pappl_job_t *job, pappl_pr_options_t *o, pappl_device_t *device, unsigned y, const unsigned char *line)
{
  (void)device;
  p15_job_t *j = papplJobGetData(job);
  if (y >= j->h) return true;
  unsigned char *dst = j->gray + (size_t)y * j->w;
  if (o->header.cupsBitsPerPixel == 1)                // 1 = black
    for (unsigned x = 0; x < j->w; x ++)
      dst[x] = (line[x >> 3] & (0x80 >> (x & 7))) ? 0 : 255;
  else if (o->header.cupsColorSpace == CUPS_CSPACE_K) // 255 = black
    for (unsigned x = 0; x < j->w; x ++)
      dst[x] = (unsigned char)(255 - line[x]);
  else
    memcpy(dst, line, j->w);
  return true;
}

static bool cb_rendpage(pappl_job_t *job, pappl_pr_options_t *o, pappl_device_t *device, unsigned page)
{
  (void)o; (void)page;
  return page_end(papplJobGetData(job), job, device);
}

static bool cb_rendjob(pappl_job_t *job, pappl_pr_options_t *o, pappl_device_t *device)
{
  (void)o;
  p15_job_t *j = papplJobGetData(job);
  bool ok = job_send(j, job, device);
  job_free(j);
  papplJobSetData(job, NULL);
  return ok;
}

// Raw XQX files go straight to the printer.
static bool cb_printfile(pappl_job_t *job, pappl_pr_options_t *o, pappl_device_t *device)
{
  (void)o;
  FILE *fp = fopen(papplJobGetFilename(job), "rb");
  if (!fp) return false;
  bool ok = load_firmware(job, device);
  char buf[65536];
  size_t n;
  while (ok && (n = fread(buf, 1, sizeof(buf), fp)) > 0)
    ok = papplDeviceWrite(device, buf, n) == (ssize_t)n;
  fclose(fp);
  return ok;
}


// ---- PDF / PostScript: Ghostscript renders 8-bit gray at the job resolution ----

static bool pnm_header(FILE *fp, unsigned *w, unsigned *h)
{
  int c;
  unsigned v[3];
  if (fgetc(fp) != 'P' || fgetc(fp) != '5') return false;
  for (int k = 0; k < 3; k ++)
  {
    while ((c = fgetc(fp)) != EOF && (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '#'))
      if (c == '#') while ((c = fgetc(fp)) != EOF && c != '\n');
    if (c == EOF) return false;
    ungetc(c, fp);
    if (fscanf(fp, "%u", v + k) != 1) return false;
  }
  fgetc(fp);
  *w = v[0]; *h = v[1];
  return v[2] == 255;
}

static bool filter_gs(pappl_job_t *job, pappl_device_t *device, void *data)
{
  (void)data;
  pappl_pr_options_t *o = papplJobCreatePrintOptions(job, INT_MAX, false);
  p15_job_t *j = o ? job_new(job, o) : NULL;
  if (!j) { if (o) papplJobDeletePrintOptions(o); return false; }

  // Text and line art are rendered without anti-aliasing so edges stay pure black.
  char cmd[2048];
  snprintf(cmd, sizeof(cmd),
           "gs -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=pgmraw -r%dx600 "
           "-dTextAlphaBits=1 -dGraphicsAlphaBits=1 "
           "-dDEVICEWIDTHPOINTS=%.2f -dDEVICEHEIGHTPOINTS=%.2f -dFIXEDMEDIA -dPDFFitPage "
           "-dFirstPage=%u -dLastPage=%u -sOutputFile=- '%s' 2>/dev/null",
           j->xj.xres, o->media.size_width * 72.0 / 2540, o->media.size_length * 72.0 / 2540,
           o->first_page ? o->first_page : 1, o->last_page ? o->last_page : INT_MAX,
           papplJobGetFilename(job));   // PAPPL-generated spool path, no quotes in it
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Running: %s", cmd);

  FILE *fp = popen(cmd, "r");
  bool ok = fp != NULL;
  unsigned w, h;
  int copies = o->copies > 0 ? o->copies : 1;
  while (ok && !papplJobIsCanceled(job) && pnm_header(fp, &w, &h))
  {
    ok = page_begin(j, w, h) && fread(j->gray, w, h, fp) == h;
    for (int c = 0; ok && c < copies; c ++)
      ok = page_end(j, job, device);   // ponytail: uncollated copies; collate if anyone asks
  }
  if (fp && pclose(fp) != 0) ok = false;
  ok = ok && job_send(j, job, device);
  job_free(j);
  papplJobDeletePrintOptions(o);
  return ok;
}


// ---- Status and toner level (PJL INFO STATUS / INFO SUPPLIES) ----

#define MANAGED_REASONS (PAPPL_PREASON_OFFLINE | PAPPL_PREASON_TONER_LOW | PAPPL_PREASON_TONER_EMPTY | \
                         PAPPL_PREASON_MEDIA_JAM | PAPPL_PREASON_DOOR_OPEN | PAPPL_PREASON_MEDIA_EMPTY | \
                         PAPPL_PREASON_OTHER)

// PJL CODE= values, from HPLIP's status-type 8 table.
static pappl_preason_t pjl_reason(int code)
{
  switch (code)
  {
    case 10002: case 10011:                        return PAPPL_PREASON_OFFLINE;
    case 10006: case 10200: case 10218: case 40038: return PAPPL_PREASON_TONER_LOW;
    case 40600:                                    return PAPPL_PREASON_TONER_EMPTY;
    case 10027: case 30119: case 40022:            return PAPPL_PREASON_MEDIA_JAM;
    case 40021:                                    return PAPPL_PREASON_DOOR_OPEN;
  }
  if (code >= 41000 && code < 42000)               return PAPPL_PREASON_MEDIA_EMPTY;
  if (code >= 42000 && code < 45000)               return PAPPL_PREASON_MEDIA_JAM;
  if (code >= 40000 && code < 41000)               return PAPPL_PREASON_OTHER;
  if (code >= 50000 && code < 51000)               return PAPPL_PREASON_OTHER;
  return PAPPL_PREASON_NONE;
}

static int pjl_value(const char *reply, const char *key)
{
  const char *p = strstr(reply, key);
  return p ? atoi(p + strlen(key)) : -1;
}

// Called by PAPPL whenever a client asks for the printer state; the USB query
// runs at most once a minute.
static bool cb_status(pappl_printer_t *printer)
{
  static time_t last;              // ponytail: one P1505 per app; per-printer data if that changes
  if (time(NULL) - last < 60)
    return true;
  last = time(NULL);

  pappl_device_t *dev = papplPrinterOpenDevice(printer);
  if (!dev)
    return true;                   // busy printing
  char reply[8192], id[1024];         // device ID read is a USB control transfer: max 4096 on Linux
  if (!papplDeviceGetID(dev, id, sizeof(id)) || !*id)
  {
    papplPrinterCloseDevice(printer);   // not a USB P1505 (network/test sink)
    return true;
  }
  if (!strstr(id, "FWVER:"))            // just powered on: load firmware now, not at the first job
  {
    bool ok = load_firmware(NULL, dev);
    papplLog(papplPrinterGetSystem(printer), ok ? PAPPL_LOGLEVEL_INFO : PAPPL_LOGLEVEL_ERROR,
             "Printer had no firmware; upload %s.", ok ? "done" : "failed");
  }
  size_t len = 0;
  papplDevicePrintf(dev, "\033%%-12345X@PJL INFO STATUS\r\n@PJL INFO SUPPLIES\r\n\033%%-12345X");
  papplDeviceFlush(dev);
  for (int i = 0; i < 4 && len < sizeof(reply) - 1; i ++)
  {
    ssize_t n = papplDeviceRead(dev, reply + len, sizeof(reply) - 1 - len);
    if (n <= 0) break;
    len += (size_t)n;
    reply[len] = '\0';
    if (strstr(reply, "CODE=") && strstr(reply, "AvgAreaCvrg5")) break;   // last SUPPLIES line
  }
  papplPrinterCloseDevice(printer);
  reply[len] = '\0';
  if (!strstr(reply, "CODE="))
    return true;

  pappl_preason_t reasons = pjl_reason(pjl_value(reply, "CODE="));
  int level = pjl_value(reply, "PercentRemaining = ");
  if (pjl_value(reply, "LowReached = ") == 1)
    reasons |= PAPPL_PREASON_TONER_LOW;
  papplPrinterSetReasons(printer, reasons, MANAGED_REASONS & ~reasons);

  if (level >= 0)
  {
    pappl_supply_t supply = { .color = PAPPL_SUPPLY_COLOR_BLACK, .is_consumed = true,
                              .level = level > 100 ? 100 : level, .type = PAPPL_SUPPLY_TYPE_TONER };
    int pages = pjl_value(reply, "PagesRemaining = ");
    snprintf(supply.description, sizeof(supply.description), TR("Black toner 36A (~%d pages)", "Toner negro 36A (~%d paginas)"), pages);
    papplPrinterSetSupplies(printer, 1, &supply);
  }
  return true;
}


// ---- Test page: small text, fine lines, gray ramp ----

static const char *testpage_cb(pappl_printer_t *printer, char *buffer, size_t bufsize)
{
  (void)printer;
  const char *home = getenv("HOME");
  snprintf(buffer, bufsize, "%s/.local/share/p1505-printer-app/testpage.ps", home ? home : "/tmp");
  FILE *fp = fopen(buffer, "w");
  if (!fp) return NULL;
  fputs("%!PS\n"
        "/Helvetica-Bold findfont 22 scalefont setfont 40 780 moveto (HP LaserJet P1505 - test page) show\n"
        "/Helvetica findfont 11 scalefont setfont 40 760 moveto (XQX driver - FastRes 1200 - error diffusion) show\n"
        "/y 730 def [4 5 6 7 8 10 12] { dup /Times-Roman findfont exch scalefont setfont\n"
        "  40 y moveto (Text ) show 3 string cvs show ( pt: Sphinx of black quartz, judge my vow 0123456789) show\n"
        "  /y y 18 sub def } forall\n"
        "0.1 setlinewidth 40 500 moveto 0 1 59 { 40 exch 4 mul add 500 moveto 0 90 rlineto } for stroke\n"
        "40 470 moveto 280 470 lineto 0.05 setlinewidth stroke\n"
        "/Helvetica findfont 9 scalefont setfont 300 540 moveto (0.1 pt lines) show\n"
        "0 1 20 { dup 20 div setgray 25 mul 40 add 380 25 60 rectfill } for\n"
        "0 setgray 40 365 moveto (gray 0% to 100%) show\n"
        "40 100 moveto /Helvetica findfont 9 scalefont setfont (Margin check: this line should sit ~14 mm above the bottom edge) show\n"
        "showpage\n", fp);
  fclose(fp);
  return buffer;
}


// ---- Driver setup ----

static bool driver_cb(pappl_system_t *system, const char *driver_name, const char *device_uri,
                      const char *device_id, pappl_pr_driver_data_t *d, ipp_t **attrs, void *cbdata)
{
  (void)system; (void)device_uri; (void)device_id; (void)cbdata;
  if (strcmp(driver_name, DRIVER))
    return false;

  d->printfile_cb  = cb_printfile;
  d->rstartjob_cb  = cb_rstartjob;
  d->rstartpage_cb = cb_rstartpage;
  d->rwriteline_cb = cb_rwriteline;
  d->rendpage_cb   = cb_rendpage;
  d->rendjob_cb    = cb_rendjob;
  d->testpage_cb   = testpage_cb;
  d->status_cb     = cb_status;
  d->has_supplies  = true;
  d->format        = "application/vnd.hp-xqx";

  snprintf(d->make_and_model, sizeof(d->make_and_model), "HP LaserJet P1505");
  d->ppm             = 24;
  d->kind            = PAPPL_KIND_DOCUMENT;
  d->color_supported = PAPPL_COLOR_MODE_MONOCHROME | PAPPL_COLOR_MODE_AUTO;
  d->color_default   = PAPPL_COLOR_MODE_MONOCHROME;
  d->content_default = PAPPL_CONTENT_AUTO;
  d->quality_default = IPP_QUALITY_HIGH;
  d->scaling_default = PAPPL_SCALING_NONE;   // A4 stays 1:1; gs fits other sizes itself
  d->raster_types    = PAPPL_PWG_RASTER_TYPE_BLACK_1 | PAPPL_PWG_RASTER_TYPE_BLACK_8 | PAPPL_PWG_RASTER_TYPE_SGRAY_8;
  d->force_raster_type = PAPPL_PWG_RASTER_TYPE_SGRAY_8;   // we dither ourselves
  d->input_face_up   = true;
  d->output_face_up  = false;
  d->duplex          = PAPPL_DUPLEX_NORMAL;   // we rotate the backs ourselves
  d->sides_supported = PAPPL_SIDES_ONE_SIDED | PAPPL_SIDES_TWO_SIDED_LONG_EDGE | PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
  d->sides_default   = PAPPL_SIDES_ONE_SIDED;

  d->num_resolution  = 2;
  d->x_resolution[0] = 600;  d->y_resolution[0] = 600;
  d->x_resolution[1] = 1200; d->y_resolution[1] = 600;   // FastRes 1200
  d->x_default = 1200; d->y_default = 600;

  d->left_right = MARGIN_LR;
  d->bottom_top = MARGIN_TB;

  d->num_media = (int)(sizeof(media) / sizeof(media[0]));
  memcpy(d->media, media, sizeof(media));
  d->num_source = (int)(sizeof(sources) / sizeof(sources[0]));
  for (int i = 0; i < d->num_source; i ++)
    d->source[i] = sources[i].pwg;
  d->num_type = (int)(sizeof(types) / sizeof(types[0]));
  for (int i = 0; i < d->num_type; i ++)
    d->type[i] = types[i].pwg;

  pwg_media_t *a4 = pwgMediaForPWG("iso_a4_210x297mm");
  for (int i = 0; i < d->num_source; i ++)
  {
    pappl_media_col_t *m = &d->media_ready[i];
    snprintf(m->size_name, sizeof(m->size_name), "%s", a4->pwg);
    m->size_width = a4->width; m->size_length = a4->length;
    m->left_margin = m->right_margin = MARGIN_LR;
    m->top_margin = m->bottom_margin = MARGIN_TB;
    snprintf(m->source, sizeof(m->source), "%s", sources[i].pwg);
    snprintf(m->type, sizeof(m->type), "stationery");
  }
  d->media_default = d->media_ready[0];

  d->darkness_supported  = 5;      // PJL DENSITY 1..5
  d->darkness_configured = 50;
  d->darkness_default    = 0;

  d->num_vendor = 4;
  d->vendor[0]  = "economode";
  d->vendor[1]  = "resolution-enhancement";
  d->vendor[2]  = "print-offset-x";
  d->vendor[3]  = "print-offset-y";

  static const char *onoff[] = { "off", "on" };
  static const char *rets[]  = { "off", "light", "medium", "dark" };
  *attrs = ippNew();
  ippAddStrings(*attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "economode-supported", 2, NULL, onoff);
  ippAddString(*attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "economode-default", NULL, "off");
  ippAddStrings(*attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "resolution-enhancement-supported", 4, NULL, rets);
  ippAddString(*attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "resolution-enhancement-default", NULL, "medium");
  ippAddRange(*attrs, IPP_TAG_PRINTER, "print-offset-x-supported", -OFFSET_MAX, OFFSET_MAX);
  ippAddInteger(*attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "print-offset-x-default", OFFSET_X);
  ippAddRange(*attrs, IPP_TAG_PRINTER, "print-offset-y-supported", -OFFSET_MAX, OFFSET_MAX);
  ippAddInteger(*attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "print-offset-y-default", OFFSET_Y);
  return true;
}

static const char *autoadd_cb(const char *device_info, const char *device_uri, const char *device_id, void *data)
{
  (void)device_info; (void)device_uri; (void)data;
  return device_id && strstr(device_id, "LaserJet P1505") ? DRIVER : NULL;
}

static char statefile[1024];

static pappl_system_t *system_cb(int num_options, cups_option_t *options, void *data)
{
  (void)data;
  const char *home = getenv("HOME");
  char spool[960], logfile[1024];   // spool + "/state" must fit statefile
  snprintf(spool, sizeof(spool), "%s/.local/share/p1505-printer-app", home ? home : "/tmp");
  snprintf(logfile, sizeof(logfile), "%s/log", spool);
  snprintf(statefile, sizeof(statefile), "%s/state", spool);
  mkdir(spool, 0700);

  const char *val = cupsGetOption("server-port", num_options, options);
  const char *lvl = cupsGetOption("log-level", num_options, options);
  const char *lf  = cupsGetOption("log-file", num_options, options);

  pappl_system_t *system = papplSystemCreate(
      PAPPL_SOPTIONS_MULTI_QUEUE | PAPPL_SOPTIONS_WEB_INTERFACE | PAPPL_SOPTIONS_WEB_LOG,
      "HP P1505 Printer App", val ? atoi(val) : 8632, "_print,_universal", spool,
      lf ? lf : logfile, lvl && !strcmp(lvl, "debug") ? PAPPL_LOGLEVEL_DEBUG : PAPPL_LOGLEVEL_INFO,
      NULL, false);
  if (!system)
    return NULL;

  papplSystemAddListeners(system, "localhost");   // local only, not on the network
  papplSystemSetHostName(system, "localhost");    // so DNS-SD advertises on loopback only; share via CUPS
  papplSystemSetPrinterDrivers(system, 1, drivers, autoadd_cb, NULL, driver_cb, NULL);
  papplSystemAddMIMEFilter(system, "application/pdf", "application/vnd.hp-xqx", filter_gs, NULL);
  papplSystemAddMIMEFilter(system, "application/postscript", "application/vnd.hp-xqx", filter_gs, NULL);
  papplSystemSetFooterHTML(system, "Free XQX driver for the HP LaserJet P1505");
  papplSystemSetSaveCallback(system, (pappl_save_cb_t)papplSystemSaveState, statefile);
  papplSystemLoadState(system, statefile);
  return system;
}

int main(int argc, char *argv[])
{
  return papplMainloop(argc, argv, "1.0", NULL, 1, drivers, autoadd_cb, driver_cb,
                       NULL, NULL, system_cb, NULL, NULL);
}
