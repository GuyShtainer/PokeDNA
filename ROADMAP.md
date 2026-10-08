# PokeDNA — Roadmap & Known Issues

The running list of what's planned and what's currently rough. PokeDNA is at **v3.0.0** and
actively developed; this is an honest backlog, not a promise of dates or order. If something here
matters to you, feel free to open an issue or a PR.

## Known issues / current bugs

- **Most of v3.0.0 has not been signed off on a cartridge yet.** The README's *Status* lists what
  has never run on an EZ-Flash Omega DE; the hardware queue is being worked through.
- **History shows the current branch only**, and undo/redo take about a second per press in the
  emulator; the cartridge number decides whether that needs work.
- **Editing a copy of a save** can make the original offer to re-apply the copy's recorded steps
  on its next load (same trainer identity, same journal key).
- **A card that fails inside a rename** can cross-link a file and its `.tmp`; a failed backup
  when the backup shelf is full can overwrite the oldest good backup. Both are being hardened.
- **Day-Care take-out** does not teach the level-up moves learned while boarded (all generations).
- **Big builds can fail to load from SD on some cards.** This is a *copy* problem, not a size
  ceiling. If the flashcart hangs at "Load game", re-copy the `.gba` and eject the card safely.
- **NIDORAN♀/♂ display as `NIDORAN?`.** The gender symbols have no glyph in the 5×7 face; the
  bytes written to the save are correct.

## Planned — editor coverage

- **The full legitimacy check.** v3.0.0 added the LEGALITY screen and the ROM-fed *legit copy*
  creation, but the pass is still structural. The remaining work is move legality against per-origin-game learnsets, PID↔nature/gender/shininess
  consistency, met location/level/ball plausibility, evolution stage at met level, ability index,
  and EV/IV bounds including the 510 total.
- ~~Gen 1 / Gen 2 support via the boxes~~ — done in v3.0.0: Red/Blue/Yellow/Gold/Silver/Crystal saves
  open and edit natively, and Pokémon travel both ways through the Bank with an exact round trip.
- **More editable flags & counters** — keep expanding the curated set.
- **Off-map places** (Sky Pillar and friends) are rect-only today, so the map cursor can't select
  them; Mirage Island needs an RNG pre-image write rather than a direct one.

## Planned — graphics & UI

- ~~Pokéblock case chrome~~ — done: the game's own case screen for RS and Emerald, generated
  locally from the decomps like the bag and trainer card (FRLG has no Pokéblocks).
- ~~Emerald's real pickup animation~~ — done: the transparency matches retail exactly (per-icon
  `BLDALPHA_BLEND(7, 11)`, holders opaque, hand solid) and so does the motion (the 17-frame grab
  dip: open hand descends onto the still-in-place Pokémon, the fist closes, both rise together).
  A matching *place* dip on drop is a possible further beat.
- **A two-column PC box view.**
- **Smoother rendering** — keep moving screens off full-screen repaints. (The PC box flip is done:
  it used to wipe the screen to black on every L/R.)

## Planned — companion tooling

- ~~rec2mp4 link-up~~ — done: [rec2mp4](https://github.com/GuyShtainer/rec2mp4) is published; the
  Records screen's `.rec` + `.txt` export is its input.
- **A PokeDNA-side preview of a recorded battle** (teams and outcome from the record) before exporting it.

## Testing / validation

- **Cross-game bank is best-tested Emerald → Emerald.** Keep exercising the bank and cross-game
  transfers with Ruby/Sapphire and FireRed/LeafGreen, including moving Pokémon *between* games.
- Anything touching the SD card, the RTC, or a destructive save operation is not done until it has
  been run on real hardware. The emulator does not model the flashcart's SD path.

## Shipped since v2.0.0

v3.0.0 added: native **Game Boy saves** (Gen 1 and 2) with every Gen-3 screen mirrored; **transfers
between generations** through the Bank with a byte-exact round trip and the TRANSFERS ledger; the
**undo/redo history** with its on-card journal and power-cut recovery; **LEGALITY** and the
ROM-fed **legit-copy CREATE**; the **art-free release** fed by the user's own ROMs (first-launch
ROM welcome, Extract art, ROM-hack lock-out); Bank and ledger **self-healing**; 64 KiB save support
and damaged-save detection; the Pokédex detail view, current-HP editing, the museum star, the
Ruby/Sapphire bag chrome, persisted rumble settings, and a text-fit lint over every screen.

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
