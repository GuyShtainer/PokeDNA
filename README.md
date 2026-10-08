# PokeDNA

**Read and rewrite the DNA of your Pokémon saves — on the Game Boy Advance itself.**

PokeDNA is a homebrew tool that runs *on the cartridge* (EZ-Flash Omega DE, EverDrive GBA X5),
reads the flashcart's microSD, and opens your saves the way the games never showed you. It started
as a Generation-III viewer and editor (Ruby, Sapphire, Emerald, FireRed, LeafGreen) and now also
opens the Game Boy generations (Red, Blue, Yellow, Gold, Silver, Crystal), moves Pokémon between
them through its own Bank, and keeps an undo history of everything you do.

The name says what it does: like reading **DNA** you can inspect the traits a Pokémon was born
with — and because DNA is **mutable**, PokeDNA can *edit* it too, all on real hardware with no PC
in the loop.

**The download contains no game art and no game descriptions or dialogue.** Every sprite, icon,
wallpaper, screen chrome and item description you see on screen is read at run time out of *your own* cartridge
dumps on the SD card, the same way the map viewer has always worked. Without a ROM beside your
save, PokeDNA still opens and edits everything, drawn with its own stand-ins. See *Credits &
legality*.

## Status

**v3.0.0 — a large release, most of it not yet signed off on real hardware.**

v2.0.0 was validated screen by screen on an EZ-Flash Omega DE. Everything added since then (the
Game Boy generations, cross-generation transfers, the undo history, the art-free posture — see
`CHANGELOG.md`) has been verified three ways, none of which is a cartridge:

- **the emulator**, with scripted screenshot chains over real saves and a virtual SD card that is
  power-cut at every write to check nothing is lost;
- **the host test suite** (`tests/`, 168 harnesses) running the pure-C cores on the PC against a
  corpus of real saves;
- **the retail gate**, which boots the actual games in mGBA on saves PokeDNA has edited and checks
  the game accepts them (123 cases).

Four shorter cartridge sessions between August and the end of September did exercise parts of the
new work on an Omega DE: opening Game Boy saves, editing a Yellow Pokémon from the summary, the
Day-Care, Pokédex and trainer-card screens, picking a Pokémon up into the Bank, and a Bank → Emerald
drop that came back byte-identical to the original. They also found bugs, which were fixed — and
those fixes have not been back on a cartridge yet.

**Not yet run on a cartridge at all** (the honest list, kept in the hardware queue):

- the Gen 3 → Game Boy transfer end to end (the Game Boy → Gen 3 direction has run once);
- the undo/redo history, its journal on the card, and the power-cut recovery offer;
- the Bank's and the transfer ledger's self-healing after a card pull mid-write;
- committing a 64 KiB Gen-3 save, and the damaged-save detection that came with it;
- ROM-hack detection (writes lock when the registered ROM is not retail);
- Ruby/Sapphire art served from the SD card rather than a fused image;
- the EverDrive GBA X5 on this build (it stays read-only by design);
- rumble settings surviving a reboot;
- how fast the art-free build is on a real card, which the emulator cannot measure.

Treat v3.0.0 as a preview until those rows are signed. **Back up your `.sav` before you edit
anything.** PokeDNA takes an immutable backup itself and keeps an undo journal, but keep your own
copy too.

## What it does

### Your own ROM supplies the art

Put a `.gba` dump of the game you play beside your save (and `.gb`/`.gbc` dumps for the Game Boy
saves). The first launch asks once whether to add game ROMs; later, *Settings → Game ROM* browses
the card and remembers the choice per game. From then on PokeDNA reads, transiently and on demand:

- box icons, front and back sprites, box wallpapers, type badges and item icons;
- the bag, trainer-card and Pokéblock-case screen chrome;
- item and move descriptions (Gen 3 from the GBA ROM, Gen 2 from the GB ROM);
- the Game Boy sprites and icons, learnsets and the data that *CREATE* and *LEGALITY* need;
- the overworld, for the map viewer.

*Extract art* caches the heavy pieces under `/PokeDNA/art/` so a cold open is faster; *ROM art* can
be switched off, and every place that would have shown art says where it comes from instead.
Nothing is copied anywhere else, and a registered ROM that is not a retail dump (a hack) keeps the
save viewable but locks every write.

### PC storage, party and the Pokémon editor (Gen 3)

- Party and all PC boxes on a game-faithful screen: real wallpapers (including the Emerald
  Walda set, recoloured from your save), box names, animated icons, a grab-and-carry hand with the
  retail pickup timing.
- **Emerald-style multi-select** — hold `A` and drag a rectangle, lift the block, carry it to
  another box, drop it where the footprint fits.
- A six-card **summary** (info / skills / IVs / EVs / battle & contest moves) with front and back
  sprites, the 28 Unown forms, the Deoxys formes, a real egg sprite for eggs — and the editor lives
  on the same cards: species, nickname, nature, ability, shininess, gender, level, current HP, held
  item, moves and PP, EVs, IVs, contest condition, Poké Ball, met location / level / origin game.
- **CREATE** on an empty slot asks first: a *legit copy* built from the ROM's own encounter data
  (a Pokémon the game could really have produced, at a level it could really have), or *from
  scratch*. Legendaries start at their real encounter level; the move picker only offers moves the
  species' generation knows.
- **LEGALITY** on any Pokémon lists what a real game could not have produced.
- Copy / paste / move / duplicate / release / hatch, single-Pokémon `.pk3` export, and automatic
  Pokédex registration for anything added to the save.

### The Game Boy generations

Open a Red, Blue, Yellow, Gold, Silver or Crystal `.sav` straight from the file picker and you get
the same screens, in the same design, with the generation's own rules:

- the box grid and party, the summary editor (moves and items filtered to what that game knows,
  Crystal's extra fields where they exist), create / duplicate / release / `.pk1` / `.pk2` export;
- Day-Care, trainer card (editable), bag and pack, event flags, Pokédex, Fly, the Gen-2 clock
  (offsets and the clock-error flag — never an absolute time), Hall of Fame;
- the map: Gen 1 draws the player's current map from the ROM and lets you place the player on
  it; Gen 2 has an all-maps browser;
- Mail is respected: anything that would strand a letter (box, release, duplicate, Day-Care) tells
  you to take it off first, the way the games do.

### Transfers between generations — through the Bank

The **Bank** is 16 named, wallpapered boxes on the card under `/PokeDNA/bank/`, and it is the only
road between saves: put a Pokémon in from one save, take it out in another. One save is open at a
time, so nothing can be cloned by accident.

- **Gen 3 → Game Boy**: a confirm screen lists exactly what travels and what stays (nature,
  ability, met data, held items that have no Gen-1/2 equivalent — an item that cannot travel is
  placed in the bag or left behind, never silently dropped). The Gen-3 original is parked in a
  sidecar record on the card, so the same Pokémon coming **back** is restored byte for byte — the
  round trip is exact, not a re-conversion.
- **Game Boy → Gen 3**: choose *keep as is* (the Gen-1/2 record travels unchanged inside a Gen-3
  shell) or *make legal* (converted into something a Gen-3 game would accept). A Bank Pokémon can
  also be dropped straight into a party slot, fully healed.
- **TRANSFERS** (nav menu) is the ledger: every parked original, where its copy is now, and a
  restore if a copy was lost. Records heal themselves after a card pull mid-write.

### Undo, redo and the History screen

There are no per-screen "Save X?" prompts any more. Every edit — Gen 3 or Game Boy — is held and
written once when you leave the save, and every step is journaled under `/PokeDNA/journal/`:

- `SELECT`+`L` undoes, `SELECT`+`R` redoes, on the box grid and the summary; a swap's two halves
  undo as one.
- **History** (nav menu) shows every recorded step, which ones are already in the save, and lets
  you jump to any point.
- After a power cut the next load offers to re-apply the steps that never reached the save.
- *History size* and *Clear history* live in Settings. The journal is one folder; delete it and
  nothing else changes.

### The overworld map viewer

With your own `.gba` on the card, *Map* renders the real world from that ROM: a five-step zoom
ladder, map-edge crossing, warps (`START` on doors, stairs, cave mouths), dive and emerge, secret
bases with their owner's decorations, NPCs drawn as hardware sprites whose script bytecode is walked
to show what they say, item balls, and the region map (with the Kanto/Sevii switch on
FireRed/LeafGreen). `A` picks your character up and `A` drops them on the cursor — anywhere,
including tiles you cannot normally stand on, with a warning. **PLACES** (`L` from the widest view)
reaches the Sky Pillar, the Sealed Chamber, the Regi chambers, Navel Rock and Birth Island.

### Everything else in the save

The `START` menu is the map of the tool:

| Menu | What it opens |
|---|---|
| Party / Bank / Daycare | the storage screens beside the PC boxes |
| Trainer | the real trainer card, front and back, for every game — the card *is* the editor (name, ID, money, play time, badges, the Lilycove museum star), plus Game Records |
| Clock fix | compares the cartridge RTC to the in-game clock and repairs a save whose clock runs behind (RSE); the Gen-2 clock screen for Gold/Silver/Crystal |
| Mirage | Mirage Island, written as an LCG pre-image so the game keeps it |
| Pokédex | seen/caught editor with a per-species detail view, National Dex unlock, bulk marks with one-step undo |
| Bag | the real bag screen, per-pocket, with icons and descriptions from your ROM; TMs show the move they teach |
| Flags & counters | money, counters, Game-Corner coins and a curated named event-flag browser |
| Bases / Blocks / Tickets | Secret Base viewer, Pokéblock case editor with the game's own screen, event tickets in the legitimate "received" state |
| Records | **Battle Record** (Emerald): export the recorded battle as a `.rec`, import an old one back — see *From a recorded Frontier battle to a video* below |
| Frontier / Fly | Battle Frontier streaks for every facility and mode; Fly destination flags |
| Transfers / History | the transfer ledger and the undo history, above |
| Map | the overworld map viewer |
| Settings | Game ROM, Extract art, ROM art on/off, backup mode, rumble strength and duration, per-screen animation, History size and Clear history — all in `/PokeDNA/config.cfg` |

### From a recorded Frontier battle to a video

Emerald keeps your last recordable Battle Frontier (or link) battle in the save as a **Battle
Record**: the RNG seed, both teams and every button both players pressed — a replayable battle,
not a video. PokeDNA's **Records** screen reads it out of sector 31 and **exports it to the card**
as a `.rec` under `/PokeDNA/battles/`, named after the facility, mode and your streak at the time,
with a plain-text sidecar carrying what the record cannot (facility, level mode, streaks, both
teams, the export time). An old `.rec` imports back into any save as "the last battle you fought".

**[rec2mp4](https://github.com/GuyShtainer/rec2mp4)**, the companion PC tool, turns that export
into an `.mp4`: it injects the record into a save image, boots *your own* Emerald ROM in a headless
mGBA, lets the game replay the battle itself, and encodes the frames and audio with an info panel
built from the sidecar — teams, moves, EVs and IVs, the outcome, an opening card with the
opponent's own pre-battle line. Panels are designable, batches run one record per CPU, and there
is a desktop GUI. Export on the cart, render on the PC, share the video.

### Safety

- **Writing is EZ-Flash Omega DE only.** The EverDrive GBA X5 browses, views and exports but
  never writes. The Omega's flash write has no retry path, which is why every write is verified
  rather than trusted.
- Every write goes through a verified-write pipeline (`.tmp` → byte-compare re-read → rename)
  behind an **immutable backup** of the original; Bank boxes and the Bank index keep a rolling
  backup too, and both are restored automatically if a write was cut short.
- A failed save is reported as a failure, with the file that still holds your bytes named on
  screen — never as a success.
- A save the tool recognises as damaged (a 64 KiB dump that grew, two slots that disagree) opens
  on its intact slot and says so.
- All of PokeDNA's files live in **one folder**, `/PokeDNA/` (config, log, bank, journal, transfer
  ledger, battle exports, art cache). Uninstalling is deleting one folder.
- **Keep your own backups anyway.** Editing save data can corrupt it.

## Hardware & requirements

- A flashcart with SD sector access: **EZ-Flash Omega DE** (read and write) or **EverDrive GBA
  X5** (read-only).
- Your own `.sav` files on the SD card. **PokeDNA ships no saves and no ROM — bring your own.**
- For the art, descriptions, map and the Game Boy creation data: your own dumps of the games the
  saves belong to, on the same card. A Ruby ROM cannot describe an Emerald save and PokeDNA refuses
  the mismatch rather than drawing garbage.

## Download

Grab **`PokeDNA.gba`** from the [Releases page](https://github.com/GuyShtainer/PokeDNA/releases)
— no compiling needed. It is the art-free build (about 1 MB). Copy it onto your flashcart's SD card
next to your saves and run it.

## How to run

- **GBA flashcart** (EZ-Flash Omega DE / EverDrive GBA X5): copy `PokeDNA.gba` to the card, run
  it next to your `.sav` files and ROM dumps.
- **Emulator:** `PokeDNA.gba` opens in mGBA, but an emulator has no flashcart and no SD card, so
  the save picker, Bank and journal are unavailable. The `delta` build variant below fuses a save
  (and, optionally, a ROM) into the image for emulator use.
- **Nintendo 3DS:** `open_agb_firm` loads it; the same no-SD caveat applies.

## Building

Requires devkitPro `gba-dev` (libtonc), or Docker:

```sh
python3 tools/gen_data.py   # once after cloning: the name/stat tables (gitignored)
make artless                # PokeDNA-artless.gba — the release build
# or
./build.sh                  # Docker, no local toolchain
```

`make artless` is what the Releases page ships (renamed to `PokeDNA.gba`). It needs nothing under
`assets/` and compiles no game-derived art or text; `tools/check_no_desc_text.sh` runs after the
link and fails the build if any description string slipped in.

### Other build variants

```sh
make delta-artless  # emulator build: edits its OWN flash save; fuse a .sav / ROM in with
                    # tools/fuse_sav.py, tools/fuse_rom.py, tools/fuse_gb.py
make                # the full-art build — needs asset packs you generate locally under assets/
                    # (see tools/gen_*.py); it is never a release asset
```

## Host tests

The save-format, map-parsing, conversion, journal and rendering cores are pure C and dual-compile
on the PC. `tests/run_host_tests.py` runs the 168 harnesses against real saves you drop into
`tests/fixtures/`; `make retail-gate` boots the real games headlessly in mGBA on edited saves to
prove the games accept them. Each `tests/host_*_test.c` lists its exact compile line in a header
comment.

## Known limitations

- Writing is EZ-Flash Omega DE only; the EverDrive GBA X5 is read-only by design.
- The hardware queue above: most of v3.0.0 has not been signed off on a cartridge.
- Legality checking is structural — it catches impossible encounters, levels, moves and items, not
  every subtle case a trade checker would.
- History shows the current branch only; undo and redo take about a second each in the emulator.
- Without a ROM the art-free screens are text and chips; that is by design.

See **[CHANGELOG.md](CHANGELOG.md)** for what landed when, and **[ROADMAP.md](ROADMAP.md)** for
the backlog.

## Credits & legality

- **Original code, documented reverse engineering.** Every byte of the save parsing, encryption,
  conversion and editing here is original C, written from scratch. PokeDNA drew inspiration from
  [PKHeX](https://github.com/kwsch/PKHeX) — the idea of a friendly save viewer/editor — but none of
  PKHeX's code was used or ported. The reverse-engineered *facts* it relies on (struct offsets, RAM
  maps, flag numbers, script opcodes) are cited in the source to the
  [pret decompilations](https://github.com/pret) as reference; no decompilation code is included.
- **What ships in the binary:** species, item, move, ability and location *name* tables and
  event-flag names (short identifiers), factual game-mechanics tables (base stats, level-up
  learnsets, egg groups, Gen-3 wild-encounter tables), the tool's own font and stand-in
  graphics, and the vendored libraries below. **What does not:** sprites, icons, wallpapers, screen chrome, descriptions,
  dialogue, maps — all of that is read from your own cartridge dumps at run time and never stored.
- **Your saves and your ROMs stay yours.** PokeDNA reads them off your own card, transiently, and
  copies nothing anywhere except the optional art cache under `/PokeDNA/art/` on that same card.
- **Vendored libraries keep their own licenses:** the flashcart I/O layer (MIT), the EZ-Flash
  `io_ezfo` driver (Apache-2.0), FatFs (BSD-1-Clause), and libtonc. Their notices are retained.
- PokeDNA's own source is **GPLv3** (see `LICENSE`).

## Trademarks / disclaimer

Pokémon, Game Boy, Game Boy Advance and all related names are trademarks of Nintendo, Creatures
Inc. and GAME FREAK Inc. **PokeDNA is an unofficial, non-commercial, fan-made tool and is not
affiliated with, endorsed by, or supported by any of them.** You supply your own save files and
your own copies of the games. Editing save data can corrupt it — keep backups and proceed at your
own risk.

---

© 2026 Guy Shtainer · GPLv3 · built on the [`gba-toolkit`](https://github.com/GuyShtainer) foundation.
