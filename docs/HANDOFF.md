# PokeDNA — Handoff

> Living resume doc maintained by the `handoff` skill. The **Current status** and **Next steps**
> sections are always kept current — start there to resume. The **Session log** grows downward,
> newest first, and is never pruned.
> Last updated: 2026-07-16

## Current status

**ACTIVE THREAD (2026-07-16): multi-select move + bank moves + batch export — code-complete,
UNCOMMITTED, builds clean (ROM ~6.56 MB), host gates green, awaiting Guy's ROUND-2 hardware test.**

- **What it adds (Guy's asks):** (1) **Emerald rubber-band multi-select** — in the orange MOVE hand,
  **hold A + D-pad** grows a rectangle; release lifts every mon inside as a **chunk**; carry it across
  boxes (edge-push L/R), UP→Bank / DOWN→PC. A plain A *tap* is still the classic single grab.
  (2) **PC→Bank = always RELEASE, no prompt** (no more auto-duplication; the old single-mon path was a
  confirmed COPY). (3) **"Export all .pk"** + (4) **"Release all"** on the box-name menu (PC *and*
  bank). (5) A shared **progress screen** (live 64×64 front sprite + N/total bar).
- **Guy's HW round-1 verdict:** PC→Bank release "works like i want"; chunk moves within the PC work.
  Round-2 fixes since: bank↔bank cross-box chunk move (was denied), Bank→PC now *looks* moved at once,
  Release-all added.
- **⚠️ THE RULE Guy set (I got it backwards once — do not re-litigate):** a move must **NOT** force an
  immediate disk write — *"there is a save button for that and also ... a promt asking if i wanna save
  changes. so make it look visually as if they really moved, the real save will happen later as a
  chunk."* → Bank→PC (single **and** chunk) stays **deferred**; the bank *display* hides moved-out mons.
- **Two adversarial-review workflows each found + fixed a real DATA-LOSS bug** (details in the session
  log): `flush_on_exit` deleted bank sources even when the PC write FAILED; and my
  `drop_chunk_bank_cross` committed a source box it had paged in **without checking the read
  succeeded** (a failed read → zeroed buffer → would wipe that box's bystander mons; bank box files
  have **no backup**).
- **Files:** new `source/gen3_chunk.{c,h}` (pure-C footprint math + `tests/host_chunk_test.c`), new
  `source/pdna_progress.{c,h}`; modified `pdna_box.c` (the bulk), `pdna_main.c`, `pdna_bank.{c,h}`,
  `pdna_pk.{c,h}`, `pdna_app.h`.
- **Also uncommitted, in the TOOLKIT repo** (`gba-toolkit`, separate repo): 3 lessons appended to the
  learn skill — `references/fatfs-and-safe-writes.md` (deferred-vs-immediate store asymmetry;
  gate-the-source-clear-on-commit-success; read-modify-write-back must verify the read),
  `references/gba-ui-input-sound.md` (a group-carry can't float a 2nd set of icons),
  `references/gen3-pokemon-saves-and-licensing.md` (the Emerald PC multi-select gesture).

---

PokeDNA is a **feature-complete** on-cartridge Gen-3 save viewer/editor (RSE + FRLG). Two shipping
builds from one codebase:
- **`PokeDNA-NOR.gba`** (~6.25 MB) — every sprite embedded (normal+shiny front, normal+shiny back,
  icons). Build: `make`. **Loads from NOR *and* from the normal EZ-Flash SD "Load game"** (Guy
  confirmed on the Omega DE — there is NO ROM-size load limit; see the 2026-07-13 entry).
- **`PokeDNA.gba`** (~3.83 MB) — trimmed; streams shiny fronts + all back sprites from a companion
  **`/PokeDNA/sprites.pak`** (2.42 MB, ship alongside). Build: `make sd`. **Now OPTIONAL** — only a
  smaller/faster variant, since the full build loads from SD too.
- **Committed on `main`** (not pushed): 4 Guy-reported bug fixes (`816fdb6`), ITEM-mode fixes
  (`8ded10f`, `ba92c29`), SD-stream engine (`75d787a`, `e89659b`). **Uncommitted:** `docs/HANDOFF.md`,
  `docs/SESSION_SUMMARY.md`, `docs/IDEAS.md` (new).
- **Research parked:** Emerald **Recorded Battle export** — sector 31 (`0x1F000`) holds a
  deterministic battle (seed + both teams + input stream), exportable. See `docs/IDEAS.md`.

## Next steps

1. **HW test ROUND 2 on the Omega DE** (the blocker for the multi-select batch — none of it is
   emulatable). On disposable save copies, check:
   a. **Bank↔bank chunk move** — lift a whole box (hold A + D-pad over a full box, release) and drop
      it into a *clean* bank box. This is the fix for "it doesn't let me".
   b. **Bank→PC empties the bank on sight** — after dropping a chunk (and a *single* mon) into the PC,
      go back to the bank: the source cells must read as GONE *before* any save prompt.
   c. **The vacated cells refuse a drop until you save** — expect "CELLS NOT FREE YET"; that is
      deliberate (their mons are still the only on-card copy until the PC is written).
   d. **Release all** on a PC box *and* a bank box (confirm → gone; check a `.bak` was made).
   e. **PC→Bank release + Export all .pk** still good (round-1 said yes; re-confirm after the churn).
2. **Then commit** the batch (don't `git push` unless Guy asks) — see "Uncommitted" above; the
   toolkit-repo learn edits are a **separate repo/commit**.
3. *(Deferred, Guy's call)* single-mon Bank→PC currently shares the chunk's deferred+hide behaviour —
   no change expected, but confirm it feels right.
4. **HW-verify** with a *fresh* flash of `PokeDNA-NOR.gba`: the 4 bug fixes
   (`816fdb6`) + ITEM-mode fixes — secret-base mons no longer show as eggs / correct stats;
   ITEM-mode held-item marker draws in front; carried-item source mon fades. (Earlier "still broken"
   reports were a **stale flash**, not a code regression.)
2. **Decide the build strategy** (open question to Guy): retire the streaming/trimmed path (drop
   `make sd`, `source/sprite_stream.c`, the `source/embed/` split, `-DPDNA_STREAM_SPRITES`) now that
   the full build loads from SD — **or** keep it as the optional smaller/faster variant.
3. *(Optional feature)* **"Battle Record → export"**: read/validate `.sav` sector 31, dump the
   `RecordedBattleSave` as JSON + both teams as PKM (reuses the existing mon parser). Details in
   `docs/IDEAS.md` and the learn `gen3-pokemon-saves-and-licensing.md` reference.
4. Commit the uncommitted docs when ready (don't `git push` unless Guy asks).

**Blocker:** hardware sign-off on the Omega DE for the bug-fix batch (SD/save path is not emulated).

## Session log

### Session — 2026-07-16 (multi-select move + bank moves + batch export; 2 rounds, 2 loss bugs fixed)

- **Intent:** Guy: *"I wanna be able to do multiple selection of pokemon and move just like in the
  emerald game"* + move a chunk into the bank **without auto-duplicating** (*"i expect them to get
  released from the save"*) + a **progress bar + sprite** during the export. Mid-session he added
  *"i also wanna be able to 'release all' from the menu of clicking the box name"*.
- **Design answers Guy gave (locked — don't re-litigate):**
  - **Multi-select is a REAL Emerald mechanic** (I wrongly claimed it wasn't): *"when hand is orange
    (quick grab mode) if i hold A button and use arrows, this expends the pickup to a rectengle i can
    shrink and enlarg, all pokemon within it are chosen, and by pushing edge of box when moving the
    chunk left and right, it goes to left or right box"*. Implemented exactly that (now also captured
    in the learn skill's Gen-3 reference).
  - **Bank storage = the existing 16 bank boxes**, *"simply into the bank"* — **not** loose `.pk3`.
    Instead, `.pk3` export became an **"Export all"** action on the box-name menu (bank **and** PC).
  - **PC→Bank = "Always release, no prompt".**
  - **Moves must NOT force an immediate disk write** — *"there is a save button for that and also ...
    a promt asking if i wanna save changes. so make it look visually as if they really moved, the real
    save will happen later as a chunk."* (I first built Bank→PC as an immediate `app_commit_pc` and
    **reverted** it to deferred + display-hide.)
- **Did — round 1 (build):** new pure-C `gen3_chunk.{c,h}` (rectangle → footprint, `chunk_can_drop`,
  host-tested by `tests/host_chunk_test.c`, 6 cases); `pdna_box.c` gained `begin_select` (hold-A
  rubber-band; a tap still routes to the well-tested single grab), `chunk_draw` (green/orange
  fit-preview), `drop_chunk`, `drop_chunk_pc_to_bank`, `s_ch_*` carry state (2.4 KB `EWRAM_BSS`,
  survives the PC↔Bank hand-off); new `pdna_progress.{c,h}` (front sprite + N/total bar);
  `pdna_pk_export_silent` + `export_box_all`; `app_pc_release_slot` (identity-matched PC release).
  **PC→Bank release ordering:** write bank box → `src->commit()` **verified** → only then clear the PC
  slots (deferred to the one exit save). This also fixed the *single-mon* PC→Bank, which was a
  confirmed **COPY** = exactly the auto-duplication Guy complained about.
- **⚠️ Round-1 adversarial review (4 dims) → 1 CONFIRMED loss bug, fixed:** `flush_on_exit`
  (`pdna_main.c`) called `app_commit_pc()` and **discarded its bool**, then ran
  `pdna_bank_flush_deletions()` **unconditionally** — so a *failed* PC write still deleted a Bank→PC
  move's bank sources ⇒ mons gone from the bank, never in the `.sav`. **Pre-existing**, but the chunk
  amplified it to a whole box. Now `if (app_commit_pc()) pdna_bank_flush_deletions();`.
- **Guy's HW round-1 result:** PC→Bank *"seems to work like i want!"*; PC-internal chunk moves work.
  Two problems: **(a)** couldn't move a chunk **within the bank** (whole box → clean box); **(b)**
  Bank→PC *"looks as if they duped"* until the exit prompt.
- **Did — round 2 (fixes):**
  - **(a)** `drop_chunk_bank_cross` — the bank pages ONE box at a time, so: commit the **dest** box
    (verified) → then clear+commit the **source** box. (My original blanket deny was copied from the
    single-mon restriction; a move into *empty* cells doesn't need the source box resident.)
  - **(b)** Bank→PC stays **deferred**; the bank **display** hides moved-out mons:
    `pdna_bank_hide_pending` blanks them in the **decoded PkMon array only** — never `g_bankbuf`, which
    `box_save` persists, so the record stays as the mon's only on-card copy until the PC commits.
    All 17 `pk_decode_box_raw` sites now go through `box_decode`/`box_decode_to`. `box_occupancy` +
    `app_bank_slot_pending` keep a hidden slot **OCCUPIED** so nothing can overwrite it (a refused drop
    explains *"CELLS NOT FREE YET"*). Export-all/Release-all skip hidden mons via the same decode.
  - **Release all** on the box-name menu (PC + bank): confirm → clear → verified commit (+ backup).
- **⚠️ Round-2 adversarial review (3 dims) → 1 CONFIRMED loss bug in MY new code, fixed:**
  `drop_chunk_bank_cross` committed the **source** box right after paging it in — but `box_load`
  **discarded the read status**, and it `memset`s the buffer to zero first. A failed/short SD read ⇒
  all-zero buffer ⇒ the unconditional commit would **wipe that box's untouched bystander mons**, and
  **bank box files take NO immutable backup** (unlike `app_commit_pc`) ⇒ unrecoverable. Fix: `box_load`
  now returns read-completeness; new `pdna_bank_clear_slots` refuses to rewrite on an incomplete
  page-in **or** when nothing identity-matched (mirroring `pdna_bank_flush_deletions`, which was
  already safe) ⇒ degrades to a recoverable duplicate.
- **Left off:** everything code-complete + **uncommitted**; `make rebuild` clean (ROM ~6.56 MB, ewram
  `.sbss` fine), `host_chunk_test` green, lossless edit gate green on all 3 fixtures. **Guy: "ill test
  later."**
- **Open threads:** round-2 HW test (Next steps #1); commit both repos; single-mon Bank→PC shares the
  deferred+hide path (unverified on HW).
- **Lesson captured** in the toolkit's learn skill (uncommitted, separate repo): deferred-vs-immediate
  store asymmetry + *gate the source-clear on the destination commit **succeeding***; **a
  read-modify-write-back must verify the read**; a group-carry can't float a 2nd set of OBJ icons; and
  the Emerald PC multi-select gesture.

### Session — 2026-07-15
- **Delivered two clearly-named builds:** `PokeDNA-NOR.gba` (6.25 MB, full, `make`) and `PokeDNA.gba`
  (3.83 MB, trimmed, `make sd`, needs `/PokeDNA/sprites.pak`). Verified the NOR build embeds the
  complete sprite set (`source/embed/mon_back_data.s` incbins both `mon_back.bin` **and**
  `mon_back_shiny.bin`; `mon_front_shiny` embedded) — nothing streamed, no companion file needed.
- **Disproved the "SD-load size wall."** Guy loaded the 6.25 MB build via the Omega DE's normal SD
  "Load game." The earlier hangs were **non-monotonic in size** (4.73 MB loaded / 4.72 hung; 6.56
  hung / 6.25 loads) → **bad/incomplete SD copies, not a PSRAM ceiling.** Corrected the diagnosis
  everywhere: this HANDOFF, `SESSION_SUMMARY.md`, learn `flashcart-sd-io.md` (+ r36s-toolkit copy),
  and auto-memory. New rule: *hang at "Load game" ⇒ re-copy the `.gba` / suspect the card, not the ROM.*
- **Battle-recorder research → `docs/IDEAS.md` rewritten (was "probably not possible").** Emerald's
  Frontier Pass **Battle Record IS a deterministic, exportable battle**: `struct RecordedBattleSave`
  at sector 31 (`0x1F000`, fills the 3968-byte data area) = `rngSeed` + both 6-mon parties (same
  100-byte encrypted format we parse) + `battleRecord[4][664]` turn-by-turn inputs. Verified vs
  pret/pokeemerald (reference-only). Export is easy (reuse the mon parser); PC-side video render is a
  separate project (trivial route: screen-record the in-game replay in an emulator). Captured in the
  learn gen3 reference too.
- Ran `/learn` (captured the size-wall correction + the recorded-battle finding into the learn skill,
  both gba-toolkit and r36s-toolkit copies) and this `/handoff`.

### ⚠️ 2026-07-13 CORRECTION — the "won't load = ROM SIZE" diagnosis was WRONG
- **There is no ROM-size ceiling.** Guy loaded the **6.25 MB** full build (`PokeDNA-NOR.gba`) via
  the EZ-Flash Omega DE's **normal SD "Load game."** Size was never the gate. The load hangs were
  **non-monotonic in size** (7.32 MB loaded but 6.56 MB hung; 4.73 MB loaded but 4.72 MB hung;
  6.25 MB loads) — a real size ceiling would be monotonic, so this rules size out. (The old notes
  even recorded the 7.32-loads-6.56-hangs contradiction and I still blamed size — my error.)
- **Real cause:** almost certainly an **intermittent bad/incomplete copy to the microSD**
  (truncated write, unflushed FS cache from pulling the card early, or FAT/card flakiness): the
  flashcart reads a bad image and hangs at "Load game." **Re-copy the `.gba` and eject safely — it
  loads.** Rule going forward: **hang at "Load game" ⇒ suspect the copy/card first, not the ROM.**
- **Consequence:** the back-sprite cut and the trimmed-SD + `sprites.pak` streaming were **not
  necessary**. The full build (all sprites incl. back-view + shiny) is the shipped `PokeDNA-NOR.gba`;
  `mon_back.c`/`gen_back.py` and `sprite_stream.c` are intact and used by the full build. The
  streamed SD build (`make sd`) remains only as an optional smaller/faster variant.
- (Below: the superseded 2026-07-07 "ROM SIZE" writeup, kept for history — do NOT act on it.)
  ~~Symptom: EZ-Flash "Load game" hangs = OS-mode SD PSRAM ceiling; cut back-view sprites → 4.73 MiB
  loads.~~ The sprite files (`mon_front.*`, `mon_back.*`, `data/*.bin`) are still **git-ignored**
  (ripped art, regenerated by `tools/gen_*.py`).
- If the ROM ever needs to grow again while keeping art: **paletted sprites** (store 4bpp + a
  normal & shiny palette; shiny = palette swap) → whole front set ~0.48 MB incl. shiny, ~1.5 MB
  smaller. A ready `gen_front.py` paletted rewrite was drafted then reverted (Guy wanted "as
  before"); the approach is validated (sprites are clean ≤15-colour, shiny is a 1:1 palette swap).

### Guy-reported bugs — FIXED 2026-07-08 (commit `816fdb6`, HW-verify pending)
- **[1] Secret-base mons shown as EGGS** — root cause: `sb_mon_edit` synthesized the display record
  with ciphertext=0, so `gen3_edit_load` decrypted key=PID^0=PID → PID-filled substructs; egg
  bit30 = PID bit30 (~half the mons). Fixed: `memset(e.sub,0,...)` + `raw[0x13]=0x02` + ability=PID&1.
- **[2] Secret-base stats** — confirmed EXACT vs pokeemerald `CreateSecretBaseEnemyParty` (fixedIV=15,
  stored avg-EV → every stat, nature/gender from personality). Were already right; now clean inputs.
- **[3] ITEM-mode held-item preview behind the hand** — dropped the ITEM hand to OBJ priority 1 so the
  priority-0 preview pops in front.
- **[4] ITEM-mode hand dimmed the holder** — hand was `ATTR0_BLEND`; now opaque (only non-holder icons fade).
- box_oam.c changes are UI-only (no host test) → **need HW check** (ITEM-mode look). Cores host-tested green.

### Known bugs — Guy-reported 2026-07-07 (FIXED — see above)
1. **Secret base: some party mons show as EGGS in the summary.** Secret-base mons are SYNTHESIZED
   into a PkMon (from SbParty species/level/moves/EVs/personality), not decoded from a real
   BoxPokemon — so `isEgg` is defaulting/garbage. Fix where the SB summary PkMon is built
   (`pdna_main.c` sb_detail/sb_mon_edit ~2406-2487 + `gen3_secretbase.c`): force `isEgg=false`,
   `isBadEgg=false`.
2. **Secret base: mon stats must mimic the EXACT game calc.** Feed `pk_calc_hp`/`pk_calc_stat`
   (`gen3_mon.c`) the exact IV/EV/nature/level the game uses for a secret-base opponent — research
   pokeemerald `CreateSecretBaseEnemyParty` (fixed IV + EV spread); the formula itself is already
   correct, only the inputs need to match.
3. **ITEM-move mode: the small held-item marker draws BEHIND the mon icon.** In `box_oam.c` the
   item markers (`OE_MARK0`=34+) sit at a higher OAM index than the icons (`OE_ICON0`=0..29), so
   at equal OBJ priority they render behind. Fix: give the markers a LOWER OBJ priority (ATTR2
   priority bits) than the icons so they "pop forward."



- **SESSION 2026-07-05 (resumed) — ALL confirmed audit findings FIXED + COMMITTED. 17 commits on
  `main` (top `6cdcf2f`), NOT pushed. Builds clean (ROM ~6.56 MB), EWRAM .sbss headroom ~4.8 KB,
  host suite green. READY FOR GUY'S HW TEST → then push + tag v2.0.0 + GitHub release.**
  - Audit-fix commits: `f586d3e` **RS daycare offsets** (pret-verified: mail 56 B, steps@272, egg
    u16@280, cnt@282; E/FRLG mail clear 56; KB corrected in the toolkit repo — CRITICAL, was a save
    corruptor); `3664233` 7 feature bugs (dex-undo reverts Natl unlock; egg icon in daycare yard +
    carry glove; honest bank exit prompt; dup skips the reversed cross-scope confirm; defer-queue-full
    refuses the move in drop_held + party ADD; trainer card single sections-0..4 commit via new
    app_commit_sb12); `3d67d45` 30+ more text overflows (all 8 box footers, 11 Omega dialogs,
    wallpaper picker, clock/secret-base/browser lines, editor header/footers, trainer rows, dex
    footers, dc_menu labels…); `fcb7866` dead-code purge (app_edit_commit, xfer trio, clip/party
    writers, log_text [8KB buffer KEPT — SD flush uses it], ui_icon_sub, macros, pk_nature dedup,
    uncompilable tests/host_test.c + the 3 gen3_save.c PokeLinkSim leftovers it referenced);
    `3a85e5f` raw-flags bound fixed to the REAL flags[] (E 2400/RS-FRLG 2304, was ~2.7x over —
    could poke vars[]) + secret-base party Unown letter/Deoxys forme; `6cdcf2f` dex partial-row wrap.
  - **Audit leftovers (LOW value, optional):** 2 finder areas never ran (feat:events+backup,
    feat:summaryanim — resume recipe below still valid, ~7 agents); PkMon write-only fields
    (evSum/otGender/ribbons) kept; ~15 -Wmisleading-indentation warnings; party_list fallback kept.
  - **HW TEST CHECKLIST for Guy:** wallpaper A/B (`a2eb21e`); RS daycare deposit/withdraw on a
    disposable RS save (new offsets); FRLG "TO DAY-CARE"; party<->box both directions + the
    save-something-else-then-decline loss repro; egg shows as Egg everywhere (box grid/panel/party/
    daycare yard/carry glove/summary) + HATCH; Deoxys forme per game + SELECT-cycle; summary anim
    (now default ON) incl. static eggs; dex UP/DOWN wrap; text no longer wraps in daycare/box
    footers/dialogs/trainer card. Then: push + tag v2.0.0 + release WITH ROM (Guy's informed call).

- **SESSION 2026-07-05 PAUSE (Guy needs tokens). Release-audit v2 RAN (22/31 agents) — full findings
  SAVED to `docs/audit-findings-v2.json` (+ `docs/audit-journal-v2.jsonl`, both untracked — parse the
  JSON's `.result`). Resumable: `Workflow({scriptPath: ~/.claude/projects/-Users-guyshtainer-VSCodeProjects-gba-toolkit/b6a21bde-*/workflows/scripts/pokedna-release-audit-v2-wf_3f77ebd4-9f7.js, resumeFromRunId: "wf_3f77ebd4-9f7"})`
  — only the 9 session-limit-failed agents re-run (finders feat:events+backup + feat:summaryanim, and
  skeptics for pokedex/daycare/dataeditor/sprites unverified items). NOTHING from the audit is fixed yet.**
  - **#1 FIX FIRST — RS DAY-CARE OFFSETS ARE WRONG (confirmed vs pret/pokeruby primary source, could
    corrupt RS saves on deposit/withdraw):** DayCareMail.names @+0x24 ⇒ MailStruct=36 ⇒ **RS mail=56 B**
    (not 54); sizeof(DayCare)=0x30B8-0x2F9C=**284**. Correct RS layout: mail@base+160+i*56 (clear 56),
    steps u32 @**272**+i*4, egg u16 @**280**, counter u8 @**282** (RS==FRLG for the egg block!). Our code
    (pdna_main.c dc_clear_slot_aux/dc_clear_egg/dc_rescan) uses 54/268/276/278 — fix all three fns; ALSO
    E/FRLG mail clear should be **56** not 54 (140=80+56+4). ALSO fix the learn-skill KB
    (`.claude/skills/learn/references/gen3-pokemon-saves-and-licensing.md` daycare bullet, toolkit repo)
    which has the same wrong RS numbers. The June "Ruby fixture validation" passed only because the
    fixture had no pending egg.
  - **Other CONFIRMED feature bugs (from `features_confirmed` in the JSON):** dex bulk "Undo last"
    doesn't revert the Natl-Dex unlock (snapshot it); egg shows SPECIES icon in the daycare yard
    (pdna_main.c ~2250/2260/2294/2303) and in the carry GLOVE (pdna_box.c oam_sync ~330 →
    boxoam_carry_held — use the egg OAM icon); bank exit "discard" over-promises (page-out auto-saves
    earlier boxes — make prompt honest or track+reload); bank-DUP dropped into PC shows reversed
    "Copy to Bank?" text (dup ⇒ skip confirm); 65th Bank→PC defer-delete silently dups (queue full ⇒
    refuse the drop); trainer card double full-file write when money+identity both dirty (single
    sections-0..4 commit).
  - **32 TEXT-OVERFLOW findings** (all in the JSON `text_overflow`, sonnet-verified arithmetic): worst
    = box/bank draw_footer (all 8 strings 2-15 cols over), the Omega read-only msg_wait lines (11 call
    sites), summary/edit confirm dialogs, wallpaper-picker header/footer, trainer-card DEX+badges rows,
    clock lines, dc_menu labels, pdna_dex_screen footer, legality lines. msg_wait/app_confirm budget =
    24 cols inside panel (26 to screen); consider clamping INSIDE msg_wait/app_confirm so callers can't
    regress.
  - **DEAD-CODE list (10 items, `dead_code` in the JSON):** app_edit_commit, app_clip_occupied +
    app_xfer_put/take + g_xfer_*, clip_write_box_slot, party_write, pk_current_box, pk_read_box, 7
    PokeLinkSim leftovers in gen3_save.c (gen3_detect_game/read_live_party/count_battleable/...),
    pk_nature (or use it at gen3_mon.c decode), log_text+8KB EWRAM_BSS buffer (!), ui_icon_sub,
    PB_ICON_MAX, UI_COLS, gen3_box.c stale fwd-decl, write-only PkMon fields (evSum/otGender/ribbons).
    Keep: party_list (no-PC fallback), ACE raw box-name helpers (documented groundwork).
  - Prior manual pass already committed: `f41602e` (first overflow batch + hand_cursor/base_name/DLAB
    removed + summary anim default ON) and `0f04004` (egg sprite in box panel + animated portrait,
    eggs don't animate). 10 commits on `main` (top `0f04004`), NOT pushed. Guy's locked decisions:
    anim ON ✓ done; endgame = push + tag + GitHub release WITH ROM after HW test.

- **SESSION 2026-06-30..07-04 — feature batch COMMITTED (8 commits on `main`, top `5376fa0`, NOT pushed);
  release-audit STOPPED mid-run by Guy (token budget).** Builds clean, ROM ~6.56 MB, EWRAM .sbss headroom ~4.8 KB.
  - **Committed this span:** `beb5259` daycare FR/LG + false-"full" fix (+`tests/host_daycare_slot_test.c`);
    `29aa019` party<->box overlay + **atomic cross-buffer saves (finalize folds g_pc — fixed a review-confirmed
    CRITICAL loss + HIGH dup)**; `a2eb21e` HW-robust wallpaper render (RAM-staged; rumble theory DISPROVEN —
    needs HW confirm); `8f7f5c5` party popup = the full party menu (old list retired to the no-PC fallback only);
    `149569c` dex cursor wrap; `9956069` Deoxys formes (4 sprites in all 4 generators, auto per game RS=Normal/
    E=Speed/FRLG=Attack default, SELECT cycles in summary; forme is version-baked — unforceable in-game);
    `c3e23e3` egg HATCH action (+`tests/host_hatch_test.c`) ; `5376fa0` eggs show the REAL decomp egg sprite
    (front/icon/OAM incl. the box grid; `graphics/pokemon/egg/` from the vendored decomp; drawn ui_egg removed).
  - **DEFINITIVE-RELEASE AUDIT (Guy's ask): text-overflow sweep + full feature audit + dead-code purge.**
    A 14-agent workflow was launched then STOPPED early: run `wf_b40f1ea0-82c`, script
    `~/.claude/projects/-Users-guyshtainer-VSCodeProjects-gba-toolkit-projects-PokeDNA/b6a21bde-.../workflows/scripts/pokedna-release-audit-wf_b40f1ea0-82c.js`,
    partial agent results harvestable from that workflow's transcript dir `journal.jsonl` (same-session resume
    won't work in a new chat — re-run the script or harvest the journal). KEY AUDIT FACTS already established:
    `ui_text` (tonc TTE, default margins) WRAPS past x=240 to the next line = the overlap Guy sees; font 8px/char;
    `ui_truncate` exists — un-truncated dynamic strings are the bug. Known dead code: `base_name()`
    pdna_main.c:179, `DLAB` pdna_summary.c:43, `party_list()` (only the no-PC fallback ~pdna_main.c:2957 —
    delete + replace fallback with the party popup), `hand_cursor.c/h` (include exists in pdna_box.c but cursor
    moved to OAM — verify zero call sites), gen3_box.c ACE helpers partially unused. Also a strncpy warning
    pdna_main.c:229 + ~15 misleading-indentation warnings to eyeball.
  - **GUY'S DECISIONS (locked in):** summary portrait animation must **default ON** (flip `g_anim_mask` init
    at pdna_main.c:81 to include `1u << ANIM_SUMMARY`; note a saved config.cfg `anim=` line overrides the
    default — existing users keep their setting). Release endgame = **push + tag + GitHub release WITH the ROM
    asset** (bundled-art posture like v1.0.0, Guy chose informed) — only AFTER the audit fixes + his HW test.
  - **NEXT STEPS (in order):** (1) re-run/harvest the audit workflow; (2) fix confirmed text overflows;
    (3) fix confirmed feature bugs; (4) delete dead code; (5) anim default ON; (6) rebuild + host tests +
    small commits; (7) Guy HW-tests (incl. wallpaper A/B on `a2eb21e`); (8) push + tag + release.

- **SESSION 2026-06-29 — Day-Care fixes (FR/LG + RSE "full" bug) + a PARTY-overlay popup (move mons
  to/from the party). UNCOMMITTED on `main`, builds clean (ROM ~6.50 MB), EWRAM `.sbss` ends 0x0203ECEC
  (4884 B headroom), host tests PASS. Adversarial multi-agent review caught + we FIXED a CRITICAL save bug;
  fix re-verified sound. AWAITING Guy's HW test.**
  - **Day-Care bug 1 (FR/LG "TO DAY-CARE" missing) FIXED:** the per-mon action + `app_to_daycare` were
    gated off for FRLG. FRLG has a real 2-mon breeding Day-Care (Four Island, **SB1 0x2F80** — verified vs
    pret/pokefirered; RS 0x2F9C verified vs pokeruby; Emerald 0x3030 vs vendored decomp). New single-source
    `dc_layout(base,stride)` (E 0x3030 / FR-LG 0x2F80 / RS 0x2F9C; stride RS?80:140) shared by the viewer +
    all deposit paths. Menu un-gated (`if (!is_bank)`).
  - **Day-Care bug 2 (RSE "DAY-CARE FULL" when not full) FIXED:** deposit decided occupancy by whether
    `pk_decode_mon()` returned true, which is true for a species-0 slot whose stored checksum != 0 (a
    "dirty-but-empty" slot, common on real saves). Now `dc_first_free()` matches the game
    (CountPokemonInDaycare: species != 0) and the viewer (`dc_rescan`): occupied = species 1..411 &&
    !isBadEgg. Used by `app_to_daycare` + `dc_deposit`. Regression test `tests/host_daycare_slot_test.c`
    reproduces the bug + proves the fix (PASS).
  - **PARTY overlay popup (feature #2)** — a Gen-4/5-style "move to/from party" popup over the box screen,
    opened from the box top **PARTY** tab (renamed from "PARTY SEL"). `app_party_overlay()` in pdna_main.c
    (2x3 icon cluster + Back), 2 modes: PLACE (carrying a box mon -> A adds/swaps into the party via the new
    shared `party_place_held()`, refactored from the old reviewed `app_carry_to_party` which is now removed)
    and GRAB (empty-handed -> A picks a party mon up to carry into a box). pdna_box.c gained a **party-origin
    carry** (`s_orig_party`): lift-don't-clear, removal deferred to a successful drop via `app_party_remove_at()`;
    cancel returns it untouched; blocked from leaving the PC to the Bank or being re-added to the party; the
    last party mon can't be grabbed (party can't be empty). Editing the full party is still START -> Party.
  - **⚠️ ADVERSARIAL REVIEW (workflow `pokedna-party-daycare-review`, 6 agents) found + we FIXED a
    CRITICAL data-loss + a HIGH dup**, both one root cause: a cross-buffer deferred move stages its SB1 half
    into `g_save` (app_stage_sb1) while its PC half lives only in `g_pc`; any intervening SB1/SB2/dex commit
    calls `app_save_finalize` which writes the WHOLE `g_save` but never folded `g_pc` -> half-saved move
    (party->box = LOSS; PC->Day-Care = DUP). **FIX:** `app_save_finalize` now folds pending `g_pc` into
    `g_save` (sections 5..13) whenever `g_pc_dirty`, clearing the flag only on success — so every whole-image
    write is self-consistent. Re-verified sound by an independent gen3-save-format agent (closes both, no new
    loss/dup, idempotent, deferred-move decline still works). This also fixes the SAME latent split-buffer
    hazard in the pre-existing Day-Care/withdraw paths.
  - **Files:** `source/pdna_main.c` (dc_layout/dc_first_free, party_place_held/app_party_overlay/
    app_party_remove_at, finalize fold), `source/pdna_box.c` (party-origin carry + PARTY-tab wiring),
    `source/pdna_app.h`, new `tests/host_daycare_slot_test.c`. Nothing committed (offer to commit).
  - **GARBLED BOX WALLPAPER — rumble theory DISPROVEN, re-fixed (uncommitted, needs HW test).** Guy
    confirmed the garble has **ZERO correlation with rumble** and predates rumble entirely, so `cba40e7`'s
    rumble-suspend theory was WRONG. Re-investigated: the data + blit are verified correct (host-render to
    PNG), icons correctly use OBJ tile ids 512..1023 (no Mode-3 framebuffer overlap), mode is Mode 3. The
    real outlier: `draw_wallpaper` was the ONLY box graphic reading ROM **per-pixel with the CPU, interleaved
    with VRAM writes, jumping randomly between map[] and tiles[]** — that access pattern thrashes the GamePak
    prefetch on the flashcart PSRAM (HW-only). The working paths avoid it (icons DMA tiles from ROM; the
    front sprite blits from a decompressed RAM buffer). **FIX:** `draw_wallpaper` now stages the tilemap +
    each tile into RAM with sequential reads, then blits from RAM (same shape as the working paths). +848 B
    IWRAM. **Unverified on HW** (can't repro in emulator). If still garbled: add a "Simple wallpaper" toggle
    (procedural grass, zero ROM tile reads) as a guaranteed fallback; a photo of the garble would confirm the
    failure mode.
  - **HW TODO (Guy):** on disposable save copies, per game: FR/LG "TO DAY-CARE" deposits + appears in the
    Day-Care; RSE deposit on a daycare that was wrongly "full"; party<->box moves both directions (grab a
    party mon -> drop in a box; carry a box mon up to PARTY -> add/swap), incl. the loss/dup repro = do a
    party->box move then SAVE something else (party edit / trainer card) WITHOUT exiting, then exit & decline
    -> the moved mon must be intact (not lost, not duplicated).

- **SESSION 2026-06-28 (part 2) — big feature batch + a CRITICAL save/mount bug, all committed on `main` (NOT pushed). Builds clean (ROM ~6.50 MB), host tests PASS. AWAITING Guy's HW test (he'll test 2026-06-29) that SAVING works again on `e3a350c`.**
  - **THE headline bug — EWRAM overflow corrupted the in-EWRAM SD driver** (`e3a350c`). Symptoms Guy hit: "save failed" (first time ever) + intermittent "SD mount failed" (8 retries exhausted). Root cause: part-2's data-safety fix (`045cdca`) stored the FULL 80-byte record per pending bank deletion (64×82 ≈ 5 KB) in `EWRAM_BSS`, pushing total EWRAM data to ~257 KB — past the 256 KB chip. **The devkitARM GBA linker does NOT bound `.sbss` to the ewram region**, so it linked clean ("ewram 0.57%") but on HW the overflow wraps (EWRAM mirrored at `0x02040000`) and overwrote the START of EWRAM = the `.ewram` `EWRAM_CODE` = the `io_ezfo` SD driver (runs from EWRAM while ROM is paged out). Corrupt driver → garbage reads (flaky mount) + failed write-verify (save fails). **The microSD was always fine** (Guy confirmed: mounts on his Mac, other carts launch). RTC was a RED HERRING (Guy: EZ-Flash RTC is GLOBAL, no per-game settings; toggling it off didn't help). FIX: store only the 8-byte identity (personality 0-3 + OT-ID 4-7) in a 640 B IWRAM table; `.sbss` now ends `0x0203ECEC`, **4884 B under** `0x02040000`. **ALWAYS** verify `arm-none-eabi-size -A` `.sbss` end ≤ `0x02040000` after any buffer change — a clean link does NOT prove EWRAM fits.
  - **National Dex unlock fixed** (`7599b8a`): marking #152-386 caught did nothing in-game; national mode needs a per-game magic+var+flag trio. `pk_dex_set_national`/`pk_dex_national_on` in `gen3_dex.c` (RS/E magic `0xDA`@pokedex+0x02, var `0x302`; **FRLG magic `0xB9`@pokedex+0x03, var `0x6258`**; flag E `0x896`/RS `0x836`/FRLG `0x840`). "Natl Dex ON/OFF" in the DEX:ALL menu + auto-enable on Catch ALL. Host-tested all 3 games (`tests/host_dex_test.c`). Also fixed `pk_pokedex()`'s national indicator (was FRLG-offset-only).
  - **Two data-loss bugs fixed** (`045cdca`, found by an adversarial gen3-save-format review): (a) carry-a-BANK-mon onto the PARTY tab zeroed the wrong (PC) slot / wrote OOB → now bank-origin defer-deletes; (b) deferred bank deletion matched a bare slot index → a re-arrange before save deleted the wrong mon → now matches the mon's identity. (The full-record match here is what overflowed EWRAM; `e3a350c` shrank it to the 8-byte id.)
  - **Garbled box wallpaper on HW fixed** (`cba40e7`) — rumble GPIO ISR (0x080000C4, cart bus) corrupts the >1-frame ROM-read wallpaper blit on the EZ-Flash. `rumble_io_suspend/resume` (a nesting-counted render guard in `rumble.c`) brackets `draw_wallpaper`, the `ui.c` blit primitives, the portrait LZ77 fetch, and the daycare/party DMA paths. Data proven correct (all 32 wallpapers host-rendered clean to PNG). Diagnosed via a multi-agent workflow. **Still wants HW A/B confirm** (cue-on garbled before / clean after).
  - **Boot hardening** (`28d8abd`, `0e7993d`): SD mount retries 8× with cart re-init + settle instead of single-try-halt; `rmbl_init` moved AFTER the mount (no cart-bus writes before SD is up); mount-fail halt shows the FatFs `fr` code.
  - Other part-2 features committed: auto-register Pokédex on every add path (`1d639ea`), edit a move's current PP (`c284937`), added bag items land in the correct pocket all games (`97e58c7`), carry a box mon onto the PARTY tab to add/swap (`afe3efd`), Bank→PC carry is a prompt-free MOVE (`36c4c52`).
  - **Guy's queued asks (deferred until he confirms saving works):** (1) make **PC→bank a deferred MOVE** (delete from PC at save, like Bank→PC) — use a tiny 8-byte-ID **IWRAM** table, NOT EWRAM; (2) add a **linker/Makefile guard** so an over-budget EWRAM fails the build (`ASSERT(__sbss_end__ <= 0x02040000)`).

- **SESSION 2026-06-28 — Pokéblock UI polish + events for all 3 games. committed; builds clean (ROM 6.50 MB), host tests PASS.**
  - Commits: `1e89137` (game-like Pokéblock UI + preset colour picker), `009f685` (RS/FRLG event tickets + legit state).
  - **Pokéblock UI (#1)**: case list shows a colour SWATCH per block + a count (N/40); per-block editor shows a
    big swatch + flavour/feel BARS. **Colour preset picker (#2)**: 15-colour swatch picker (no free number); A on
    the colour row opens it. `pokeblock_rgb()` colour table (runtime — RGB15 isn't const-foldable).
  - **Event tickets RS + FRLG + legit state**: per-game tables (RS Eon `FLAG_SYS_HAS_EON_TICKET` 0x853; FRLG
    Aurora 0x84B/Mystic 0x84A, received 0x2A7/0x2A8; Emerald unchanged), flags pulled from the pret decomp
    headers via WebFetch (SYSTEM_FLAGS 0x800 for RS/FRLG, 0x860 for Em). Grant now sets item + ship-enable +
    RECEIVED flag; MG indicator + MG-off message. **Deliveryman path NOT done** (needs a Wonder Card = separate
    save data) — granted directly to the bag, identical working result. HW-EXPERIMENTAL (confirm boats appear).
  - **#3 contest condition** is ALREADY editable on all games via the mon summary CONDITION card (commit
    d29008d) — RSE meaningfully; FRLG has the 6 bytes but no contests. No new work unless the user wants ribbons.
  - **#4 wallpapers**: still the unreproduced HW-only "jumble"; data+blit verified correct (20x18 tilemap into
    66 8x8 tiles via m3_plot). Open: get a photo to diagnose, or add a "Simple/solid wallpaper" fallback toggle.

- **SESSION 2026-06-24 (part 5) — Pokéblock case editor done + events researched. committed; builds clean (ROM 6.50 MB), host tests PASS (clock + build + pokeblock).**
  - Commit: `f86db9e` (Pokéblock case editor).
  - **Pokéblock case editor** — MENU -> Pokeblocks: 40 slots; A edits/creates (colour + 5 flavours + feel),
    "Delete this block" clears one. RS/Emerald only (FRLG has no contests). New `gen3_pokeblock.{c,h}` pure core
    (offsets RS 0x7F8 / Emerald 0x848, 40 × 8-byte stride, pad byte preserved), host-tested
    (`tests/host_pokeblock_test.c`). Offset verified vs the decomp (byte-exact) + the codebase's HW-validated
    dex seen1 (0x848+40*8=0x988). Writes SaveBlock1 sections 1-4 via `app_commit_pokeblocks`.
  - Note: the `daycare map/POKEMON_EMER_BPEE00.sav` is BLANK (all 0xFF, no section signatures) — not usable for
    real-save host checks; the synthetic round-trip + dual offset cross-check is the validation.

  ### Events (event-ticket grants) — DONE for Emerald (commit `ba58c72`)
  MENU -> Event tickets grants Eon/Aurora/Mystic/Old Sea Map: gives the key item + sets the verified
  ferry-enable flag (Eon 0x8B3, Aurora 0x8D5, Mystic 0x8E0, Old Sea Map 0x8D6 = SYSTEM_FLAGS+0x53/0x75/0x80/0x76)
  the game checks (`CheckBagHasItem && FlagGet(FLAG_ENABLE_SHIP_*)`). Idempotent; READY tag when already set;
  writes via `app_commit_sb1`. **Emerald only** (RS/FRLG flag numbers not in the local refs — reference/pokeruby
  is a partial decomp, pokefirered lacks the constants; pull RS Eon + FRLG Aurora/Mystic from a full decomp/web,
  add to `event_tickets()`). **HW-EXPERIMENTAL**: confirm the boat actually appears on a real Emerald save.

  ### Original research notes (kept for RS/FRLG follow-up)
  Goal: a one-tap "enable event" that gives the key ITEM + sets the access FLAG(s) per game, so the user can
  reach the event islands/legendaries. Emerald data (from the local decomp, clean-room):
  - **Eon Ticket** (item 275) -> Southern Island (Latios/Latias): FLAG_SHOWN_EON_TICKET 0x1AE (+ unhide
    FLAG_HIDE_SOUTHERN_ISLAND_EON_STONE 0x38E).
  - **Aurora Ticket** (item 371) -> Birth Island (Deoxys): FLAG_RECEIVED_AURORA_TICKET 0x13A,
    FLAG_SHOWN_AURORA_TICKET 0x1AF (+ FLAG_HIDE_DEOXYS 0x2FB / triangle 0x2FC).
  - **Mystic Ticket** (item 370) -> Navel Rock (Lugia 0x1DD-area + Ho-Oh): FLAG_RECEIVED_MYSTIC_TICKET 0x13B,
    FLAG_SHOWN_MYSTIC_TICKET 0x1DB.
  - **Old Sea Map** (item 376) -> Faraway Island (Mew): FLAG_RECEIVED_OLD_SEA_MAP 0x13C,
    FLAG_SHOWN_OLD_SEA_MAP 0x1B0 (+ FLAG_HIDE_MEW 0x2CE).
  RS + FRLG have DIFFERENT item ids / flag numbers (FRLG: Mystic Ticket + Aurora Ticket via the FRLG decomp;
  RS: Eon Ticket only). TODO: pull the RS/FRLG ids+flags from reference/pokeruby + reference/pokefirered,
  build a small per-game table, add a "Events" screen (list tickets -> grant = add item + set flags via
  pk_bag_* + pk_flag_set), and HW-validate that each event actually triggers in-game. The data_editor already
  exposes raw flags + the bag, so this is a convenience layer over existing safe writes.

- **SESSION 2026-06-24 (part 4) — more backlog + a carry fix. committed; builds clean (ROM 6.50 MB), host tests PASS.**
  - Commits: `50c9929` (swap-and-hold), `2aedc35` (dex bulk undo).
  - **Box swap keeps the displaced mon in hand** (user report): dropping a carried mon onto an occupied cell
    now places it and HOLDS the previous occupant (you place it yourself), instead of auto-throwing it into the
    carried mon's old cell. New `s_held_dup` distinguishes a discardable fresh duplicate from a real swapped-out
    mon (cancel places the latter in the first free slot; a RAM-only displaced mon can't cross the PC<->Bank
    boundary, so it can never be lost across two save scopes). Within-box / PC cross-box swaps atomic; bank
    cross-box swap still denied.
  - **1-click Pokédex bulk + UNDO**: `dex_bulk` (START -> Mark all) already had Catch/See/Wipe ALL with a
    confirm; added a snapshot-based "Undo last" so an accidental bulk change reverts in one step (RAM revert;
    the bulk isn't written until the dex-save confirm). Snapshot resets per dex-screen open.
  - **BACKLOG REMAINING (research-first, next batch):** Pokéblock case editor (SaveBlock1 add/change/delete —
    need the per-game Pokéblock-case offset + 8-byte struct, clean-room) and Events (#7, Gen-3 event flags /
    Mystery Gift). Still TODO: HW-validate everything from parts 2-4 (carry/item, contest, create-a-mon, dex undo).

- **SESSION 2026-06-24 (part 3) — feature backlog STARTED. 2 features + a data bugfix, committed. builds clean (ROM 6.50 MB), host tests PASS (clock + new build test).**
  - Commits: `d29008d` (contest condition), `db5ae55` (create-a-mon), `c411775` (gen_data exp fix).
  - **Contest condition editing** — new CONDITION summary card (card 7, NCARDS 8) edits cool/beauty/cute/
    smart/tough/sheen (sheen = Pokéblocks fed). `em_set_contest` patches bytes 6..11 of the EVs substruct
    (cosmetic, lossless round-trip preserved); F_CT0..F_CT5 fields in pdna_edit.
  - **Create a mon from scratch** — empty box/bank slot -> A -> CREATE: pick species, `gen3_build_mon`
    (pure, host-tested in `tests/host_build_test.c`) makes a valid Lv5 record (species/exp/hasSpecies/checksum,
    OT=save's, Poké Ball, Tackle placeholder), then the editor opens to customise; write via the gated path.
    `app_create_mon` + A_CREATE in app_mon_menu; box opens the menu on an empty slot when editable.
  - **DATA BUGFIX** (found by the new host test): `pk_exp_for_level(Medium-Slow, L1)` underflowed (-54 ->
    ~4.29e9) corrupting a level-1 Bulbasaur-line mon. Fixed in the tracked generator `tools/gen_data.py`
    (clamp the formula + guard L<=1=0); the generated `source/data_tables.c` (git-ignored) carries the same guard.
  - **HW TODO**: contest edit + save; create-a-mon (pick/build/edit/place, then check it loads + battles in-game).
  - **BACKLOG REMAINING (next batch):** Pokéblock case editor (SaveBlock1 add/change/delete — needs offset
    research), 1-click Pokédex fill (seen/caught) with warning + snapshot-revert, Events (#7, research event
    flags / Mystery Gift). Also still TODO: HW-validate the part-2 carry/item rework.

- **SESSION 2026-06-24 (part 2) — carry/item rework DONE + committed. builds clean (ROM 6.50 MB), host tests PASS.**
  - Commits this session: `c23db74` (batch 4-5: data-safety + daycare/PC-box), `5404022` (mon-in-hand carry + item).
  - **Mon-in-hand carry (replaces swap-in-place)** — `s_held`/`s_holding`/`s_orig_*` in pdna_box.c; helpers
    `start_carry`/`clear_origin`/`drop_held`; `boxoam_carry_held`/`boxoam_carry_end`/`boxoam_hide_slot` +
    region-A management (`s_rega`, `load_rega_hand`) in box_oam.c. **Lift-don't-clear** (origin kept until a
    successful drop → never loses a mon). Held mon decoded into region A (front-most PRIO 0; orange fist PRIO 1
    behind) → carry z-order fixed (#1a). **Carry into a FULL box** (L/R floats it; A drops/swaps) (#1b). Within
    a scope = move; across PC↔Bank = COPY with a confirm prompt (#1 cross-screen, loss-proof). Carry survives
    the PC↔Bank hand-off (state persists across pdna_box runs; `pdna_box_clear_carry()` per save). Bank entry
    while carrying → bottom row (#3a); pass-through never dirties the bank → no spurious save prompt (#3b).
  - **Item mode**: HOVER = small real item bottom-left on top (#2a); GRAB = full item front-most (region A) +
    orange transparent grab fist (region B) (#2b).
  - **Adversarially reviewed** (gen3-save-format): NO loss path; fixed A1 (cross-scope drop now confirms so it's
    not a silent dup) + B1 (reset s_orig_box/bank fail-closed). Remaining note E1 (species-0 carry shows only the
    fist) = cosmetic, can't occur in practice.
  - **HW TODO**: validate carry (grab/drop/swap, into full boxes, PC↔Bank copy-confirm), item hover/grab visuals,
    daycare spacing (#4), PC-opens-box-1 (#6). BACKLOG below unchanged (#7/#8/contest/pokeblock/1-click-dex).

- **SESSION 2026-06-24 — feedback batch 5. PARTIAL: #4/#5/#6 done + built clean; #1/#2/#3 = the carry/item rework (designed below, NOT yet built); #7/#8 + extras = backlog.**
  - **#5 daycare secondary-type areas (DONE):** `dc_region_pick` now walks BOTH types; a type with a dedicated
    area adds it (fire->lava, water, flying->sky, electric, grass/bug->grass), a type with NO area (dragon,
    normal, rock/ground-not-fire, ...) adds the fallback GRASS+EMPTY. e.g. Dragonite(dragon+flying)∈{sky,grass,empty}.
  - **#6 PC opens on box 1, app-remembered (DONE):** new `g_pc_last_box` (config key `pcbox=`), `pc_box_source`
    uses it instead of `pk_current_box(g_pc)`; `app_note_pc_box()` is called from `SWITCH_BOX` (PC only) and
    persisted via `cfg_save()` on PC-screen exit. First run = box 0; afterwards remembers the app's last box.
  - **#4 daycare slot spacing (DONE, HW-verify):** spread `DC_SPOT` so the two 32x32 icons per area don't
    overlap (lava stacks vertically dy~36; others spread horizontally dx>=34). Tune on HW if a mon lands off-region.

  ### NEXT: carry/item OAM rework (#1,#2,#3) — design to implement as ONE reviewed pass
  Root cause: move-carry is swap-in-place (mon stays in its slot, `OE_CARRY` shows a lifted copy reusing the
  source slot's tiles). That breaks across boxes (can't enter a FULL box; tiles clobbered on box reload) and
  dirties the bank on pass-through. Fix = a real **mon-in-hand** model:
  - **Held buffer:** `s_held_rec[80]` + `s_holding`; origin `(scope,box,slot)`. **Lift-don't-clear**: on grab,
    copy to the buffer and VISUALLY hide the origin icon (track origin; hide `OE_ICON0+slot` when rendering the
    origin box, even after paging) — do NOT clear the record. Drop within the SAME scope = true move (place +
    clear origin; swap lifts the occupant). Drop in the OTHER scope (PC<->bank) = COPY (origin kept) — keeps the
    two-save-scope case loss-proof. Cancel (B) = just stop holding (origin never cleared). This also makes
    cross-screen carry a real move within a scope while staying safe across scopes.
  - **Dedicated tiles (fixes 1a z-order for real):** during a mon carry the cursor hand is hidden, so region A
    (`TID_HAND`,16 tiles) is FREE — decode the held mon's icon there; `OE_CARRY` uses region A (survives box
    reloads). Single top sprite at PRIO 0 above all PRIO-2 box icons.
  - **1b full-box carry:** with the mon in hand (no slot needed) L/R just switches boxes; A swaps onto the
    cursor mon. No more "destination full -> denied".
  - **3a:** entering the bank while carrying -> cursor at the BOTTOM row (held mon floats there).
  - **3b:** pass-through never touches a bank slot -> bank not dirtied -> no "Save bank changes?" prompt;
    return lands on the same PC box.
  - **#2 item display:** [2a DONE] HOVER now shows a small 16x16 item in region B at the cell's BOTTOM-LEFT,
    PRIO 0, clear of the hand (`load_regb_item(item, full)` + `boxoam_carry_item(cur, item, full)` parameterized
    by size; `citem_index(..., full)` down-scales 24->16 for !full). [2b TODO] GRAB = FULL 32x32 item in region A
    (TID_HAND, free while the hand is hidden) + the ORANGE grab fist (`PB_HANDORG`, blended) in region B.
  - Verify with the gen3-save-format agent again (held-buffer loss/dup + tile packing) before calling done.

  ### BACKLOG (user "for later"/"start thinking" — NOT started)
  - **#7 Events:** add Mystery-Gift / event flags or event mons (needs research: Gen-3 event flag offsets, Wonder
    Card, distribution-mon templates). Design first.
  - **#8 Create a mon from nothing:** build a legal mon from scratch (species/level/IVs/EVs/nature/moves/OT/PID);
    reuse `gen3_mon` encode + the data editor; legality + PID/nature/shiny consistency.
  - **Contest stats:** edit coolness/beauty/cuteness/smartness/toughness + sheen; Pokeblock feed count; add/
    change/delete Pokeblocks (SaveBlock1 pokeblock case + the mon's contest-stat bytes).
  - **1-click Pokedex fill** (+ other bulk sorts): fill all seen/caught with a WARNING prompt + easy REVERT
    (snapshot the dex flag region before the bulk op so it can be undone in one step).
  - All blobs/ROM/decomp stay OUT of git (`daycare map/` gitignored).

- **SESSION 2026-06-23 (part 2) — feedback batch 4. UNCOMMITTED, builds clean (ROM 6.50 MB), host tests PASS.**
  - **Carry z-order:** OE_CARRY now ATTR2_PRIO(0) (front-most), so a carried mon draws above all box icons.
  - **#1 dual-type:** added slot randomization in `dc_take_slot` (random slot when both free) on top of the
    per-visit area pick, so mons visibly move between visits even within one area.
  - **#3a backups:** the picker now lists `.bak` files (`has_sav_ext` matches any name containing ".sav");
    reverted the session-once backup -> back to per-save backup of the pre-save file.
  - **#3b carry across boxes:** L/R (shoulders) while holding moves the mon to the next/prev box's first free
    slot (carry continues there).
  - **#2 real item sprite (FULL SIZE):** dropped the per-holder glyph markers; `load_regb_item` now renders the
    real 24x24 item icon centred in a 32x32 (16-tile) OBJ in region B (`citem_index` packer, palette bank 15).
    Holders stay OPAQUE while non-holders fade (blend in `boxoam_item_markers`); hovering a holder shows its real
    item full-size on top, and grabbing carries it full-size. L/R flips boxes while carrying an item;
    `s_item_from_box` lets B (put back) return the item to its source box across box switches.
    Adversarially reviewed (gen3-save-format agent): tile packing + item-carry state = no loss/dup.
  - **#4 PC<->Bank cursor flow + carry:** PC top tabs + UP -> Bank (cursor at bottom row); Bank bottom row +
    DOWN -> PC (cursor on tabs). `pdna_bank_show` returns the box code; `app_box_start_set/take` is the
    entry-cursor hint (1=tabs, 2=bottom). Cross-screen carry COPIES (see safety fix below).
  - **DATA-SAFETY FIX #1 (found by the review):** the MOVE-mode cross-BOX carry (L/R) was buggy in the BANK only —
    the bank pages all boxes through ONE shared buffer, so `records(nbx)` invalidated the old `recs` and the
    carry duplicated one mon + destroyed another. Fixed: snapshot the mon before paging, place in dest FIRST,
    clear source LAST (fail toward duplicate, never loss). PC path was always fine (distinct per-box buffers).
  - **DATA-SAFETY FIX #2 (cross-SCREEN carry):** carrying a mon across the PC<->Bank boundary spans two
    independent save scopes (PC = whole-save prompt; bank = per-box-file prompt). Clearing the source there could
    LOSE a mon on "save one / decline the other". Now cross-screen carry COPIES (original stays put) — at worst a
    duplicate; the user deletes the original to finish a move. Within-screen cross-box carry remains a true move.
  - Files: box_oam.c, pdna_box.c, pdna_main.c, pdna_bank.c/.h, pdna_app.h. NOT committed (offer to commit).

- **SESSION 2026-06-23 — feedback batch 3 (committed). builds clean (ROM 6.50 MB), host tests PASS.**
  1. **Daycare dual-type random:** `dc_region_pick(species, *rng)` collects ALL areas a mon qualifies for
     (fire->lava, water, flying->trees, electric->yellow, grass/bug->grass; non-fire rock/ground->empty) and
     randomly picks one per visit (per-visit RNG `s_dc_visit_rng`, stable within a visit). Fire rule kept.
  2. **Item-mode transparency per-object:** removed BLD_OBJ from the BLDCNT 1st-target (only ATTR0_BLEND
     objects fade now). `boxoam_item_markers` sets a per-icon `s_icon_blend[]` = non-holders; `place_grid_slot`
     applies ATTR0_BLEND. Result: glove + non-holder icons translucent; item HOLDERS, item badges, and the
     carried item stay OPAQUE (fixes the Magikarp "glitch blob" = the now-opaque carried item).
  3a. **Backup = session original:** new `g_session_backed_up` (reset per `view_save`) — back up ONLY on the
     first save of a session, so the `.bak` is always the file you chose, never an intermediate save state.
  3b. **Daycare flow:** "TO DAY-CARE" now opens the Day-Care page after depositing (forward-decl + call
     `pdna_daycare`). Withdraw-to-PC parks the mon in a free PC slot and sets a pending pickup
     (`g_pickup_box/slot` + `app_take_pickup`); `pdna_daycare` returns, and `pdna_box` (entry + post-menu)
     opens that box with the mon lifted in the move glove so you place it. All still deferred.
  4. Top-tab nav (PARTY SEL / SAVE) — confirmed working.
  - Commits: `454d979` (the big batch) + this refinement batch. Files: pdna_main.c, pdna_box.c, box_oam.c.

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

1. **WAIT for Guy's HW test (2026-06-29) of the EWRAM-overflow fix `e3a350c`.** Decisive check: make an
   edit, SAVE → it must succeed (no "WRITE FAILED"); reboot a few times → must mount reliably. This is the
   gate — everything below is blocked on saving being solid again. If saving STILL fails on `e3a350c`,
   re-open the EWRAM angle first (re-check `arm-none-eabi-size -A` `.sbss` end ≤ `0x02040000`), then the
   mount-fail halt now prints the FatFs `fr` code — get that number; `fr=13` = NO_FILESYSTEM (read returns
   garbage), and remember the EZ read returns success-on-timeout so a hard fault surfaces here, not as a
   read error.
2. **Once saving is confirmed, implement Guy's two queued asks:**
   - **PC→bank = a deferred MOVE** (currently a confirmed COPY that keeps the PC original). Mirror the
     Bank→PC path: on the PC→bank drop, place into the bank + queue a PC-slot deletion applied at the save
     phase (after the PC commit, fail-toward-dup). **Keep the deletion table tiny (8-byte id) and in IWRAM
     `.bss`, NOT EWRAM** — EWRAM has only ~4.9 KB headroom. Touch points: `drop_held` PC→bank branch
     (`source/pdna_box.c`), a new PC-side defer-delete list in `pdna_main.c`, applied in `flush_on_exit`.
   - **Linker/Makefile EWRAM-overflow guard:** add `ASSERT(__sbss_end__ <= 0x02040000, "EWRAM overflow")`
     to the linker script (or a post-link `arm-none-eabi-size` check in the Makefile) so an over-budget
     EWRAM **fails the build** instead of silently corrupting the SD driver on hardware. This is the
     footgun that caused this whole saga.
3. **Still-pending HW A/B confirms from this batch** (not blocking, but unverified on HW):
   - **Wallpaper fix** (`cba40e7`): rumble ON at full strength → box wallpaper clean; also check the dex
     grid, party list, summary, secret base, daycare render clean with rumble on. Haptics should still fire
     (maybe a hair delayed on big paints), and a save (RCUE_SAVE) still verifies.
   - **National Dex** (`7599b8a`): enable it + Catch ALL → in-game dex shows 386, not 151 (esp. LeafGreen).
   - **The two data-loss fixes** (`045cdca`) on disposable saves: bank-mon→party ADD; re-arrange a bank box
     after queuing a Bank→PC move, then save (must not delete the wrong mon).
4. **Then push when Guy OKs** — nothing pushed; this session is ~14 commits local on `main` (top = `e3a350c`).
   Public repo `github.com/GuyShtainer/PokeDNA`, GPLv3, no-reply identity. Don't `git push` until asked.
5. **Deferred / optional (older):** RTC "Clock fix" screen still needs its HW test (see
   `pokedna-rtc-fix-feature` memory); multi-select move; Legality V2 encounter half; Sky-wallpaper 1-px strip.
6. **Watch (IP):** item icons + mon sprites are ripped art already in the ROM; the released v1.0.0 ROM still
   has the unresolved sprite infringement (deferred). Don't add NEW ripped art to a public build without Guy's OK.
   Also: the sibling **File-Browser-GBA** has the same single-try SD-mount pattern → port the retry there.

## How to build / test / run

```
# Build the ROM (local devkitPro; ./build.sh uses Docker if preferred):
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C projects/PokeDNA rebuild
#   -> projects/PokeDNA/PokeDNA.gba   (TITLE=PokeDNA; do NOT gbafix -p pad — just bloat, no benefit)
#   `make` = full build (all sprites embedded); `make sd` = optional trimmed build (streams from
#   /PokeDNA/sprites.pak). No ROM-size load limit — the full ~6.25 MB build loads from SD fine.
#   If a build hangs at the flashcart "Load game": re-copy the .gba + eject safely (bad SD copy).

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

- **~~EZ-Flash PSRAM ceiling~~ — MYTH, corrected 2026-07-13.** There is no few-MB SD-load size
  ceiling: the 6.25 MB build loads via normal SD "Load game." A hang at "Load game" = a bad/
  incomplete SD copy → re-copy the `.gba`, eject safely. Don't `gbafix -p` pad (just bloats the
  file, no benefit). LZ77-compressing big art is still fine for size, but not load-required.
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

### Session — 2026-06-28 (part 2) (feature batch + a critical save/mount bug — all committed, builds clean, HW test 6-29)

- **Intent:** Continue Guy's rapid HW-feedback batches. Then chase two "critical bug" reports he hit on
  hardware: (a) garbled box wallpaper, (b) "SD mount failed" HALT on launch, then (c) "save failed (first
  time ever)". Guy: *"stop focusing on the RTC, its not it, look for the real cause"* and *"the issue is in
  your code"* — both correct.
- **Did:**
  - National Dex unlock (`7599b8a`), PP edit (`c284937`), item-pocket fix (`97e58c7`), carry-to-party
    (`afe3efd`), Bank→PC prompt-free move (`36c4c52`), dex auto-register (`1d639ea`).
  - Adversarial gen3-save-format review of the cross-storage moves → fixed 2 data-loss bugs (`045cdca`).
  - Wallpaper garble = rumble GPIO ISR corrupting the ROM-read blit on HW; multi-agent workflow confirmed
    it (host-rendered all 32 wallpapers clean to prove the data was fine) → `rumble_io_suspend/resume`
    render guard (`cba40e7`).
  - SD mount: added retry (`28d8abd`), moved rumble init after mount + fr-code halt (`0e7993d`), then a
    sector-0 diagnostic (`29b73e8`).
  - **Found THE root cause via `arm-none-eabi-size -A`:** EWRAM `.sbss` had overflowed past `0x02040000`
    (the 5 KB full-record bank-deletion table from `045cdca`), wrapping over the `EWRAM_CODE` SD driver →
    flaky reads + failed save-verifies. Fix `e3a350c`: 8-byte identity in a 640 B IWRAM table; EWRAM now
    4884 B under the ceiling. Removed the diagnostic; clean halt keeps the `fr` code.
  - `/learn`: updated `rumble-haptics.md` (rumble↔ROM-render corruption), `gba-build-and-memory.md`
    (corrected "clean link proves EWRAM fits" → it doesn't; EWRAM-overflow-corrupts-EWRAM_CODE), and
    `flashcart-sd-io.md` (EZ read returns success-on-timeout; robust mount; init cart bus after mount).
- **Left off:** All committed on `main` (top `e3a350c`), nothing pushed. Delivered the fixed ROM. Guy will
  HW-test saving tomorrow (2026-06-29). The save fix is the gate for the two queued asks (PC→bank move +
  linker EWRAM guard).
- **Open threads:** HW confirm saving works on `e3a350c`; the wallpaper/national-dex/data-loss A/B HW
  checks; PC→bank deferred move; linker `ASSERT(__sbss_end__ <= 0x02040000)`; port the SD-mount retry to
  sibling File-Browser-GBA.

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
