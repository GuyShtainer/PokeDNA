# How Gen-3 (pokeemerald) animates the Pokémon front sprite in the summary

Derived clean-room from the pokeemerald decomp (reference-only). The summary front
sprite is **two independent animation systems running on the same OAM sprite at once** —
neither is a single static blit. PokeDNA currently draws the front pic as one Mode-3
bitmap (`ui_sprite`), so it replicates *neither* yet.

## 1. The two layers

- **Layer A — frame (cel) animation.** Every Gen-3 front "pic" is not one 64×64 image but
  **two** 64×64 4bpp frames packed back-to-back in one LZ77 blob (frame 0 = neutral,
  frame 1 = open-mouth/alternate). A per-species *AnimCmd script* holds the choreography
  ("frame 0 for 30 ticks, frame 1 for 30, frame 0 for 1, end"). The OAM hardware shows one
  64×64 frame at a time; the engine hot-swaps which decompressed frame occupies the VRAM
  tile region via a queued DMA. The little mouth-flap. **No affine matrix** — pure tile swap.
- **Layer B — movement/affine ("personality") animation.** Separately, each species maps to
  one `ANIM_*` id that selects a procedural per-frame *sprite callback*. Each frame it writes
  a **position offset** (`sprite->x2/y2`, free — slides/vibrates/hops) and/or an **OAM affine
  matrix** (`gOamMatrices[matrixNum]` via BIOS `ObjAffineSet` — rotate/scale/squish/twist).
  The bounce/spin/squash on entry. Runs in parallel with Layer A.

## 2. Frame animation ("not a single sprite")

- **Storage:** one frame = `MON_PIC_SIZE` = 64*64/2 = `0x800` B (`include/constants/pokemon.h`;
  `MAX_MON_PIC_FRAMES=4`, front uses 2). `HandleLoadSpecialPokePic`/`LoadSpecialPokePic_2`
  (`src/decompress.c:73,316`) LZ77-decompress `gMonFrontPicTable[species]`
  (`src/data/pokemon_graphics/front_pic_table.h`) → frame0 @`+0x000`, frame1 @`+0x800`.
  `gMonFrontPicTable[].size` is `MON_PIC_SIZE` (per-frame copy size, not the total).
  `DuplicateDeoxysTiles` (`src/decompress.c:407`) fakes frame1 for the 1-frame Deoxys.
- **Frame index:** buffer sliced into `struct SpriteFrameImage {const void* data; u16 size;}`
  (`include/sprite.h:26`), one per frame (`src/pokemon.c:7015,7091`). Becomes `template.images`
  so anim image 0→frame0, 1→frame1.
- **AnimCmd scripts:** `union AnimCmd[]` (`include/sprite.h:74-89`), `ANIMCMD_FRAME(img,dur)`/
  `_END`/`_LOOP`/`_JUMP`. e.g. `sAnim_Bulbasaur_1 = {FRAME(0,30),FRAME(1,30),FRAME(0,1),END}`
  (`front_pic_anims.h:9`). `SINGLE/DOUBLE_ANIMATION` bundle each species into `sAnims_<Name>[]`
  (slot 0 = static `sAnim_GeneralFrame0`, slot 1 = the motion). Master table
  `gMonFrontAnimsPtrTable[]` (`front_pic_anims.h:5255`) → `template.anims` (`src/pokemon.c:3502`).
- **Cycling (per VBlank):** `AnimateSprites`→`AnimateSprite` (`src/sprite.c:308,901`):
  `BeginAnim` reads duration + `RequestSpriteFrameImageCopy` (queues the 0x800-byte VRAM DMA,
  `src/sprite.c:802`); `ContinueAnim` decrements, on 0 advances `animCmdIndex` and `AnimCmd_frame`
  swaps the VRAM frame (`src/sprite.c:968`). Gated on `HasTwoFramesAnimation(species)`
  (`src/pokemon.c:6974`) — TRUE except Castform/Deoxys/Spinda/Unown. Started via
  `StartSpriteAnim(sprite, 1)`.

## 3. Movement animation (bounce/spin/squash)

- **Selection:** `sMonFrontAnimIdsTable[species-1]` (`src/pokemon.c:1405`) → `ANIM_*` id;
  `sMonAnimationDelayTable[species-1]` (`:1795`) → start delay. id indexes `sMonAnimFunctions[]`
  (`src/pokemon_animation.c:630`), table of `void(*)(struct Sprite*)`.
- **Position-only (cheap, no matrix):** write only `sprite->x2/y2`, folded into `oam.x/y` by
  `UpdateOamCoords` (`src/sprite.c:339`). e.g. `Anim_HorizontalSlide` (`:1131`),
  `Anim_HorizontalVibrate` (`:1110`), vertical hops (`:1181`). Flip = negate x2 (`TryFlipX:1031`).
- **Affine (rotate/scale/squish, needs a matrix):** first frame `HandleStartAffineAnim` (`:1003`)
  sets `oam.affineMode = ST_OAM_AFFINE_DOUBLE` (double-size so a grown sprite isn't clipped) +
  allocs a matrix slot. Each frame `SetAffineData` (`:984`) feeds `{xScale,yScale,rotation}` to
  BIOS `ObjAffineSet`, writing 8.8 fixed `a,b,c,d` into `gOamMatrices[oam.matrixNum]`. e.g.
  `Anim_VerticalSquishBounce` (`:1834`), `Anim_ShrinkGrow` (`:1878`), `Anim_Twist` (`:1420`),
  `Anim_BounceRotateToSides` (`:1939`). Scale 8.8 (256=1.0, neg xScale=H-flip). **Only 32 matrix
  slots** total (`OAM_MATRIX_COUNT`, `include/sprite.h:4`).
- **Launch:** battle → `LaunchAnimationTaskForFrontSprite` (`:941`) via `Task_HandleMonAnimation`;
  summary → `StartMonSummaryAnimation` (`:949`) assigns the callback directly (`sIsSummaryAnim`).
- **Stop:** callback writes identity, zeroes x2/y2, `ResetSpriteAfterAnim` (`:1061`, affineMode→
  NORMAL, `FreeOamMatrix`), then `callback = WaitAnimEnd`.

## 4. Summary-screen wiring

- **Entry:** `CB2_InitSummaryScreen` state 17 → `LoadMonGfxAndSprite` (`pokemon_summary_screen.c:3900`)
  decompress pic + palette + `SetMultiuseSpriteTemplateToPokemon` → `CreateMonSprite` (`:3975`),
  `callback = SpriteCB_Pokemon`. `SpriteCB_Pokemon` (`:3994`) waits for the fade, then `PlayMonCry`
  + `PokemonSummaryDoMonAnimation` (`src/pokemon.c:6844`): (1) `StartSpriteAnim(sprite,1)` = Layer A;
  (2) if `sMonAnimationDelayTable` nonzero → `Task_PokemonSummaryAnimateAfterDelay` (`src/pokemon.c:6779`)
  counts down then `StartMonSummaryAnimation`; else immediate = Layer B.
- **Scroll mon (U/D):** `Task_ChangeSummaryMon` (`:1628`) tears down the delay task + old sprite,
  `LoadMonGfxAndSprite` for the new mon, then re-ungates `SpriteCB_Pokemon` → both layers **replay**.
- **Page (L/R):** background/text only — sprite untouched, no restart.
- **Exit:** `StopPokemonAnimations` (`:4030`) pauses + dummies the callback;
  `SummaryScreen_DestroyAnimDelayTask` is the final safety net.

## 5. What this means for PokeDNA (Mode-3 bitmap, no OAM/affine)

- **Cheap & faithful — DO:**
  - **Layer A 2-frame swap.** The source already holds two 64×64 frames. Decode both into EWRAM
    (RGB555), alternate which one `ui_sprite` blits on a ~30/30-tick timer (the AnimCmd cadence).
    One extra decoded buffer + a counter. Captures the recognizable idle mouth-flap. Apply the
    Castform/Deoxys/Spinda/Unown single-frame exclusion (or skip when frame1==frame0).
  - **Position bob** (the position-only `Anim_*` subset). Blit the already-rendered 64×64 at
    `(x, y+bob)` from a small sine table; repaint the vacated strip with the bg. Same arithmetic
    as `x2/y2`, into framebuffer coords. One extra blit/tick.
- **Hard — SKIP:** Layer B **affine** (rotate/scale/squish). On a bitmap you'd software-rasterize
  the inverse 2×2 transform per output pixel per frame (keep the undecorated source, re-clear the
  box) — a real cost on a 16.78 MHz ARM7 for a viewer. Not worth it.
- **Recommendation:** decode both frames once; drive Layer A as a 2-state toggle; add a single
  gentle vertical bob on entry (optionally axis-by-species loosely from `sMonFrontAnimIdsTable`);
  restart both when the viewed mon changes; **skip affine**. ~90% of the "it's alive" feel using
  only blits + a sine table, zero OAM/affine dependency.
