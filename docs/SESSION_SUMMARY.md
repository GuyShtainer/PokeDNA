# gba-pokeviewer — developer context / resume guide

> One-stop context for picking this project back up. Read this first, then
> `CLAUDE.md` (toolkit root) for the shared hardware/safety rules. This file is
> kept current at the end of each working session.

**Last updated:** 2026-06 (after the file-browser sort/filter/reboot batch, commit `741dc7d`).

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
| `gen_data.py` | `reference/pokeemerald_data/` (trimmed decomp) | `source/data_tables.c` — all name/desc/stat tables |

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

**Pending hardware sign-off (not emulatable):**
- The **reboot-to-loader** path (START → Reboot → A should land in the EZ-Flash game list).
- Any SD-write edit path is already gated + verified, but re-confirm after big changes.

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
