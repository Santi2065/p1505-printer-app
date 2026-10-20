# p1505-printer-app

A from-scratch driver for the **HP LaserJet P1505**, packaged as a
[PAPPL](https://www.msweet.org/pappl/) Printer Application: the driver model
CUPS 3 requires. It has no PPDs, no CUPS filters, and no HP proprietary code
apart from the printer's own firmware, which you supply.

It speaks the printer's native **XQX** protocol (ZjStream variant + JBIG) and
adds what the Linux drivers never had for this model:

| | HPLIP / hplip-printer-app | this driver |
|---|---|---|
| Resolution | 600 dpi (each dot sent twice) | **FastRes 1200** (real 1200x600 half-dots) |
| Duplex | none on this model | **manual duplex with the Go button**, as on Windows |
| Toner level | no | **yes**, `PercentRemaining` via PJL (also works with non-HP cartridges) |
| Printer status | limited | paper out, jam, door open, toner low/empty, offline |
| JBIG compression | HP proprietary plugin | jbigkit (free) |
| Position calibration | no | `print-offset-x/y`, tenths of mm |
| Gray / photos | n/a | Floyd–Steinberg error diffusion; text stays solid black |
| Runs as | root (for the plugin) | your user |
| Printers supported | ~3000 HP models | **P1505 only** (see "Other models") |

hplip-printer-app is the right choice for any other HP printer. This
project is for getting everything out of the P1505.

## Status

Tested on paper with one P1505 (USB `03f0:3f17`, firmware `20080415`), Fedora 45,
PAPPL 1.4.9, CUPS 2.4: simplex at 600 and 1200, manual duplex (long edge, odd and
even page counts), calibration, toner and status reporting, and firmware loading
after power-on.

## How it works

```
PDF/PS ──gs (8-bit gray, 1200x600)──┐
PWG raster (from CUPS/IPP) ─────────┤→ crop + offset → error diffusion → XQX/JBIG → USB
                                    │   (duplex: fronts, pause, backs reversed + rotated)
```

- `src/xqx.c`: XQX stream encoder. Its JBIG output is byte-identical to
  HPLIP's for the same page (`make test` checks this against `ref/hplip-Normal.bin`).
- `src/main.c`: the Printer Application, built on PAPPL. It handles options,
  manual duplex, firmware upload, and PJL status and toner reporting.
- `docs/xqx-spec.md`: the protocol, with sources and results measured on hardware.

### Manual duplex

Choose two-sided printing. The printer prints the fronts, then stops with the
attention light blinking, and a desktop notification tells you what to do.
Put the printed stack back in the tray **without flipping or rotating it**, then
press the Go button. The backs print in the right order and orientation. If the
page count is odd, the last sheet gets a blank back. The notification closes
by itself when the printer reports that the job has ended.

## Requirements

- PAPPL ≥ 1.4 (`pappl-devel`), jbigkit (`jbigkit-devel`), libusb, CUPS
  development headers, gcc and make.
- Ghostscript (`gs`), to render PDF and PostScript input.
- `notify-send` and `gdbus`, both optional, for the duplex notification.
- **The P1505 firmware.** The printer loses it at every power-off, and the driver
  uploads it when needed. It is HP-proprietary and **not included**. Install
  HPLIP's plugin once (`hp-plugin`), then copy
  `/usr/share/hplip/data/firmware/hp_laserjet_p1505.fw.gz` to
  `~/.local/share/p1505-printer-app/`. After that, HPLIP can be removed.

Fedora:

```sh
sudo dnf install pappl-devel jbigkit-devel libusb1-devel cups-devel gcc make ghostscript
```

## Build and install

```sh
make && make test          # encoder check against the HPLIP reference
tests/e2e.sh "$(mktemp -d)"  # full app against a fake printer: order, pauses, rotation
make install               # -> ~/.local/bin/p1505-printer-app

# USB access for your user
sudo cp contrib/60-p1505-printer-app.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger

# Run as a user service; linger keeps it up while you are logged out
cp contrib/p1505-printer-app.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now p1505-printer-app
loginctl enable-linger "$USER"

# Add the USB printer
p1505-printer-app autoadd
```

The web interface is at <http://localhost:8632>. It listens on localhost only.

### Using it from CUPS 2.x

CUPS 3 discovers Printer Applications by itself. With CUPS 2.x, add a
permanent IPP Everywhere queue, and turn off CUPS' own scaling. Otherwise
CUPS shrinks every page to fit its idea of the printable area:

```sh
p1505-printer-app printers     # shows the printer's ipp://localhost:8632/ipp/print/... URI
sudo lpadmin -p HP_P1505 -E -v <that URI> -m everywhere -o print-scaling-default=none
```

Add `-o printer-is-shared=true` to share it on the LAN. On Fedora,
`system-config-printer-udev` creates a generic USB queue every time the printer
powers on. Run `sudo systemctl mask configure-printer@.service` to stop that.

## Options

| IPP attribute | Values | Effect |
|---|---|---|
| `printer-resolution` | 600x600, **1200x600** | FastRes 1200 is the default |
| `sides` | one-sided, two-sided-long-edge, two-sided-short-edge | two-sided = manual duplex |
| `print-darkness` | -100..100 | PJL `DENSITY` 1..5 |
| `print-quality=draft` | | turns on EconoMode |
| `economode` | off, on | toner saving |
| `resolution-enhancement` | off, light, medium, dark | PJL `RET` |
| `print-offset-x` / `-y` | -40..40 (tenths of mm) | moves the image: +x right, +y down |
| `media-type`, `media-source` | plain, labels, envelope, cardstock... / auto, by-pass-tray, manual | |

You can set defaults in the web interface, or with
`p1505-printer-app modify -d <printer> -o economode-default=on`.

### Calibrating the position

Print `ref/calibration.pdf`, which is A4, and measure from the paper edge to the
solid frame. Every side should be 10 mm. If the frame is 11 mm from the left and
9 mm from the top, move it 1 mm left and 1 mm down:

```sh
p1505-printer-app modify -d <printer> -o print-offset-x-default=-10 -o print-offset-y-default=10
```

The printer cannot print within about 4 mm of the edges, and an offset uses up
part of that margin on one side.

## Other models

The P1005, P1006, P1007, P1008 and P1009 use the same XQX protocol and the same
kind of firmware upload, so they will likely work after small changes: the USB
ID, the device ID match, and the firmware file name. Untested. Reports and
patches are welcome.

## Credits

The protocol facts come from reading [foo2zjs](https://github.com/koenkooi/foo2zjs)
(Rick Richardson) and [HPLIP](https://developers.hp.com/hp-linux-imaging-and-printing),
and were confirmed on hardware. No code was copied from either. Built on
[PAPPL](https://github.com/michaelrsweet/pappl) (Michael R Sweet) and
[jbigkit](https://www.cl.cam.ac.uk/~mgk25/jbigkit/) (Markus Kuhn).

Not affiliated with HP. "HP" and "LaserJet" are trademarks of HP Inc.

## License

GPL-2.0-or-later (see `LICENSE`). jbigkit, which this links against, is
GPL-2.0-or-later.
