# Map feature — hardware test script

Short on purpose. Everything below was verified in an emulator against your real ROMs; this
list is only the things **an emulator cannot decide**.

**Before you flash:** delete the old `.gba` from the card, eject, remount, copy fresh — so it
lands contiguous. (Fragmentation, not size, is what breaks the SD load; see `rom-load-lab`.)
Use the normal **`PokeDNA.gba`**, not the fused one — the fused image is an emulator artifact.

**Back up your saves first.** Step 5 writes to your save on purpose.

---

## 1. The Emerald fix — the whole reason for this list ⭐

Your Emerald save is standing in the **Battle Arena Lobby**. Open the map there at 1:1.

- ✅ **Correct:** a clean room — wooden counters left and right, a PC with a green screen top
  right, a chest below the centre, a mat at the bottom. It should look like the room you see
  in the game.
- ❌ **Wrong:** the same tiles scattered into fragments, like the photo you sent.

**Why only hardware can answer this.** The bug was in the SD driver: `disk_read` bounced
unaligned buffers on `& 0x1`, but the EZ-Flash path is a **DMA32** copy and the GBA's DMA
unit force-aligns its address *down* — so any buffer landing at 2-mod-4 received every whole
sector **two bytes early**, and the driver still reported success. Emerald's metatile tables
are `u16`-aligned and about half start at 2 mod 4; FireRed's are `u32` and were structurally
immune, which is exactly why LeafGreen always looked fine. An emulator reads the ROM from
cartridge space and never touches FatFs, so it renders correctly either way.

If this step is wrong, nothing else in the list matters — tell me and stop.

**This fix also lands in File-Browser-GBA, gba-watch, PokeLinkSim, rtc-doctor, rgb-lab,
rumble-lab and rom-load-lab** — same defect, same 16 sites. On the *write* path it was
silent data loss, so it is worth a sanity check of a file copy in File-Browser-GBA too.

## 2. Zoom out — all three games

Press **L** from 1:1. Then again. Then again twice more.

| Level | HUD | What you should see |
|---|---|---|
| 1/2 | `1/2` | the map at half scale, sharp |
| 1/4 | `1/4~` | much wider view; **blocky patches are expected** — the tilde means approximate |
| region | `RGN` | the game's own region map, zoomed on where you are |
| region wide | `RGN+` | the whole of Hoenn/Kanto |

- ✅ **Correct at 1/2:** continuous outlines, no vertical stripes, no doubled columns.
- ❌ **Wrong:** the map sheared into alternating columns. That was the zoom bug — every affine
  tilemap entry was written with a byte store, and GBA VRAM writes a byte into *both* halves
  of the halfword.

Pan around for ~30 s with the D-pad held at 1/2. Brief black-map-with-HUD frames while panning
a long way are the mip dictionary rebuilding and are expected; garbled map-like tiles are not.

**Then press R four times to come back down to 1:1.** The map must look exactly as it did
before you zoomed. (The region view borrows the same VRAM *and* the same scratch memory the
map's blockdata lives in, so this round trip is the one I'd most expect to break.)

## 3. Walk across a map edge

Hold a direction until you reach the edge of the map and keep going.

- ✅ **Correct:** before you cross, you can already see the neighbouring map beyond the seam;
  crossing loads it and the name in the HUD changes. Walking back returns you the way you came.
- ❌ **Wrong:** a hard wall of repeated border pattern where a neighbour should be, or the
  position jumping.

Some edges genuinely have no neighbour — the cursor just stops. That is correct.

## 4. Go in and out of a room

Stand on a door/stairs/cave tile — the HUD shows **`START enter`** — and press **START**.

- ✅ **Correct:** the room loads; walking onto its exit warp and pressing START brings you back.
- Some warps have no fixed destination (the game picks at runtime). Those say so and offer
  your **escape point** (where Dig would drop you). Accepting should land you somewhere sane.

## 5. Grab and drop your character ⚠️ writes your save

**Back up your `.sav` first.**

Press **A** while the cursor is on your character. It lifts, bobs, and sweats.
Move anywhere, press **A** again → a confirmation → **A** to place, **B** to cancel.

- ✅ **Correct:** B cancels the placement *and keeps carrying*. Placing says `PLACED`.
  Load the save in the game and you appear exactly there.
- Unwalkable tiles are allowed on purpose and warn first — that was your request.
- ❌ **Wrong:** B throwing you out of the map screen, or a confirmation you never asked for.

## 6. NPCs

- ✅ **Correct:** the people standing on the map are the ones actually there in your game.
  Put the cursor on one — the HUD shows its graphics id, movement type, and the flag that
  controls it.

## 7. Afterwards — the log

Open `/PokeDNA/_log.txt` (or the tool's log screen) and check the `map:` lines:

- `arena peak` must be **≤ 35712** and `phase2` well under it.
- `mt-miss` must be **0**.

That arena is borrowed from your PC box storage, so an overrun would corrupt Pokémon rather
than glitch a tile — those two numbers are the safety check, not a curiosity.

---

## Known limits (not bugs)

- **1/4 zoom is approximate.** The hardware caps an affine layer at 256 distinct tiles and
  that view needs up to 428, so the rarest tiles become flat colour. The `~` says so.
- **Region map on Sapphire / LeafGreen is unverified.** 8 of 11 game-revision addresses come
  from decomp symbol files rather than a dump I could check. They fail *closed* — if it can't
  validate the data it refuses the level rather than drawing garbage.
- **Everdrive is read-only by design.** Placement is Omega-only.
- NPCs always face south; the ROM stores a movement type, not a facing.
