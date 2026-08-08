# Changelog

All notable changes to PokeDNA. Versions follow semantic versioning (`MAJOR.MINOR.PATCH`).

## Unreleased (2026-08-08) — ⚠️ not yet validated on hardware

Emulator-verified against real saves + the host test suite; the hardware pass is still
outstanding for everything below (per-item state in the commit messages).

### Features
- **The art-free build is a real product**: a clean clone builds a ~0.45 MB PokeDNA with zero
  game-derived art — original name chips in the PC grid, text lists for the Pokédex and the
  species/item pickers, coloured type chips, an original arrow cursor.
- **Game ROM registration**: Settings → *Game ROM* (plus a first-run offer on the art-free
  build) browses the SD for your own `.gba`, remembers it per game (shared with the map), and
  the PC box then streams the **real box icons out of your ROM at runtime** — fused or SD file
  (Emerald/FireRed/LeafGreen; Ruby/Sapphire fail closed for now).
- **Retail pickup, measured from the real game**: the exact PC transparency, the 17-frame grab
  dip, a real PLACE beat, the hand idle bounce (icons static, as retail), the 6-frame cursor
  slide, and retail's carry look (big white fist in front, no pop).
- **Pokéblock case**: the game's own screen chrome (RS + Emerald) with the FLAVOR/FEEL board
  filled for the selected block.
- Create-from-scratch opens the six-card summary; hatching names the Pokémon; the Day-Care
  explains its two real slots and its scenery visitors (with a toggle); the Records screen
  says what a `.rec` is for.

### Fixes
- **Every front sprite was horizontally mirrored** (visible as Unown p↔q) — generator fixed,
  all fronts regenerated.
- The PC box cursor survives L/R box flips, and a flip no longer wipes the screen to black.
- A full backup shelf (21 files) no longer blocks every save; the backup mode (new-each-time /
  single rolling / none) finally **persists**; *Clear backups* logs exactly what it removed
  or why it could not.
- The summary's confirm dialog no longer overflows the panel; the artless summary no longer
  paints open-bus garbage in the portrait.

## v2.0.0 — everything since the first release (2026-08-04)

**Validated on real hardware** (EZ-Flash Omega DE), against real Generation-III saves, with the
pure-C cores additionally covered by the host test suite. Back up your `.sav` before you edit
anything — PokeDNA keeps an immutable backup itself, but keep your own too.

### Added
- **Overworld map viewer** — reads *your own* Pokémon ROM off the SD card at runtime and renders
  the real world: a five-step zoom ladder, map-edge crossing, warps in and out of buildings,
  dive/emerge, secret-base entrances and interiors (the owner's decorations, correct collision),
  NPCs walked from the ROM's own script bytecode, item balls, and a region map. Grab your
  character and put them anywhere — including places you should not be able to stand.
- **PLACES list** (L on the widest region view) for the spots the region map cannot point at:
  the Sky Pillar and its top floor on Hoenn, Navel Rock and Birth Island on Kanto/Sevii.
- **The real bag screen** for Ruby/Sapphire, Emerald and FireRed/LeafGreen, both genders, with
  full item descriptions and per-pocket browsing.
- **Real trainer cards** for all three games — front and back pages, five star tiers, both
  genders — with the underlying achievements editable.
- **Battle Record** (Emerald): read the recorded battle out of the save, export it to the card,
  and import an old one back as if it were the last battle you fought.
- **Battle Frontier** win streaks, **Fly** destination flags, **Mirage Island**, **Pokéblocks**,
  event tickets, egg hatching, Deoxys formes, a Pokédex undo, and an in-game **clock fix**.
- **Emerald-style multi-select** in the PC: hold A and drag a rectangle, carry the block between
  boxes, drop it whole.
- A **proportional font** of our own, so item and Pokémon names print in full — `SUPER POTION`
  and `No01 FOCUS PUNCH` where nine characters used to be the ceiling.
- TMs and HMs now show the move they teach, the way the games do.

### Fixed
- The SD driver returned **success** after exhausting its read retries, handing back stale bytes
  that FatFs then reported as `FR_OK`. Failure is now reported as failure.
- FatFs bounced unaligned buffers on the wrong mask, so half of Emerald's metatile tables were
  read two bytes early — the cause of the corrupted map rooms.
- The nav menu recomposited itself on every keypress and visibly stacked; it is drawn once now.
- Dozens of truncated strings across the box panel, summary, bag, pickers and dialogs.

### Known limitations
- Writing is EZ-Flash Omega DE only; EverDrive GBA X5 runs read-only by design.
- Deeper encounter/move legality checking is still basic.

## v1.0.0 — first public release (2026-06-15)

First downloadable build, **validated on real hardware (EZ-Flash Omega DE).** Editing was exercised
end-to-end on a real cart: flags/counters, copy/duplicate/move, the SD bank (byte-identical
round-trips that survive a power-cycle), and a full from-scratch Pokémon edit (species + stats +
moveset) that loads and **battles correctly in the actual game** with no corruption or crashes.
Every write still keeps an immutable backup first — keep your backups regardless.

### Added
- **Viewer:** party + all PC boxes on a game-faithful screen (real box wallpapers incl. the Emerald
  "secret"/Walda set, box names); a 6-card Pokémon summary (info / skills / IVs / EVs / battle &
  contest moves) with front sprites, type badges and the 28 Unown forms; full trainer card +
  Game-Record stats. All the hidden data: IVs, full per-stat EVs, nature, ability, shininess,
  computed stats, met info.
- **Editor (EZ-Flash Omega DE only):** species, nickname, nature, ability, shininess, gender, level,
  held item, moves, EVs/IVs; copy / paste / move / duplicate / release; `.pk3` export; a **bank** of
  16 named, wallpapered boxes on the SD card; trainer card (name, sex, TID/SID, money, play-time,
  badges, Battle-Frontier symbols); a data editor for money, counters, the bag, and a comprehensive
  named **event-flag** browser. EverDrive GBA X5 runs read-only.
- **Safety:** verified-write pipeline (`.tmp` → byte-compare re-read → rename) behind an immutable
  backup; box moves are batched and saved on one prompt when you leave the save.

### Known limitations / planned
- Edited or injected Pokémon are **not** registered in the Pokédex (seen/caught).
- The **Poké Ball** and **met / caught location** can't be edited yet.
- A deeper "encounter legality" check and minor cosmetic polish are still to come.

### How to run
- **GBA flashcart** (EZ-Flash Omega DE / EverDrive GBA X5): copy `PokeDNA.gba` to the SD, run it next to your `.sav` files.
- **Emulator:** open `PokeDNA.gba` in mGBA.
- **Nintendo 3DS:** launch `PokeDNA.gba` via `open_agb_firm`.

### Notes
- The release `PokeDNA.gba` bundles Generation-III sprites that are © Nintendo / Creatures Inc. /
  GAME FREAK Inc. — included for convenience on a non-commercial, tolerated basis (see the README's
  *Credits & legality*). The source repository contains no copyrighted art. PokeDNA is unofficial and
  not affiliated with Nintendo, Game Freak, or The Pokémon Company.
- PokeDNA's own code is GPLv3.
