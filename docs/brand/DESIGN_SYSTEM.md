# The vrx design system: using it, changing it, fixing a UI flaw

How every vrx UI gets its look, how a brand overrides it, and what to do when
a screen looks wrong. The decisions behind it are plan 0025 (D1 to D15). This
file is carried into every vrx repo as `docs/brand/DESIGN_SYSTEM.md` by
`scripts/brand-sync.sh`; edit it here, in the vrx repo.

## Where the look comes from

One source: evrx-rt's `standard/vrx-ui-standard.toml`. It holds vrx's values
for every visual token (colour slots and schemes, type roles, spacing, radii,
borders, control and icon sizes, elevation, gradients, motion), the component
specs (each component's parts, variants, sizes and states, written as
references to those tokens), each token's realisation per display tier
(full, light, grey, epaper), and the usability floors.

A brand changes values, never structure, in its package's `design.toml`
(`brand/design.toml` in vrx, empty on vrx's own branches). It may set any
value the standard defines; it cannot add a component, a state or a role,
and it cannot set the family names (those are read from its font files) or
the touch minimums.

`scripts/lib/design_build.py` in evrx-rt resolves the standard with the
package's design and generates everything both kits compile in:

| Step | Writes |
|---|---|
| `resolve` | the resolved standard, and the log of every value a floor moved (always runs) |
| `schemes` | the colour schemes, including the brand's seeded scheme 21 |
| `c-standard` | the LVGL kit's headers (`ui_standard.h`, `ui_design.h`, `ui_class.h`) |
| `type-scale` | `ui_type.h`: each role's size at each panel density |
| `lvgl-fonts` | the bitmap faces, one per size and weight a role reaches |
| `applet-glyphs` | applets' own glyph sets (sizes outside the type band) |
| `splash` | the runtime splash per panel, from the package's mark |
| `icon-fonts` | the icon fonts per size |
| `design-site` | the generated design pages (`docs/brand/design/`) |

vrx-linux's Flutter kit reads the same resolved standard through its own
`scripts/brand-hook` (the vendored standard, the type table, `cargo xtask
ui-standard`, `vrx glyphs`). Nothing is parsed on a device: every token is a
compile-time constant, and the one run-time choice is the display tier,
picked once from the display facts.

## Running it

```bash
cd <evrx-rt>
python3 scripts/lib/design_build.py                 # incremental: only what changed
python3 scripts/lib/design_build.py --full          # everything, stamps ignored
python3 scripts/lib/design_build.py --check         # the gate: fail if a committed output is stale
python3 scripts/lib/design_build.py --only lvgl-fonts
python3 scripts/lib/design_build.py --package <identity package>   # a brand's look
```

Incremental by default: each step records a hash of exactly the sections and
files it reads and reruns only when they change, and an output that comes
out byte-identical is not rewritten. A colour change reruns the schemes and
the pages, never the fonts. Each repo's `scripts/brand-hook <package>` runs
its part the same way; `scripts/brand-apply.sh` (vrx) calls them all when it
makes a client's `brand/<id>` branches.

## Type: the band around each device's median

Type sizes are points, written at the reference handheld median (9.9 pt
body). A device draws a role at `size_pt + (its median - 9.9)`, snapped to
0.5 pt and held to its band of median +/- 3 pt (13 sizes). The median comes
from the panel's viewing class (handheld, desk, wall, by diagonal) and a
visual angle on the face's own x-height; the band never goes below the
16-arcminute legibility minimum. A role outside the band is moved to its
edge and logged. A figure bigger than the band (a clock's numerals) is the
applet's own glyph set: declared in its manifest (`[[applet.glyphs]]` in
vrx-linux, `glyphs.toml` in evrx-rt), cut to exactly its characters, and
counted in its own memory budget.

Viewing class scales what is seen, never what is touched: the 9 mm touch
minimum and the 6.4 mm along-row minimum hold on every panel at any
distance, and a hit target is the larger of the minimum and the drawn size.

## Usability first: the floors

The resolver holds vrx's values and every brand's to the floor rows in the
standard (`[floor.<name>]`): contrast pairs, focus visibility, touch sizes,
the type band, motion per tier, and that every interactive state looks
different from rest on every kind of panel. A value that breaks a floor is
moved as far as the floor needs, the foreground before the ground, and the
move is logged with the floor's `catches` and `source`. A structural break
is refused. Where no floor is touched, the design's value stands exactly.

## When a screen looks wrong: fix it in the system

A UI flaw found on the bench, in a frame dump or in a layout test is fixed
in exactly one of three places, never in one widget:

1. **A token value**, when vrx's default is wrong (a radius, a spacing, a
   size, a slot).
2. **A component spec**, when a state or a part is missing or points at the
   wrong token (a banner's message type, a missing compact size).
3. **A new floor row**, when it is a class of mistake any brand could make
   (a state that looks like rest on e-paper, a focus ring lost on a fill).
   Give it `kind`, the tokens it binds, its limit, `catches` (the defect in
   one line) and `source` (the test, the board, the date).

Then run `design_build.py` (incremental), and the change reaches both kits
and every brand. Widget code cannot take a hand-set look: G085 refuses
literal colours, sizes, radii, durations and font names there.

### Promoted or local: every UI commit says which (G086)

A commit that changes UI code carries a trailer saying where the fix went:

```
Design-system: promoted component.banner
Design-system: promoted floor.state_distinct
Design-system: local: the clock's dial hand angle is one screen's logic
```

`promoted <key>` names the standard key, floor or component the fix landed
in (the hook checks it exists); `local: <why>` says why the change is one
screen's behaviour, not a look. Most UI changes are logic and say `local`;
anything that changes how something looks is promoted, or says why not.

Worked example (2026-11-15): Part D's e-paper renders showed a selected
segment drawing exactly like rest, because the selected fill and the surface
both map to white on a 2-level panel. The fix was a floor row
(`floor.state_distinct`, catches "a state that looks like rest on a panel
that cannot show the difference") plus per-tier state realisations in the
specs (selected inverts on e-paper). Every brand got it; no widget changed.

## A brand's look

A package's `design.toml` sets values only. A minimal one sets the colour
seed; a full one (EverOS, `tmp/brands/tools/everos-design.toml` in vrx) sets
both palettes' slots, type weights and sizes, radii, elevation and
component properties. `vrx-brand check <package>` validates the package;
`design_build.py --package <package> --only resolve` shows what the floors
moved. See [WHITE_LABEL.md](WHITE_LABEL.md) for the rest of the package.
