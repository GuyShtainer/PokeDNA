# Overworld map feature — continuation + definition of done

> Working doc for the 2026-07-30/31 session. Guy is asleep; this is the contract for what
> "finished" means so it is not a judgement call at 4am. **Update the checklist every cycle.**
> Resume by reading this file top to bottom, then `docs/HANDOFF.md`.

## What Guy asked for, in his words

- 2026-07-29: *"3) change the location of my character … fly over the whole map and see all the
  NPCs … grab it and place everywhere i want (also unstepable places lol, but be warned) …
  We will use the real real map!"*
- 2026-07-29: *"perhaps try to stitch everything is this phase, and let me go in and out of room
  so i can explore more and better test in all games"*
- 2026-07-30: *"lets try to advance another step while tackleing the curruption while zooming out.
  lets stitch the map, and enable enterning and exiting"*
- 2026-07-31: *"keep building this feature to the end … Try to move the character, load maps, grab
  it, move maps etc (everytime ask you QA agent to think of what to test and do it) … look back at
  how I described the full feature and check if it implamneted. I also had an idea to draw the
  character in slight disstress with sweat … Go all out."*

## Decisions Guy made 2026-07-31 (binding — do not re-litigate)

| Question | Answer |
|---|---|
| Grab & place | **Write it for real**, with a confirm dialog + automatic pre-edit snapshot so it is undoable |
| NPC overlay | **Real overworld sprites decompressed from his ROM**, only NPCs actually present in his save |
| NPCs at zoom | **Yes**, shown at zoomed-out levels too (dots below 1:1 — a 16 px sprite covers 2 tiles at Z1) |
| NPC detail panel | **Yes**, on the cursor |
| Seams w/ mismatched secondary tileset | **Stitch anyway, per-cell fallback to the border** (~95% of cells exact) |
| Zoom ladder | **5 levels** (see below) |
| Test save | Auto-detect. **Resolved: `/Users/guyshtainer/Desktop/pokemon sav/POKEMON_EMER_BPEE00.sav`**, standing at map **0.33**, pos (33,9). The `daycare map/` copy has no valid slot. |

### Controls — Guy's exact mapping (do not "improve" it)

| Key | Action |
|---|---|
| **D-pad** | move the cursor; crosses map seams into connected maps |
| **A** | grab the character / drop it here |
| **B** | cancel the carry if carrying; otherwise exit the map view |
| **SELECT** | toggle the camera between the character's spot and the last place scrolled to |
| **START** | enter the door / cave / warp under the cursor |
| **L / R** | zoom out / in |

Note: there is deliberately **no back-stack key**. Rooms are left the way the game leaves them —
walk onto the exit warp and press START. Record this rather than inventing a hidden binding.

### Zoom ladder — 5 levels

| Level | View |
|---|---|
| Z0 | 1:1, 16 px per metatile (Mode 0, two tiled BGs) |
| Z1 | 1/2, 8 px per metatile (affine BG + per-metatile mip dictionary) — **exact** |
| Z2 | 1/4, 4 px per metatile — **approximate**: an affine BG caps at 256 distinct tiles and a 60x40 window needs up to 428, so the rarest metatiles fall back to an average colour |
| Z3 | the game's own **region map, zoomed-in mode**, from the ROM |
| Z4 | the game's own **region map, whole region**, from the ROM |

## Definition of done — every box must be ticked or explicitly reported as unmet

### Correctness fixes
- [x] **P0** FatFs `disk_read`/`disk_write` bounce on `& 0x3` not `& 0x1` (16 sites, toolkit-wide). Root cause of Emerald 1:1 corruption: EZ-Flash path is DMA32, GBA DMA force-aligns down, a 2-mod-4 destination gets every sector 2 bytes early and still returns `RES_OK`.
- [x] **P1** Affine tilemap written as aligned halfword pairs; `ZW` 32 -> 34. Verified at instruction level: no `strb` targets 0x0600D000.
- [x] **P1b** Six side defects: palette banks 13/14 uploaded, HUD tile cap 239 -> 127, negative-shift guard, two DMA32 source alignments, stale-tile-on-miss, SD-read-from-render-loop.
- [x] **P2** Metatile tables moved to OBJ VRAM (0x06010000). Phase-2 arena worst case 29,280 -> 13,824 of 35,712.

### Features
- [x] **P3** `map_view` re-entrant: `MapViewState`, `mgfx_can_load()` validate-before-destroy, loading frame, no `DCNT_BLANK` white flash.
- [x] **P4** Connection crossing — walk off an edge into the neighbour map, coords rebased.
- [x] **P5** Stitching — neighbour blockdata drawn beyond the seam, per-cell border fallback.
- [x] **P6** Warps — START enters; destination resolved like `SetPlayerCoordsFromWarp`; dynamic/dummy/bad-map refused with a reason.
- [x] **G1** Grab & drop on A — logic + confirm + undo snapshot; **grab + carry VERIFIED in mGBA**. Drop-commit path not yet exercised (it writes the save; test only against a COPY).; place anywhere incl. unwalkable (warn); confirm + undo snapshot; writes `continueGameWarp`.
- [x] **G2** Distressed-carry animation: character drawn straining with a sweat drop while carried; **dust puff on drop**.
- [x] **S1** SELECT toggles camera: character spot <-> last scrolled position.
- [x] **N1** NPC overlay — **VERIFIED in mGBA**: real sprites render on Route 118, and the cursor
  detail panel matches ROM ground truth exactly (`NPC g50 mv9 / flag 0 elev 3` at (28,8) == object #12).
  `source/npc_gfx.{c,h}` verified (238/239 Emerald ids resolve; contact
  sheets read back as real people). `source/map_oam.{h}` interface written; `map_oam.c` in progress.
  BUDGET IS FINE: worst real map = 46 objects, 24 distinct sprites, 5 palettes vs 128 OAM / 16 banks.
  ⚠️ MUST resolve the VAR ids: 578 of Emerald's 2,941 placed objects use OBJ_EVENT_GFX_VAR_0..F,
  resolved via SaveBlock1 var 0x4010+n. Without it a fifth of RSE NPCs are holes.
- [x] **Z2** Approximate 1/4 zoom — **VERIFIED in mGBA**, clean (one artifact found + fixed: flat
  fallback tiles keyed on palette banks 13-15 painted UI colours into the map; now clamped to 0..12). 64x44 window, dictionary split
  into 239 exact mip slots + 16 flat per-palette-bank fallback tiles (slots 240..255); a metatile that
  misses the exact budget is drawn as its centre-pixel colour. HUD labels it `1/4~` so the
  approximation is visible to the user. NEEDS an emulator run to judge how blocky it actually looks.
- [x] **Z3/Z4** Region map — **WIRED AND VERIFIED in mGBA**: Hoenn renders at both levels from the
  ROM (`RGN` zoomed 2x on the player's section, `RGN+` whole region), and the round trip back down
  to 1:1 is byte-identical to a run that never entered it.
  Levels are ONE dataset: the game's "zoomed" mode is just `BG2PA=BG2PD=0x80` + a scroll.
  `source/map_region.{c,h}` owns the VRAM/palette contract; decompression uses **SWI 0x12
  (LZ77UnCompVram)** because the destination is VRAM.
  ⚠️ 8 of 11 (game,revision) anchors come from pret .sym files, unverified against a dump; they
  fail closed (validated at open, never render garbage) with a bounded signature-scan fallback.

### Verification
- [x] Builds clean. ROM ~12.24 MB, IWRAM 16,936 B (51.7%), EWRAM .bss 1,500 B, arena peak 32,768/35,712.
- [x] Host suite green on Emerald + `host_conn_test` 0/4,822 on all three ROMs. (The older render/rommap tests assert Emerald-specific ground truth, so their Ruby/FireRed 'failures' are by design.)
- [x] New pure-C math host-tested (`tests/host_conn_test.c`): **0 failures, 4,822 checks across Emerald/Ruby/FireRed**. Emerald 134 cardinal connections all reciprocal, 1,313 warps (1,268 resolve, 45 dynamic); FireRed has **2 one-way connections** — never chain seams into a global atlas.
- [x] **mGBA run** — all of it, screenshots read back: boot, map, move, cross a seam (Route 117 -> Verdanturf), NPCs + detail panel vs ROM truth, grab+carry, all 5 zoom levels, region round trip.
- [x] **QA agent pass 1** — designed its own plan, built an offline oracle from the ROM tables, drove
  151 screenshots, and asserted on MEMORY (decoded `s_mv`/`gfx`/arena counters out of the ELF) rather
  than pixels. Results: seam crossing 20/20, **stitching pixel-EXACT** (strip before a seam matches
  the real map after crossing, including a mismatched-secondary seam), warps 20/20, robustness 24/24,
  Ruby 20/20, grab+drop commit verified end-to-end, `mgfx_mt_misses()` **0 in ~150 samples**, arena
  peak 32,768/35,712 everywhere, map switch ~9 frames, box screen byte-identical after a map visit.
  **7 defects found; 1,2,4,5,6 FIXED + retested, 3 FIXED by design change, 7 deferred.**
- [x] Arena peak 32,768 / 35,712 and `mgfx_mt_misses()` = 0 across ~150 QA samples.
- [x] Hardware test script: **`docs/MAP-HARDWARE-TEST.md`** — 7 short steps, each with what correct looks like and what wrong looks like. Step 1 (Emerald at the Battle Arena Lobby) is the only proof of P0 that exists.

## QA defects and what was done (pass 1, cycle 4)

| # | Defect | Status |
|---|---|---|
| 1 | **A dialog's dismissal keypress was consumed twice** — `s_wait()` returns on the frame the edge is seen and nothing re-polls, so the map loop saw the same press. Threw you out of the map view, dropped a carry, and could chain into an unintended save write. | **FIXED** — `s_flush_keys()` after every dialog; retested (B now cancels only the dialog, carry survives) |
| 2 | **Every dialog drew over confetti.** Worse than reported: `mgfx_exit()` goes to Mode 3, whose framebuffer IS the tileset char VRAM, so dialogs both drew over garbage AND destroyed the map's tiles. | **FIXED** — new `map_dialog()` never leaves Mode 0; draws with the HUD's tte context (CBB3/SBB31/bank 15, none of which a map load touches). No SD reload needed. |
| 3 | Rooms whose only exits are dynamic warps are inescapable (32 EM / 30 RU / 9 FR maps; FireRed's test save starts on one). | **FIXED by design** — offer the save's own `escapeWarp` (SB1+0x24), i.e. where Dig/Escape Rope would drop you. Read-only. |
| 4 | OBJ palette bank 15 not restored on map exit; `box_oam` re-uploads only 0..14. | **FIXED** — `moam_shutdown()` zeroes bank 15. |
| 5 | Drop dialog printed "A place / B cancel" twice. | **FIXED** |
| 6 | `"Written to flash + verified."` (27 ch) overflowed the 25-column panel. | **FIXED** |
| 7 | `g3warp_risky_map()` only covers Emerald groups 25-28 — Terra Cave and all of RS/FRLG get no warning. | **DEFERRED** — needs a per-game risky-map table; the unwalkable/odd-map warning still fires for bounds problems. |

**QA could not test:** P0 (fused path bypasses FatFs — hardware only), the `.gba` picker, real SD/OS-mode/rumble, Z2 *exactness* (only that it renders and misses nothing), NPC present-vs-flag correctness, and FireRed seam crossing (its save is trapped indoors — synthesise an outdoor save for pass 2).

## QA pass 2 (cycle 7) — 4 defects, all fixed + retested

Build under test crc32 CCD24B65. 191 screenshots, every assertion on decoded memory or save bytes.
**Passed:** region round trip byte-identical to a control (hard rule 0 independently re-confirmed),
all pass-1 fixes hold, dynamic-warp escape works on FireRed 0.1, drop writes EXACTLY the right bytes
(only SB1+0x0C..0x13, SB2+0x09 bit 0, and 2 sector checksums; `pos`/`location` untouched),
**NPC overlay 25/25 exact set-match against ROM + save flags**, FireRed/Ruby 22/22 with synthesised
outdoor saves (incl. a 1x1 map and a genuine one-way connection), memory 72/72 — peak 32,768 of
35,712 everywhere, `mt_misses` 0 in every sample, `g_pc` byte-identical after a map visit.

| # | Defect | Fix |
|---|---|---|
| 1 | **Player character vanished after any region-map visit** (and grab then drew a dot with no sweat). `moam_init()` clears the player slot; only `moam_load_map()` ran afterwards. | `sprites_rearm()` pairs init+set_player so the two can't be separated |
| 2 | **A successful drop left the screen as garbage.** `app_commit_sb12()` draws through the shared Mode-3 UI, and in Mode 3 that framebuffer IS the tileset + HUD-font VRAM — so it ran while still in Mode 0. The "SAVED"/"do not power off" panels were unreadable too, meaning a FAILED write would have been invisible. | `do_drop()` now `mgfx_exit()`s before committing and rebuilds the map after |
| 3 | **FRLG region map rendered noise and reported success** — `map_region.c` only implemented the RSE 8bpp affine layout; FRLG is a 4bpp TEXT BG with 2-byte entries. | **IMPLEMENTED** (cycle 8): separate text-BG path — tilemap expanded in EWRAM with SWI 0x11 then re-pitched 30-wide into a 32-wide screenblock as halfwords, 4bpp palette at its own base with bank 15 left to the HUD. **Kanto verified rendering in mGBA, and the return to the tile view is clean.** Anything that is neither layout is still refused. FRLG shows the SAME picture at levels 3 and 4 because the game itself has no zoomed region map. |
| 4 | A refused escape warp left the map invisible (only BG0 on). | unconditional `dialog_done()` when `map_switch` refuses |
| 5,6 | Cosmetic: placement reads as "didn't take"; a one-frame B drop in the region view. | wording now says "Map shows old spot"; `goto hud_done` instead of `continue` |

**Found while fixing:** `s_msg()` had the same unflushed-keypress bug as pass-1 defect 1 — dismissing
the "PLACED" panel silently re-grabbed the character. `s_flush_keys()` is now in BOTH dialog helpers.

**Still open:** pass-1 defect 7 (`g3warp_risky_map()` covers only Emerald groups 25-28);
`s_rgn_tried` is dead; FRLG's three Sevii region views are unreachable (`RGN_VIEW_MAIN` is
hard-coded); a possible cosmetic clip on the region-view HUD's bottom lines, unconfirmed —
worth a look in a future QA pass. **Untestable without hardware:** P0, real SD/OS-mode,
rumble, the `.gba` picker.

## Round-3 fixes (2026-08-01)

| item | status |
|---|---|
| 6 save fails on hardware | **FIXED** — EWRAM `.sbss` ran 868 B past 0x02040000; EWRAM mirrors every 256 KiB so the overhang landed on the EZ-Flash driver at 0x02000000 (`w_aligned_buff` over `_EZFO_readSectors`, `ROMPAGE_ROM` INSIDE `_EZFO_writeSectors`). Writes died, reads did not — hence "save bug". Fixed by sharing ONE bounce buffer + halving the save-copy buffers; **post-link Makefile guard** now fails the build on overflow. devkitARM's `AT>ewram` cannot catch this. |
| 7 Safari passages | **FIXED** — zoom-scaled steps made 3 of every 4 tiles unreachable at 1/4, so warp tiles were skipped. A single tap now always moves exactly 1 tile; only auto-repeat scales. |
| 2 FRLG region zoom | **FIXED** — FRLG genuinely has no zoomed region map (no `ProcessRegionMapInput_Zoomed` in pokefirered). `mr_region_tick` had no text-BG guard, so level 3 wrote affine-only registers that fail SILENTLY and applied *2 scaling to the cursor. Level 3 now behaves as 4 there and L/R steps straight past it. |
| 5 map corruption | **DETECTOR SHIPPED, root cause still open.** The "count unrendered tiles" idea CANNOT work: `mt_misses` is a bounds check on an ID, and over 802,299 real blockdata cells the observed corruption would have raised it **0 times**. Shipped instead: a CBB3 canary (44 cycles, catches any Mode-3 excursion — the Mode-3 framebuffer covers 69% of the metatile tables) plus stride fingerprints of the metatile tables / char data / arena / palette, one slice per frame (~0.5%). Tiered repair via the existing `map_switch`, rate-limited (30 frames apart, 4/map, 12/session), permanent `!N` HUD marker, and **hold SELECT on entry for detect-only** so a corrupt frame can be photographed. |

⚠️ The integrity check runs in the TILE VIEW ONLY. The region view legitimately owns CBB2, the
affine map, the palette and the arena, so checking there false-positives (it did, first run).

## Round-4: Guy's list of 2026-08-01 (items 1,3,4 + secret bases + Sevii)

All verified in **mGBA against Guy's own five ROMs/saves**. Nothing here is hardware-tested yet;
the flash-write half of "put it back" and every SD path still need the cartridge.

| item | status |
|---|---|
| 1 item balls: grey when collected, identify, reactivate | **DONE.** A collected ball is no longer hidden — `map_oam` keeps it and draws the ROM's own ball sprite recoloured onto a 3-step grey ramp (`gen_ghost`, 4 tiles at `TID_GHOST`, no extra palette bank). HUD names the item via `rom_script_item_ball` and offers `START put it back`; START clears the ball's flag and commits SB1 only. Verified on Route 119 with `SUPER REPEL`: grey → confirm → `RESTORED` → the ball is red/white again. |
| 3 real player face on the region map | **DONE.** The magenta pin is replaced by the player's own overworld sprite, cut out of the tiles `map_oam` already loaded (`gen_player_face`). Guy asked for the shoulders gone — `RGN_FACE_SHIFT 6` takes rows 6..21 instead of 8..23, which is why it is a re-blit and not simply `tid+2` (OBJ tile ids are 8-px granular). Falls back to `tid+2`, then to the pin. |
| 4 NPC dialog on selection | **DONE.** `rom_script.c` (walker + Gen-3 text decoder, vendored from the round-3 research) resolves the first dialog line straight out of the ROM's bytecode. `map_text_page` pages it in Mode 0. Labelled **SAYS** when no conditional was passed and **MAY SAY** otherwise — it is a walker, not an interpreter, and must never pretend otherwise. MEASURED resolution: Emerald 99.5%, Ruby/Sapphire 99.6%, FRLG 99.7%. |
| secret bases: see the open path, enter, see the interior | **entrances + enter/exit DONE.** Entrances come from BG events of kind 8 (`rom_bg_event`), never from a metatile-behaviour scan — those ids mean food/posters/windows in FRLG and a behaviour scan finds 596 false hits there against 0 real entrances. An OCCUPIED base gets `sbmap_open_metatile` patched into `gfx.blocks[]`, which a pixel-diff against a de-registered control proved changes **exactly one 16x16 tile**. HUD names the owner (`GUYA`). START enters, B returns to the door via a one-slot remembered position — interiors only have MAP_DYNAMIC exit warps, so there is nothing to follow. **Interior decorations are NOT yet composited** — see below. |
| FRLG Sevii switching | Working — `L switch map` on the region view. |

### Bugs found while testing round 4

| bug | fix |
|---|---|
| HUD text invisible over pale terrain | The HUD is white text on transparent paper, and tte's SE renderer snaps glyphs to 8x8 cells so a drop shadow is not expressible. WIN0/WIN1 now cover the two HUD strips and brightness-decrease the map under them (`MGFX_HUD_WIN`, `MGFX_HUD_BLDCNT`). **Every map-screen `REG_DISPCNT` assignment must OR in `MGFX_HUD_WIN`** — `mgfx_z0_show` was missed first time and the whole screen darkened, because the blend was live with no window to confine it. |
| the dust puff zeroed `REG_BLDCNT` | BLDCNT holds one mode; the puff borrows it for its alpha fade. It now restores `MGFX_HUD_BLDCNT` instead of 0, or the HUD stayed unreadable for the rest of the session after the first drop. |
| region marker parked at Hoenn's top-left | `mr_region_cell` fell back to the rect table, whose **(0,0,1,1) is a PLACEHOLDER** for sections that are drawn nowhere (secret bases, link rooms, FRLG dungeons). It is now rejected when the grid search already failed. |
| region marker drawn on the wrong view | FRLG's L switches Kanto <-> Sevii and the grid coords do not carry over — Red's face appeared on Sevii while he stood in Lavender Town. `mr_region_cell` now reports which view it found the section in, and the marker only draws on that view. |
| **the greyed-ball tiles were optimised away** | The first `gen_ghost` built the tiles in a local buffer and `dma3_cpy`'d it out. tonc's `dma3_cpy` is INLINE and only stores the source ADDRESS into a volatile register, so GCC sees a local that is written and never read and deletes the whole remap loop at -O2. Symptom: a ball drawn from whatever junk the previous screen left, appearing and disappearing with unrelated edits. **Now writes straight to VRAM with volatile word stores.** `upload_frame` gets away with the same shape only because `rom_read_at` is a real call. Worth checking anywhere else this pattern appears. |

### Five-game sweep (2026-08-01)

All five load OK, `mt-miss 0`, no integrity repairs, clean round trip tile -> Z1 -> Z2 -> RGN -> RGN+ -> tile:
Emerald 25.6 (a friend's secret base) · Ruby 4.6 Lavaridge · Sapphire 15.2 Sootopolis ·
FireRed 0.1 · LeafGreen 3.4 Lavender.

## Round-5: secret-base interiors + the head sprite (2026-08-01)

### Secret-base interiors — DONE

A group-25 map in the ROM is a bare TEMPLATE. Everything that makes it someone's base lives
in that owner's 160-byte save record, and the game composites it in at load. Three separate
things were wrong, all now fixed:

1. **Decorations were not drawn at all.** `source/gen3_sbdecor.c` now does what
   `InitSecretBaseAppearance` does: per record slot, `id = rec[0x12+i]`, `pos = rec[0x22+i]`,
   look up `gDecorations[id]` for permission (byte 17), shape (byte 18) and a tiles pointer
   (u32 at 28), and write `rel + (metatiles_primary | overlapsWall)` into each covered cell.
   The position is the BOTTOM-left cell and the footprint grows upward.
   **Host regression** (`<scratchpad>/sbdecor/hostcmp.c`): metatile ids are **byte-identical**
   to the verified reference renderer across all six real bases in Guy's Emerald and Ruby
   saves — 0 differing ids.
2. **Collision was stale.** The reference preserved the template's collision bits; the game
   recomputes them from the decoration's own metatile (`behaviour == 0xB9`, or a non-normal
   layer type on anything but `DECORPERM_PASS_FLOOR`). Keeping the template's was wrong on
   **70.7% of decorated cells, always calling a blocked tile walkable** — which is precisely
   the direction that would mislead a placement. Elevation is also rewritten for the only two
   decorations the game places with `MapGridSetMetatileEntryAt` (`DECOR_SLIDE` 34,
   `DECOR_STAND` 38).
3. **The dolls were fake.** Every interior carries 15 DUMMY object-event templates: localId 1
   is the owner, localIds 2..15 are 14 decoration slots with graphics 240..253
   (`OBJ_EVENT_GFX_VAR_0..VAR_D`) parked at meaningless coordinates (0,0)..(1,6). The map
   script hides all 14 and the game re-spawns only what the save holds. PokeDNA was drawing
   the templates, i.e. **up to fourteen wrong dolls in a column against the left wall**.
   `moam_drop_gfx_range(240,253)` removes them and `sbdecor_sprites()` re-adds the owner's,
   subject to the game's own gate: the cell must already hold a decoration-holder behaviour
   (0xB5/0xC3), which only exists because a metatile decoration painted it — so
   `sbdecor_apply` MUST run first.

**The register PC.** `InitSecretBaseAppearance` does one more thing after the decoration loop:
in ANOTHER player's base it swaps the single PC cell (metatile 0x220) for the REGISTER PC
(0x221) and marks it impassable. Slot 0 is always the player's own base, so this fires for
every record-mixed one — four of the six real bases in Guy's saves, each a visible one-tile
difference. Caught by the adversarial verifier, not by the original research, and not
implemented by the reference renderer either: the "0 differing metatile ids" result only holds
when the comparison is scoped to the decoration loop.

**Which record.** The game keeps the answer in `VAR_CURRENT_SECRET_BASE` (0x4054), which
holds the record INDEX 0..19 — Emerald `SB1+0x1444`, RS `SB1+0x13E8`, plaintext. It is
**never cleared** (Guy's Ruby save is on map 4.6 and still reads a stale 19), so it is only
trusted when `location.mapGroup == 25`. Verified end to end: Guy's Emerald save resolves to
index 1 = base 12 = "GUY"'s RedCave2 = map 25.6, agreeing with `pos`, `mapLayoutId`,
`VAR_SECRET_BASE_IS_NOT_LOCAL` and the entrance table. Fallback is a unique interior match,
and an ambiguous match draws NOTHING — `secretBaseId/10` collapses 240 ids onto 24 rooms, so
with a full registry roughly half the records are ambiguous and a stranger's furniture is
worse than none.

**Entry position.** Entering a base now lands on the interior's **warp event 0**, which is
what `SetSecretBaseWarpDestination` uses for all 24 groups. Centring instead could be seven
rows from the door (25.6 is 7x16). The coordinate cannot be derived from the dimensions —
it is usually (w/2, h-2) but six of the 24 break that.

**Finding `gDecorations` without scanning 16 MB off the SD card.** Emerald and FRLG carry
Game Freak's own index block `sGFRomHeader` at the FIXED link address `0x08000100`, whose
field at +0x4C points at `gDecorations` — one 4-byte read, revision-proof. Ruby/Sapphire
predate the block and are pinned per revision. Every candidate is validated by a 64-byte
signature over entries 0 and 1, whose strong part is a game-independent invariant: Game Freak
gave `DECOR_NONE` the same name, description pointer and tiles pointer as `DECOR_SMALL_DESK`,
differing only in id and price. MEASURED unique in all five ROMs at every byte offset, and it
rejects the other ten games' pins and every real inter-revision displacement.
**Do NOT derive these from region_map.c's anchors** — the rev0→rev1 delta matches for Ruby,
Sapphire and LeafGreen but is 0x60 rather than 0x70 for FireRed. There is deliberately no
scan fallback: the bare template is always available and is better than a wrong table.

⚠️ `scan_secret_bases` and `apply_base_decor` both rewrite blockdata that `mgfx_load` has
already fingerprinted, so **`mgfx_fp_capture` must be re-run after them**. Without it the
integrity checker reported the map as corrupting itself, repaired it (re-applying the same
patches), and gave up with "DISPLAY UNSTABLE".

### The region-map head sprite

Rows 8..23 of the overworld frame, with the two outermost non-transparent pixels trimmed off
each end of the bottom row — the tops of the shoulders either side of the neck. The trim
scans inward from each end rather than using fixed columns, because the four playable
characters do not put their shoulders in the same place. Guy signed this off on a photo.

### FRLG Safari Zone — re-confirmed 2026-08-01

Both games, driven end to end. The zone is four areas (1.63 Center, 1.64 East, 1.65 North,
1.66 West) plus five rest houses (1.67-1.71); every passage is a group of THREE adjacent warp
tiles, and every arrival tile is itself a warp back. LeafGreen: 3 taps up from (26,8) lands
exactly on (26,5), HUD reads `START enter`, START -> 1.65 at (31,34). FireRed: 2 taps right
from (41,16) lands on (43,16), START -> 1.64 at (8,27), START again -> back to 1.63 (43,16).
The round trip works in both directions in both games — the round-3 "one tap = one tile at
every zoom" fix is what makes the passage tiles reachable.

### Five-game sweep after round 5

All five load OK, `mt-miss 0`, no repairs, clean round trip through the region map and back.
Emerald reports `sbdecor: base 12 (var) on 25.6 -> ok placed 12 sprite 4/4 bad 0`.

### Known-remaining secret-base defect (found by the adversarial verifier, NOT yet fixed)

The interiors' **owner** placeholder is still wrong. localId 1 of every group-25 map has
graphics id 255 (`OBJ_EVENT_GFX_VAR_F`) and flag 0xAD, and the game fills that var via
`SetSecretBaseOwnerGfxId` from the record's gender bit (`rec[0x01]` bit 4). PokeDNA instead
reads whatever `VAR_OBJ_GFX_ID_F` happens to hold in the save. MEASURED on Guy's Ruby save:
flag 0xAD is CLEAR and the var reads 20, so **every** group-25 interior draws a stray
`OBJ_EVENT_GFX_WOMAN_2` at the owner tile. On Emerald it happens to look plausible.
Fix: resolve the owner sprite from the record's gender bit rather than the var, and hide it
for an unoccupied base. Small, self-contained, needs the two gender->graphics-id constants.

## Round-6 (2026-08-03): Z2 quality, Dive, record sidecar

### Z2 dictionary now ranks by screen coverage (Guy's design)

At 1/4 the affine layer holds 1-byte tilemap entries, so only **242** metatiles can be exact
while a window can need **442** (MEASURED worst case: Emerald map 26.4, the Battle Frontier;
Ruby 304 at 0.5; FireRed 309 at 3.10). WHICH 242 was the whole problem, and first-come was the
worst possible answer twice over: `slot_of` is persistent and only grows, so after panning the
budget is spent on tiles that have scrolled off screen — which is exactly why leaving and
re-entering "fixed" it — and even fresh, first-come means scan order, so the top of the screen
is exact and the bottom is a slab of flat colour.

`mgfx_zoom_optimise()` re-ranks by how much of the window each metatile actually covers,
centre-weighted 2:1 (a wrong tile at the edge matters less than one under the cursor). The
histogram lives **inside `slot_of`** — 1,024 counts written over the 1,024 slot bytes then
converted back in place, ascending, each entry read once before being overwritten — so it costs
**zero extra EWRAM**. A count-of-counts picks the cut in one pass over 256 bins.

Trigger is Guy's: run only when **more than 1%** of the window came out as filler, and only when
the dictionary has not already been optimised for this window (`opt_valid` + `opt_mx/opt_my`), so
a genuinely irreducible view settles after ONE pass instead of thrashing. It fires when the D-pad
is **released** — the re-rank costs up to 242 metatile renders, which is invisible on a static
frame and unaffordable per step.

MEASURED across the hardest windows in Emerald and FireRed, every one improved and none regressed:

| map | distinct | filler before | after |
|---|---|---|---|
| Emerald 26.4 Battle Frontier | 442 | 14% | **7%** |
| Emerald 26.14 Battle Frontier | 434 | 15% | **7%** |
| Emerald 0.1 Slateport | 317 | 8% | **3%** |
| Emerald 0.5 Lilycove | 304 | 4% | **2%** |
| FireRed 3.10 Saffron | 309 | 11% | **2%** |
| FireRed 3.6 Celadon | 288 | 3% | **2%** |

Easy maps (Underwater, Route 130, Diglett's Cave — around 30 distinct) sit at 0% and the
optimiser never fires on them. The HUD shows the number (`1/4~ 7%`), so a photo is
self-describing.

THREE bugs found while building it, the last one only visible because the gallery covered maps
beyond the one it was developed against:
- The first threshold walk took only bands that fit WHOLLY, leaving **39 of 242 slots unspent**
  because counts tie heavily in the tail.
- **The straddling band then swallowed everything.** With 434 distinct metatiles averaging 6
  cells each, counting down from 254 does not reach 242 until `thresh == 1`, so `c >= thresh`
  matched every tile and the assignment loop took the first 242 **by metatile id** — a tile
  covering 500 cells losing its slot to one covering a single cell because its id was higher.
  MEASURED 15% -> 27%, i.e. worse than the lazy dictionary it replaced. Fixed by splitting the
  band: everything strictly above the threshold is in (that set is smaller than the budget by
  construction), and the equal-count band fills the remainder against a quota, both computed
  from `cc[]` before `slot_of` is touched — so still no second buffer.
- **The centre weighting was a red herring and is gone.** It was removed on the suspicion that
  it caused the regression; it did not (the band bug did). It is left out because ranking on RAW
  count is by construction the maximum area 242 slots can cover, so it can never lose to
  first-come, whereas any weight on the counts optimises a different quantity than the % the
  user sees. Centre distance belongs as a tie-break WITHIN the equal-count band — which is now
  a natural place to add it — never as a multiplier.
- `mgfx_zoom_optimise` only rewrites the DICTIONARY; the caller must `mgfx_zoom_build` before
  `mgfx_zoom_show` or the old tilemap stays on screen pointing at slots that now mean something
  else. (Also why the first measurement read "15% -> 15%".)
- Stale comment fixed: `MGFX_EXACT_MAX` is **242** and the flat band **243..255**, not 239/240.

### Dive and surface on START

RSE records dive/emerge as a **connection** with direction 5 / 6 — not a warp — and the
destination keeps the SAME coordinates, which is what makes the two maps feel like one place at
two depths. `dive_conn()` scans the header's connections; START switches maps and the HUD says
`START dive` / `START surface`. Offered anywhere on the map rather than only on dive-capable
tiles, consistent with this viewer already allowing unwalkable ground.
MEASURED: Emerald 7 dive + 7 emerge pairs, Ruby 4 + 4, **FireRed/LeafGreen 0** (correct — no Dive
in FRLG). Verified round trip Route 126 (0.41) -> Underwater 126 (0.51) -> back, all at (40,40).

### Battle-record sidecar carries the save state

`g3_record_sidecar()` gained a machine-readable `state.*` block so a rendered video can open on
who the trainer actually is. None of it is in sector 31, so it is read at export time or lost.
Keys: `playtime`, `dex_seen`, `dex_caught`, `bp`, `bp_card`, `symbols` (7 chars, one per facility
in Frontier Pass order, `-`/`s`/`G`), `symbols_silver`, `symbols_gold`. The numeric block needs
SaveBlock2; the symbol lines are **Emerald-only** and are omitted rather than zeroed elsewhere.
Signature gained `sb1` + `game`.
Consumer contract: **`projects/rec2mp4/docs/REC-SIDECAR.md`** (rec2mp4 does not read it yet).
Covered by `tests/host_streak_test.c` — every key present, symbol field exactly 7 of `-`/`s`/`G`,
`dex_seen >= dex_caught`. Against Guy's real save: 116h, 221 seen / 162 caught, BP 18, no symbols.

## Round-7 IN PROGRESS (2026-08-03): off-map places + Mirage Island

### Mirage Island — pure-C core DONE and host-tested, UI NOT wired

`source/gen3_mirage.c/.h` + `tests/host_mirage_test.c`. Full mechanism in
`gba-toolkit/docs/kb/pokemon/mirage-island.md`. Headlines:
- There is **no flag**. Presence is recomputed live from `VAR_MIRAGE_RND_H` vs the personality
  low half of any of the SIX party slots (eggs included).
- **Writing the target directly does NOT work.** The save owes a day-rollover catch-up that runs
  BEFORE the map script on load and rewrites the value. MEASURED owed: Emerald 5, Ruby 68,
  Sapphire 9409. Both adversarial verifiers caught this; the first research draft had it wrong.
- The correct write is the **LCG pre-image** (`Minv = 4005161829`), so the game's own catch-up
  turns it into the target. `N` must come from the live cart RTC and the boundary is the save's
  `localTimeOffset` time-of-day, NOT midnight.
- Lasts one in-game day; two at best; three is impossible (the LCG has no fixed point).
- Host test passes on all three real saves and proves solve->advance round-trips for every donor
  across 0..80 days. Party keys independently reproduce the research's measured values
  (Emerald 82EF, Ruby 9CA2, Sapphire 8009).

**Remaining:** the UI. Suggested shape — on the region map or a Route 130 context action, list the
six party donors, show days owed and what the value is now, write the pre-image via
`app_commit_sb1()`, and state plainly that it lasts until the next in-game day. Offer the
`VAR_DAYS` freeze only as an explicit, separately-worded choice.

### Off-map places (Sky Pillar, boat-only islands, FRLG) — RESEARCHED, NOT IMPLEMENTED

Workflow `wf_5be90300-754` (5/6 agents reported; read its journal.jsonl). Key measured facts:
- **THREE classes, not two.** Beyond "on-map" and "placeholder" there is a **rect-only** class
  whose rect points at a cell *owned by another section* — so a marker can be drawn but the
  cursor can never select it and the HUD shows the HOST section's name.
- **SKY PILLAR — definitive.** mapsec **85**, 8 maps at **24.77-24.85** (24.83 is NOT one of them,
  it is Shoal Cave), rect **(19,10,1,1)** but `rgn_find_mapsec` FAILS because grid cell (19,10)
  belongs to **mapsec 46 = ROUTE 131**. Three independent reasons Guy cannot see it, all measured:
  (1) **no tower is drawn** — cells (18,10)/(19,10)/(20,10) are byte-identical to each other and
  to Route 130's, plain sea tint; (2) no selectable cell; (3) **retail renames it on purpose** —
  `sRegionMap_SpecialPlaceLocations[]` maps SKY_PILLAR -> ROUTE_131, so the Pokenav map never
  says "SKY PILLAR". The same table also remaps Jagged Pass, Mt Pyre, Petalburg Woods, Trainer
  Hill, Altering Cave, Artisan Cave, Mirage Tower, Desert Underpass, Abandoned Ship, both hideouts
  and every Underwater route.
  Reachability is a plain warp, NOT a dive chain: Route 131 (0.46) warp 0 at **(36,6)** -> 24.77.
  **Rayquaza is at 24.85 (Sky Pillar Top): (14,7) gfx RAYQUAZA flag 0x305, and (14,6)
  RAYQUAZA_STILL flag 0x050** in Emerald; Ruby has one object at (14,6) flag 0x305.
  There is NO story-flag gate on the region map (`FLAG_LANDMARK_SKY_PILLAR` is read only by the
  Pokedex area screen).
- **Ruby/Sapphire have ZERO reachable class-A places** — their only class-A ids are INSIDE OF
  TRUCK, SECRET BASE and DYNAMIC, all plumbing.
- FRLG additionally has a **dungeon-plane** class (`layer==1`): drawn on the picture, but
  `mr_region_cursor_mapsec()` only queries LAYER_MAP, so the cursor never names those either.
- Emerald has **45** mapsecs in use with no reachable cell; LeafGreen 23.
- Navel Rock (211) is a true PLACEHOLDER — nowhere at all. Southern/Birth/Faraway Island and
  Artisan Cave have real map data but no selectable cell.
- **FRLG's islands are ALREADY ON THE REGION MAP — ask (b) needs no new code there.** VERIFIED
  independently: Navel Rock is mapsec **174**, view **2 (SEVII 4-5)**, cell **(10,8)**, 22 maps
  from 2.0; Birth Island is mapsec **187**, view **3 (SEVII 6-7)**, cell **(18,13)**, 2 maps from
  2.56. Both `rgn_find_mapsec` hits on layer 0. Guy simply has to be on the right view — so the
  work is DISCOVERABILITY (label the L key, and auto-switch view when jumping to a place), not a
  new list. An earlier note in this doc claimed they carry the generic `SPECIAL AREA` (196)
  mapsec; that was WRONG — 196 is a different set of 5 maps.
  Note retail HIDES both behind `FLAG_WORLD_MAP_NAVEL_ROCK_EXTERIOR` (0x8B5) and
  `FLAG_WORLD_MAP_BIRTH_ISLAND_EXTERIOR` (0x8C2); PokeDNA reads static ROM so it is MORE
  revealing than retail. Fine, but those are the flags if fidelity is ever wanted.
- LeafGreen already has 4 views (KANTO, SEVII 1-3, 4-5, 6-7); **RSE has exactly 1** — there is
  only one Hoenn tilemap in the ROM, so any RSE "islands view" must be PokeDNA's own synthetic
  list, never ROM data.
- There is **no ROM artwork** containing these places, so a third "view" cannot be the game's own
  picture. The honest design is a LIST, not a map.

**Remaining:** implement the list on L from the region view, jumping into the tile view at each
place's entry warp. Must respect hard rule 0 (leaving the region view destroys the arena, so the
jump has to go through the same reload path `map_switch` already uses).

## Bag item names vs the game (Guy 2026-08-03) — DIAGNOSED, NOT FIXED

MEASURED against `source/data_tables.c`'s `s_item[377]`:
- **Berries are already right** — `CHERI BERRY`, `CHESTO BERRY`, ... exactly as the game shows.
- **TMs and HMs are bare**: items 289..338 read `TM01`..`TM50` and 339..346 read `HM01`..`HM08`
  with no move. That IS the item's own name in every Gen-3 ROM — the game appends the move at
  DISPLAY time (FireRed's TM Case prints `TM01 FOCUS PUNCH`), so a bare `TM26` tells the player
  nothing, which is Guy's complaint.

Fix shape: a 58-entry TM/HM -> move-id table, rendered as `"%s %s"` with `pk_move_name()`
(which already exists, `data_tables.c:1432`). `data_tables.c` has NO stdio include and is
compiled into the host tests, so build the string with a hand-rolled concat, not siprintf.

⚠️ **Do not write that table from memory.** A first attempt did and was wrong — including two
entries (`059`, `063`) that are invalid/misleading C octal literals. A wrong mapping mislabels
items, which is worse than showing nothing. Generate it in `tools/gen_data.py` from the ROM or
the decomp and check it against all five of Guy's dumps, the same way the other tables are made.

Still to check (Guy: "check the rest too"): every other pocket against the ROM's own item table.
The right test is a host probe that walks `gItems` in each ROM and diffs all 377 names against
`s_item[]`.

## Visual defect sweep (2026-08-03)

Seven defects CONFIRMED by independent reproduction, none fixed yet — full table with file:line
in **`docs/VISUAL-DEFECTS.md`**. Includes Guy's own bank-cursor report (D2), proven by a pixel
diff showing 0 changed pixels in the banner band between two different focus states.

## Still open

1. **Item 5, the corruption that heals on reload.** A 13-agent workflow ran on 2026-08-01; its full
   plan is in `docs/MAP-CORRUPTION-PLAN.md`. Outcome so far:

   **FOUND AND FIXED — a confirmed miscompile, the #1 suspect (~60%).** `mgfx_load`'s blank-tile
   `memset` AND `build_flat_tiles`' `tile[i] = w` loop were BOTH deleted by GCC, for the same
   reason as the greyed-ball bug: the buffer's only consumer was tonc's INLINE `dma3_cpy`, which
   stores only the source ADDRESS into a volatile register. Verified in the shipped binary at
   `0x08009176`: 13 DMAs all sourcing `sp+208` with nothing ever written there. So every map load
   blitted 64 bytes of uninitialised stack into CBB2 slot 0 and all 13 Z2 flat tiles. Both now use
   `memset32` straight to VRAM with no staging buffer (a real out-of-line call — nothing for
   dead-store elimination to remove); re-verified at instruction level.
   Why it was hardware-only: the *defect* is identical in both builds, the *bytes* are not. On
   hardware `sp+208` was last written by `f_read`/`_EZFO_readSectors`; in the fused build by a
   `memcpy` over WRAM that mGBA zero-fills at reset — index 0 = transparent = looks correct.
   It also predicts P2/P3/P5 exactly (the mip dictionary only grows and is cleared on reload; only
   a window needing >242 distinct metatiles ever reaches the flat band; Route 111 needs 428).
   **Its one limitation: it CANNOT corrupt at 1:1** — Mode 0 never reads CBB2. If Guy has a corrupt
   photo labelled `1:1`, something else is also wrong.

   **STILL OPEN — #2, silent SD reads (~25%).** `Wait_SD_Response` is a busy-check only, `times=2`
   sits outside the chunk loop, and an exhausted timeout FALLS THROUGH to `dmaCopy` and returns
   success (io_ezfo.c:62-96). Never measured. The discriminating test is in the plan doc: hash the
   four loaded regions per load and compare two loads of the same map.

   ⚠️ **Two instrumentation defects that made us blind, both still unfixed:**
   - `mgfx_fp_capture` is the LAST statement of `mgfx_load` (map_gfx.c:531), so the integrity
     baseline is taken over whatever came off the card. Any LOAD-TIME fault is adopted as correct
     and `!N` can never fire for it.
   - `fp_slice` covers no part of CBB2 — a 16 KB blind spot, exactly where the miscompile landed.
   - `s_mt_misses` is never reset and is logged only on `map_view`'s first load, never after a
     `map_switch`. **"mt_misses == 0" was probably never sampled during a corrupt frame — do not
     lean on it as evidence.**
2. **Nothing is committed.** The whole of rounds 3, 4 and 5 is uncommitted working tree.
3. **No hardware validation yet.** Everything in rounds 4 and 5 is emulator-only. The flash write
   behind "put it back", every SD read path, and the FatFs bounce fix are hardware-only.
4. Six of the eleven `gDecorations` pins (Ruby r0/r1, Sapphire r0/r2, FireRed r0, LeafGreen r1)
   come from pret .sym files rather than a dump Guy owns. They agreed with all five measured ones.
   A wrong pin fails the signature and degrades to the bare template, never to garbage.
5. Non-US ROMs are not covered by the pins. Emerald/FRLG still resolve via `sGFRomHeader`; a
   non-US Ruby/Sapphire would draw the bare template.

## Hard constraints that must not be broken

0. **The region map DESTROYS the arena.** `mr_region_enter()` stages its compressed stream through
   the borrowed arena — which is where the map's blockdata, connection strips and warp list live. So
   leaving the region view must RELOAD the map (`map_switch` on the same map), not merely redraw it.
   Found by hashing VRAM: the tilesets were intact but the Z0 tilemaps had been rebuilt from
   overwritten blockdata, i.e. a scrambled map. Re-entering also re-uploads, because going back down
   rebuilds the mip dictionary over the region data's VRAM.
1. **OS-mode**: no rendering or ROM-resident code during an SD transfer; no `rmbl_*` between pause/resume.
2. **The arena aliases `g_pc`** — an overrun corrupts the user's Pokémon. `bump()` refuses rather than overflows; never soften that.
3. **Pure-C cores** stay free of tonc/GBA headers so they dual-compile in `tests/`.
4. **PokeDNA ships no Nintendo data.** Everything is read transiently from the user's own ROM.
5. A fused ROM contains a commercial game — **never commit, publish, or transmit it**.

## Infrastructure notes

- **mGBA 0.10.5** at `/opt/homebrew/bin/mgba`; Python core bindings vendored at
  `projects/rec2mp4/vendor/libmgba-py.zip` (see `rec2mp4/driver.py` for a working consumer).
- Testing the map in an emulator needs the **fused ROM** path: an emulated GBA has no microSD, so
  the map reads the Pokémon ROM out of cartridge address space instead. `tools/fuse_rom.py` appends
  the ROM and patches a `"PDNAFUSE"` + u32 offset + u32 size locator record.
- **The fused path bypasses FatFs entirely, so it can NOT test P0.** Emerald will render correctly
  in mGBA whether or not P0 is fixed. P0 is hardware-only.
- **TEST CORPUS: `gba-toolkit/roms/`** (gitignored) — all FIVE Gen-3 games (Guy's own cartridge dumps) with
  uniform names `Emerald|Ruby|Sapphire|FireRed|LeafGreen` `.gba`/`.sav`, plus `roms.sh` exporting
  `$PKROMS` / `$PK_GAMES`. In the toolkit at Guy's request, covered by a `roms/` .gitignore entry (verified with
  git check-ignore + git ls-files).
  All five saves verify 14/14 sections. Sapphire and LeafGreen were never tested before and
  can now verify `region_map.c` anchors that were pret .sym guesses.
  Emerald's save has the player in a **SECRET BASE** (map 25.6, type 9) — a save-driven map
  type, like the Battle Frontier rooms.
- Full engineering plan with code: `<scratchpad>/PLAN.md` (55 KB, from the 10-agent workflow).

## Cycle log

- **Cycle 8 (2026-07-31, ~05:30)** — **FRLG region map IMPLEMENTED** rather than left failing closed,
  so all three games now have the full 5-level ladder. Kanto renders and the round trip back to the
  tile view is clean. **The feature is complete**; the loop is stopped. Everything that remains is
  either hardware-only (P0, SD, rumble, the ROM picker) or a documented nicety (Sevii views, risky-map
  table, the HUD clip). Deliverables: `docs/MAP-HARDWARE-TEST.md` for Guy, this doc for the next chat. — QA pass 2 landed; all 4 defects fixed and re-verified in mGBA
  (player survives a region round trip and grabs properly; a drop now leaves a fully rebuilt map with
  legible panels and the carry cleared). Caught a regression of my own in the process: `s_msg` never
  flushed keys, so dismissing "PLACED" re-grabbed the character — both dialog helpers flush now.
  FRLG region map made to fail closed rather than render noise. **FEATURE COMPLETE** apart from that
  known gap. Hardware script at `docs/MAP-HARDWARE-TEST.md`.

- **Cycle 6 (2026-07-31, ~04:30)** — **Drop-commit VERIFIED at byte level** against a save COPY:
  after dropping at (36,10) on map 0.33, `continueGameWarp` reads group=0 num=33 warpId=-1
  pos=(36,10) and SB2+0x09 has the CONTINUE_GAME_WARP bit set. Wrote `docs/MAP-HARDWARE-TEST.md`
  (7 steps, correct-vs-wrong for each). QA pass 2 launched against the complete build, focused on
  the region map, the pass-1 fix re-verification, the dynamic-warp escape fallback, NPC
  present-vs-flag correctness, and FireRed/Ruby with synthesised outdoor saves.

- **Cycle 5 (2026-07-31, ~04:00)** — **ALL FIVE ZOOM LEVELS DONE.** `map_region.{c,h}` wires the
  region map in as levels 3/4; Hoenn renders at both scales from the ROM. Found and fixed a nasty
  one on the way back down: the region upload stages through the ARENA, so the map's blockdata /
  strips / warps were being overwritten and the tile view came back scrambled. Diagnosed by MD5-ing
  each VRAM block against a control run — tilesets identical, tilemaps different — which pointed
  straight at the caches rather than at VRAM. Fix: reload the map on the way down. Re-verified:
  SBB29/SBB30 now byte-identical to the control. Also fixed a stale HUD on the zoom-change frame.
  REMAINING: drop-commit test on a save COPY, QA pass 2, HW script + summary.

- **Cycle 4 (2026-07-31, ~03:30)** — QA pass 1 landed (see the defect table above). Fixed 6 of 7,
  retested the three HIGH ones in mGBA. The important one was NOT the one reported: dialogs were
  round-tripping through Mode 3 and **destroying the tileset VRAM**, which would have shown up as a
  corrupt map after any dialog. `map_dialog()` now stays in Mode 0 entirely.
  REMAINING: Z3/Z4 region map, QA pass 2 (needs a synthesised FireRed outdoor save), HW script.

- **Cycle 3 (2026-07-31, ~03:00)** — `map_oam.c` landed; **TREE LINKS AGAIN**. Full feature now runs
  in mGBA and was checked visually at every step: NPCs render with real ROM sprites, the NPC detail
  panel matches ROM ground truth, grab + carry works (character rides the cursor while NPCs stay
  anchored), Z0/Z1/Z2 all clean. Fixed a Z2 artifact (flat fallback tiles keyed on palette banks
  13-15 = UI colours, clamped to 0..12). Added the NPC/tile cursor panel and a context-sensitive
  footer. Wrote `<scratchpad>/run/npcs.c` — dumps a map's real object/warp coordinates from the ROM,
  which is how to drive the cursor onto a known NPC instead of hunting visually.
  **Gotcha for future cycles:** re-Reading the SAME screenshot path can return a stale decode —
  write each test's screenshots to a FRESH path or you will "verify" the previous build.
  REMAINING: Z3/Z4 region-map levels, drop-commit test on a save COPY, QA pass 2, HW script.

- **Cycle 2 (2026-07-31, ~02:30)** — Grab & drop LOGIC done (`do_drop`: bounds check, unwalkable +
  risky-map warnings, confirm dialog, one-shot undo snapshot, `g3warp_apply` + `app_commit_sb12`),
  A/B wired, `sprites_follow()` keeps OBJ in step with the BG camera. **`tests/host_conn_test.c`:
  0 failures / 4,822 checks across all three ROMs** — every cardinal connection's seam is exactly
  adjacent, every round trip is the identity, every resolved warp lands in bounds. FireRed has 2
  genuinely one-way connections (never chain seams into a global atlas). Z2 implemented.
  `region_map.*` and `npc_gfx.*` landed from parallel agents. QA agent running against the
  pre-sprite build. ⚠️ TREE DOES NOT LINK until `source/map_oam.c` lands — pdna_map.c already calls
  `moam_*`. Both map_gfx.c and pdna_map.c compile clean.

- **Cycle 1 (2026-07-31, ~02:00)** — **Fused-ROM emulator path built and PROVEN**: `source/fused_rom.{c,h}`
  + `tools/fuse_rom.py`; the map menu entry is no longer compiled out of the delta build. mGBA now
  boots a 27.67 MiB fused image, reads Emerald out of cartridge space, and renders the map.
  **Z1 zoom verified visually clean** (the P1 fix) at rest and while panning. P3/P4/P5/P6 implemented
  and building: `MapViewState`, `mgfx_can_load`, `loading_frame`, `map_switch`, `build_connections`
  + strips, `build_warps`, `rom_conn_origin`/`rom_conn_contains`/`rom_warp_dest` (pure C), and Guy's
  control scheme. **Verified in mGBA: walked west off Route 117 and Verdanturf Town loaded in place,
  map name in the HUD, zoom clean on the new map.** Also fixed: the Makefile never recursed on a
  plain `make` (`$(BUILD)` was not PHONY), so every build was a full clean rebuild.
  NEXT: grab+drop with the distress/sweat carry animation, NPC overlay, Z2, region map Z3/Z4, QA.

- **Cycle 0 (2026-07-31, ~01:00)** — P0/P1/P1b/P2 landed and building; instruction-level verified;
  host tests green on Emerald. Three background agents launched: mGBA harness + `fuse_rom.py`,
  `region_map.*`, `npc_gfx.*`. Starting P3.


---

# COLD RESUME (written 2026-08-01, session near its usage limit)

## State of the tree
NOTHING IS COMMITTED. Rounds 3, 4 and 5 are all uncommitted working tree. `git status` shows
modified `Makefile`, `lib/fatfs/diskio*.c`, several `source/*.c`, and untracked
`source/gen3_sbdecor.*`, `source/gen3_sbmap.*`, `source/rom_script.*`, `source/fused_rom.*`,
`source/flashsave.*`, `docs/MAP-*.md`, `docs/DELTA-BUILD.md`.

## Builds shipped to Guy (rebuild with `make` and `make delta` after any change)
- `~/Desktop/PokeDNA-Emerald/PokeDNA.gba`      — EZ-Flash Omega DE / GBA SP / DSL
- `~/Desktop/PokeDNA-Emerald/pokedna-emerald.gba` — Delta on iOS (fused with Emerald)
- same two files in `~/Library/CloudStorage/OneDrive-Tel-AvivUniversity/PokeDNA/`
Standing rule: EVERY change ships both.

## How to re-run the emulator tests
Harness: `<scratchpad>/harness/drive.py` (libmgba-py, headless).
Nav helper + all test scripts: `<scratchpad>/run/`, entry point `lib_nav.open_map(shots=, rom=, sav=)`.
Fuse first: `python3 tools/fuse_rom.py pokedna-delta.gba <roms>/Emerald.gba -o <scratchpad>/run/fused-Emerald.gba --force`
Useful existing scripts: `t_final.py` (5-game sweep), `t_decor.py` (secret-base decorations),
`t_ball5.py` (item balls), `t_npcdlg.py` (NPC dialog), `t_safari.py` / `t_safari_fr.py`,
`t_face2.py` (region-map head), `t_sbase*.py` (secret-base entrances).
Save patcher: `<scratchpad>/run/savtool.py` — `patch_sb1(src, dst, off, bytes)` fixes checksums
in every slot. Place the player with `patch_sb1(sav, out, 0x00, pack('<hh',x,y)+bytes([g,n,0xFF,0,0,0]))`.
On-device log: read `s_buf` at 0x020005dc, length at 0x030009fc (addresses from
`arm-none-eabi-nm -n pokedna-delta.elf`).

## Host regressions
`<scratchpad>/sbdecor/hostcmp.c` — proves gen3_sbdecor.c's metatile ids match the verified
reference renderer. Build:
`clang -O2 -I<PokeDNA>/source hostcmp.c <PokeDNA>/source/{gen3_sbdecor,gen3_sbmap,rom_map,gen3_save}.c -o hostcmp`
Run: `./hostcmp <roms>/Emerald.gba <roms>/Emerald.sav 0x085A5C08` (Ruby 0x083EB6E0, Sapphire 0x083EB73C).
Expect `id=0` diffs; collision diffs are the intended correction.

## Workflows
- `wf_a08200a4-d93` secret-base decorations — COMPLETE, 8/8. Findings consumed and shipped.
- `wf_2f568ca9-afb` identity-edit consequences — STOPPED by Guy at his request, partial.
  Re-launch or resume: `Workflow({scriptPath: ".../identity-edit-consequences-wf_2f568ca9-afb.js",
  resumeFromRunId: "wf_2f568ca9-afb"})`. Purpose: warn before editing NAME/SEX/TID/SID.
  **The core question is ALREADY ANSWERED and written up in
  `gba-toolkit/docs/kb/pokemon/secret-base-identity-and-record-mixing.md`**: record mixing matches
  bases on `gender + all 4 trainerId bytes + trainerName`, NOT on secretBaseId, so changing any of
  those three makes other cartridges insert a duplicate instead of updating. What the resumed run
  still owes: the per-mon OT / obedience consequences, the exact shiny formula, recoverability,
  and the wrapped GBA dialog text. Seven UI sites need gating: card_editor CARDF_NAME/CARDF_ID/
  CARDF_SEX (pdna_trainer.c ~446/448/456) and the plain list TF_NAME/TF_SEX/TF_TID/TF_SID
  (~591/593/599/601). `app_confirm` is only 2 lines — add a multi-line `identity_warn()`.
- `wf_468055bd-abb` map corruption root cause — running at time of writing.
  Read `<claude>/subagents/workflows/wf_468055bd-abb/journal.jsonl` for results.

## What the corruption workflow had found when this was written (8/12 agents)
- **silent SD read errors: LIKELY THE CAUSE.** "The map viewer is the only place in PokeDNA that
  pulls tens of KiB off the card with no integrity check whatsoever." EZ-Flash's Read_SD_sectors
  returns RES_OK unconditionally, so a bad read is neither retried nor reported.
- **A CONFIRMED MISCOMPILE, two sites, proven at assembly level: GCC deleted the initialisation
  of TWO stack buffers in `mgfx_load()`** because their only consumer is tonc's INLINE dma3_cpy,
  which stores only the source ADDRESS into a volatile register. Same class as the greyed-ball
  bug fixed earlier this session. FIX THIS REGARDLESS of whether it is the corruption.
- arena overrun: RULED OUT. OS-mode violation: contributing only. Two hypotheses REFUTED.
