# gba-pokeviewer — developer context / resume guide

> One-stop context for picking this project back up. Read this first, then
> `CLAUDE.md` (toolkit root) for the shared hardware/safety rules. This file is
> kept current at the end of each working session.

**Last updated:** 2026-06 (advanced-editing `66c4f17`; legality V2 `1aeafe1`; named flags `50632ac`;
origin/met checks; **UX polish pass** `28b4216` — app-wide PSG sound, framed dialogs, clarity; then
**editable trainer card** `3c706b9`, **more named flags** `a17fb21`, **editable box name+wallpaper**
`bf66a75`, and the **real 16 box wallpapers** `048eb71`. All host-verified but NOT yet hardware-tested).

---

## 1. What this is

An **on-cartridge "PKHeX for the GBA"**: a Gen-3 Pokémon save **viewer + editor** that
runs on an EZ-Flash Omega DE / Everdrive GBA X5, reads the flashcart microSD via FatFs,
loads any of the 5 Gen-3 saves (Ruby/Sapphire/Emerald/FireRed/LeafGreen), and shows
party + PC boxes + trainer/stats with the game's pixel-accurate UI **plus** the hidden
data (IVs, full EVs, computed stats, nature, ability, shininess) — with full editing.

Lives at `gba-toolkit/projects/gba-pokeviewer/` (own git repo, MIT). It vendors the
toolkit's `lib/` (flashcartio, FatFs, ezfo, everdrive, sys.h) so it's a standalone
public repo. **Ripped art is never committed** — see §4.

### Locked product decisions
- All 5 games; party + PC boxes + trainer/stats together.
- Pixel-accurate sprite UI: full 64×64 front sprites (normal + shiny), 32×32 box icons.
- Summary-cards-first; full faithful trainer memo.
- Full PKHeX-style editing, **EZ-Flash-Omega-only** (writes gated on
  `active_flashcart == EZ_FLASH_OMEGA`; Everdrive is read-only).
- Public-safe repo: code only; ripped art + generated blobs are git-ignored.

---

## 2. Build / test / commit

**Build (local devkitPro):**
```
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C <proj> rebuild
```
or `./build.sh` (Docker). Output: `pokeviewer.gba` (Makefile `TITLE := PKVIEW`,
`gbafix -t`; **do NOT `-p` pad** — see §5 PSRAM). The Makefile auto-globs `source/*.c`
and `source/*.s`, so new files need no Makefile edit.

**Host tests (pure-C core, no hardware):**
```
cc -std=c11 -I source tests/host_edit_test.c source/gen3_save.c source/gen3_mon.c \
   source/gen3_box.c source/gen3_edit.c source/data_tables.c -o /tmp/he
/tmp/he tests/fixtures/POKEMON_EMER_BPEE00.sav     # + FIRE_BPRE01, RUBY_AXVE02
```
The lossless gate: a no-op load→commit must be **byte-identical** for every party + box
mon (host test asserts 0 diffs across Emerald/FireRed/Ruby). Run this after ANY change
to `gen3_edit.c` / `gen3_mon.c` / `gen3_save.c` / `data_tables`.

Legality gate (after any `gen3_legality.c` / `learnsets` / `gen_legality.py` change — first
re-run `python3 tools/gen_legality.py`):
```
cc -std=c11 -I source tests/host_legality_test.c source/gen3_save.c source/gen3_mon.c \
   source/gen3_box.c source/gen3_legality.c source/learnsets.c source/data_tables.c -o /tmp/hl
/tmp/hl tests/fixtures/POKEMON_EMER_BPEE00.sav   # asserts 0 false-positive move warnings
```

**Commit pattern** (verify no git-ignored generated/art file leaks first):
```
git -c user.name="Guy Shtainer" -c user.email="293649481+GuyShtainer@users.noreply.github.com" commit -q ...
# end body with:  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

**Memory budget:** ROM 2.97 MB (9.3% of 32 MB). EWRAM ~205 KB of 256 KB live (the
128 KB `g_save` + reassembled blocks + the LZ77 sprite scratch). `--print-memory-usage`
reports ewram ≈ 1.3 KB only because the big `EWRAM_BSS` buffers land in a region it
doesn't tally — this is normal and has always been the case.

---

## 3. Module map (`source/`)

**Pure-C core (host-dual-compiled, no tonc/sys.h — only `<stdint.h>`/`<string.h>`):**
- `gen3_save.{c,h}` — sector framing, checksums, slot selection, SaveBlock1/PC reassembly,
  per-mon decrypt kernel (`k_substruct_pos[24][3]`, key = personality^otId).
- `gen3_mon.{c,h}` — full per-mon decode → `PkMon` (species, IVs/EVs/evSum, nature,
  ability, shiny/egg, moves/pp, computed stats). `pk_read_party_auto` (detects FRLG by
  validity — FRLG party at SB1+0x034/0x038 vs R/S/E 0x234/0x238). Stat order
  HP,Atk,Def,**Spe,SpA,SpD** (PK_HP..PK_SPD; note Spe before SpA internally).
- `gen3_box.{c,h}` — PC storage read (`pk_read_box`, `pk_resolve`, box names/wallpaper).
- `gen3_clip.{c,h}` — **mon clipboard + slot ops** (copy/paste/dup/release foundation):
  `clip_copy_from`/`clip_to_record` (box↔party kind conversion via `em_set_party_flag`),
  `pk3_validate`, box-slot write/clear, and **count-aware** party append/release (the
  gap-free party invariant — append refuses at 6, release shifts-down + decrements).
- `gen3_legality.{c,h}` — `pk_check_legality(PkMon)` → `PkLegality` (V1 structural checks:
  bad-egg, EV>510, level/EXP mismatch, bad moves/PP, ability slot, met-level, met-level>100,
  **origin-game validity**, **met-location dead-zone** (0xD6..0xFC = used by no game), ball, …)
  **plus V2 move-source** (warn-only, sev 0): flags a move the species *line* can't learn by any
  Gen-3 method. Independent of the PP check (a doubly-tampered move fails both). The *simple*
  met-location validity (is the location a real place) is done; the full species-vs-met-LOCATION
  check ("Skitty on Route 101") stays deferred — see the comment in `gen3_legality.c` (needs a
  PKHeX-scale per-game MAPSEC + wild-table + special-source-exclusion dataset to stay zero-FP).
- `learnsets.{c,h}` — `pk_can_learn(species, move)` over a generated per-species bitset
  (`learnsets.c` git-ignored, 45 B/mon, ~18.5 KB ROM `.rodata`). Built by
  `tools/gen_legality.py` as the **union across all three Gen-3 decomps** (pokeemerald +
  pokefirered + pokeruby) of level-up + egg moves over the *whole pre-evolution chain*, PLUS
  all TM/HM + tutor moves accepted **globally** (per-species TM/tutor parse is fragile;
  over-accepting them = zero false positives), PLUS code-taught specials no data file lists
  (Blast Burn/Frenzy Plant/Hydro Cannon FRLG ultimate tutor; **Volt Tackle** Light-Ball egg
  move). Conservative by design: a wrong-TM-combo hack is missed on purpose so a legit mon is
  never flagged. **Cross-game union is mandatory** — Emerald-only data false-flags legit FRLG/RS
  mons (Mr. Mime/Magical Leaf, Charizard/Blast Burn, Togetic/AncientPower, Dugtrio/Fury Swipes).
- `gen3_flags.{c,h}` — event-flag get/set (plaintext bit array, per-game base offset) +
  `pk_named_flags(g, &nf)` → a generated **per-game curated** NamedFlag list (badges with the
  right Hoenn/Kanto names + key system flags: Pokédex, National Dex, PokéNav (RSE), Game-clear,
  Running Shoes, …). Flag numbers differ per game, so the table is resolved per game (verified
  against known values — E badge1 0x867, FRLG 0x820, RS 0x807). `num == NAMED_FLAG_HEADER` rows
  are category headers. Emitted into `data_tables.c` by `gen_data.py`.
- `gen3_items.{c,h}` — item-bag pockets (per-game offsets; quantity XOR'd with the SB2 key
  on E/FRLG, plaintext on RS).
- `gen3_edit.{c,h}` — **the lossless edit core.** `EditMon{raw[100], sub[4][12] canonical
  G/A/E/M, personality, otId}`. `gen3_edit_load`/`gen3_edit_commit` (patch-in-place +
  re-encrypt; no-op = byte-identical). `em_set_*` mutators, `em_reroll` (bounded shiny-PID
  construction), `recompute_party_stats`. **em_set_move clears the slot's PP-Up bonus**
  (PP Ups bind to the move, in Growth byte 8).
- `gen3_trainer.{c,h}` — trainer card / game-stats / HoF (Elite-Four) clear time.
- `data_tables.h` + generated `data_tables.c` — `pk_species_name/national_no/base_stats/
  type1/type2/ability/growth`, `pk_move_name/type/pp/power/accuracy/desc`, `pk_item_name/
  desc`, `pk_ability_name/desc`, `pk_nature_*`, `pk_type_name`, `pk_location_name`.

**GBA glue (tonc; never contains format logic):**
- `ui.{c,h}` — Mode-3 bitmap primitives: `ui_panel/ui_text/ui_text_sel/ui_truncate/
  ui_hline/ui_fill_rect/ui_progress/ui_sprite` (0x8000-keyed blit), `ui_icon_sub` (32→16),
  `ui_icon_scaled` (32→N nearest-neighbour). Color tokens `UI_TITLE/PANEL/BORDER/DIM/OK/
  WARN/DIRCLR/SAVECLR/SEL/SELTEXT/TEXT`.
- `osk.{c,h}` — on-screen keyboard, QWERTY ASCII (no shift). `osk_input` (names, no empty)
  / `osk_search` (allows empty so you can clear a query). Hold B clears fast.
- `snd.{c,h}` — tiny PSG UI sound effects (no samples/maxmod/EWRAM): `snd_move/ok/back/tab/
  deny/edit/save/error/boot`, an envelope so notes auto-decay, a 4-note queue ticked by
  `snd_vblank()` (called from every screen's vsync) for the save/boot jingles, and a
  `snd_set_enabled` mute. Hooked at the input chokepoints (`wait_keys` + each file's `s_wait`,
  fresh presses only so held scroll stays silent) + explicit save/error/deny sites. `snd_init()`
  runs once in `init_system()`.
- `pkview_pick.{c,h}` — the rich editor pickers: `pick_species` (icon grid, name+number
  search, filter L/R + START menu, sort, partial redraw), `pick_move` (list + real type
  badge + power/acc/PP + desc, sort, type filter), `pick_item` (4 view modes — see §6),
  `pick_nature` (list), `pick_ability` (species' two abilities by name+desc), `type_chip`
  (text) / `type_icon` (real 32×14 badge).
- `pkview_edit.{c,h}` — `F_*` field enum + `em_field_press`/`em_field_adjust` dispatchers
  (the inline editor's per-field behavior).
- `pkview_summary.{c,h}` — `pkview_inspect(rec, is_party, can_edit, out_rec)`: the 6-card
  view+EDIT screen (INFO / SKILLS / IVs / EVs / BATTLE MOVES / CONTEST MOVES). Cards
  register editable fields via `reg(field,x,y,w)`; U/D moves the field cursor, A / LEFT-
  RIGHT edit in place. SKILLS stat rows edit that stat's **EV** (the only persistent lever).
- `pkview_box.{c,h}` — Emerald-style PC: tab bar, grass wallpaper + leaf motif, tan banner,
  checkered front-sprite monitor, 32×32 icon grid, white Gen-3 hand cursor.
- `pkview_trainer.{c,h}` — trainer/stats screen.
- `pkview_main.c` — entry point: `init_system` (tonc + key repeat) → `flashcartio_activate`
  → `f_mount` → `browse_pick` (file browser w/ sort/filter/menu/reboot) → `view_save` →
  party_list / box loop. Owns `app_can_edit`, `app_commit_block` (the one safe write path:
  section re-checksum → `sf_backup` → `sf_write_verified`), `app_edit_commit`,
  `app_quick_item`/`app_quick_moves`, `app_mon_menu` (the PC-style A menu).
- `pkview_app.h` — shared app glue (`app_can_edit`, `app_edit_commit`, `app_mon_menu`).
- `savefile.{c,h}` — `sf_read_full`, `sf_backup`, `sf_write_verified` (.tmp → byte-compare
  re-read → unlink → rename), `sf_status_str`.
- `log.{c,h}`, `gba_rtc.{c,h}` — vendored logger + RTC.

**Generated, git-ignored** (regenerate locally): `mon_icons.*` + `_data.s`, `mon_front.*`
+ `_data.s`, `type_icons.*`, `item_icons.*` + `_data.s`, `hand_cursor.*`, `data_tables.c`,
and `data/*.bin`.

---

## 4. Asset pipelines (`tools/`) — all output is git-ignored

| Script | Reads | Emits (git-ignored) |
|---|---|---|
| `gen_icons.py` | `assets/sprites/.../Icons/` | `data/mon_icons.bin` + `source/mon_icons.{c,h}` + `_data.s` — 32×32 box icons |
| `gen_front.py` | `assets/sprites/.../Front[ shiny]/` | `data/mon_front[_shiny].bin` + `mon_front.{c,h}` + `_data.s` — 64×64 front sprites, **LZ77-compressed** (see §5) |
| `gen_types.py` | `assets/types/*.png` (pokeemerald) | `source/type_icons.{c,h}` — 18 real 32×14 type badges |
| `gen_items.py` | `assets/items/{icons,icon_palettes,meta}/` | `data/item_icons.bin` + `source/item_icons.{c,h}` + `_data.s` — 24×24 item icons (resolves the decomp's decoupled id→pic + id→palette tables; deduped) |
| `gen_hand.py` | `assets/storage/hand_cursor.png` | `source/hand_cursor.{c,h}` — Gen-3 PC hand, recolored WHITE |
| `gen_data.py` | `reference/{pokeemerald,pokefirered,pokeruby}_data/` (trimmed decomp; flags.h from all 3) | `source/data_tables.c` — all name/desc/stat tables + per-game **named-flag** tables (`pk_named_flags`: badges, system, gyms, Elite Four, legendaries) |
| `gen_wallpaper.py` | `reference/wallpapers/<name>/{bg,frame}.png + tilemap.bin` (decomp) | `source/wallpapers.c` — the 16 PC box wallpapers as deduped 8×8 RGB15 tiles + a 20×18 tilemap each (`wallpaper_tile_data`/`wallpaper_tilemap`). Reconstructed exactly as the game composites them (verified vs all 16). ~164 KB ROM, no EWRAM. |
| `gen_legality.py` | `reference/{pokeemerald,pokefirered,pokeruby}_data/` (level-up + egg + tutor + tms_hms + evolution) | `source/learnsets.c` — per-species learnable-move bitset for `pk_can_learn` (3-game **union**; see §3 `learnsets`) |

- Species are keyed by **INTERNAL Gen-3 id** (from `reference/.../constants/species.h`),
  NOT the national+25 shortcut (that mismapped legendaries — Kyogre→Registeel).
- Item descriptions: `gen_data.py` pulls them from `src/data/text/item_descriptions.h`
  (fetched into the trimmed local reference), mirroring the move-description path.
- Pixel convention everywhere: `u16` per pixel, `0x0000` transparent / `0x8000|rgb15` opaque.
- `.incbin` pattern for big blobs (a generated `*_data.s` `.incbin`s a `data/*.bin`); the
  Makefile uses `DATADIRS :=` (empty) so there is NO `bin2o` double-include.

To regenerate everything after pulling the repo fresh you need the local-only assets
(the sprite pack under `assets/sprites/`, plus the pokeemerald reference under
`reference/`); run the relevant `gen_*.py`, then build.

---

## 5. Hard-won lessons / gotchas (READ before changing these)

- **EZ-Flash PSRAM ceiling (~7.5 MB), NOT the 32 MB cart limit.** Our tool needs the ROM
  PSRAM-resident for the OS-mode SD path, and that pool is ~7.5 MB. A 7.63 MB build hung
  at the EZ-Flash "LOADING GAME" kernel screen (before our code runs); a 7.32 MB build
  loaded. **Do not let the ROM grow toward 7 MB.** Power-of-2 padding (`gbafix -p`) was a
  wrong fix — it re-inflates and the tiny sibling tools load unpadded; it's reverted.
  Front sprites are the big lever, so they are **LZ77-compressed** (`gbalzss`) and
  decompressed one-at-a-time at render via BIOS `LZ77UnCompWram` into a single 8 KB EWRAM
  scratch buffer (`mon_front_for`). Both 64×64 blobs total 1.7 MB; ROM is 2.97 MB. This
  technique can absorb future sprite-heavy features for free. Blobs are `.balign 4` +
  per-entry 4-byte padded (the BIOS call needs a word-aligned src).
- **tonc key-repeat:** `key_hit()` NEVER returns repeats — `key_repeat()` is a separate
  function. Every wait loop ORs `key_hit(m) | key_repeat(m & dpad)` to get held-scroll.
  `key_repeat_mask`/`key_repeat_limits` are set once in `init_system`.
- **Edge-scroll:** keep `top` (first visible row) **persistent** across frames; only move
  it when the cursor would leave the page. Recomputing `top` from `sel` each frame pins the
  cursor to an edge (the bug the user hit). pick_species / pick_item / list_pick / pick_move
  / browse_pick / filter_menu all do persistent edge-scroll now.
- **Grid perf:** never `ui_clear()` + re-blit a whole icon grid on every cursor move (too
  slow on Mode-3). Full redraw only on scroll/list/view change; on cursor move just swap the
  selection frame + repaint the small header/desc strip. (`pick_species`, `pick_item`.)
- **Gen-3 ability is a 1-bit slot.** A mon can only have its species' ability 0 or 1; the
  game recomputes anything else. `pick_ability` offers exactly those two (PKHeX does the
  same). An arbitrary-ability picker is impossible for vanilla Gen-3.
- **Stats are derived** (base+IV+EV+level+nature). You can't persistently set a final stat —
  edit IVs/EVs. The SKILLS card routes stat-row edits to that stat's EV.
- **m3_plot does NOT clip** — any x≥240 or y≥160 corrupts memory. Trace every new coordinate.
  TTE text is 8px tall/8px per char from its top-left.
- **Reboot-to-loader** lives in the vendored lib now (the pokeviewer's `lib/` was older than
  the toolkit's and lacked it). `flashcartio_reboot` gates per cart, refuses mid-transfer,
  IRQs off; Omega → `_EZFO_reboot` (SetRompage BOOTLOADER + SoftReset). **Not emulatable —
  needs hardware sign-off.**

---

## 6. Feature status

**Done + on-hardware-validated (earlier milestones):** M0 picker, V1 6-card summary,
V2 box grid, V3 trainer screen, V4 edit (lossless core host-verified; gated verified-write
commit). Species mapping fix (internal ids). Editing confirmed working on real Omega DE.

**Done this session (commits `3f46ac2`..`741dc7d`), build-clean + host-tests green:**
- Inline view+edit summary (no View/Edit menu); PC-style A action menu (SUMMARY/ITEM/MOVES).
- Real Gen-3 **type badges**, **white PC hand** (above the mon), Emerald-style **box chrome**.
- **Species picker**: 32×32 icon grid, name **and number** search, L/R + START filter menu,
  sort, partial-redraw (fast), edge-scroll.
- **Move picker**: real type badge, fixed-width Pow/Acc/PP, sort, type filter.
- **Item picker**: real 24×24 icons, **item descriptions**, **4 view modes** (L/R cycles:
  list / icons / grid / split-with-description), sort, search.
- **Ability picker** (species' abilities, name+desc).
- **Shiny sprites** restored at full 64×64 via LZ77 compression.
- **Held-arrow auto-repeat** + **edge-scroll** everywhere.
- **SAV file browser**: sd-browser look + SELECT=sort (Name/Size/Date ×asc/desc),
  START=FILE MENU (sort key/order, .sav-only⇄all, show-hidden, **Reboot to flashcart menu**).

**Advanced editing batch (`7ed5725`..`66c4f17`), build-clean + 4 host gates green (edit/clip/legality/data):**
- **Copy / Paste / Duplicate / Release** + **move held item** (take/give) in the PC `A` menu;
  empty box slots offer PASTE HERE. All ride the one `app_commit_block` write path.
- **Legality V1+V2-moves** card (`LEGALITY` action) — structural hacked-mon flags plus a
  warn-only "move not learnable by species" check (see `learnsets.{c,h}`). Read-only; **no SD
  write**, so it needs no hardware sign-off. Host-verified: 0 false positives across 651 valid
  mons in all 3 fixtures; survived a 4-dimension adversarial review (the one real FP it found —
  Volt Tackle on the Pichu line — is fixed + regression-guarded).
- **Export `.pk3`** (`EXPORT .pk`) to `/pokeviewer/bank/`.
- **External bank** (START → MENU → Bank): a grid of stored `.pk3`; inject into the loaded
  game's first free box slot (record is byte-identical across all 5 games) / delete. Also the
  **import** path — drop PKHeX `.pk3` files into `/pokeviewer/bank/`.
- **Data editor** (START → MENU → Data editor, Omega-only): COUNTERS (named game stats) / BAG
  (all 5 pockets) / FLAGS. The FLAGS tab now opens on a **named list** (badges + system flags +
  **gyms, Elite Four, legendaries**, ON/off, A toggles with a one-time soft-lock caution) with a
  drill-in to the guarded raw `#N` browser (`flags_raw_view`, B returns). Edited in RAM, committed once on exit.
- **Editable trainer card** (Omega): U/D pick a field, A edits — NAME (OSK), SEX, ID No, SID,
  MONEY, play TIME. Commits SaveBlock2 (section 0, `app_commit_sb2`) + money to SaveBlock1
  (`app_commit_sb1`). Dex/HoF/records stay read-only.
- **Editable box** (Omega): move the hand UP onto the box title → Rename / Wallpaper. Rename
  writes the box-name field; the wallpaper chooser previews + sets the id. Both commit PC
  storage (`app_commit_pc`). The box screen renders the **real Gen-3 wallpapers** (all 16) via
  the generated `wallpapers.c` (`draw_wallpaper`).

**Pending hardware sign-off (not emulatable):**
- The **reboot-to-loader** path (START → Reboot → A should land in the EZ-Flash game list).
- **The whole advanced-editing batch** — copy/paste/release (mutates party structure), `.pk3`
  export, bank inject/delete, and counter/bag/flag writes are all new SD-write paths.
- Re-confirm any SD-write path after big changes.

**Next (deferred):** legality V2 **encounter** half (the "Skitty on Route 101" check — needs
generated wild-encounter + met-location tables, the messy per-game part; move-source half is
already done). The named-flag editor could be widened later (trainers-defeated, hidden-items)
by adding more curated symbols to `NFLAG_SYSTEM`/category lists in `gen_data.py`.

---

## 7. How to resume / add things

1. Read this file + toolkit `CLAUDE.md` (OS-mode rule, verified-write, Omega-only writes).
2. Pure-C core change? Run host tests (the lossless gate) before touching hardware.
3. New ripped art / data? Add a `gen_*.py`, output to a git-ignored path, mirror the
   `.incbin` (big) or direct-`.c` (small) pattern, add to `.gitignore`, **verify no leak**
   before committing.
4. Watch ROM size (§5 PSRAM); compress sprite-heavy additions.
5. UI: reuse `ui.c`; for any scroll list use persistent-`top` edge-scroll + `key_repeat`
   held-scroll + partial redraw for icon grids.
6. Anything touching SD-write / RTC / reboot → **`hardware-testing-protocol` sign-off**.

**The proven reference implementation** for the modules not unique to this tool is the
Pokémon record-mixer at `gba-toolkit/projects/pokemon-record-mixer/`.
