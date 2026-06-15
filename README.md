# PokeDNA

**Read and rewrite the DNA of your Generation-III Pokémon saves — on the Game Boy Advance
itself.**

PokeDNA is a homebrew tool that runs *on the cartridge* (EZ-Flash Omega DE, EverDrive GBA X5),
reads the flashcart's microSD, and opens your Gen-3 saves (Ruby, Sapphire, Emerald, FireRed,
LeafGreen) the way the games never showed you: the PC boxes and party with a pixel-accurate UI,
**plus every hidden value** — IVs, the full per-stat EV spread, nature, ability, shininess,
computed stats, and met info, alongside the trainer card and Game-Record stats.

The name says what it does: like reading **DNA** you can inspect the traits a Pokémon was born
with — and because DNA is **mutable**, PokeDNA can *edit* it too (and the rest of the save), all
on real hardware with no PC in the loop.

It is **entirely original work**. It is not a port of, fork of, or front-end for any other save
editor — the save-format logic is clean-room (see *Credits & legality*).

## Status

**v1.0.0 — validated on real hardware.** Editing has been exercised end-to-end on an EZ-Flash
Omega DE: event flags and game counters, copy / duplicate / move, the SD **bank** (byte-identical
round-trips that survive a power-cycle), and a **full from-scratch Pokémon edit** (species + stats
+ moveset) that loads, displays, and **battles correctly in the actual game** — no corruption, no
crashes. The pure-C save core is also covered by host tests. Every write still keeps an immutable
backup first; hold onto it until you're happy.

What it does:

- **View** — party + all PC boxes on a game-faithful screen (real box wallpapers, including the
  Emerald "secret"/Walda ones; box names), a 6-card Pokémon summary (info / skills / IVs / EVs /
  battle & contest moves) with front sprites, type badges and the 28 Unown forms, and the full
  trainer card + Game-Record stats.
- **Edit** *(EZ-Flash Omega DE only)* — species, nickname, nature, ability, shininess, gender,
  level, held item, moves, EVs/IVs; copy / paste / move / duplicate / release; `.pk3` export; a
  **bank** of 16 named, wallpapered boxes on the SD card; the trainer card (name, sex, TID/SID,
  money, play-time, badges, Battle-Frontier symbols); and a data editor for money, game counters,
  the bag, and a comprehensive named **event-flag** browser. EverDrive runs as a **read-only** tool.
- **Safety** — every write goes through a verified-write pipeline (`.tmp` → byte-compare re-read →
  rename) behind an immutable backup of the original; moves are batched and saved on one prompt
  when you leave the save.

Known limitations / planned: edited or injected Pokémon are **not** registered in the Pokédex
(seen/caught), and the **Poké Ball** and **met / caught location** can't be edited yet — both
planned for a later release. A deeper "encounter legality" check is also on the roadmap. See
`docs/HANDOFF.md` for the current state.

## Download

Grab the latest **`PokeDNA.gba`** from the [**Releases page**](https://github.com/GuyShtainer/PokeDNA/releases)
— no compiling needed. Copy it onto your flashcart's SD card next to your saves and run it (see
*Hardware & requirements* and *How to run* below). The release `.gba` includes the Pokémon sprites
baked in (see *Credits & legality* for the copyright note).

## Hardware & requirements

- A flashcart with SD sector access: **EZ-Flash Omega DE** (read **and** write) or **EverDrive
  GBA X5** (read-only). Writing is gated to the Omega DE because its flash write has no retry path.
- Your own Gen-3 `.sav` files on the SD card. **PokeDNA ships no save/game data — bring your own.**

## How to run

- **GBA flashcart** (EZ-Flash Omega DE / EverDrive GBA X5): copy `PokeDNA.gba` to the SD, run it next to your `.sav` files.
- **Emulator:** open `PokeDNA.gba` in mGBA.
- **Nintendo 3DS:** launch `PokeDNA.gba` via `open_agb_firm`.

## Building

Requires devkitPro `gba-dev` (libtonc), or just Docker:

```sh
./build.sh        # Docker (no local toolchain needed)
# or
make rebuild      # local devkitPro
```

Output: `PokeDNA.gba`. Copy it to the flashcart SD next to your `.sav` files and run it.

### Art: bundled in releases, not in the source tree

The release `.gba` includes the Pokémon sprites, box wallpapers, type badges, item icons and the
PC-hand cursor so the download "just works." Those assets are **ripped Game Freak art and are NOT
committed to this repository** — they are git-ignored, and only the release binary carries them
(the same way community tools like PKHeX ship sprites). The source builds with text fallbacks
without them. To build the graphics yourself, drop the asset packs under `assets/` and regenerate
the git-ignored blobs:

```sh
python3 tools/gen_icons.py      # box icons          (needs Pillow)
python3 tools/gen_front.py      # front sprites       (needs Pillow + gbalzss)
python3 tools/gen_wallpaper.py  # box wallpapers
python3 tools/gen_hand.py       # PC-hand cursor
python3 tools/gen_data.py       # names/stats + the named event-flag tables
```

## Host tests

The save-format core is pure C and dual-compiles on the PC — no hardware needed. Drop real `.sav`
files into `tests/fixtures/` and run a harness, e.g.:

```sh
cc -std=c11 -I source tests/host_edit_test.c source/gen3_save.c source/gen3_mon.c \
   source/gen3_box.c source/gen3_edit.c source/data_tables.c -o /tmp/he && /tmp/he tests/fixtures/*.sav
```

Each `tests/host_*_test.c` lists its exact compile line in a header comment.

## Credits & legality

- **Clean-room save format.** All Gen-3 save parsing/encryption logic here is original. Where
  reverse-engineered facts were used (struct offsets, RAM maps, flag numbers) they are *facts and
  addresses only* — no third-party game code is bundled. PKHeX and the pret decomps were used as
  **reference for facts**, not as a source of code.
- **Bundled sprites are Nintendo's — not mine, not GPLv3.** The downloadable `.gba` includes
  Generation-III box/front sprites, box wallpapers and UI icons that are
  **© Nintendo / Creatures Inc. / GAME FREAK Inc.** They are not licensed for redistribution and
  are **not** covered by this project's GPLv3 — they are bundled for convenience on the same
  tolerated, **non-commercial** basis that community tools like [PKHeX](https://github.com/kwsch/PKHeX)
  (via [pokesprite](https://github.com/msikma/pokesprite)) rely on. **The source repository contains
  none of this art** (it is git-ignored); wallpapers are reconstructed from the pret decomps. Nothing
  here is official or authorized — if a rights-holder objects, the art comes out.
- **Vendored libraries keep their own licenses:** the flashcart I/O layer (MIT), the EZ-Flash
  `io_ezfo` driver (Apache-2.0), FatFs (BSD-1-Clause), and libtonc. Their notices are retained.
- PokeDNA's own source is **GPLv3** (see `LICENSE`) — it stays free and open; you may use, study,
  modify and share it, and any distributed derivative must stay GPLv3.

## Trademarks / disclaimer

Pokémon, Game Boy Advance, and all related names are trademarks of Nintendo, Game Freak, and The
Pokémon Company. **PokeDNA is an unofficial, non-commercial, fan-made tool and is not affiliated
with, endorsed by, or supported by any of them.** The release binaries bundle Game Freak sprites
(© Nintendo / Creatures Inc. / GAME FREAK Inc. — see *Credits & legality*); you supply your own
save files. Editing save data can corrupt it — keep backups and proceed at your own risk.

---

© 2026 Guy Shtainer · GPLv3 · built on the [`gba-toolkit`](https://github.com/GuyShtainer) foundation.
