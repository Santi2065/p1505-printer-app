# HP LaserJet P1505: XQX protocol spec (for the PAPPL driver)

Device: HP LaserJet P1505, USB `03f0:3f17`, class 7/1/2 (bidirectional printer).
Endpoints: bulk OUT `0x01`, bulk IN `0x81`, 512 bytes each. Seen on the attached unit with `lsusb -v`.

This file records protocol facts only. The references are GPL (foo2zjs) and BSD-style (HPLIP hpcups). Don't copy code from either. See section 10.

## 0. References and citation keys

| Key | Source |
|---|---|
| `F:` | foo2zjs `foo2xqx.c` v1.25 (2011), mirror github.com/koenkooi/foo2zjs. Line numbers refer to this file. |
| `F128:` | Debian dgit foo2zjs `foo2xqx.c` v1.28 (2019). Its only functional change is item `0x20000005` = copies, at line 490. Everything else matches v1.25, with line numbers shifted by +6 after line 9. |
| `H:` | foo2zjs `xqx.h` |
| `W:` | foo2zjs `foo2xqx-wrapper.in` |
| `D:` | foo2zjs `xqxdecode.c` (the decoder) |
| `J:` | foo2zjs bundled jbigkit `jbig.c` / `jbig.h` |
| `L:` | foo2zjs `hplj1000` (firmware hotplug script) |
| `A:` | foo2zjs `arm2hpdl.c` (firmware wrapper tool) |
| `Z:` | HPLIP 3.26.6 `prnt/hpcups/LJZxStream.cpp`. The installed HPLIP PPD for the P1505 says `*hpPrinterLanguage: "ljzxstream"`. |
| `M:` | HPLIP 3.26.6 `prnt/hpcups/ModeJbig.cpp` |
| `S:` | installed `/usr/share/hplip/base/status.py` |

Sources:
- foo2zjs: <https://github.com/koenkooi/foo2zjs>
- HPLIP 3.26.6: <https://sourceforge.net/projects/hplip/>

## 1. Byte-level primitives

All integers are **big-endian uint32**. The writers byte-swap with `be32()` before writing (`H:192-208`, `F:336-350`, `Z` via `Utils.cpp:76`).

**Chunk header** (8 bytes, `H:27-30`, `F:330-340`):
```
u32 type      1=START_DOC 2=END_DOC 3=START_PAGE 4=END_PAGE
              5=START_PLANE 6=END_PLANE 7=JBIG          (H:14-21)
u32 count     non-JBIG chunks: number of items that follow
              JBIG chunk: number of raw data bytes that follow
```

**Item** (`H:172-180`, `F:342-367`):
```
u32 id
u32 size      payload byte count: 4 for integer items, 20 for the BIH item
u8  payload[size]
```

Every item list:
- starts with item `0x80000000`, whose value is the total byte length of the item list, including this item;
- ends with item `0x80000001` (`XQXI_END`), whose value is `0xDEADBEEF`.

Example check: START_DOC has 7 items, 7×12 = 84, so the value is `0x54` (`F:639`, `Z:79`). START_PAGE has 15 items, 180 = `0xB4`, or 16 items, 192, with duplex (`F:483`).

## 2. Job stream layout

```
PJL header (ASCII)
",XQX"                               4 raw bytes, magic (F:610,633; D:177)
START_DOC chunk + 7 items
for each page:
    START_PAGE chunk + 15|16 items
    for each JBIG data segment (<= 65536 bytes):
        START_PLANE chunk + 4 items
        JBIG chunk (count = segment length) + segment bytes
        END_PLANE chunk (count 0)
    END_PAGE chunk (count 0)
END_DOC chunk (count 0)
PJL trailer
```

### 2.1 PJL header (`F:620-633`)

Lines end in LF in foo2xqx. HPLIP uses CRLF (`Z:58-62`). Both work.

```
ESC%-12345X@PJL JOB\n
@PJL SET JAMRECOVERY=OFF\n
@PJL SET DENSITY=<1..5>\n            default 3
@PJL SET ECONOMODE=<ON|OFF>\n
@PJL SET RET=MEDIUM\n
@PJL INFO STATUS\n
@PJL USTATUS DEVICE = ON\n
@PJL USTATUS JOB = ON\n
@PJL USTATUS PAGE = ON\n
@PJL USTATUS TIMED = 30\n
@PJL SET JOBATTR="JobAttr4=YYYYMMDDhhmmss"   (no newline) followed by one 0x00 byte
ESC%-12345X
,XQX
```

HPLIP sends only these lines: `JOB`, `JAMRECOVERY=OFF`, `DENSITY=3`, `RET=MEDIUM`, `ECONOMODE=`, then `ESC%-12345X,XQX` (`Z:58-74`). The INFO/USTATUS/JOBATTR lines are optional. The USTATUS lines make the printer send unsolicited status on bulk IN (section 7).

### 2.2 START_DOC: type 1, 7 items (`F:635-646`, `Z:75-86`)

| id | value | meaning |
|---|---|---|
| `0x80000000` | 84 | byte length of the list |
| `0x10000005` | 1 | unknown, constant |
| `0x10000001` | 0 | unknown, constant |
| `0x10000002` | 0 = simplex, **2 = any duplex** | `XQXI_DMDUPLEX` (`H:33`, `F:642-643`) |
| `0x10000000` | 0 | unknown, constant |
| `0x10000003` | 1 | unknown, constant |
| `0x80000001` | `0xDEADBEEF` | end |

### 2.3 START_PAGE: type 3, 15 items (16 with duplex) (`F:477-508`, `Z:157-175`)

Items in this exact order:

| id | name | value |
|---|---|---|
| `0x80000000` | list length | 180, or 192 with duplex |
| `0x20000005` | copies (`F128:490`, `XQXI_COPIES`) | v1.25 and HPLIP send 1. v1.28 sends the copy count. See note (a). |
| `0x20000006` | `XQXI_DMDEFAULTSOURCE` | tray code (section 3.2) |
| `0x20000000` | `XQXI_DMMEDIATYPE` | media code (section 3.3) |
| `0x20000007` | ? | 1, constant |
| `0x20000008` | `RESOLUTION_X` | **always 600** (`F:1429-1430`) |
| `0x20000009` | `RESOLUTION_Y` | 600 |
| `0x2000000D` | `RASTER_X` | JBIG XD = bits per row, padded (section 4) |
| `0x2000000E` | `RASTER_Y` | JBIG YD = rows |
| `0x2000000A` | `VIDEO_BPP` | bits per 600-dpi pixel: 1, or 2 for "1200x600" |
| `0x2000000F` | `VIDEO_X` | real (unpadded) width in 600-dpi pixels = real bits / BPP |
| `0x20000010` | `VIDEO_Y` | rows (same as RASTER_Y) |
| `0x20000011` | `XQXI_ECONOMODE` | 0 or 1 (section 5) |
| `0x20000012` | `XQXI_DUPLEX_PAUSE` | **only present when duplex.** Values in section 6. |
| `0x20000001` | `XQXI_DMPAPER` | paper code (section 3.1) |
| `0x80000001` | end | `0xDEADBEEF` |

(a) foomatic's P1505 PPD sets `*cupsManualCopies: True`. The safe choice is to send 1 and repeat the pages yourself. Whether the firmware honors copies > 1 hasn't been verified.

### 2.4 Plane data (`F:393-448`, `F:547-605`)

The JBIG encoder output is split as follows:
- **BIH**: the first 20 bytes. These are never sent as raw data; they travel inside the item list.
- **BID**: everything after the BIH, split into segments of at most **65536 bytes**. The source comment at `F:556` reads: "Printer does strange things otherwise".

foo2xqx emits this for each segment `k`:
```
START_PLANE (type 5, count 4):
  0x80000000 = 64 (k==0) | 48 (k>0)
  0x40000000 = 0
  k==0: 0x40000002 (XQXI_BIH), size 20, payload = the 20-byte BIH
  k>0 : 0x40000003, size 4, value 1         (continuation marker)
  0x80000001 = 0xDEADBEEF
JBIG (type 7, count = segment length) + segment bytes
END_PLANE (type 6, count 0)
```

HPLIP variant (`Z:209-267`) is also accepted by the printer:
- it repeats the 64-byte BIH item list for every segment;
- it omits END_PLANE between segments;
- it sends one END_PLANE + END_PAGE at page end (`Z:180-187`: the 16 bytes `00000006 00000000 00000004 00000000`).

Use the foo2xqx form.

**Planes:** monochrome only. The plane number never appears in the stream; `write_plane(4,...)` passes it only for debug output (`F:536`).

### 2.5 End of page and end of job

- END_PAGE: `00 00 00 04 00 00 00 00` (`F:516-519`).
- END_DOC: `00 00 00 02 00 00 00 00`, then `ESC%-12345X@PJL EOJ\n` and `ESC%-12345X` (`F:649-659`, `Z:189-196`).

## 3. Code tables

### 3.1 Paper codes, raster sizes and margins

Paper codes are Windows DMPAPER values (`H:100-170`). The sizes and clips below are from `W:432-473`:
- XDIM is in **1200-dpi** pixels and YDIM in 600-dpi pixels.
- The clip is `-u ulx×uly -l lrx×lry` in the same units, removed from each side.
- For 600x600, X dimensions and X clips are halved (`W:362,369,479`).

| Paper | code | XDIM×YDIM (1200×600) | clip L/R (1200 px) | clip T/B (600 px) | ≈ margin mm |
|---|---|---|---|---|---|
| Letter | 1 | 10200×6600 | 177 / 177 | 84 / 84 | 3.7 / 3.6 |
| Legal | 5 | 10200×8400 | 177 / 177 | 96 / 96 | 3.7 / 4.1 |
| Executive | 7 | 8700×6300 | 192 / 192 | 96 / 96 | 4.1 / 4.1 |
| A4 | 9 | 9920×7016 | 176 / 176 | 84 / 84 | 3.7 / 3.6 |
| A5 | 11 | 6992×4960 | 192 / 192 | 96 / 96 | |
| B5 (JIS 182×257) | 13 | 8598×6070 | 192 / 192 | 96 / 96 | |
| Env #10 | 20 | 4950×5700 | 171 / 171 | 78 / 78 | |
| Env DL | 27 | 5200×5200 | 176 / 176 | 84 / 84 | |
| Env C5 | 28 | 7650×5408 | 170 / 169 | 80 / 80 | |
| Env B5 | 34 | 8316×5892 | 174 / 174 | 74 / 74 | |
| Env Monarch | 37 | 4650×4500 | 174 / 173 | 78 / 78 | |
| 16K 197×273 | 257 | 9306×6448 | 173 / 173 | 84 / 84 | |
| 16K 184×260 | 263 | 8692×6142 | 122 / 122 | 87 / 87 | |
| 16K 195×270 | 264 | 9212×6378 | 126 / 126 | 87 / 87 | |
| Custom | 359 | from the PS size | 2 / 2 | 84 / 84 | (`W:389,429`) |

The XDIM/YDIM values are the paper size in mm × 1200/25.4 and × 600/25.4. It is better to compute them from the PAPPL media size than to hard-code them.

Other sources:
- HPLIP's paper codes (`cupsInteger0` → `pcl_id`, sent in `0x20000001`) agree for Letter 1, Legal 5, Exec 7, A4 9, A5 11, Env10 20 and EnvDL 27. HPLIP also uses B5-ISO 357, Hagaki 43 and FLSA 258, and 96 for everything else (installed PPD lines 36-59).
- HPLIP's ImageableArea gives margins of about 12 pt left/right and 13 pt top/bottom, which is about 4.2/4.6 mm (A4: 13.56/13.5 pt).
- foomatic's P1505 entry says 4 mm on all sides.
- **Recommendation:** use about 4.2 mm on all sides (12 pt), and keep the margins **symmetric**. See the uncertainty about image placement in section 9.

### 3.2 Media source (`0x20000006`) (`H:61-65`, foomatic `foo2xqx-InputSlot.xml`)

| value | meaning |
|---|---|
| 1 | Tray 1 (priority single-sheet slot) |
| 4 | Manual feed |
| 7 | Auto (default; HPLIP hard-codes 7, `Z:163`) |

The "2=lower" in the usage text (`F:172`) is generic and not valid on the P1505.

### 3.3 Media type (`0x20000000`) (`H:67-88`, foomatic `foo2xqx-MediaType.xml`)

| value | type |
|---|---|
| 0 | Unspecified |
| 1 | Plain (default; HPLIP hard-codes 1) |
| 2 | Transparency |
| 258 | Light |
| 260 | Bond |
| 261 | Cardstock |
| 262 | Heavy |
| 263 | Rough |
| 265 | Labels |
| 267 | Envelope |
| 300 | HP Office |
| 301 | HP Multipurpose |
| 302 | HP LaserJet / Premium |
| 303 | HP All-in-one |
| 304 | HP Premium LaserJet |
| 512 | Color |
| 513 | Letterhead |
| 514 | Preprinted |
| 515 | Prepunched |
| 516 | Recycled |

The values 3/257/259/262/263 listed in `W:66-67` and the man page were copied from foo2zjs. Ignore them.

## 4. Resolution, bit depth, alignment, JBIG

**Supported modes** (`W:324-328`, foomatic Resolution xml): `600x600` and `1200x600` (the default). There is **no 1200x1200**.

**"1200x600"** is 2 bits per 600-dpi pixel (`F:1429`: `Bpp = ResX/600; ResX = 600`):
- Rasterize at 1200×600 dpi as a 1-bit bitmap. Each pair of horizontal bits is one 600-dpi pixel's 2-bit value (half-pixel / FastRes style).
- Send RESOLUTION_X=600, VIDEO_BPP=2, VIDEO_X = real_bits/2, RASTER_X = padded bit width.
- HPLIP always uses BPP=2, even from 600-dpi input. It duplicates each bit: black becomes `11`, white `00` (`M:250-260`, `Z:166-173`).
- **Recommendation:** always send BPP=2. Use 1200×600 rendering, or expand 600-dpi bits as HPLIP does.

**Width alignment** (`F:842`): the JBIG width XD is `(real_bits + 127) & ~127`, a multiple of 128 bits.
- Each row buffer has `((bytes+15) & ~15)` bytes (`F:1242`), so the padding bits are zero.
- HPLIP pads to 64 bits (`width` rounded to 32 px, ×2): `Z:153,168`. 128 is safe.

**Height:** no padding. YD = rows = VIDEO_Y = RASTER_Y (`F:491-496`).

**Bit order:** PBM convention, MSB = leftmost pixel, 1 = black (toner).

**JBIG** uses jbigkit's `jbg_enc_init(w,h,1 plane,...)` followed by `jbg_enc_options(order, options, l0, mx, my)` (`F:116-128,862-866`).
- No `jbg_enc_layers`/`lrlmax` call is made, so D = DL = 0, a single layer.
- Expected 20-byte BIH (`J:1832-1855`):

```
off 0  DL = 0
off 1  D  = 0
off 2  P  = 1
off 3  0
off 4  XD  u32 BE  (padded width in bits)
off 8  YD  u32 BE  (rows)
off 12 L0  u32 BE  = 128            (rows per stripe)
off 16 MX = 16
off 17 MY = 0
off 18 order   = 0x03  (JBG_ILEAVE 0x02 | JBG_SMID 0x01)
off 19 options = 0x5C  (LRLTWO 0x40 | TPDON 0x10 | TPBON 0x08 | DPON 0x04)
```

`JBG_DELAY_AT` (`0x100`) is also passed. It is an encoder-only flag and is masked out of the BIH (`& 0x7f`). It delays ATMOVE until the next stripe, which is what the printer expects.

The printer most likely needs a T.82 encoder with these semantics:
- 3-line template (LRLTWO);
- typical prediction (TPBON);
- adaptive template with MX = 16 (ATMOVE marker segments allowed);
- stripes ending with SDNORM (`FF 02`);
- no NEWLEN, no VLENGTH, and no comment or other markers.

You can use jbigkit (GPL, section 10) or write your own. Leave out the private DP table: DPPRIV stays clear because the default table is used.

## 5. EconoMode and density

- **Density:** PJL only, `@PJL SET DENSITY=n` with n = 1..5 (3 = medium), in the job header (`F:622`, `W:333-336`). No XQX item carries it.
- **EconoMode:** two places.
  1. PJL `@PJL SET ECONOMODE=ON|OFF` (`F:623`, `Z:62-70`).
  2. Item `0x20000011` = 1/0 (`F:497`; foo2xqx `-t` only sets `EconoMode=1`, `F:1431-1435`).

  HPLIP sends item `0x20000011 = 1` **always** and uses only PJL for EconoMode (`Z:173`), so the item's exact meaning is uncertain. Recommendation: follow foo2xqx and set both consistently.
- **RET:** `@PJL SET RET=MEDIUM` is constant in both drivers.

## 6. Manual duplex (the important part)

### 6.1 How foo2xqx does it

The printer itself pauses. The pause is driven by a per-page item, `XQXI_DUPLEX_PAUSE = 0x20000012`, documented as `// 0,1=front side; 2-3=back side` (`H:45`). There is no special chunk and no PJL command for it. All duplex modes (2, 3, 4, 5) use the same manual path; the P1505 has no duplexer:

```c
// F:1437-1445
switch (Duplex) {
case DMDUPLEX_LONGEDGE: case DMDUPLEX_SHORTEDGE:
case DMDUPLEX_MANUALLONG: case DMDUPLEX_MANUALSHORT:
    EvenPages = tmpfile();
```

Every page carries the extra item. Odd pages get 0 (page 1) or 1 (later odd pages):
```c
// F:477-480, 498-506
nitems = 15;
if (Duplex != DMDUPLEX_OFF) ++nitems;
pause = (PageNum == 1) ? 0 : 1;
...
if (Duplex != DMDUPLEX_OFF) {
    item_uint32_write(XQXI_DUPLEX_PAUSE, pause, ofp);
    if ((PageNum & 1) == 0 && EvenPages)
        SeekRec[SeekIndex].pause = ftell(EvenPages) - 4;   // remember where the value is
}
```

START_DOC also says duplex is active: `0x10000002 = 2` (`F:642-643`).

Odd pages go straight to the output in **forward** order. Even pages are buffered (`F:1257-1269`). At the end of the job:
- if the page count is odd, a blank back page is added;
- the even pages are emitted in **reverse** order;
- the first emitted back page has pause = **2**, and every later back page has pause = **3**.

```c
// F:1474-1506
if ((PageNum & 1) == 1) { ... blank_page(EvenPages); ... SeekIndex++; }
// Write even pages in reverse order
for (i = SeekIndex-1; i >= 0; --i) {
    fseek(EvenPages, SeekRec[i].pause, 0L);
    if (i == SeekIndex-1) pause = be32(2);
    else                  pause = be32(3);
    fwrite(&pause, 1, sizeof(DWORD), EvenPages);
    fseek(EvenPages, SeekRec[i].b, 0L);  /* copy page bytes to stdout */
}
```

Everything goes into **one job / one `,XQX` document**. The `0x20000012 = 2` page is where the printer stops. It waits, with the Go/attention light, for the user to reload the stack and press the button, then prints the backs. This matches the Windows behavior the user described.

### 6.2 Rotation (`F:1253-1260`)

- **Long-edge** (`-d2`, or `-d4` MANUALLONG): even pages are rotated 180°. In code, the page buffer is reversed and each byte is bit-mirrored (`Mirror1` for 1-bit data).
- **Short-edge** (`-d3`/`-d5`): no rotation.

When rotating, rotate the whole padded buffer, as foo2xqx does (`rotate_bytes_180(buf, buf+bpl16*h-1, Mirror1)`). The zero padding then moves to the left. This is safe because padding is blank. Rotate *before* JBIG encoding.

### 6.3 Physical model (why this order works)

1. The tray feeds the top sheet first. The output bin is face-down.
2. After the odd pass, the bin stack has the sheet with page 1 at the bottom and the sheet with the last odd page on top. The blank sides face up.
3. The user lifts the stack and puts it in the tray **as is**: no flipping, no rotating.
4. The tray feeds the last odd sheet first, so its back must be printed first. Hence the reverse order N', ..., 6, 4, 2, with a blank page first if the count is odd.
5. Those sheets now feed with their original top edge trailing. For long-edge binding that means a 180° rotation of the backs; for short-edge it doesn't.

Tell the user on screen: "Remove the printed stack from the output bin, place it in the tray without flipping or rotating, then press Go."

### 6.4 Sequence to emit for N pages (manual duplex)

```
START_DOC(0x10000002=2)
p1(pause=0), p3(1), p5(1), ... last odd(1)
[if N odd: blank(pause=2) then backs below with 3]
backs in reverse order: p_maxEven(pause=2 unless a blank already took 2), ..., p4(3), p2(3)
END_DOC
```

### 6.5 Bugs in foo2xqx not to copy

- **Blank padding page.** `blank_page()` (`F:1276-1295`) calls `pbm_page()` while `PageNum` is still odd, so `SeekRec[].pause` is never recorded for the blank page. It stays 0 (static zero-init).
  - Consequence 1: the final loop writes `be32(2)` at **offset 0** of the even-page temp file. That corrupts the first buffered page's START_PAGE chunk type from 3 to 2 (END_DOC).
  - Consequence 2: the blank page itself carries pause 1, or 0 when N = 1.
  - Implement the intent instead: the blank page is the first back page and gets pause = 2.
- **Fixed-size array.** `SeekRec[2000]` caps the job at about 4000 pages.

### 6.6 Uncertain

No reference states what the firmware does with 0 vs 1 on front pages, or 2 vs 3 on back pages. It also isn't documented whether `0x10000002 = 2` is required for the pause. Copy foo2xqx's values exactly. Test with a 3-page job: you should get front 1, front 3, a pause, then the blank back and the back of page 2. foomatic exposes Duplex for the P1505 (`PPD/HP-LaserJet_P1505.ppd:264`). HPLIP ignores duplex for this printer (`LJZxStream` has no pause item).

## 7. Status over USB

**IEEE-1284 Device ID:** USB printer-class `GET_DEVICE_ID`. That is bmRequestType `0xA1`, bRequest 0, wValue 0, wIndex 0 (interface 0, alt 0). The first 2 bytes are a big-endian length that includes themselves.
- Live reading from this unit:
  `MFG:Hewlett-Packard;MDL:HP LaserJet P1505;CMD:HBS,PJL,ACL;CLS:PRINTER;DES:HP LaserJet P1505;FWVER:20080415;`
- `FWVER` matches the firmware build date in the installed `.fw` (section 8). PAPPL's `papplDeviceGetID()` does this.
- The ID carries no paper, door or jam fields.

**PJL status on bulk IN:** HPLIP status-type 8, which is what `models.dat` gives the P1505 (`status-type=8`).
- Send `ESC%-12345X@PJL INFO STATUS \r\nESC%-12345X`, then read up to 1024 bytes from bulk IN with a timeout of about 5 s (`S:1348-1360`).
- Parse `CODE=nnnnn`.
- Mapping (`S:1162-1340`):

| Code | Meaning |
|---|---|
| 10001 | idle / online |
| 10002 | offline |
| 10003 | warming up |
| 10006 | low toner |
| 10023 | printing |
| 41xyy | load paper / out of paper (x = tray) |
| 42xxx, 44xxx | jam |
| 40xxx | operator intervention, e.g. door open |
| 50xxx | hardware error |

- The job header's `USTATUS DEVICE/JOB/PAGE/TIMED=30` lines make the printer push `@PJL USTATUS ...` blocks, each ended by FF (0x0C), during the job. **Drain bulk IN while printing.** This is plausibly where the duplex-pause state appears; not verified.
- HPLIP notes the query "will error if printer is busy printing" (`S:1350`).
- The exact codes for door-open and paper-out on this model have not been verified; capture them on hardware.

**Port status:** the USB printer-class `GET_PORT_STATUS` (bRequest 1, 1 byte: 0x20 paper empty, 0x10 selected, 0x08 not-error) is standard. Its reliability on the P1505 is unknown.

## 8. Firmware

**File:** `/usr/share/hplip/data/firmware/hp_laserjet_p1505.fw.gz` is 166020 bytes gzipped and **248683 bytes** decompressed. It is a complete, ready-to-send stream:

```
0x00000  "\x1b%-12345X@PJL ENTER LANGUAGE=ACL\r\n"          (35 bytes)
0x00023  00 AC C0 DE  00 03 CB 30                         ACL magic + BE length 0x3CB30 = 248624
0x0002B  "20080415" + ARM ELF image ...                   payload (length counted above)
...      "HP LaserJet P1505\0" "20080415\0" "0x3F17\0" + 7 bytes (checksum/trailer)
0x3CB62  "\x1b%-12345X"                                   (last 9 bytes)
```

This is exactly the format foo2zjs builds with `arm2hpdl` from a `"20…"`-prefixed `.img`:
- `ENTER LANGUAGE=ACL` line, then `00 AC C0 DE` + BE32(size−8), then the data, then UEL (`A:181-203,246-247`).
- So the HPLIP file needs **no wrapper**.

**Upload:**
- Decompress the file and write all bytes unchanged to **bulk OUT `0x01`**, in one transfer or any chunking. foo2zjs uses `cat sihpP1505.dl > /dev/usb/lpN` (`L:226`). HPLIP uses `writePrint(gzip.open(f).read())` over the hpmud print channel, which is raw bulk (`/usr/share/hplip/base/device.py:2674-2678`).
- There is no handshake. foo2zjs only sleeps 3 s before the upload, after hotplug (`L:262`).
- **Recommendation:** after uploading, poll the device ID about once a second until `FWVER` appears (timeout about 30 s), then send jobs.

**Detection (`L:279-289`):**
- Read the device ID. If `MDL` matches "HP LaserJet P1505" and there is **no `FWVER`** field, upload. If `FWVER` is present, firmware is already loaded.
- The firmware is RAM-resident and must be reloaded after every power cycle.

**Conflict with HPLIP:** HPLIP's udev rule on this system (`/usr/lib/udev/rules.d/*hpmud*`) already runs `hp-config_usb_printer` on plug, which downloads the firmware. Your app should tolerate that (FWVER is already present) and not upload twice.

**`models.dat` mismatch:** the `[hp_laserjet_p1505]` section says `usb-pid=4117`, but the device and the firmware trailer say `0x3F17`. Match on `03f0:3f17` and/or `MDL`.

## 9. Encoder pseudo-code (one job, N pages)

```
job(pages[1..N], opt):        # opt: paper, media, source, density 1-5, econo, duplex {off,long,short}
  send PJL header (2.1) with DENSITY/ECONOMODE
  send ",XQX"
  START_DOC: [0x80000000=84, 0x10000005=1, 0x10000001=0,
              0x10000002=(duplex?2:0), 0x10000000=0, 0x10000003=1, END]
  if !duplex:
      for p in pages: emit_page(p, pause=None)
  else:
      for i odd:  emit_page(pages[i], pause = (i==1 ? 0 : 1))
      backs = [pages[i] for i even] + ([blank] if N odd)
      first = true
      for b in reverse(backs):
          if duplex==long: b = rotate180(b)
          emit_page(b, pause = first ? 2 : 3); first = false
  send 00000002 00000000, "ESC%-12345X@PJL EOJ\n", "ESC%-12345X"

emit_page(bitmap 1-bit, 1200x600 dpi, clipped to printable area, symmetric margins):
  realbits = width in bits; xd = (realbits+127)&~127; zero-pad rows to xd
  jbig = T82_encode(xd, h, DL=0,D=0,P=1, L0=128, MX=16, order=0x03, options=0x5C, DELAY_AT)
  bih = jbig[0:20]; bid = jbig[20:]
  n = 15 + (pause!=None)
  START_PAGE count n:
     0x80000000=n*12, 0x20000005=1, 0x20000006=source, 0x20000000=media, 0x20000007=1,
     0x20000008=600, 0x20000009=600, 0x2000000D=xd, 0x2000000E=h, 0x2000000A=2,
     0x2000000F=realbits/2, 0x20000010=h, 0x20000011=econo,
     [0x20000012=pause], 0x20000001=paper, 0x80000001=0xDEADBEEF
  for k, seg in enumerate(chunks(bid, 65536)):
     START_PLANE count 4: 0x80000000=(k?48:64), 0x40000000=0,
                          k==0 ? item(0x40000002, size 20, bih) : 0x40000003=1, END
     JBIG count len(seg) + seg
     END_PLANE count 0
  END_PAGE count 0
```

Uncertain points:
- **Image placement.** The protocol carries no X/Y offset, and foo2xqx's `-L` "logical clipping" is computed but never written (`F:93-98`). foo2xqx and HPLIP both crop symmetric margins, which suggests the engine centers or self-positions the VIDEO_X×VIDEO_Y image. Keep margins symmetric and verify alignment with a test page.
- `-X` ExtraPad (`F:91`) is parsed but unused. No extra padding is needed.

## 10. Licensing notes

- foo2zjs/foo2xqx is GPLv2+ (`F:22-26`). Only the protocol facts are taken here.
- The bundled jbigkit is GPLv2+. If you link libjbig, the app must be GPL-compatible. Otherwise write your own T.82 encoder. The JBIG arithmetic-coding patents have expired.
- HPLIP hpcups (`Z`) is BSD-style (HP copyright header).
- HPLIP's JBIG encoder for this path is in HP's proprietary plugin (`hp_encode_bits_to_jbig`, `M:139`). Don't depend on it.
- The firmware is HP-proprietary. Load it at runtime from the HPLIP-installed path and don't redistribute it.

## 11. Hardware results (2026-10-20, this unit)

- 1200x600 (VIDEO_BPP=2 with real half-pixels): prints correctly.
- VIDEO_BPP=4 (2400x600 layout, RASTER_X = 4 x VIDEO_X): printed garbage, even though `@PJL INFO CONFIG` lists BITSPERPIXEL 1/2/4. Not offered.
- Manual duplex pause values 0/1/2/3 as in section 6: works; printer waits for Go with the attention light.
- `@PJL USTATUS JOB = ON` in the header: printer sends JOB END after the last back page.
- `@PJL INFO SUPPLIES` reports PercentRemaining / PagesRemaining even with a non-HP cartridge.
- Calibration on the author's unit: image landed 1 mm right and 1 mm high, corrected with print-offset-x=-10, print-offset-y=10. Other units will differ; the driver default is 0.
- `@PJL SET RET=OFF|LIGHT|MEDIUM|DARK` (2026-10-24): same text, 0.25/0.5 pt diagonals and circles printed at all four levels looked identical at 1200x600. The engine probably ignores RET for host-rendered data. The driver keeps MEDIUM.
- Halftoning (2026-10-24): Floyd–Steinberg printed photos as grain, and a 45° 106 lpi clustered-dot screen left white blotches. A 0° 100 lpi screen was clean, but its grid showed like woven fabric. Green noise on 600-dpi cells with a 1.4 midtone curve was preferred.
- CUPS 2.4 does not pass vendor attributes such as `resolution-enhancement` from an IPP Everywhere queue to the app. Set them as printer defaults in the app instead.
