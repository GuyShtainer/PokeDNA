# PokeDNA

**Read and rewrite the DNA of your Generation-III Pokémon saves — on the Game Boy Advance
itself.**

PokeDNA is a homebrew tool that runs *on the cartridge* (EZ-Flash Omega DE, EverDrive GBA X5),
reads the flashcart's microSD, and opens your Gen-3 saves (Ruby, Sapphire, Emerald, FireRed,
LeafGreen) the way the games never showed you: the PC boxes and party with a pixel-accurate UI,
**plus every hidden value** — IVs, the full per-stat EV spread, nature, ability, shininess,
computed stats, and met info — alongside the real trainer card, the real bag, the Battle Frontier
records, and now **the overworld itself**, rendered live out of your own copy of the game.

The name says what it does: like reading **DNA** you can inspect the traits a Pokémon was born
with — and because DNA is **mutable**, PokeDNA can *edit* it too (and the rest of the save), all
on real hardware with no PC in the loop.

**The code is entirely original.** It is not a port of, fork of, or front-end for any other save
editor — the save-format logic was written from scratch. The *data* it draws on (species and item
names, move tables, flag names) is extracted from the pret decompilations by the generators in
`tools/`, and the release binary additionally embeds game-extracted artwork. See
*Credits & legality* — that section is specific about what ships and what does not.

## Status

**v2.0.0 — validated on real hardware (EZ-Flash Omega DE).**

Every screen in this release has been exercised on a real cartridge against real saves, not just in
an emulator: the map viewer reading the game ROM off the card, flash writes, the SD bank
(byte-identical round-trips that survive a power cycle), and a from-scratch Pokémon edit that loads
and *battles correctly in the actual game*. The pure-C cores are additionally covered by the host
test suite (`tests/`, 28 harnesses).

Back up your `.sav` before you edit anything. PokeDNA takes an immutable backup itself, but keep
your own too.

## What it does

### The overworld map viewer

Point PokeDNA at **your own `.gba` dump of the game you play** (on the SD card — nothing is copied,
the ROM is read transiently and the path is remembered per game) and it renders the real world from
that ROM's own data:

- **A five-step zoom ladder** — 1:1, ½, ¼, the region map, and a wide region view. The tile levels
  are the game's actual metatiles, decompressed and composited on the fly.
- **Getting around** — the cursor walks off a map edge into the connected one, `START` enters a
  warp (doors, stairs, cave mouths), and dives or surfaces where the map has a dive connection.
- **Secret bases** — entrances are marked on the ground with their owner's name; `START` walks
  inside, and the interior is drawn with that base's own decorations and collision, straight from
  the save's registry.
- **NPCs** — the people standing on the map are real object events, drawn as hardware sprites with
  their own graphics, and `START` on one *walks its script bytecode in the ROM* to show what it
  says. Because a script can branch on flags the tool can't evaluate, the dialog is honestly
  labelled "says" (unconditional) versus "may say" (behind a branch).
- **Item balls** — labelled with the item they hold and whether you've already taken it, and
  `START` puts a collected one back on the ground by clearing its flag.
- **The region map** — the game's own, with its cursor; on FireRed/LeafGreen `L` switches between
  the Kanto and Sevii views exactly like the retail "SWITCH MAP" button.
- **Grab and place your character** — `A` picks the player up, `A` again drops them on the cursor.
  Unwalkable tiles are **allowed but warned about** ("Tile is NOT walkable"), as are maps that are
  odd to warp into. The placement is written through the same verified-write pipeline as everything
  else, behind the usual immutable backup. The new position takes effect the next time you load the
  save.

**PLACES** (`L` from the widest region view) reaches the spots the region map cannot point at,
because the game deliberately hides them behind another section: on Hoenn the **Sky Pillar** and its
**top floor**, the **Sealed Chamber** (braille room and inner room) and the **Underwater Sealed**
room above it, and the three Regi chambers — **Desert Ruins**, **Island Cave**, **Ancient Tomb**. On
Kanto/Sevii it reaches **Navel Rock** and **Birth Island**.

### PC storage, party and the Pokémon editor

- Party and all PC boxes on a game-faithful screen: real box wallpapers (including the Emerald
  "secret"/Walda set, recoloured from your save), box names, animated box icons, and a grab-and-carry
  hand.
- **Emerald-style multi-select** — hold `A` and drag a rectangle over the box, lift the whole block,
  carry it to another box, and drop it where the footprint fits.
- A **bank** of 16 named, wallpapered boxes living on the SD card under `/PokeDNA/bank/`, with
  cross-box chunk moves, batch export/release, and a "copy to game" action.
- Single-Pokémon **`.pk3` export** in the community interchange format.
- A six-card **summary** (info / skills / IVs / EVs / battle & contest moves) with front *and* back
  sprites, type badges, the 28 Unown forms, the Deoxys formes, and a real egg sprite for eggs.
- **Full editor:** species, nickname, nature, ability, shininess, gender, level, held item, moves and
  their current PP, EVs and IVs, contest condition (cool / beauty / cute / smart / tough / sheen),
  Poké Ball, met location / level / origin game — plus create-from-scratch, copy / paste / move /
  duplicate / release, and **hatch** on an egg. Anything added to the save is registered in the
  Pokédex automatically.

### Everything else in the save

The `START` menu is the map of the tool:

| Menu | What it opens |
|---|---|
| Party / Bank / Daycare | the three storage screens beside the PC boxes |
| Trainer | the **real in-game trainer card**, front *and* back, for all three games and both genders — the card *is* the editor (name, ID, money, play time, badges, the gender photo), with the five star tiers driven by the underlying achievements, plus Game Records |
| Clock fix | compares the cartridge RTC to the in-game clock and repairs a save whose clock runs behind — the cause of frozen berries, tides and lottery (Ruby/Sapphire/Emerald; FRLG have no RTC) |
| Mirage | Mirage Island — written as an LCG **pre-image**, because writing the target value directly is silently undone by the game |
| Pokédex | full seen/caught editor, National Dex unlock, bulk "mark all" with a **one-step undo** |
| Bag | the **real in-game bag screen** for RS / Emerald / FRLG, both genders: the game's own chrome, per-pocket browsing, item icons and full descriptions. **TMs and HMs show the move they teach**, the way the games do |
| Flags & counters | money, game counters, Game-Corner coins, and a large curated, named **event-flag** browser (including the R/S hidden-item names) |
| Bases | Secret Base viewer — the registry, its parties, and a clear action |
| Blocks | Pokéblock case editor (add / change / delete) with a game-style colour picker |
| Tickets | event tickets — Eon, Aurora, Mystic, Old Sea Map — granted in the legitimate "received" state |
| Records | **Battle Record** (Emerald): read the recorded battle out of sector 31, **export it to the card** as a `.rec` under `/PokeDNA/battles/` with a plain-text sidecar (facility, streak, teams, seed), and **import an old one back** as if it were the last battle you fought — the exported `.rec` is what **rec2mp4** turns into a video on a PC (see below) |
| Frontier | Battle Frontier win streaks — every lane (7 facilities × modes × Lv50/Open), current and best, the records board, and presets that write the exact value the Frontier Brain test expects |
| Fly | Fly destination flags, on their own screen because these bits are among the safest in the save (marking a town still doesn't grant Fly without the badge and the move) |
| Map | the overworld map viewer, above |
| Settings | backup mode, clear backups, **rumble haptics** (per-cue toggles) and **per-screen animation** toggles, all persisted in `/PokeDNA/config.cfg` |

#### From a recorded battle to a video

Each export from the **Records** screen writes two files into `/PokeDNA/battles/`: the `.rec` — a
byte-exact copy of the save's sector 31, which *is* the battle (the RNG seed, both teams, and every
button both players pressed) — and a plain-text `.txt` sidecar carrying what sector 31 cannot hold
(facility, level mode, the current streaks, both teams, the export timestamp).

That pair is a **replayable battle, not a video**. Closing the loop is **rec2mp4**, a companion PC
tool from the same toolkit: it injects the `.rec` back into a save image, boots *your own* Emerald
ROM in a headless mGBA, lets the game replay the battle itself, and encodes the result to MP4 with
an info panel built from the sidecar. Export on the cart, render on the PC — and the `.rec` still
imports back into any save as "the last battle you fought".

> **rec2mp4 is not published yet.** A link will land here when it is; until then the `.rec`/`.txt`
> pair is the durable artefact — nothing about the export changes when the tool ships.

### Interface

Text is drawn with a **proportional 5×7 face of our own**, so real names print in full —
`SUPER POTION` and `No01 FOCUS PUNCH`, where nine fixed-width glyphs used to be the ceiling. Box
icons, the hand cursor and the map's characters are hardware sprites, so nothing flickers.

### Safety

- **Writing is EZ-Flash Omega DE only.** The **EverDrive GBA X5 runs read-only by design** — it
  browses, views and exports, but never writes. The Omega's flash write has no retry path, which is
  why every write is verified instead of trusted.
- Every write goes through a verified-write pipeline (`.tmp` → byte-compare re-read → rename) behind
  an **immutable backup** of the original.
- Box moves are batched and committed on one prompt when you leave the save.
- All of PokeDNA's own files live in **one folder**, `/PokeDNA/` on the card (config, log, bank,
  battle exports) — uninstalling is deleting one folder.
- **Keep your own backups anyway.** Editing save data can corrupt it.

## Hardware & requirements

- A flashcart with SD sector access: **EZ-Flash Omega DE** (read **and** write) or **EverDrive
  GBA X5** (read-only).
- Your own Gen-3 `.sav` files on the SD card. **PokeDNA ships no saves and no ROM — bring your own.**
  (The release binary does embed game-extracted artwork and text so the download works out of the
  box; see *Credits & legality*. It never contains anyone's save or a playable ROM.)
- For the map viewer only: your own `.gba` dump of the game the save belongs to, on the same card.
  A Ruby ROM cannot describe an Emerald save's maps, and PokeDNA refuses the mismatch rather than
  drawing garbage.

## Download

Grab the latest **`PokeDNA.gba`** from the
[**Releases page**](https://github.com/GuyShtainer/PokeDNA/releases) — no compiling needed. Copy it
onto your flashcart's SD card next to your saves and run it.

## How to run

- **GBA flashcart** (EZ-Flash Omega DE / EverDrive GBA X5): copy `PokeDNA.gba` to the SD, run it
  next to your `.sav` files.
- **Emulator:** open `PokeDNA.gba` in mGBA. (See *Build variants* — an emulator has no flashcart,
  so the SD-backed features need either the `delta` build or a fused ROM.)
- **Nintendo 3DS:** launch `PokeDNA.gba` via `open_agb_firm`.

## Building

Requires devkitPro `gba-dev` (libtonc), or just Docker:

```sh
./build.sh        # Docker (no local toolchain needed)
# or
make rebuild      # local devkitPro
```

Output: `PokeDNA.gba`. Copy it to the flashcart SD next to your `.sav` files and run it.

### Build variants

```sh
make sd           # PokeDNA-SD.gba — trimmed image that streams shiny/back sprites
                  # from /PokeDNA/sprites.pak instead of carrying them
make delta        # pokedna-delta.gba — emulator build (Delta / RetroArch): no flashcart,
                  # it edits its OWN 128 KiB flash save (copy your .sav over pokedna-delta.sav)
python3 tools/fuse_rom.py PokeDNA.gba YOUR_GAME.gba -o fused.gba
                  # append your game ROM to the image so the map viewer works under an
                  # emulator, where there is no microSD to read the ROM from
```

### Graphics assets are generated locally, not committed

The image assets (Pokémon sprites, box wallpapers, type badges, item icons, the bag and
trainer-card chrome, the PC hand) are **not part of this repository** — they are git-ignored and
generated on your machine from asset packs you place under `assets/`.

**A fresh clone builds an art-free PokeDNA out of the box.** Every art module has a weak
fallback (`source/art_fallbacks.c`): with no generated art present you get a fully working build
with original stand-ins — name chips in the PC grid, text lists for the Pokédex and pickers,
coloured type chips, an original arrow cursor — at about 0.4 MB. Running the generators below
upgrades it to the full visuals.

To generate them:

```sh
python3 tools/gen_icons.py      # box icons              (needs Pillow)
python3 tools/gen_front.py      # front sprites          (needs Pillow + gbalzss)
python3 tools/gen_back.py       # back sprites
python3 tools/gen_wallpaper.py  # box wallpapers
python3 tools/gen_hand.py       # PC-hand cursor
python3 tools/gen_bag_bg.py     # bag screen chrome
python3 tools/gen_card_bg.py    # trainer-card chrome
python3 tools/gen_items.py      # item icons
python3 tools/gen_types.py      # type badges
python3 tools/gen_font.py       # the proportional 5x7 face
python3 tools/gen_data.py       # names/stats, TM->move table, named event-flag tables
```

## Host tests

The save-format, map-parsing and rendering cores are pure C and dual-compile on the PC — no hardware
needed. `tests/` holds 28 harnesses, from `host_mon_test.c` (the per-mon decryption kernel) to
`host_render_test.c` (which byte-compares the metatile compositor against an independent Python
reference renderer run over a real ROM). Drop real `.sav` files into `tests/fixtures/` and run one,
e.g.:

```sh
cc -std=c11 -I source tests/host_edit_test.c source/gen3_save.c source/gen3_mon.c \
   source/gen3_box.c source/gen3_edit.c source/data_tables.c -o /tmp/he && /tmp/he tests/fixtures/*.sav
```

Each `tests/host_*_test.c` lists its exact compile line in a header comment.

## Known limitations

- Writing is EZ-Flash Omega DE only; the EverDrive GBA X5 is read-only by design.
- **Deeper encounter/move legality checking is still basic** — PokeDNA will happily build a Pokémon
  a real game could never produce, and only flags the obvious cases.
- The map viewer needs your own ROM; without it the screen says so and does nothing else.

See **[CHANGELOG.md](CHANGELOG.md)** for what landed when, and **[ROADMAP.md](ROADMAP.md)** for the
backlog.

## Credits & legality

- **Original code, documented reverse engineering.** Every byte of the Gen-3 save parsing /
  encryption / editing here is original C, written from scratch. **PokeDNA drew inspiration from [PKHeX](https://github.com/kwsch/PKHeX)**
  — the idea of a friendly Gen-3 save viewer/editor — but **none of PKHeX's code was used or ported**
  (PKHeX is C#/.NET and GPLv3, and can't run on a GBA). The reverse-engineered *facts* PokeDNA relies
  on (struct offsets, RAM maps, flag numbers, script opcodes) come from the **pret decompilations**
  ([pokeemerald](https://github.com/pret/pokeemerald) / pokeruby / pokefirered). No third-party
  *code* is bundled — but be clear that more than addresses crosses over: the generators in `tools/`
  extract the games' **name and text tables** from those decompilations (species, items, moves,
  locations, the TM→move mapping, event-flag names, and the item/move description strings), and
  those tables ship in the binary. The single-Pokémon `.pk3` export uses the community interchange
  format (PKHeX-compatible).
- **Your saves and your ROM stay yours.** PokeDNA never copies either anywhere; the map viewer reads
  your ROM off your own card transiently, while the screen is open.
- **The repository carries no game art — the release binary does.** The generated graphics
  (sprites, wallpapers, badges, item icons, the bag and trainer-card chrome) are git-ignored and
  built locally from asset packs you supply, so nothing derived from the games is in git. The
  released `PokeDNA.gba` **does** embed them, so the download works out of the box the way community
  tools like PKHeX ship sprites. Being plain about it: that binary contains several megabytes of
  Game Freak / Nintendo artwork and the games' own item and move description text. If you would
  rather run a build with none of that in it, clone the repo, skip the art generators, and build —
  you get the art-free build described above, with every screen usable on original stand-ins.
- **Vendored libraries keep their own licenses:** the flashcart I/O layer (MIT), the EZ-Flash
  `io_ezfo` driver (Apache-2.0), FatFs (BSD-1-Clause), and libtonc. Their notices are retained.
- PokeDNA's own source is **GPLv3** (see `LICENSE`) — it stays free and open; you may use, study,
  modify and share it, and any distributed derivative must stay GPLv3.

## Trademarks / disclaimer

Pokémon, Game Boy Advance, and all related names are trademarks of Nintendo, Game Freak, and The
Pokémon Company. **PokeDNA is an unofficial, non-commercial, fan-made tool and is not affiliated
with, endorsed by, or supported by any of them.** You supply your own save files and your own copy
of the game. Editing save data can corrupt it — keep backups and proceed at your own risk.

---

© 2026 Guy Shtainer · GPLv3 · built on the [`gba-toolkit`](https://github.com/GuyShtainer) foundation.
