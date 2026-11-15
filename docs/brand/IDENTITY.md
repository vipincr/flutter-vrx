# vrx identity

The vrx wordmark, its mark, the palette and the system fonts, and every place
they appear. Approved by the owner on 2026-11-14 (the design canvas: "vrx
identity and boot splash", a private claude.ai artifact the owner holds).

**This directory is the identity package (plan 0024)**, the one source of every
name, identifier, colour, logo and face a person sees on a vrx device:
`brand.toml` (names, identifiers, palette, font roles), this document,
`logo/<shape>-<ground>.svg` and `fonts/` with their licences. Every surface is
built from it: the boot splash, the bootloader logo, os-release, the hostname,
the USB gadget, Matter, the file names, the kernel release, the UIs. No recipe,
applet or document carries a hand-made copy of the logo or spells a brand
colour. vrx's logo SVGs are written from the geometry below by
`scripts/vrx-logo.py`; every raster is rendered from the SVGs by `vrx-brand`
(`meta-vrx-distro/recipes-core/vrx-brand`), the same tool a client's package
goes through.

A white-label client's identity replaces this package on its own
`brand/<client>` branch of every vrx repo (`scripts/brand-apply.sh`,
docs/howto/WHITE_LABEL.md); the product keeps "powered by vrx" on its About
screen, in os-release (`VRX_POWERED_BY`) and as the kernel release's `-vrx`.

Every vrx repo carries the active identity in its own `docs/brand/`, written by
`scripts/brand-sync.sh` from this package and stamped with its revision;
`scripts/brand-sync.sh --check` fails on a copy that has drifted.

## The mark

The v is a prism of three strata, seen from above: three layers narrowing to a
point, the way an operating system narrows from the applets through the
system to the kernel. The tip is the kernel, and it carries the one accent.

The wordmark is one strip of material folded twice. The v's top layer, the
r's shoulder and the x's back stroke share a single top line, parted by two
seams; the r's stem and the x's strokes are where the strip turns down. One
light rule colours all of it: faces that lie flat are paper, faces that turn
down are shade.

There are no curves. Every edge is a straight line, which is what lets a
bootloader, a 1-bit framebuffer and a 16 px favicon draw it as exactly as
print does.

## Construction

Units; the wordmark is 127 x 40, the mark (the v alone) 48 x 40.

| Element | Value |
|---|---|
| Cap height | 40 |
| The v's bands | three, 9 tall, 6.5 apart (at 0, 15.5, 31) |
| The v's sides | fall 0.6 units per unit of height; the tip is a triangle from 18.6 to 29.4 |
| The r | stem 12 wide (53 to 65), square shoulder 20 wide and 12 tall |
| The x | strokes 13 wide, falling 0.6 per unit, from 90 to 127 |
| Seams | 5 units, between the v and the r and between the r and the x |
| Facet split | the v's bands divide at x = 24; left is shade, right is face |

The coordinates themselves live in `scripts/vrx-logo.py` (`V` and `RX`); this
table explains them.

## Variants

| Ground | Colours | For |
|---|---|---|
| `dark` | shade, paper, core, signal on ink | boot splashes, GRUB, the UI, print |
| `light` | slate, ink, core, signal on paper | light grounds |
| `mono` | one colour | 1-bit framebuffers, e-paper, etching, the smallest sizes |

The package holds each as `logo/{mark,wordmark}-<ground>.svg`; the device
carries them in `/usr/share/vrx/brand` (package `vrx-brand`) beside
`brand.json`, the facts the UIs and vrxhost read.

## Palette

| Name | Hex | Role |
|---|---|---|
| Ink | `#0B0D10` | the ground of every dark surface |
| Shade | `#9AA3B2` | faces that turn down, on ink |
| Paper | `#EEF1F4` | faces that lie flat, on ink; text on ink |
| Slate | `#545C6A` | faces that turn down, on paper |
| Mist | `#8B93A1` | secondary text on ink |
| Track | `#1C2129` | an unfilled progress track on ink |
| Signal | `#5B8DEF` | the one accent: the kernel tip's lit face, progress |
| Core | `#385794` | the kernel tip's shaded face (Signal x 0.62) |

The accent appears once per surface: the tip of the logo, or a progress line,
never two competing blues.

## Typography

| Role | Face | Files |
|---|---|---|
| UI | Fira Sans | Light, Regular, Medium (the three weights the UI uses) |
| Console, terminal, logs, code | Fira Code | Regular, Bold (the editor's keyword emphasis) |
| Math symbols (fallback) | Noto Sans Math | Regular |
| Music symbols (fallback) | Noto Music | Regular |

About 2.6 MB in all, in one package (`vrx-fonts`), with fontconfig making them
the defaults: `sans-serif`, `system-ui` and `serif` resolve to Fira Sans,
`monospace` to Fira Code, and both fall back to the two symbol faces, so a
math operator or a note value renders inside any text.

Why these. A system font has constraints a design font does not: a licence
that allows shipping it inside firmware (SIL OFL 1.1 for all four), hinting
that holds at small pixel sizes on low-DPI panels (the Brain is 178 ppi at
854 x 480), coverage of Latin, Greek and Cyrillic, and a true pair so the UI
and the console read as one system. Fira Sans was drawn for Firefox OS, an
operating system's UI on small phone screens, and Fira Code is its monospace
sibling with programming ligatures. JetBrains Mono, Cascadia Code and Iosevka
were weighed as the mono and lost on pairing (none has a UI sans of its own);
Hack is the strongest no-ligature option and stays the named alternative if
ligatures ever cost more than they give. Roboto and Liberation, the faces
before, are gone.

Ligatures: on in the terminal, off where text is shown verbatim (logs, code
listings, file names), because `!=` drawn as `≠` in a log is a lie about the
bytes.

Math and music are part of the system, not an add-on: the evrx-rt runtime
writes flats, sharps, the diminished ring and the major-seventh triangle
inside running text (`VRX_MUSIC_*` in its `ui_icons.h`), and vrx keeps every
such character renderable everywhere.

## Surfaces

| Surface | How it gets the identity | Status |
|---|---|---|
| Boot splash (psplash) | `psplash_%.bbappend` renders the wordmark, a 120 x 2 px hairline progress bar on an invisible frame, and psplash's colour header from `vrx-brand-native`; the logo's size is `vrx-splash-policy` (a tenth of `VRX_PANEL_HEIGHT`). vrx's pre-init (`meta-vrx/files/vrx-preinit.sh.in`) starts it before systemd, the kernel's text stays behind it (`quiet`), and `vrx-splash` drives it: the bar every second (systemd's progress or the time against the device's last boot, whichever is further), the end when systemd reports the boot finished, and Q to end it early | built |
| SVG kit | `vrx-brand` installs every variant in `/usr/share/vrx/brand` | built |
| System fonts | `vrx-fonts` in `packagegroup-vrx`, every image | built |
| Flutter UI (vrx-linux) | `vrx_kit` theme names Fira Sans, applets' mono text Fira Code; the layout gate measures in the same files | in progress |
| LVGL UI (evrx-rt, `lvgl-vrx`) | `lv_font_conv` from the same TTFs, Noto Sans Math and Noto Music chained as `lv_font_t.fallback` | owed |
| u-boot logo | the mark alone (the strata), shown over u-boot's console where its own badge would be, the way Linux shows its penguin: `vrx-uboot-logo.bbclass` renders it with `vrx-brand boot-logo bmp` over `drivers/video/u_boot_logo.bmp`. Bootloaders and kernels show the mark, never a copy of the full splash, so nothing has to match psplash's geometry | built (pwsh7) |
| Kernel boot logo | the same mark as the kernel's clut224 logo (`vrx-brand boot-logo ppm`) | owed: the logo is a tracked file in each family's kernel tree, and rewriting it marks the version `-dirty`; it needs a per-family logo patch |
| GRUB (am62x) | a `prism` PNG background rendered by `vrx-brand-native` | owed |
| Kernel console | a PSF built from Fira Code | owed |

## Rules

- The logo is always lowercase, always the geometry above. Never retype it in
  a font, stretch it, outline it, add a gradient, or put it on a busy image.
- One accent per surface.
- A surface that needs a new size or format adds a command to `vrx-brand`
  and renders at build time; it never commits a rendered file.
- The fonts are the four above. A new face needs the owner's decision and an
  entry in this document; a new weight needs a screen that uses it.
