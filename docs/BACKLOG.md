# PokeDNA backlog — Guy's requests (2026-07-18)

Guy: "remember for later" — six items, verbatim intent + first technical notes.
Work order/priority: Guy decides; none started. (Wallpaper hunt + HW test results are the
active thread — see `analysis-2026-07-17/CONTINUATION.md`.)

## 1. Unown letter picker = the dex-style species grid, reused
When creating **or** editing a mon and the species chosen from the dex-style picker is
Unown (letter A shown in the list), immediately show **the exact same picker layout again**,
now populated with all 28 Unown forms (A..Z ! ?) — pick the letter there. "Simple, intuitive,
looks good."
- Today: species picker in `pdna_pick.c` (dex-style grid); a separate small
  `pick_unown_form()` grid exists (`pdna_pick.c:~967`) and is already wired in the editor
  (`pdna_edit.c:~176` → `em_set_unown_form`, PID-preserving nature+shiny). So the *plumbing*
  exists — the ask is purely: form chooser must REUSE the species-picker's grid layout/look,
  and must also trigger on the CREATE path, not just edit.

## 2. Data editor flags: foldable sections + the Elite-4 question
- Each titled flag list becomes **collapsible**: pressing the title toggles the section.
  State remembered **per session only** (RAM), and every session **starts collapsed**.
- BUG/question: Elite-4 flags show OFF despite Guy having beaten them. Investigate before
  "fixing": RSE lets you rechallenge the E4 — the game likely **clears the E4 trainer-defeat
  flags** to enable rematches, so OFF may be game-truth. Check whether the section should
  instead surface `FLAG_SYS_GAME_CLEAR` / Hall-of-Fame state (and label it so it isn't
  confusing). Verify against pokeemerald/pokeruby decomps.

## 3. Ruby/Sapphire hidden-item flag labels look wrong
Ruby: under "hidden items", right below "Lavaridge Town ice hea..." there are raw **numbers**
instead of names. Check Sapphire too. Likely cause: the flag-name table was generated from
one game's decomp (Emerald?) while R/S hidden-item flag IDs differ → unmapped IDs print
numeric. Audit `gen3_flags.c` / the generated name tables **per game** against pokeruby
decomp (`reference/pokeruby*`); also confirm the truncation ("ice hea...") isn't hiding a
mislabeled row.

## 4. Bag — ✅ EMERALD DONE 2026-07-19 (impl-bag.md; RS/FRLG art not local, plain-tab fallback)
Mimic the real bag screen using art from the decomps — **match game version AND player
gender** (RS/E/FRLG bags all differ; male/female bags differ). MUST keep current abilities:
browse items like the game, **add items from thin air**, choose quantity.
- ⚠️ IP note (established repo posture): decomp-ripped art = generated data, git-ignored
  source-side; it still ships in the release ROM (same deferred concern as the sprite packs —
  see the PokeLinkSim remediation history). Flag at release time.

## 5. Trainer card — ✅ EMERALD DONE 2026-07-19 (impl-card.md; stars = achievement toggles, RS model wired, FRLG n/a)
Mimic the real trainer card; match game + gender. Let Guy **edit the star count**.
- Investigate first: Gen-3 card stars are **computed** from achievements (Hall of Fame,
  full dex, all contests, Battle Tower/Frontier feats — per game), not one stored number.
  "Edit stars" = set/clear the underlying flags/counters; enumerate them per game from the
  decomps and expose them honestly (e.g. toggles that together yield N stars).

## 6. Battle Record export (Emerald) — ✅ IMPLEMENTED 2026-07-19 (HW export test pending)
Built: `source/gen3_record.{c,h}` (pure-C sector-31 parser, clean-room byte-exact spec in
`analysis-2026-07-17/record-spec.md` — NOTE it CORRECTS the framing notes below: sentinel
0xB39D at 0x1F000, struct at 0x1F004, u32 BYTE-SUM checksum over first 3964 struct bytes,
no footer signature), `tests/host_record_test.c` (validates against the REAL record in the
Emerald fixture: Battle Factory Lv50 by "GUYA", both teams decode), new START-menu entry
"Battle record" (nav rh 10→9 to fit 13 rows) → info screen + A = export raw 4 KiB sector to
`/PokeDNA/battles/<name>_<seed>.rec` (verified write, Omega-gated, logged). Per hard rule 7
the SD export needs HW sign-off. Original notes:
The full design is parked in `docs/IDEAS.md` (this repo): Emerald Frontier **Battle Record**
lives at .sav **sector 31 (0x1F000)** — seed + both teams + input stream; deterministic and
exportable; optional round-trip write-back with checksum recompute (verified-write
discipline). **Emerald-only** — RS predate the Frontier, FRLG have none (so no porting).
One record at a time (sector is overwritten per recording).

## 8. On-cart .rec IMPORT (QUEUED 2026-07-27 — implement after the bag/card fix agents land)
Guy: import an old .rec into the save "as if it's the last one I recorded". FEASIBILITY
PROVEN: the game gates the Frontier Pass Battle Record purely on sector 31's own validity
(pokeemerald recorded_battle.c CanCopyRecordedBattleSaveData -> read + sentinel/flags/
checksum; frontier_pass.c:638 hasBattleRecord = that call — NO other flag), and the PC
round-trip is verified (tools/read_rec.py --inject, tested with Guy's real records against
the fixture save). PC version WORKS TODAY. On-cart plan: Battle Record screen gains
"SELECT = import" -> picker over /PokeDNA/battles/*.rec (f_opendir list) -> validate via
g3_record_scan on the loaded file -> app_confirm ("Overwrites the save's current record")
-> memcpy into g_save+0x1F000 -> the existing verified full-save commit (backup + .tmp +
verify + rename; the full-save write already covers sector 31). Omega-only. Deferred only
to avoid racing the in-flight pdna_main.c edits (workflow wf_5ce72e7e-561).

## 9. ✅ DONE 2026-07-27 (impl2-vendor/bag-all/card-all.md) — RS/FRLG bag + card, both genders (was QUEUED — Guy: "on all gen 3
version sex variants"). Requires vendoring art the local checkouts lack: clone/sparse-fetch
pret/pokeruby + pret/pokefirered graphics (bag/item_menu + trainer_card dirs) + each game's
src/item_menu.c + src/trainer_card.c for layouts/coords into assets/bag/<game>/ +
assets/card/<game>/; extend gen_bag_bg.py + gen_card_bg.py with per-game entries + layout
tables (RS bag layout differs; FRLG bag is 3-pocket — v1 keeps FRLG chrome for all 5
PokeDNA pockets; FRLG card front differs). BONUS unlocked by the clone: pokefirered
src/trainer_card.c star formula -> wire FRLG stars (currently "n/a"). Also extend the
pocket-switch bag animation (item 4 fix round) to the new games' sprite sheets.

## 10. Flags tab partial redraw (QUEUED 2026-07-27) — data_editor repaints the whole list
(ui_clear) on every cursor move; Guy: slow + ugly; apply the pick_species pattern (full
repaint only on scroll/fold change; cursor move = erase + redraw the two affected rows).
Deferred only to avoid racing wf_5ce72e7e-561 in pdna_main.c.

## 11. Deeper Pokémon validation (Guy 2026-07-27: "the validation check is very basic")
Strengthen gen3_legality/pdna_legality beyond the current basics. Candidate checks (scope
next session, decomp-verify each rule): move legality vs learnsets.c (level-up/TM/HM/egg/
tutor per species + origin game), IV/EV bounds + EV-sum 510, PID consistency (nature/gender
ratio/shiny vs TID-SID, Unown letter), met-location/met-level/origin-game plausibility +
ball legality, species-vs-evolution stage at met level, language/OT sanity, held-item
validity, egg-flag coherence, ability index vs species. UI: per-mon "legality report" list
(warnings vs hard-illegal), maybe a box-wide sweep. Pure-C core + host tests against the
fixture saves; read-only analysis (no auto-fix without explicit action).
