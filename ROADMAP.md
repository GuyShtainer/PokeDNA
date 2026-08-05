# PokeDNA — Roadmap & Known Issues

The running list of what's planned and what's currently rough. PokeDNA is at **v2.0.0** and
actively developed; this is an honest backlog, not a promise of dates or order. If something here
matters to you, feel free to open an issue or a PR.

## Known issues / current bugs

- **A fresh clone doesn't build until you run the art generators.** Some modules have weak
  fallbacks and degrade to text layouts, but `box_oam.c` and the icon/type/item modules `#include`
  a generated header unconditionally, so a clean checkout stops at `mon_icons_oam.h: No such file
  or directory`. See *Graphics assets* in the [README](README.md#graphics-assets-are-generated-locally-not-committed).
  Making every art module optional the way the bag already is would be a welcome contribution.
- **The map viewer can't zoom out past the affine tile budget.** A full-region zoom would need 428
  distinct tiles where the hardware affine mode allows 256, so the widest zoom is capped.
- **Big builds can fail to load from SD on some cards.** This is a *copy* problem, not a size
  ceiling — the "ROM too big" theory was investigated and disproved. If the flashcart hangs at
  "Load game", re-copy the `.gba` and eject the card safely; a NOR-loaded build is unaffected.
- **The map screen's undo is dead code.** The snapshot is taken but nothing outside the host tests
  ever restores it.
- **NIDORAN♀/♂ display as `NIDORAN?`.** The Gen-3 gender symbols have no glyph in the 5×7 face, so
  they decode to `?`. The bytes written to the save are correct; only the on-screen rendering is
  affected, and it matches what a Pokémon caught in the real game already looked like here.

## Planned — editor coverage

- **The full legitimacy check.** The current legality pass is explicitly structural only. The
  remaining work is move legality against per-origin-game learnsets, PID↔nature/gender/shininess
  consistency, met location/level/ball plausibility, evolution stage at met level, ability index,
  and EV/IV bounds including the 510 total.
- **Gen 1 / Gen 2 support via the boxes**, for the Pokémon that can legitimately travel forward.
  Needs its own research pass: which species and moves actually transfer, what the RBY/GSC save
  layout requires (different checksums, no personality value, different name encoding), and whether
  "transfer" means converting to a real Gen-3 record or viewing only.
- **More editable flags & counters** — keep expanding the curated set.
- **Off-map places** (Sky Pillar and friends) are rect-only today, so the map cursor can't select
  them; Mirage Island needs an RNG pre-image write rather than a direct one.

## Planned — graphics & UI

- **Pokéblock case chrome**, the same treatment the bag and trainer card already got: the game's own
  screen art, per game, generated locally from the decomps.
- **The rest of Emerald's real pickup animation.** The *transparency* now matches retail exactly
  (per-icon `BLDALPHA_BLEND(7, 11)` against all layers, holders opaque, hand solid). The *motion*
  does not: retail's grab is 17 frames of pure movement — the hand descends 8 px at 1 px/frame in
  an OPEN pose, switches to a FIST at the bottom where the Pokémon detaches and starts riding the
  cursor, then both rise 8 px together. Ours is still a static hold. Needs two extra hand poses
  (open, reach) added to `tools/gen_hand.py`, which is why it did not land with the rest.
- **A two-column PC box view.**
- **Smoother rendering** — keep moving screens off full-screen repaints. (The PC box flip is done:
  it used to wipe the screen to black on every L/R.)

## Planned — companion tooling

- **rec2mp4 link-up.** The Records screen exports a `.rec` + `.txt` pair that the companion PC tool
  **rec2mp4** replays into an MP4. The on-cart text and the README now name it; a link lands here
  once that project is published.

## Testing / validation

- **Cross-game bank is best-tested Emerald → Emerald.** Keep exercising the bank and cross-game
  transfers with Ruby/Sapphire and FireRed/LeafGreen, including moving Pokémon *between* games.
- Anything touching the SD card, the RTC, or a destructive save operation is not done until it has
  been run on real hardware. The emulator does not model the flashcart's SD path.

## Shipped since v1.0.0

Kept here so the backlog above reads as what's left, not as the whole story. v2.0.0 added: the
overworld **map viewer** reading your own ROM off the card; the real **bag** and **trainer card**
screens; **Secret Bases** including other trainers'; the **Day-Care** viewer; the Battle **Frontier**
streaks and **Fly** flags; **Battle Record** export *and* import; a **Pokédex** editor with automatic
registration of edited mons; **animated summary portraits** and back-sprite view; **rumble haptics**
and per-screen animation toggles; backup modes and *Clear backups*; **Copy-to-game** from the bank;
the **Clock fix** screen; and PokeDNA's own **proportional 5×7 font**, which retired the truncated
`JIGGLYPU~` class of layout bug across every screen.

---

*PokeDNA is unofficial and not affiliated with Nintendo / Game Freak / The Pokémon Company. See the
[README](README.md) for the full disclaimer and license (GPLv3).*
