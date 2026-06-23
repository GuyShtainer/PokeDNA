# PokeDNA — Handoff

> Living resume doc maintained by the `handoff` skill. The **Current status** and **Next steps**
> sections are always kept current — start there to resume. The **Session log** grows downward,
> newest first, and is never pruned.
> Last updated: 2026-06-22

## Current status

- **SESSION 2026-06-22 (part 5) — feedback batch 2. UNCOMMITTED, builds clean (ROM 6.50 MB), host tests PASS.**
  1. **Daycare type rule:** `dc_region_for_species` now sends a mon to the lava ONLY if it's FIRE; non-fire
     rock/ground fall through to grass (if grass/bug-typed) or the empty area.
  2. **Item grab mode:** fixed the translucent glove — `box_oam.c` ITEM-hand blend set BG2 as a *1st*-target
     bit with no 2nd target, so the semi-transparent OBJ rendered opaque; now `REG_BLDCNT = BLD_OBJ |
     ((BLD_BG2|BLD_OBJ)<<8) | BLD_STD` (blends over wallpaper + icons). NOTE: the "items on the mons look
     glitchy" part is unresolved — the marker glyph data looks correct in source; need a photo of the glitch.
  3. **Daycare fully deferred:** ALL in/out (incl. the cross-buffer PC transfers) now defer — withdraw→PC uses
     `app_inject_to_game_deferred` (place in a free PC slot, mark dirty, no write), to-daycare-from-PC clears
     the PC slot + stages SB1, both no-write. Replaced the two exit flushes with ONE `flush_on_exit()` that
     saves (app_commit_pc folds in the staged Day-Care sections) or discards EVERYTHING in a single prompt, so
     a Day-Care<->PC move can't be half-saved. Verified all decline/confirm paths are loss-free.
     (Withdraw→PC places in a free slot rather than literally "held in the glove"; the cross-screen glove
     hand-off would be a follow-up.)
  4. **Box top tabs clickable:** press UP on the box name to focus the top tabs; L/R picks, A activates —
     PKMN DATA→grid, PARTY SEL→party list (new pdna_box return code 3), and CLOSE B is renamed **SAVE**
     (→ exit, which fires the deferred-save prompt). `s_tab_focus` in pdna_box; view_save handles r==3.
  - Files: `pdna_main.c` (type rule, deferred daycare + unified flush + r==3), `box_oam.c` (blend),
    `pdna_box.c` (tab nav + footer).

- **SESSION 2026-06-22 (part 4) — DAYCARE rebuilt from the user's map image. UNCOMMITTED, builds clean.**
  - New bg from the user's map (`Screenshot 2026-06-22 at 22.35.21.png`, 815x570) → `tools/gen_daycare_img.py`
    fits it to the 240x112 scene as RGB15 → `source/daycare_bg_data.h` (git-ignored). Verified the downscale
    looks good + emits `/tmp/daycare_overlay.png` with the slot footprints over the terrain.
  - **6 type-areas, 2 slots each** (`DR_LAVA/WATER/SKY/ELEC/GRASS/EMPTY`, `DC_SPOT[6][2]`): rock/ground/fire→
    lava, water→water-below-waterfall, flying→trees, electric→yellow, grass/bug→flowers, else→empty (priority
    in that order). `dc_take_slot` fills 2 per area then overflows to EMPTY. Real boarders placed first, then
    decos.
  - **Random 2..5 decoration mons per visit**: `dc_roll_decos()` (called once on entry) rolls the count+species
    via `dc_seed()` (session counter XOR cart RTC seconds → genuinely different each open); `dc_rescan` places
    them by type. (Replaces the old tid-fixed 6-spot decos.)
  - **IP:** added `daycare map/` to .gitignore — it holds the **pokeemerald decomp (reference-only/unlicensed)**,
    pokeemerald-expansion, porymap, and a real ROM/.sav. Never commit. (ROM/.sav were already `*.gba/*.sav`
    ignored; the decomp dirs were NOT — now covered.)
  - **OPEN / offered:** the decomp has real **overworld walking sprites** (pokeemerald-expansion) — the daycare
    mons still use the small box icons; offered to swap them to real overworld sprites (a sizable new asset
    pipeline) — awaiting the user's go.
  - Files: `tools/gen_daycare_img.py` (new), `source/daycare_bg_data.h` (regenerated), `source/pdna_main.c`
    (region enum/DC_SPOT/dc_region_for*/dc_take_slot/dc_seed/dc_roll_decos + dc_rescan placement).

- **SESSION 2026-06-22 (part 3) — feedback batch. UNCOMMITTED, builds clean (ROM 6.50 MB), host tests PASS.**
  - **Rumble strength + duration:** Settings ▸ Rumble now has 5-level **Strength** + **Duration** steppers
    (`<>` adjust with a live buzz preview via `rmbl_demo`), punchier defaults (str 5/dur 4), persisted
    (`rstr`/`rdur` in config.cfg). `rmbl.c` scales each cue's duty/frames by STR_PCT/DUR_PCT.
  - **RTC screen reworded:** the "30 days" was a benign "days since last berry update" (events catch up,
    not an error). Now a POSITIVE gap shows "N day(s) will pass when you play" (green/healthy); only a
    clock running BEHIND the save is flagged. Added a note that the in-game clock need not match today.
  - **Daycare in/out is now DEFERRED to true exit** (no save dialog per move). New `app_stage_sb1()`
    stages SaveBlock1 into the in-RAM image (no SD write) + `g_sb1_deferred`; `flush_sb1_on_exit()` (next
    to flush_pc_on_exit, called at the B-to-browser exit) prompts once. reload_saveblocks reads g_sb1 back
    from g_save so staged edits survive screens AND don't contaminate declined non-daycare SB1 edits; any
    real commit flushes them (app_save_finalize clears the flag). SB1-only ops deferred (clipboard
    deposit, withdraw→party, TO-DAY-CARE-from-party, daycare edit); the cross-buffer ones
    (withdraw→PC, TO-DAY-CARE-from-PC) stay IMMEDIATE/atomic for safety (no data-loss window).
  - **Duplicate → held in the glove:** A-menu DUPLICATE on a box/bank mon now copies it into a free slot
    and picks the copy up in the move cursor so you position it (deferred via mark_dirty, like MOVE);
    party DUPLICATE still appends+commits. New `g_dup_req`/`app_take_dup_request`; box loop handler in
    `pdna_box.c`. Box-full checked in the menu (msg there); B keeps the copy at the default slot.
  - **Wallpapers — could NOT reproduce "broken".** Dumped ALL 32 wallpapers from the live `wallpapers.c`
    via a host tool (`/tmp/wpdump.c`) into a montage (`/tmp/wp_all.png`): every one renders as a correct,
    coherent Gen-3 wallpaper — no scramble. `draw_wallpaper` (pdna_box.c:189, `m3_plot` of
    `tiles[map[ty*20+tx]*64]`) uses identical logic to the dump; icon OBJ tiles (512+ → 0x06014000) sit
    ABOVE the Mode-3 framebuffer end (0x06012C00) so no VRAM collision; `wallpapers.c` is present + linked
    (`build/wallpapers.o`). User reports "a jumble of the wallpaper tiles" — a tilemap-scramble symptom the
    current data does NOT have. NEXT: user must re-flash THIS build + confirm (a prior flash may predate the
    Jun-15 wallpaper regen); if STILL jumbled it's environment/HW-specific (e.g. high-ROM-offset read on the
    flashcart) → need a photo. Do NOT blind-edit the verified-correct render code.
  - Files: `rmbl.{c,h}`, `pdna_main.c`, `pdna_box.c`, `pdna_app.h`, `gen3_save.c` (clock reword path).

- **SESSION 2026-06-22 (part 2) — RTC CLOCK CHECK & FIX implemented. UNCOMMITTED, builds clean
  (ROM 6.50 MB), host-tested (`tests/host_clock_test.c` ALL PASS), NOT hardware-validated.**
  New nav entry **"Clock fix"** (NV_CLOCK): reads the live cart RTC, diagnoses whether the save's
  clock is wrong/desynced (which freezes berries / Shoal tides / Lottery / Mirage Island), and offers
  **Auto-sync** (in-game clock = cart clock) or **Manual set** (dial date/time).
  - Built from a research + adversarial-review **workflow** (high confidence; offsets cross-checked vs
    the vendored pret decomps in `reference/`). Findings: localTimeOffset @ SaveBlock2 **0x98**,
    lastBerryTreeUpdate @ **0xA0** (RS == Emerald), struct Time {s16 days, s8 h/m/s}, **PLAINTEXT**
    (no security-key XOR), game day-count = `gen3_rtc_days + 1`. Adversarial review found one real bug
    (s16 day-count overflow on a battery-dead cart's implausible year) — FIXED (writers now return
    bool + refuse + warn).
  - **Pure-C core** in `source/gen3_save.{c,h}`: `gen3_days_to_date`, `gen3_clock_read` (diagnosis +
    verdict), `gen3_clock_manual`/`gen3_clock_autosync` (re-anchor lastBerryTreeUpdate so events
    resume with 0 elapsed). Cart RTC via existing `source/gba_rtc.*`. UI `pdna_clock()` +
    `clock_manual_entry()` in `source/pdna_main.c` (nav menu `rh` 13→12 to fit the 10th item).
  - **Scope:** RSE only (FRLG gated off — no time events); Omega-only to write; commits via
    `app_commit_sb2()` (same verified-write + `.bak` path as the HW-validated trainer card). SB2-only
    fix (berries resume immediately; per-day events resume once real time passes the old VAR_DAYS —
    VAR_DAYS in SaveBlock1 deliberately not touched; a future "complete fix" could add it).
  - **HW TEST (pending):** enable GAME RTC for the ROM on the Omega; on a DISPOSABLE save copy run
    Auto-sync, diff vs `.bak`, **boot the patched save in-game** (proves the checksum size) and confirm
    berries/events resume. Files: `source/gen3_save.{c,h}`, `source/pdna_main.c`, `tests/host_clock_test.c`.

- **SESSION 2026-06-22 — RUMBLE haptics + per-place ANIMATION toggles + confirmed folder/sort
  memory. ALL UNCOMMITTED, builds clean (`make rebuild` exit 0, ROM 6.49 MB / IWRAM 35.8%).**
  NOT hardware-tested yet (rumble is not emulated). On top of the 2026-06-21 batch.
  - **Rumble layer:** vendored a trimmed driver `source/rumble.{c,h}` (GPIO bit-3 motor + TIMER2
    PWM, from `projects/rumble-lab`) + a PokeDNA cue layer `source/rmbl.{c,h}`. Five **independent**
    cues, each its own on/off toggle (like the anim toggles): **Scroll**=weak, **Room change**
    (PC/Party/Bank/Secret Base)=medium, **Stat edit**=medium, **Save done**=strong, **Error**=strong
    double "heartbeat" (ba-bump). Strength = PWM duty (`D_WEAK/D_MED/D_STR` in `rmbl.c` — **tune by
    feel on HW**). Wired by piggybacking the existing earcons in `snd.c` (snd_move→SCROLL,
    snd_edit→EDIT, snd_save→SAVE, snd_deny/snd_error→ERROR) + explicit `rmbl_fire(RCUE_ROOM)` at the
    four room entries in `view_save`. The cue ticks via `rmbl_vblank()` inside `snd_vblank()` (so it
    advances on every screen). `rmbl_init()` after flashcart detect in `main`.
  - **SD-write safety:** `rmbl_pause()/rmbl_resume()` wrap every destructive write so the motor never
    toggles the cart bus mid-transfer — `app_save_finalize` (backup+verified write), `cfg_save`,
    `pdna_bank.c` (meta+box), `pdna_pk.c` (export), and the clear-backups path. (Browser rename/dup
    run only after multi-frame osk/confirm flows, so no cue is ever active there.)
  - **Animations setting is now per-place:** replaced the single `g_anim_on` with a bitmask
    `g_anim_mask` over `enum { ANIM_BOX, ANIM_PARTY, ANIM_DEX, ANIM_DAYCARE, ANIM_SUMMARY }`;
    `app_anim_enabled(int kind)` (signature changed — pdna_app.h + all call sites updated). Box/Party/
    Dex/Daycare default ON, **Summary default OFF** (the portrait wiggle is re-enabled at compile time,
    `SUMMARY_ANIM 1`, but gated on the toggle).
  - **Settings UI:** `Settings` now has `Animations >` and `Rumble >` submenus (each a per-item On/Off
    list; toggling a rumble cue ON fires it as a live preview). New `anim_settings()` + `rumble_settings()`.
  - **Persistence:** `config.cfg` now also stores `anim=` + `rumble=` masks; **folder + sort were
    already persisted** (`dir/sort/rev/all/hidden`) — confirmed working, now in the same file.
  - **HW REQUIREMENT for rumble:** EZ-Flash Omega DE **with RTC enabled for the ROM** in the EZ
    settings (rumble shares the RTC GPIO port). Omega-only in practice; no-op on EverDrive/emulator.
  - **Files touched:** new `rumble.{c,h}` `rmbl.{c,h}`; edited `snd.c`, `pdna_app.h`, `pdna_main.c`,
    `pdna_box.c`, `pdna_pick.c`, `pdna_summary.c`, `pdna_bank.c`, `pdna_pk.c`. Makefile auto-globs.

- **SESSION 2026-06-21 — three rounds of UI/feature fixes, ALL UNCOMMITTED, builds clean (ROM ~6.19 MB),
  awaiting the user's hardware test "tomorrow" (2026-06-22).** Nothing committed this session — every
  change below is in the working tree on top of `231b364`. Build: `make rebuild` exit 0; host
  `host_secretbase_test` green. **User-confirmed on HW:** the box icon **bob is "perfect"** (now a real
  2-frame OAM pose swap). Items done this session (all flag-as-NOT-hardware-tested except the bob):
  - **Animation:** PC-box bob re-enabled and upgraded to the **full 2-frame pose bob** (regenerated the
    OAM blob with frame 1 — `tools/gen_icons_oam.py`, blob ~206→413 KiB — and DMA-swap the whole box's
    tiles in vblank via `boxoam_set_frame`); **Pokedex stutter fixed** (animate only on idle frames,
    read input first); **party** idle bob via a per-column compose (`party_bob_recompose`) that fixed an
    earlier square/cut artifact; summary portrait animation **disabled** (`SUMMARY_ANIM 0`).
  - **Daycare REBUILT** (`tools/gen_daycare_map.py` — crisp **clean-room procedural** native-res scene,
    no more downscale "corruption"): lava/water/hill/grass/house/power-line/trees/fence; **2 mon spots
    per terrain** + **random decoration mons** on grass; placement verified to land on the right ground.
    **Put/take:** "TO DAY-CARE" in each mon's action menu (a move, dest-committed-before-source);
    take-out → **To Party** (atomic, full-checked) / **To PC** (places DIRECTLY in a free PC slot).
  - **#2 DATA-LOSS BUG found + FIXED:** the old take-out→PC only put the mon on the volatile clipboard
    and cleared the daycare → lost if not pasted. Now it injects directly into the PC. Recovery for a
    mon already lost: clipboard (if not overwritten/power-cycled) or the pre-write `.bak`.
  - **Secret base:** party mons now show the **same rich 7-card summary** (synthesize an 80-byte mon:
    IV=15 like the game, EV byte→all stats; write back species/level/item/moves/PID + avg EV);
    `sb_mon_edit` rewritten as a `pdna_inspect` wrapper. Owner overworld class editable (10 presets).
  - **Items:** fixed the `off_tbl==0` "mess of pixels" (68 unmapped items shared the blank id-0 icon) by
    regenerating `item_icons.c` + hardening `gen_items.py`'s accessor; the box panel now shows the
    selected mon's real item icon.
  - Earlier this session: party full-size icons + alignment, glove centered (resting + pickup), EXP on
    the SKILLS card, browser remembers last folder+sort (`/PokeDNA.cfg`), daycare summary editable,
    secret-base "battled today" own line. (Round-1 also fixed a latent build-blocker: a stale
    worktree-absolute `.incbin` path in `mon_icons_oam_data.s` — regenerated from the repo root.)
  - **Adversarial review (round-1 batch)** confirmed + fixed 2 defects: shared-`g_sb1` contamination
    (now `reload_saveblocks()` re-syncs from the image before each screen) + a stale TID display.
  - **Generated/regenerated (git-ignored):** `mon_icons_oam*` (2-frame), `item_icons.c`,
    `daycare_bg_data.h` (from the new `gen_daycare_map.py`). New tools committed-pending:
    `tools/gen_daycare_map.py` (used) + `tools/gen_daycare_bg.py` (round-1, now superseded).
- **Repo / branch:** `projects/PokeDNA` (own git repo, under the git-ignored
  `gba-toolkit/projects/`) / `main`. Public: **`github.com/GuyShtainer/PokeDNA`** (GPLv3). History
  scrubbed to the no-reply identity. Pushed through the rebrand; the **v1.0.0** release commits +
  tag may still be local — check `git log origin/main..HEAD`.
- **Goal:** an on-cartridge original Gen-3 Pokémon save **viewer + editor** (do **not** brand it
  "PKHeX for GBA" — it's the user's own work) that runs on EZ-Flash Omega DE / Everdrive GBA X5,
  reads the flashcart microSD via FatFs, loads any of the 5 Gen-3 saves (R/S/E/FR/LG), and shows
  party + PC boxes + trainer/stats with the game's pixel-accurate UI plus the hidden data — with
  full editing (writes are **EZ-Flash-Omega-only**).
- **State right now (2026-06-18): a large UI/UX batch — code-complete, builds clean, 4 new commits
  LOCAL on `main`, NOTHING PUSHED.** The PC/Bank box icons are now **hardware OAM sprites**; the user
  flashes + **hardware-tests TOMORROW** — the OAM/visual result is emulator-untestable, so that is the
  **#1 open item**. This session's commits (oldest→newest): `fa8d9a6` daycare breeding-compat +
  per-species Emerald anim-family pure-C tables; `29a5690` HGSS Pokedex redesign + SELECT cursor/item
  modes + Party-via-START nav + cute daycare scene + Emerald per-species summary animations + blue
  summary background + egg counters + flicker-free compose-DMA rendering + 8/10 adversarial-review
  fixes; `8e493f7` box icons → OAM sprites (4bpp asset pipeline + `box_oam.*`); `231b364` fix — the
  OAM icons were rendering BEHIND the Mode-3 BG2 wallpaper (a classic BG-vs-OBJ priority bug; dropped
  BG2 to prio 3 in the box). Full feature list in the 2026-06-18 session-log entry. **Build: ROM 6.21 MB
  (18.5%), IWRAM 34.5% (the 1 KiB OAM shadow), EWRAM .bss 0.57%** (big `EWRAM_BSS` buffers unchanged —
  removing `s_nat2spc` freed ~784 B; OAM lives in OBJ VRAM + IWRAM). Host suite green (added
  `tests/host_daycare_test.c`, 18 assertions). A **pre-OAM stable ROM** is at
  `/tmp/PokeDNA_stable_pre-oam.gba`. Earlier work (`v1.0.0` + the GitHub-issue batch #4–#15) is
  committed and was HW-validated on a real EZ-Flash Omega DE; **GitHub issues #4–#15 are still OPEN and
  nothing is pushed.** Pre-existing **uncommitted** `source/gen3_box.c/.h` (a raw/ACE box-name access
  WIP) predates this session — left untouched.
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
- **Post-v1.0.0 GitHub-issue batch — DONE this session (2026-06-15), committed LOCALLY only (11 commits, `origin/main..HEAD`), NOT pushed, issues NOT closed:**
  Worked the issue list in priority order; each built clean + host-gates green. Items #13/#7/#4/#6/#5/#9
  were done earlier; this session added:
  - **#14 Backup management** — `sf_backup_rolling`/`sf_clear_backups` (savefile) + a **MENU > Settings**
    screen: backup policy (new-each-time / single rolling `.bak` / skip) honoured by `app_save_finalize`
    via `g_backup_mode`, plus "clear all backups for this save". Session-only setting.
  - **#15 Built-in SD file ops** — `sf_copy` (verified) + a **FILE MENU (START)** entry in the save
    browser: **Duplicate** (→ free `"<name> copy.sav"`), **Rename** (OSK + collision check), **Delete
    backups**. Write ops Omega-gated; browser re-scans after.
  - **#10 Summary polish** — redesigned the shared portrait column (framed sprite, `#001` dex no, colour-
    coded sex M/F, type badges by the portrait, egg/Pokerus tag) + cyan header accent rule.
  - **#12 Back-sprite view** — new `tools/gen_back.py` pipeline (`mon_back.*`, git-ignored like front);
    **SELECT** in the summary flips front/back portrait (falls back to front if a back is missing).
  - **#11 Animated icons** — `gen_icons.py` now keeps **both** icon frames (frame-aware
    `mon_icon_for_frame`); the box grid does the **Gen-3 two-frame bob** every ~0.5s via the existing
    partial-redraw path (paused during move-mode; no flicker). Icon blob ~1.65 MiB.
  - **#8 Secret Bases** — pure `gen3_secretbase.*` parser (+ host test) for the 20-record SB1 array; a
    **MENU > Secret Bases** viewer (own + friends': name/sex/level/party/decor + a 6-mon party detail)
    and **SELECT = clear a base** (Omega-only, confirm + verified SB1 write). FR/LG → "no Secret Bases".
    Party-mon **editing** intentionally deferred (issue's "read/display first").
  - ROM now **~5.99 MB** (17.8 % of 32 MB; back sprites +1.5 MiB, 2nd icon frame +0.85 MiB). EWRAM is
    **tight: ~4 KB free** (back-sprite scratch +8 KB, `g_sb_recs` +2.9 KB) — watch this before adding
    more big EWRAM buffers.
- **Adversarial review of the batch (2026-06-15, multi-agent workflow `pokedna-batch-review`, 34 agents):**
  10 reviewers (one per changed area) → 2 independent skeptics per finding. **9 confirmed defects, 0
  disputed; all fixed + committed (3 fix commits: `7a6e5cd` savefile, `aaee372` box, `f47f4b7` pdna_main).**
  - **HIGH:** `dup_name()` (#15 Duplicate) `siprintf`'d into a `NAME_MAX` buffer **before** its length
    guard → a 63-char filename overflowed the stack. Now bounded `sniprintf`.
  - `sf_backup_rolling` truncated the only `.bak` before the new copy was verified → fixed with the
    `.baktmp`→verify→swap invariant.
  - Box icon animation erased the whole grid each tick (flicker risk) → per-cell `redraw_region`.
  - **Pre-existing bug found:** `pdna_daycare` used stride 140 for **all** games, but Ruby/Sapphire
    store the 2 daycare mons contiguously (stride 80) and the egg word is a u16 → RS missed mon[1] and
    showed a false "egg ready". Fixed + **validated against the Ruby fixture**.
  - nav-menu panel overflowed the 160 px screen; secret-base detail overlapped the footer; "clear
    backups" wasn't Omega-gated; secret-base count label `/19`→`/20`. All fixed.
  - Host suite still green; ROM ~5.99 MB; build clean.
- **Blocked / needs the user:**
  1. **`git push` is held** — the standing publish preference (no push unless asked) blocked it; **17
     commits** are local on `main` (12 feature + 3 review fixes + handoff/README docs). Say the word to
     push + close issues #4/#5/#6/#7/#8/#9/#10/#11/#12/#13/#14/#15.
  2. **Hardware sign-off** of this batch's SD-write + visual paths (the user's task, deferred — see Next steps).

## Next steps (resume here)

1. **HARDWARE-TEST this whole session's batch on the EZ-Flash Omega DE — the user's task, 2026-06-22.**
   Flash `PokeDNA.gba`. **Save-write paths are the priority** (not emulable, all go through the
   verified-write + `.bak`): **daycare deposit/withdraw + "TO DAY-CARE" move + "Take out → To Party/To PC"**
   (re-verify the #2 fix: a mon taken out to PC actually lands in a free PC box, never lost), and the
   secret-base edits (party-mon rich-card edits + owner-class change). Test on **disposable save copies**.
   Visual: the daycare scene (lava/water/hill/grass/house + decoration mons + 2-spots), party full-size
   bob, Pokedex (no stutter), item icons in the box panel, secret-base summary cards. The box bob is
   already user-confirmed perfect.
2. **Then COMMIT this session's work** (the user said "I'll test tomorrow" — commit after they're happy,
   or when they OK it). 16 working-tree files + 2 new tools (see `git status`). Group sensibly: animation
   (box 2-frame/dex/party), daycare rebuild + put/take, secret-base rich cards + write-back, item-icon
   fix, the data-loss fix, the earlier round-1/2 fixes. Pre-existing uncommitted `gen3_box.c/.h` (ACE
   box-name WIP) predates this session — **leave it out** of the commits.
3. **Optional: adversarial review of the SD-write paths** before relying on them — the #2 data-loss bug
   slipped through because the new daycare write paths weren't reviewed. Focus: `app_to_daycare`,
   `dc_deposit`/`dc_withdraw`, the secret-base `sb_write_mon`/`sb_mon_edit` commit ordering, and the
   `reload_saveblocks()` interaction.
4. **RTC clock check & fix — IMPLEMENTED 2026-06-22; now needs HARDWARE TEST.** "Clock fix" nav
   screen (Auto-sync / Manual set). HW-test on the Omega with GAME RTC enabled for the ROM: confirm the
   live clock reads sane, run Auto-sync on a DISPOSABLE save copy, diff vs `.bak`, **boot the patched
   save in-game** (proves the SaveBlock2 checksum size) and confirm berries/Shoal/Lottery/Mirage resume.
   Optional future "complete fix": also set VAR_DAYS in SaveBlock1 so per-day events resume instantly
   (the current SB2-only fix resumes berries immediately, dailies once real time passes the old
   VAR_DAYS). (See the `pokedna-rtc-fix-feature` memory.)
5. **Deferred / optional (older):** the queued **multi-select move** (Gen-4 rectangle on the OAM box);
   bank-as-boxes + the prior SD-write checklist HW sign-off; Legality V2 encounter half; Sky-wallpaper
   1-px strip. **Push + close GitHub issues** only when the user OKs (nothing pushed; ~17 commits + this
   session's pending commits are local on `main`).
6. **Watch (IP):** the daycare scene is clean-room procedural (safe). But item icons + mon sprites are
   ripped art already in the ROM; PokeDNA's released v1.0.0 ROM still has the unresolved sprite
   infringement (deferred). Don't add NEW ripped art to a public build without Guy's OK.

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

### Session — 2026-06-21 (three rounds of fixes from live HW testing — all uncommitted, builds clean, HW test 6-22)

- **Intent (user):** *"continue the pokedna project"* → the user flashed + tested on real hardware (GBA SP
  + EZ-Flash Omega DE) and reported bugs across THREE rounds; I fixed each, building after every change.
  The user batches HW testing ("I'll test tomorrow"). Worked autonomously; used multi-agent Workflows for
  research (decomp/secret-base/daycare-tiles) + an adversarial review.
- **Round 1 (6 items):** box-icon visibility (was the stale-ROM BG2-priority bug → rebuild) + restore EXP
  on the SKILLS card (`card_skills`); daycare summary editable; daycare scene from the user's template
  (first attempt: downscaled PNG via `gen_daycare_bg.py` → later found "corrupt"); party full-size icons;
  config persistence `/PokeDNA.cfg` (last folder + sort, `cfg_load`/`cfg_save`). Fixed a **latent
  build-blocker**: `mon_icons_oam_data.s` had a worktree-absolute `.incbin` path → regenerated.
  **Adversarial review (5 reviewers + verify)** → 2 real defects fixed: shared-`g_sb1` contamination
  (`reload_saveblocks()` at each screen) + stale TID.
- **Round 2 (10 items):** box bob cancelled-then... ; dex flicker (compose-over-DMA, `ui_blit_over`
  alignment-safe for odd-x); glove centered (`hand_xy`, grab fist); party single-column align;
  secret-base party scroll/view/edit + owner NPC-class (10 presets; **derived**, answered the user's "can
  I pick more trainers" = no, engine limit) + "battled today" own line; daycare put/take plumbing.
- **Round 3 (the user's retest feedback):** party bob artifacts → **per-column compose** (`party_bob_recompose`);
  **box FULL 2-frame pose bob** → regenerated OAM blob with frame 1 + `boxoam_set_frame` DMA-swap (user:
  **"PERFECT"**); summary anim off; **daycare rebuilt from clean-room procedural native-res tiles**
  (`gen_daycare_map.py`) — fixes the downscale "corruption" — with 2 spots/terrain + decoration mons;
  daycare put-in moved to the mon's action menu ("TO DAY-CARE") + take-out To Party/To PC; secret-base
  **rich 7-card summary** via synthesize→`pdna_inspect`→extract (IV=15, EV avg); item `off_tbl==0` fix
  (regen + hardened `gen_items.py`) + item icon in the box panel.
- **DATA-LOSS bug (user hit it):** take-out→PC only used the volatile clipboard → mon lost. **Fixed**:
  direct PC placement; hardened the `app_to_daycare` move to commit the destination before clearing the
  source. Recovery: clipboard or the `.bak`.
- **`/learn` captured** (toolkit `.claude/skills/learn/references/`, uncommitted): OAM 2-frame DMA-swap +
  column-compose for overlapping sprites + animate-only-on-idle + odd-x DMA alignment + don't-downscale
  (UI ref); cross-block-move-commit-dest-first + no-volatile-clipboard-take-out (safe-writes); SB
  reduced-format + synthesize-for-editor + derived owner-class (gen3); generated-file staleness (build).
- **Left off:** everything code-complete + builds clean (`make rebuild` exit 0, ROM ~6.19 MB), **NOTHING
  committed**. Next: user HW-tests 6-22 → then commit (see Next steps). Pre-existing uncommitted
  `gen3_box.c/.h` ACE WIP left untouched.

### Session — 2026-06-18 (big Pokedex/box/summary UI batch + hardware-OAM box — code-complete, push held, HW test tomorrow)

- **Intent (user):** started with *"open PokeDNA"* + 7 Pokedex improvements, then iterated live across
  many messages (cursor modes, daycare, summary animation, blue background, egg steps, the box-animation
  smoothness). Worked autonomously, building after each change; the user batches hardware testing.
- **Done (all committed on `main`, 4 commits `fa8d9a6`/`29a5690`/`8e493f7`/`231b364`, NOTHING PUSHED):**
  - **HGSS Pokedex redesign** (`pdna_pick.c:pdna_dex_screen`, replaces the old text-list `pdna_dex_edit`):
    sprite-state grid — **greyscale=unseen, full-colour=seen, colour+unison-bob+Poké-Ball=caught** (per the
    user's spec); **A cycles** a species unseen→seen→caught; **L/R** = Grid / List / by-Type views; SELECT
    = name/number search; the species-picker Gen/type/legendary filters + a new **caught/seen/unseen**
    filter; seen/caught counters. Reuses `build_species`/`g_list` (no new EWRAM; removed `s_nat2spc` →
    freed ~784 B). New `ui_icon_scaled_grey` + `ui_pokeball` (an 11×11 procedural ball — the first 9×9 one
    "looked like a mushroom").
  - **Box SELECT cursor modes** (`pdna_box.c`): NORMAL → **orange MOVE** (A auto-grabs the mon, no menu) →
    **translucent ITEM** (mons show held items; A picks up / drops / **swaps** held items, never loses one;
    deferred save). Held-item edits reuse `gen3_edit` (`em_set_item`). **Party/Bank/Daycare moved under the
    START nav menu** (added "Party"); SELECT freed for the mode cycle.
  - **Daycare** (`pdna_main.c:pdna_daycare`): cute procedural scene (house/sign/fence/sun, bobbing mons) +
    the Day-Care man's **get-along verdict** + **"next Egg check ~N steps (X%)"**. New pure-C `gen3_daycare.*`
    (egg-group table + `GetDaycareCompatibilityScore` + breedability), host-tested. **Egg HATCH counter**
    added to the summary INFO card (`Hatch ~N steps` = friendship×256).
  - **Summary** (`pdna_summary.c`): **per-species Emerald intro animation** — extracted the decomp
    `sMonFrontAnimIdsTable` into `mon_anim.*` (10 procedural transform families: squish-bounce/stretch/
    v-/h-shake/grow/shrink-grow/v-/h-slide/jumps/wobble), plays once then a gentle float; **LEFT/RIGHT also
    flips cards** in VIEW; **Emerald blue gradient background**.
  - **Flicker-free rendering:** `ui_blit_over` (compose background+sprite per scanline → DMA, **no erase**)
    for the summary portrait + daycare mons; the software box bob was banded (later superseded by OAM).
  - **Adversarial review** (Workflow, 23 agents) found **10 real bugs**; fixed 8: the daycare egg-word
    offset/width is **per-game** (RS u16@+276, FRLG u16@+280, Emerald u32@+280 — the old code's +280/u32
    mis-read RS+FRLG), a Pokedex `key_repeat_mask` regression that killed app-wide L/R auto-repeat, B-on-
    title, MOVE-on-empty beep, item-no-icon fallback, bad-egg guard.
  - **Box icons → HARDWARE OAM SPRITES** (`8e493f7`, via a `gba-ui-logging` worktree agent + `231b364`
    integration fix). Why: on the single-buffer Mode-3 screen, software can't repaint 30 large icons
    "instantly + flicker-free" (flash if one-shot, sequential sweep if banded — which also stalls the
    cursor). Hardware OBJ composites all icons for free; the bob is a 1-px in-vblank OAM Y-nudge.
    New `box_oam.{c,h}` + `tools/gen_icons_oam.py` (RGB15→4bpp, **13 shared OBJ palettes** — icons aren't
    ≤16-colour so they're clustered) + `tools/gen_hand_oam.py`. Icons/cursor/carry/markers are sprites;
    wallpaper/banner/panel/tabs/footer stay software BG2. **Bug found + fixed (`231b364`):** the icons were
    hidden BEHIND the Mode-3 BG2 wallpaper (BG2 prio 0 ≥ OBJ prio 2) → only the software left-panel sprite
    showed ("only icons around the cursor"); fix = drop BG2 to prio 3 in the box + clear all 128 OAM on entry.
- **Build/test:** `make rebuild` clean throughout; **ROM 6.21 MB (18.5%)**, IWRAM 34.5%, EWRAM .bss 0.57%.
  Host suite green (`tests/host_daycare_test.c` added, 18 assertions). Pre-OAM stable ROM saved to
  `/tmp/PokeDNA_stable_pre-oam.gba`.
- **Also (toolkit repo, separate, UNCOMMITTED):** `/learn` updated two references — `gba-ui-input-sound.md`
  (OAM-over-a-Mode-3-bitmap: halved tile VRAM 512–1023, the BG2-priority trap, clear-all-OAM/OBJ-off-on-exit,
  compose-then-DMA anti-flicker) and `gen3-pokemon-saves-and-licensing.md` (**corrected** the daycare egg
  offsets per game + added egg production/hatch mechanics, the breeding-compat rules, and the Emerald
  procedural-animation table).
- **Blocked / needs the user:** (1) **hardware test of the OAM box** (the emulator can't prove sprite
  compositing/timing — see Next steps #1); (2) push + close issues (held); (3) the queued multi-select move.

### Session — 2026-06-15 (post-v1.0.0 GitHub-issue batch — code-complete, local commits, push held)

- **Intent (user):** *"tackle the rest of the list one by one, you set the priority"* + *"Go on, I'll
  test later."* Work autonomously through the open GitHub issues, build + commit each, defer hardware
  testing to the user.
- **Done:** finished the list — #14 backup management, #15 SD file ops, #10 summary polish, #12 back
  sprites, #11 animated box icons, #8 Secret Bases (viewer + clear) — on top of the earlier
  #13/#7/#4/#6/#5/#9. 11 commits this session, all clean builds; full host-test suite green (added
  `tests/host_secretbase_test.c`). See **Current status** for the per-issue detail and the new
  files/functions.
- **Key decisions:** back sprites use a `gen_back.py` mirror of `gen_front.py` (no L/R flip — backs
  already face away); icon animation reuses the box screen's existing partial-redraw discipline (paused
  during move-mode) to avoid re-introducing the #3 flicker; Secret Bases shipped **read/display + clear**
  only (party-mon editing deferred per the issue); secret-base offset derived from `g_game` (not the
  possibly-mis-defaulted `version_guess`) so an FR/LG save can't parse garbage.
- **Then: adversarial review (multi-agent `Workflow`, 34 agents).** 10 area reviewers → 2 independent
  skeptics per finding. **9 confirmed / 0 disputed**, all fixed + committed (`7a6e5cd` savefile,
  `aaee372` box, `f47f4b7` pdna_main): HIGH `dup_name()` stack overflow (siprintf before the length
  guard → bounded `sniprintf`); `sf_backup_rolling` truncated the only `.bak` before verifying (→
  `.baktmp`→verify→swap); whole-grid icon-anim repaint flicker (→ per-cell `redraw_region`); a
  **pre-existing** RS daycare-stride bug (used 140 for all games; RS BoxPokemon are contiguous at 80 and
  the egg word is a u16 — **validated against the Ruby fixture**, was showing a false "egg ready");
  nav-menu panel overflowing 160 px; secret-base detail overlapping the footer; ungated "clear backups";
  `/19`→`/20` count label.
- **Then: PKHeX credit reworded** (`ad0a902`, user ask) — README now says PokeDNA drew *inspiration*
  from PKHeX but used **none** of its code (C#/GPLv3); the save-format *facts/offsets* come from the
  pret decomps. (Internal `docs/SESSION_SUMMARY.md` still says "PKHeX for the GBA" — left as-is; scrub
  if the user wants it gone everywhere.)
- **Key decisions:** back sprites use a `gen_back.py` mirror of `gen_front.py` (no L/R flip — backs
  already face away); icon animation reuses the box screen's partial-redraw discipline, paused during
  move-mode (and after review, per-cell so the grid is never fully blanked); Secret Bases shipped
  **read/display + clear** only (party-mon editing deferred per the issue); secret-base + daycare
  offsets derived from `g_game` (not the possibly-mis-defaulted `version_guess`).
- **Watch:** EWRAM headroom is now ~4 KB (back-sprite scratch +8 KB, `g_sb_recs` +2.9 KB). ROM ~5.99 MB.
  The next big EWRAM consumer should first reclaim the 8 KB by sharing the front/back `s_decomp` buffers.
- **Blocked:** `git push` held by the standing publish preference — **17 commits** local on `main`;
  issues still open on GitHub. Hardware sign-off of this batch's SD-write + visual paths pending (user's task).

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
