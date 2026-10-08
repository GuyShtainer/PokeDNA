# Changelog

All notable changes to PokeDNA. Versions follow semantic versioning (`MAJOR.MINOR.PATCH`).

## v3.0.0 — Game Boy saves, transfers, undo history, art-free (2026-10-08)

**Hardware status: mostly not yet signed off on a cartridge.** v2.0.0 was validated screen by
screen on an EZ-Flash Omega DE. Everything below was verified in mGBA (scripted screenshot
chains, a virtual SD card power-cut at every write), by the host suite (168 harnesses over real
saves) and by the retail gate (the real games booted headlessly on edited saves, 123 cases).
Four shorter cart sessions (August to late September) exercised parts of it — opening Game Boy
saves, a Yellow summary edit, Day-Care, Pokédex, trainer card, a Bank grab, a Bank → Emerald drop
that came back byte-identical — and found bugs that were fixed afterwards and not re-run on a
cart. Never on a cart: Gen 3 → Game Boy end to end, the undo journal and its power-cut recovery,
Bank/ledger self-heal after a card pull, the 64 KiB save commit, ROM-hack detection, Ruby/Sapphire
art from the SD card, the EverDrive on this build, rumble settings persistence, real-card speed.
The hardware queue is the source of truth; treat this release as a preview until it is signed.

### The release is art-free
- **`PokeDNA.gba` on the Releases page is now the art-free build** (about 1 MB). It compiles no
  game-derived sprites, chrome, maps or text; `tools/check_no_desc_text.sh` fails the build if a
  description string gets in. The full-art build still exists for local use and is never released.
- **Your own ROM supplies the art, at run time:** box icons, front/back sprites, wallpapers, type
  badges, item icons, the bag / trainer-card / Pokéblock-case chrome, item and move descriptions
  (Gen 3 from the `.gba`, Gen 2 from the `.gbc`), the Game Boy sprites and icons, and the
  learnset / encounter data CREATE and LEGALITY use. Ruby and Sapphire are served too.
- **First launch asks once whether to add game ROMs**; *Settings → Game ROM* registers them per
  game (also from inside a Game Boy save); *Extract art* caches the heavy pieces under
  `/PokeDNA/art/`; *ROM art* can be switched off. Every place that would have shown art says
  where it comes from instead.
- **ROM-hack detection:** a registered ROM that is not a retail dump keeps the save viewable but
  locks every write, and says why.
- Reads are cancellable (B) and no longer die on a slow card; the icon cache is one store.

### Game Boy saves (Red, Blue, Yellow, Gold, Silver, Crystal)
- Open a Gen-1/2 `.sav` straight from the picker into the box grid; the same screens in the same
  design as Gen 3: box grid and party, the summary editor (moves and items filtered to that
  game, Crystal's extra fields), create (ROM-free for Gen 1), duplicate, release, move, `.pk1` /
  `.pk2` export, EXPORT ALL / RELEASE ALL.
- Day-Care, trainer card (editable), bag and pack, event flags, Pokédex (with the new per-species
  detail view every generation shares), Fly, the Gen-2 clock screen (offsets and the clock-error
  flag), Hall of Fame.
- Map: Gen 1 draws the player's current map from the ROM and places the player on it with a
  warning and an undo; Gen 2 has an all-maps browser.
- Mail is respected: box / release / duplicate / Day-Care refuse while a letter is attached, the
  way the games do, instead of stranding a mail slot.

### Transfers between generations, through the Bank
- The Bank is the only road between saves (one save open at a time, so nothing clones).
- **Gen 3 → Game Boy** with a confirm screen that lists what travels and what stays; held items
  that cannot travel are placed or left behind, never dropped silently; the Gen-3 original is
  parked in a sidecar so the **round trip back is byte-exact**.
- **Game Boy → Gen 3**: *keep as is* or *make legal*; a Bank Pokémon can drop straight into a
  party slot, and a full party offers to send someone to a box first.
- **TRANSFERS** screen: every parked original, where its copy is, restore / re-link / mark
  finished; records and the ledger heal themselves after a card pull mid-write.

### Undo, redo and the History screen
- No per-screen "Save X?" prompts: every edit (Gen 3 and Game Boy) is held and written once at
  exit, and journaled under `/PokeDNA/journal/`.
- `SELECT`+`L` / `SELECT`+`R` undo and redo on the box grid and summary; a swap's two halves and
  a chained swap undo as one press, with an all-or-nothing rollback.
- **History** (nav menu) lists every step, which are already saved, and jumps to any point.
- After a power cut the next load offers to re-apply the steps that never reached the save.
- *History size* and *Clear history* in Settings; the EverDrive stays untouched.

### Editing and legality
- **LEGALITY** on any Pokémon; **CREATE** asks *legit copy* (built from the ROM's own encounter
  and evolution data, at a level the game could produce) or *from scratch*; static legendaries
  start at their real level; the move picker only offers the species' generation.
- Current HP is editable; the Lilycove museum star is settable (and heals older saves); the
  Pokédex has a detail view; Shedinja keeps its 1 HP when moved to the party; a party Pokémon's
  mail byte is written the way the games write it.

### Safety and robustness
- Bank boxes and the Bank index keep a rolling backup and restore themselves when a primary is
  missing or short; a failed Bank or ledger write names the file that still holds your bytes.
- A failed save is reported as a failure, never a success; cancelling never costs a Pokémon.
- 64 KiB Gen-3 dumps are committed at 64 KiB; a save damaged by an earlier build (two slots that
  disagree) opens on its intact slot and says so.
- Copying a Pokémon can no longer clone it into another save (the clipboard empties on close).
- Build-time guards: every save-image write goes through one funnel; the build fails if the
  variant marker, the stack budget or the EWRAM budget is violated.

### Also
- Pokéblock case with the game's own screen; the real Ruby/Sapphire bag chrome; retail pickup
  timing in the PC; rumble strength and duration persist; dozens of texts that overflowed their
  panel now fit (a lint checks every footer against the 240 px screen).
- The emulator-only `delta` builds are a test vehicle and are not released.

### Known issues (open at release)
- History shows the current branch only; undo and redo take about a second in the emulator.
- Editing a *copy* of a save can make the *original* offer to re-apply the copy's steps.
- A card that fails inside a rename can cross-link a file and its `.tmp`; a failed backup with a
  full backup shelf can lose the oldest backup. Both are being hardened.
- A Day-Care take-out does not teach the moves learned while boarded.
- Without a ROM the art-free screens are text and chips, by design.

## v2.0.0 — everything since the first release (2026-08-04)

*The original v2.0.0 download was withdrawn on 2026-10-08; use the art-free build from v3.0.0.*

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
- The original v1.0.0 download was withdrawn on 2026-10-08; the art-free `PokeDNA.gba` on the
  Releases page is the only binary. The source repository has never contained game art.
- PokeDNA is unofficial and not affiliated with Nintendo, Game Freak, or The Pokémon Company.
  PokeDNA's own code is GPLv3.
