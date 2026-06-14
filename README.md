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

## Status — honest

Builds clean and runs in emulator; the pure-C save-format core is covered by host tests. **The
editing / SD-write features are implemented but have NOT yet been validated on real hardware** —
treat write mode as experimental until that's done, and always keep your own backups. Reading is
the safe, well-exercised path.

What works (in code / emulator / host tests):

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

Roadmap / not done: real-hardware validation of all SD-write paths; a deeper "encounter legality"
check; minor cosmetic polish. See `docs/HANDOFF.md` for the current state.

## Hardware & requirements

- A flashcart with SD sector access: **EZ-Flash Omega DE** (read **and** write) or **EverDrive
  GBA X5** (read-only). Writing is gated to the Omega DE because its flash write has no retry path.
- Your own Gen-3 `.sav` files on the SD card. **PokeDNA ships no game data — bring your own.**

## Building

Requires devkitPro `gba-dev` (libtonc), or just Docker:

```sh
./build.sh        # Docker (no local toolchain needed)
# or
make rebuild      # local devkitPro
```

Output: `pokedna.gba`. Copy it to the flashcart SD next to your `.sav` files and run it.

### Art is not in the repo (bring your own / regenerate)

Pokémon sprites, box wallpapers, type badges, item icons and the PC-hand cursor are **ripped
Game Freak art and are not committed**. The build falls back to text without them; to enable the
real graphics, drop the asset packs under `assets/` and regenerate the git-ignored blobs:

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
- **Vendored libraries keep their own licenses:** the flashcart I/O layer (MIT), the EZ-Flash
  `io_ezfo` driver (Apache-2.0), FatFs (BSD-1-Clause), and libtonc. Their notices are retained.
- PokeDNA's own source is **GPLv3** (see `LICENSE`) — it stays free and open; you may use, study,
  modify and share it, and any distributed derivative must stay GPLv3.

## Trademarks / disclaimer

Pokémon, Game Boy Advance, and all related names are trademarks of Nintendo, Game Freak, and The
Pokémon Company. **PokeDNA is an unofficial, fan-made tool and is not affiliated with, endorsed by,
or supported by any of them.** It contains no copyrighted game content; you supply your own saves.
Editing save data can corrupt it — use backups and proceed at your own risk.

---

© 2026 Guy Shtainer · GPLv3 · built on the [`gba-toolkit`](https://github.com/GuyShtainer) foundation.
