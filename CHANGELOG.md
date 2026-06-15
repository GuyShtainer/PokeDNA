# Changelog

All notable changes to PokeDNA. Versions follow `MAJOR.MINOR.PATCH`; pre-1.0 means the editor
is still experimental.

## v0.1.0 — first public release (2026-06-15)

First downloadable build. **Reading is solid; the editing / SD-write features work in emulator and
host tests but are NOT yet validated on real hardware — back up your saves before using write mode.**

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

### Known limitations
- **Not hardware-validated yet** — write mode is experimental; keep backups.
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
