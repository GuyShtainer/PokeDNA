# PokeDNA — Handoff

> Living resume doc maintained by the `handoff` skill. The **Current status** and **Next steps**
> sections are always kept current — start there to resume. The **Session log** grows downward,
> newest first, and is never pruned.
> Last updated: 2026-06-12

## Current status

- **Repo / branch:** `projects/PokeDNA` (own git repo, under the git-ignored
  `gba-toolkit/projects/`) / `main`. Public: **`github.com/GuyShtainer/PokeDNA`** (GPLv3). History
  scrubbed to the no-reply identity. Pushed through the rebrand; the **v1.0.0** release commits +
  tag may still be local — check `git log origin/main..HEAD`.
- **Goal:** an on-cartridge original Gen-3 Pokémon save **viewer + editor** (do **not** brand it
  "PKHeX for GBA" — it's the user's own work) that runs on EZ-Flash Omega DE / Everdrive GBA X5,
  reads the flashcart microSD via FatFs, loads any of the 5 Gen-3 saves (R/S/E/FR/LG), and shows
  party + PC boxes + trainer/stats with the game's pixel-accurate UI plus the hidden data — with
  full editing (writes are **EZ-Flash-Omega-only**).
- **State right now: `v1.0.0` — hardware-validated, released-grade.** Feature-complete viewer **and**
  editor, all committed on `main`. ROM **~3.6 MB** (under the ~7.5 MB PSRAM ceiling); EWRAM **~242 KB
  / 256 KB**. Host gates green (box test's 216/2 is the known garbage-fixture baseline).
  **Validated on a real EZ-Flash Omega DE (2026-06-15):** flags/counters, copy/duplicate/move, the
  new per-box SD **bank** (byte-identical round-trips that survive a power-cycle), and a **full
  from-scratch mon edit** (species+stats+moveset) that loads and **battles correctly in-game** — no
  corruption, no crashes. The release `PokeDNA.gba` **bundles Gen-3 sprites** (© Nintendo/Creatures/
  Game Freak), PKHeX-style — source repo ships none (git-ignored). **Known gaps (→ v1.1):** edited/
  injected mons aren't registered in the Pokédex, and the Poké Ball + met/caught location aren't
  editable yet.
- **7-fix batch — DONE this session (uncommitted, plan: `~/.claude/plans/glistening-drifting-willow.md`):**
  1. **Un-mirrored PC box icons** — `pdna_box.c:blit_icon` reads forward again (was mirrored in `91d8de6`).
  2. **Sticky summary card** — `pdna_inspect` gained an in/out `int* card`; `app_box_browse` keeps it across mon-scroll.
  3. **LEFT/RIGHT on the box title flips boxes** — added to the `on_title` branch in `pdna_box.c` (fresh-press only).
  4. **Real-PC grab animation** — `gen_hand.py` now emits 3 frames (open/reach/grab); move-mode plays a pickup
     animation and the held mon **rides the closed-fist cursor** (`play_grab_anim`, carry render in `render_full`).
  5. **Bank = parallel set of 16 boxes** — big refactor: `pdna_box` now drives a **`BoxSource`** vtable; new
     `source/pdna_bank.c` stores the bank as per-box files (`/PokeDNA/bank/boxNN.box`) + `bank.meta`
     (names+wallpapers), paged one box at a time (EWRAM-safe), migrates old flat `.pk3` on first open. Mons move
     in/out via the **universal copy/paste clipboard** (old inject/withdraw menu retired). `app_mon_menu` /
     `app_box_browse` / quick-editors now take an `AppCommitFn commit` instead of `(sect_lo,sect_hi)`.
  6. **Deferred save for moves only** — move-mode drop calls `src->mark_dirty` (PC: `app_mark_pc_dirty`, no write);
     `flush_pc_on_exit` (in `view_save`) asks **once** on B-exit; A=`app_commit_pc`, B=revert `g_pc` from `g_save`.
     The bank mirrors this (`pdna_bank_show` prompts on bank-exit). All other edits keep their immediate prompt.
  7. **Comprehensive flags** — `gen_data.py` auto-scans every meaningful `FLAG_*` (hidden items, item balls, got/
     received, trainers, system) excluding TEMP/HIDE/UNUSED noise: **E=491 F=304 R=395** named flags (+~22 KB ROM).
     Flags tab gained a **SELECT = jump-to-next-category** control.
- **Done (this + prior sessions, with hashes):**
  - Core viewer + lossless editor; legality V1+V2 move-source (`1aeafe1`), origin/met checks,
    simple met-location dead-zone (`9c2f89e`); named-flag editor (`50632ac`, `a17fb21`).
  - UX polish pass `28b4216` — app-wide PSG **sound** (`source/snd.{c,h}`), framed dialogs,
    "Saving" busy panel, clarity labels, save-success flourish.
  - Editable **trainer card** `3c706b9` (name/sex/TID/SID/money/time) + **badge & Battle-Frontier
    toggles** `5897463`.
  - Editable **box** name/wallpaper `bf66a75`; the **real 16 box wallpapers** generated from the
    decomp `048eb71`; **partial redraw + mirrored icons** `91d8de6`; **move-mode** `4bc8608`;
    **Emerald secret/Walda wallpapers** `d944573`+`1fab267`.
  - **Summary VIEW/EDIT** rework + scroll-through-box `f871bf7`.
  - **Unown** real letters everywhere `18d2df0` + form choice on species set `b0d0077`.
  - Browser LEFT/RIGHT fast-jump `fef2104`.
  - Toolkit repo (separate): `docs/kb/file-browser-conventions.md` (`7a12a6e`) + `learn`-skill
    lessons (`3042841`) + the `handoff` skill itself.
- **Blocked / needs the user:** **Hardware sign-off** of the untested SD-write batch (the user's
  task, deliberately deferred). Nothing is half-built.

## Next steps (resume here)

1. **Commit the 7-fix batch** (working tree is dirty; nothing committed yet). Suggested grouping —
   small UX fixes (1-3), grab animation (4, incl. regenerated `hand_cursor.{c,h}` are git-ignored —
   commit `gen_hand.py` only), the bank rework + deferred-save (5-6, incl. new `source/pdna_bank.*`
   and `tests/host_bank_test.c`), flags (7, commit `gen_data.py` — `data_tables.c` is git-ignored).
   Flag each SD-write path commit "NOT hardware-tested" per convention. Use the `git-commit` skill.
2. **Hardware-validate on a real EZ-Flash Omega DE** (the user batches this). New things this batch
   added that the emulator cannot prove — exercise on disposable copies:
   - **Bank as boxes:** first-open **migration** of any old `/PokeDNA/bank/*.pk3` into `boxNN.box`
     + `bank.meta`; rename a bank box, change its wallpaper; **copy a mon PC→bank and bank→PC** (and
     bank→party) via the clipboard; move/swap within a bank box; confirm each `boxNN.box` re-reads
     intact and an interrupted write leaves the original recoverable (`.tmp`/rename).
   - **Deferred move-save:** rearrange several mons in a PC box, press B → the single "Save box
     changes?" prompt; verify **A saves** and **B discards** (reloads the box unchanged). Same on
     bank-exit. Confirm other edits (paste/release/rename/wallpaper/summary/trainer/data) still
     prompt+save immediately.
   - Re-run the prior SD-write checklist (copy/paste/release, `.pk3` export, data editor, trainer
     card, box rename/wallpaper incl. an Emerald Walda one, summary edits). Use the
     `hardware-testing-protocol` agent.
3. **(Optional, big) Legality V2 encounter half** — the "Skitty can't appear on Route 101" check.
   Deferred on purpose: to stay zero-false-positive it needs per-game MAPSEC tables, MAP→MAPSEC
   aggregation, wild tables, evolved-mon skipping, and a curated special-source exclusion set
   (PKHeX-scale). See the comment in `source/gen3_legality.c`.
3. **(Cosmetic) Sky wallpaper** has a 1-px dark strip at its very bottom edge (mostly below the
   visible box area) — chase only if it shows on hardware.
4. **Push** the local `main` if/when the user wants it on a remote.

## How to build / test / run

```
# Build the ROM (local devkitPro; ./build.sh uses Docker if preferred):
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C projects/PokeDNA rebuild
#   -> projects/PokeDNA/PokeDNA.gba   (TITLE=PokeDNA; do NOT gbafix -p pad — PSRAM ceiling)

# Regenerate the git-ignored data after editing a generator (run from the project root):
python3 tools/gen_data.py        # data_tables.c (names/stats + comprehensive per-game named flags: E491/F304/R395)
python3 tools/gen_legality.py    # learnsets.c (3-game move-source union)
python3 tools/gen_wallpaper.py   # wallpapers.c (16 standard + 16 Walda); add --sheet out.png to eyeball
python3 tools/gen_front.py        # mon_front.c (front sprites + 28 Unown forms)  [needs Pillow + gbalzss]
python3 tools/gen_icons.py        # mon_icons.c (box icons + 28 Unown forms)       [needs Pillow]
python3 tools/gen_hand.py         # hand_cursor.{c,h} — 3 frames open/reach/grab (PC hand)  [needs Pillow]

# Host tests (pure-C core, no hardware) — e.g. lossless edit gate + legality + bank layout:
cc -std=c11 -I source tests/host_edit_test.c source/gen3_save.c source/gen3_mon.c \
   source/gen3_box.c source/gen3_edit.c source/data_tables.c -o /tmp/he && /tmp/he tests/fixtures/*.sav
cc -std=c11 -I source tests/host_bank_test.c source/gen3_save.c source/gen3_mon.c \
   source/gen3_box.c source/gen3_edit.c source/gen3_clip.c source/data_tables.c -o /tmp/hb && /tmp/hb tests/fixtures/*.sav
#   NOTE: gen3_box.c + gen3_trainer.c reference gen3_encode_char, so any host test linking them must
#   ALSO link source/gen3_edit.c (see each test's header comment for the exact cc line).
#   box test: 216 mons / 2 fails is the EXPECTED baseline (2 pre-existing garbage fixture slots).
```

## Key decisions (and why)

- **Writes Omega-only; all SD writes batched for hardware testing.** EZ-Flash writes have no retry;
  Everdrive write isn't wired. The user explicitly defers HW testing — keep shipping features, flag
  each new SD-write path "NOT hardware-tested" in its commit, don't re-prompt for sign-off.
- **Move-source legality unions all 3 decomps + over-accepts TM/HM/tutor; warn-only.** Zero false
  positives is the priority — a wrong-TM-combo hack is missed on purpose.
- **Unown form choice searches for a PID, never bit-twiddles.** Every PID-derived trait moves
  together; `em_set_unown_form` brute-forces a PID with the target letter while keeping nature/shiny.
- **Secret wallpapers ride the cartridge's single Walda slot.** Box byte 16 (`WALLPAPER_FRIENDS`) +
  the `WaldaPhrase` config (SB1 `0x3D70`, Emerald-only); all "Friends" boxes share it, as in-game.
- **Bank stored as per-box files, not one `bank.bin` (plan deviation, intentional).** EWRAM has no room
  for the whole bank, so it pages one box at a time; per-box `boxNN.box` (2400 B each) + `bank.meta`
  means every write is a small, fully-`sf_write_verified` round-trip with NO whole-file buffer. Bank
  has 16 boxes, plain-ASCII names, standard wallpapers only (no Walda).
- **Deferred save is move-only; everything else commits immediately.** Per the user: rearranging mons
  must not nag — it marks dirty and asks once on leaving the save (PC) / bank. A reverts cleanly because
  uncommitted moves live only in `g_pc`/the bank box buffer, never in `g_save`/the box file yet.
- **Clean-room / generated art is git-ignored.** `data_tables.c`, `learnsets.c`, `wallpapers.c`,
  `mon_*.c/.s`, `hand_cursor.{c,h}`, and everything under `reference/` are generated locally and
  never committed — commit the **generators** (`tools/gen_*.py`), not their output.
- **One box screen, two sources (`BoxSource` vtable in `pdna_box.h`).** The PC and the bank share
  `pdna_box`; each supplies `records(box)`/name/wallpaper/`commit`/`mark_dirty`. The bank is
  **paged** (one 2400-byte box in RAM) because EWRAM has no room for a second full PC blob.
- **Bank moves mons via the universal clipboard, not a bespoke menu.** Copy in one place, paste in
  another (PC↔bank↔party) — so the bank really is "just more boxes". The old inject/withdraw is gone.

## Where things live

- `source/pdna_box.c` — the **shared box screen** over a `BoxSource`: `render_full`/`move_cursor`
  (partial redraw), `blit_icon` (un-mirrored now), the **carry render + `play_grab_anim`** (move-mode
  grab), `box_options_menu`+`wallpaper_pick` (rename/wallpaper), title LEFT/RIGHT box-flip. `BoxSource`
  is defined in `pdna_box.h`.
- `source/pdna_bank.c` — the **bank as 16 parallel boxes**: per-box files `boxNN.box` + `bank.meta`,
  paged via `banksrc_records`, `migrate_flat_pk3` (one-time `.pk3` import), `pdna_bank_show` (builds
  the bank `BoxSource`, runs `pdna_box`, prompts deferred-move save on exit).
- `source/pdna_summary.c:pdna_inspect` — VIEW/EDIT modes; returns nav 0/±1 + `*saved`, and now
  threads an in/out `int* card` (sticky card across mon-scroll). Driven by `pdna_main.c:app_box_browse`.
- `source/pdna_trainer.c` — editable trainer card + `flag_set_editor` (badges/frontier).
- `source/pdna_main.c` — the one safe write path `app_commit_block` + wrappers `app_commit_pc/
  _sb1/_sb2`, `app_walda_pattern/app_set_walda`, `app_box_browse`, `app_mon_menu` (the A-menu).
- `source/snd.{c,h}` — PSG UI sound; hooked at `wait_keys` + each file's `s_wait` (fresh presses).
- `source/gen3_*.{c,h}` — pure-C save cores (parse/decrypt/edit/box/clip/legality/flags/items/
  trainer). `gen3_mon`: `pk_unown_form` + `PkMon.form`. `gen3_box`: box name/wallpaper + Walda.
- `tools/gen_*.py` — the 6 generators (data tables, learnsets, wallpapers, front, icons, **hand**).
- **Reference impl / siblings:** `gba-toolkit/projects/sd-browser` (file-browser conventions),
  `projects/pokemon-record-mixer` (origin of gen3_save/ui). Toolkit KB: `docs/kb/`.

## Gotchas / constraints

- **EZ-Flash PSRAM ceiling ~7.5 MB** (NOT the 32 MB cart limit) — the load-game kernel hangs above
  it. Don't `gbafix -p` pad; LZ77-compress big art. ROM now ~3.57 MB.
- **OS-mode rule:** never render/sound/IRQ during an SD transfer; wrap them around the write.
- **Verified-write always:** `.tmp`→re-read compare→rename, immutable backup first; never corrupt
  user data.
- **EWRAM budget ~242 KB/256 KB live** (~14 KB headroom). The bank holds only ONE box (2400 B) in
  RAM — a second full 35 KB PC blob does NOT fit. Wallpapers/Unown/hand sprites are const ROM, 0 EWRAM.
- **rapid raw.githubusercontent fetches get throttled** to 0 bytes — fetch sequentially with
  `--retry-delay` + `sleep` when pulling decomp assets.

## Related docs

- `docs/SESSION_SUMMARY.md` — the curated, detailed developer/resume guide (module map, the 5+1
  asset pipelines, build/test/commit, hard-won gotchas, full feature status). Read it for depth;
  this HANDOFF is the quick resume point.

---

## Session log

### Session — 2026-06-12 (7-fix UX batch — code-complete, uncommitted)

- **Intent:** resume the queued 7-fix batch (un-mirror PC icons; sticky summary card; LEFT/RIGHT box
  flip on the title; real-PC grab-hand animation; **bank = parallel set of boxes**; deferred save for
  moves only; **discover all flags**). User: "if needed plan first and do things in phases."
- **Did:** recon (manual + a 5-reader workflow) → wrote/approved a phased plan
  (`~/.claude/plans/glistening-drifting-willow.md`) → implemented all 7 in 5 phases, building clean
  after each:
  - **A** fixes 1-3 (3 small edits in `pdna_box.c`/`pdna_summary.*`/`pdna_main.c`).
  - **B** fix 4 — `gen_hand.py` now extracts 3 frames (open/reach/grab); move-mode plays a pickup
    grab animation and the held mon rides the closed-fist cursor.
  - **C** fix 6 — PC dirty flag (`app_mark_pc_dirty`/`app_pc_dirty`), `flush_pc_on_exit` prompts once
    on save-file exit (A=commit, B=revert from `g_save`); other edits unchanged.
  - **D** fix 5 — **big refactor**: `pdna_box(BoxSource*)` vtable + `pk_decode_box_raw`; commit path
    generalized to `AppCommitFn` across `app_mon_menu`/`app_box_browse`/quick-editors; new
    `source/pdna_bank.{c,h}` (per-box files + `bank.meta`, paged, `.pk3` migration); old
    `bank_screen`/inject/withdraw deleted; bank wired into `view_save`. **EWRAM went DOWN** 248.3→241.6 KB.
  - **E** fix 7 — `gen_data.py` auto-scans all meaningful `FLAG_*` (E491/F304/R395, +~22 KB ROM);
    flags tab SELECT = jump-to-category.
- **Verified:** clean `make rebuild` (ROM 3.59 MB, IWRAM 9.6 KB, EWRAM 241.6 KB). Host gates all green
  (edit/clip/data/legality/trainer + new `tests/host_bank_test.c`); box test 216/2 = expected baseline.
  Data gate confirms 491 Emerald flag rows, badge1=0x867.
- **Left off:** everything **code-complete and building but UNCOMMITTED** (working tree dirty). Not
  hardware-tested. Next: commit the batch (see Next-steps 1), then the batched hardware validation.
- **Open threads:** HW validation of all SD-write paths incl. the new bank + deferred-move-save;
  legality V2 encounter half; Sky-wallpaper 1-px strip; `main` is local-only.

### Session — 2026-06-10 (b, cut short)

- **Intent:** resume from this handoff and implement a 7-fix UX batch (un-mirror PC icons, sticky
  summary card, LEFT/RIGHT box-switch on box title, grab-hand move animation, bank-as-parallel-
  boxes with names/wallpapers, deferred save-on-B-exit for move-mode only, full flag discovery).
- **Did:** resumed + briefed from the handoff; quick inline scout only — confirmed the bank is a
  flat `.pk3` list (`pdna_main.c` `BANK_MAX 150`, `PDNA_BANK_DIR`), `hand_cursor.{c,h}` +
  `tools/gen_hand.py` already exist, and the named-flag allowlist lives in `tools/gen_data.py`
  (~L266-410) resolving against the local `reference/*/include/constants/flags.h` checkouts.
  Launched a 5-reader recon Workflow, then the user stopped the session before it returned.
  **No code changes; working tree has only this handoff doc.**
- **Left off:** the full 7-fix batch is specced as Next-steps item 1 above — start there.
- **Open threads:** unchanged from the previous session (HW validation batch, legality V2
  encounter half, Sky wallpaper 1-px strip, local-only `main`).

### Session — 2026-06-10

- **Intent:** a large feature batch on top of the editor: Emerald secret (Walda) wallpapers; file
  browser LEFT/RIGHT jump + a cross-project browser-conventions doc; more editable trainer-card
  fields + badge + Battle-Frontier toggles; move Pokémon in the box (menu-activated); no
  full-screen reload when browsing the PC; flip the box sprite facing; summary A=edit/B=exit +
  scroll-through-box + save-on-leave; fix Unown showing only "A" + choose the form when setting
  species. (User paused mid-way for tokens, then "go on" to finish all of it.)
- **Did:** shipped all of the above across 13 PokeDNA commits (`fef2104`→`1fab267`, + docs
  `acdc1bd`). Reverse-engineered the Walda secret-wallpaper system and the box-wallpaper tile/
  palette format; reconstructed + visually verified all 32 wallpapers. Added 28 Unown form sprites
  (front+icon) + PID-form decode + a PID-search form setter. Reworked the summary into VIEW/EDIT
  with box scroll. Did the box partial-redraw + mirrored icons + move-mode. In the toolkit repo:
  wrote `docs/kb/file-browser-conventions.md` + a `file-browser-feature-baseline` memory, and via
  `/learn` captured the PSG-sound recipe, the Mode-3 partial-redraw pattern, and the Gen-3 edit
  offsets (trainer/PC/flags/Walda) into the skill references (`3042841`).
- **Left off:** everything committed, working tree clean, ROM builds clean. Stopped at the user's
  request to write this handoff.
- **Open threads:** (1) the whole SD-write batch is **untested on hardware** (deferred by the user);
  (2) legality V2 **encounter** half is still the one deferred feature; (3) Sky wallpaper's 1-px
  bottom-edge strip; (4) `main` is local-only (not pushed).
