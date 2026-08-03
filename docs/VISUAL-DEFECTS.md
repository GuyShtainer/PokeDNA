# Visual defect sweep (2026-08-03) — CONFIRMED, not yet fixed

2,600+ emulator screenshots across four sweep passes, each defect then INDEPENDENTLY REPRODUCED
by a second agent from the repro steps. Only reproduced defects are listed. Screenshots live in
`<scratchpad>/visual/` and on Guy's Mac at `~/Desktop/PokeDNA-sweep/`.

| # | screen | defect | file:line |
|---|---|---|---|
| D1 | Summary (egg) | "Met Lv..." row drawn ON TOP OF the footer — two strings superimposed, neither readable. Egg rows push the running `y` cursor down 21 px and there is no bottom clamp. Non-egg mons land at y=131 and are fine, so it is specifically egg + 3-line ability desc. **severe** | `pdna_summary.c:197`, egg rows `:162`/`:164`, footer `ui_text(4,152,...)` |
| D2 | Bank / PC box | **Guy's own report.** The box-name "selected" look is decoupled from the cursor: moving up from the grid onto the box name, then again onto the SAVE tab, changes **0 pixels** in the banner band (rows 12..27) — verified by pixel diff. The cue is a 1-px `#FFFF94` frame indistinguishable from the banner's gold body, and the cursor hand parks mid-banner hiding the box number ("BANK [hand] 0/30"). The FOOTER does distinguish the states, so the minimal fix is to make the banner + hand honour `s_tab_focus`. | `pdna_box.c:798` (`draw_box_banner` takes only `on_title`), `:754`, `:791`, `:1612`, `:810` |
| D3 | Summary | Labels overwritten by their own values: "SpecieWHISMUR", "AbilitSOUNDPROOF", "Beauty255", "NatureADAMANT". Label ends at x=153 but the value starts at x=146. | `pdna_summary.c:168`, `:185`, `:189`, `:316`, `:323` |
| D4 | Summary (condition) | "Sheen = Pokeblocks fed" runs to x=240 and wraps — "s fed" is drawn over the portrait column on top of "Lv74"/"METAGROSS". x=98 leaves 17 columns; the string needs 22. **severe** | `pdna_summary.c:329` |
| D5 | Summary (origin) | "TID %05u SID %05u" is 19 chars truncated to 17, so only 2 of the 5 SID digits survive. | `pdna_summary.c:308-309` |
| D6 | Summary (egg) | Egg header rows truncate mid-word: "Hatch ~10240 ste~", "40 egg cycles le~". Both strings are 18 chars into a 17-char truncate. | `pdna_summary.c:162-165` |
| D7 | PC box / Bank | PKMN DATA panel truncates species to 9 columns and the held item to 5: "JIGGLYPU~", "BUTTERFR~", and EVERY berry collapses to "LUM ~" so you cannot tell which berry is held. The party overlay shows the same fields at 10 columns, so the two screens disagree with each other. | `pdna_box.c:38` (`PANEL_W 76`), `:225`, `:230`, `:235`; cf. `pdna_main.c:1316/1320/1323` |

Already FIXED this session (same class, found earlier):
- Fly destinations "Littleroot Townoff" — `%-15s` pads but does not truncate. Now `%-16.15s`.
- Region map said "AQUA HIDEOUT" over open sea in Kanto — FRLG's empty-cell mapsec is 197, RSE's
  is 213, and there is ONE 256-entry name table. Now per-family in `map_name()`.
- Mirage Island screen overflowed 30 columns and its rows collided with the footer.

Remaining passes (map, editors, PC fidelity) reported too — read
`<claude>/subagents/workflows/wf_97c17967-70d/journal.jsonl` for their confirmed lists.

⚠️ The PC-fidelity comparison was re-run after Guy caught that it opened on **box 7, which is
empty** (his `currentBox` is 7). Full boxes are 1, 2, 3, 5 (30 each). Any future PC work must
switch box or set `currentBox` first, and verify the grid is full ON SCREEN before capturing.

## Added by Guy 2026-08-03 (after the sweep)

| # | screen | ask |
|---|---|---|
| D8 | nav menu | **The selection bar crosses the bottom of the text.** The highlight rectangle clips the descenders / sits too low against the row. `nav_menu()` in `pdna_main.c` uses `rh = 9` with an 8 px font (it was dropped 10 -> 9 to fit more entries), so the bar and the glyph box are off by a pixel. Fix the bar's y/height so it frames the text instead of cutting it. |
| F1 | PC box / Bank | **Two-column box layout**, with a transparent effect over the grid that stays READABLE. Guy's words: "its time for a 2 column box, with a nice transparent effect but still readable." Relevant existing work: the `boxfidelity` sweep pass compared PokeDNA's box against retail Emerald's multi-select transparency — read that agent's findings in `wf_97c17967-70d/journal.jsonl` before designing, since it measured what the real game does (GBA alpha is BLDCNT/BLDALPHA in 1/16 steps). Note D7 above: the current PKMN DATA panel is only `PANEL_W 76` and truncates species to 9 cols / items to 5, so a re-layout should fix D7 at the same time. |

**Fix order agreed with Guy:** he said "you can compact and start the fixing", so work D1-D8 then F1.

## MEASURED: how retail Emerald does the multi-select transparency

Read straight out of the running retail ROM's registers in each state (not inferred):

**Real game, multi-select (several mons picked up):**
- `BLDCNT   = 0x3F00` — 2nd target = BG0|BG1|BG2|BG3|OBJ|BD (everything)
- `BLDALPHA = 0x0B07` — **EVA = 7/16, EVB = 11/16**
- OAM: the selected icons are set to **OBJ mode 1 (semi-transparent)** — exactly the 23
  item-less Pokemon in the test box. **Item HOLDERS stay opaque.**
- i.e. the game does NOT blend via a global effect; it flags the individual sprites
  semi-transparent and lets the OBJ hardware do it against everything beneath.
- While an item is actually in hand: `BLDCNT = 0x0000`.

**PokeDNA today (item move mode):**
- `BLDCNT   = 0x1440` — 2nd target = BG2|OBJ only
- `BLDALPHA = 0x080A` — EVA = 10/16, EVB = 8/16 (too solid)

**To match:** `REG_BLDALPHA = (7) | (11 << 8);` and widen the 2nd target to every layer, so a
faded icon blends against the wallpaper the same way. For the SELECTION specifically, do not
drive BLDCNT/BLDALPHA at all — set OBJ mode 1 on the chosen icons and leave the registers alone.
A consequence worth having: with a real OBJ-alpha effect the cursor hand no longer needs hiding.

Full detail (3 defects with repro steps and register dumps) is in the `boxfidelity` agent's entry
in `wf_97c17967-70d/journal.jsonl`.
