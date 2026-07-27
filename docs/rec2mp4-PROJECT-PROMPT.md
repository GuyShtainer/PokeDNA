# PROMPT — paste this into a fresh Claude session in a NEW, EMPTY project folder

---

Build **rec2mp4**: a macOS-first (Windows-compatible where practical) command-line tool
that converts Pokémon Emerald **Battle Record files (`.rec`) into MP4 videos** of the
battles, faithfully — the exact battle as it happened, move for move.

## What a `.rec` file is

A `.rec` is the raw 4096-byte **save flash sector 31** of Pokémon Emerald, exported by an
on-cartridge tool (PokeDNA). Emerald's Frontier Pass "Battle Record" stores the single
most-recently recorded battle there as a **deterministic replay**: the RNG seed at battle
start + both full teams + every battler's input stream. The game itself can re-simulate it
perfectly. Byte layout (little-endian):

```
+0x000  u32  sentinel 0x0000B39D (must match, else not a record)
+0x004  struct RecordedBattleSave (3968 bytes):
   +0     playerParty[6]    6 x 100-byte standard encrypted party Pokémon
   +600   opponentParty[6]
   +1200  playersName[4][8] Gen-3 charset, 0xFF-terminated
   +1232  playersGender[4]  0=M 1=F        +1236 playersTrainerId[4] u32
   +1256  rngSeed u32                      +1260 battleFlags u32
   +1268  opponentA u16  +1270 opponentB u16  +1272 partnerId u16
   +1274  multiplayerId u16 (which players[] entry is the recorder)
   +1276  lvlMode u8 (0=Lv50 1=Open)       +1277 facility u8 (0..6 Tower..Pyramid)
   +1308  battleRecord[4][664]  per-battler input lanes, 0xFF-terminated
   +3964  checksum u32 = byte-sum of the struct's first 3964 bytes
```

A working reference parser (pure Python, tested against real records) exists —
ask the user for `read_rec.py`; they have it, and it already includes the injection step:
`python3 read_rec.py --inject file.rec save.sav` overwrites `.sav` bytes
0x1F000..0x1FFFF with the `.rec` verbatim (it IS the sector; single-copy, never
slot-rotated, no footer to fix up — round-trip verified against real records). The game
gates the Frontier Pass "BATTLE RECORD" entry purely on this sector's own validity
(pokeemerald `CanCopyRecordedBattleSaveData` reads + validates it; no other flag), so an
injected record replays exactly like a freshly recorded one.

## The one architecture decision that is already made — respect it

**Do NOT reimplement the battle engine** (in Python or anything else). Replay fidelity
requires consuming RNG in exactly the game's order and count — every damage/accuracy/AI
roll, including RNG advancement tied to frame timing. Any deviation desyncs silently.
This kills any generic simulator (Pokémon Showdown etc.) and makes an engine port a
months-long trap.

**Instead, automate the REAL engine**: run the user's own Pokémon Emerald ROM in an
emulator core, inject the `.rec` into the save, drive the game to Frontier Pass →
Battle Record → replay with scripted inputs, and capture frames + audio to MP4 via
ffmpeg. The game re-simulates the battle byte-perfectly because it's the same engine
and seed that recorded it.

Recommended stack (verify current state of each, pick what's healthiest):
- **mGBA** headless with its scripting API (Lua bindings ship with mGBA; Python bindings
  exist in the mGBA source tree and are used by the pokebot-gen3 ecosystem — that project
  is good prior art for driving Gen-3 games programmatically, memory reading, and input
  injection). BizHawk is a fallback on Windows.
- **ffmpeg** for encoding (frame pipe or image sequence + audio dump).
- Orchestration in **Python** (argparse CLI, cross-platform paths).

## Inputs the user will provide (ask for them; do not assume paths)

1. A folder of `.rec` files (they have ~10 real ones, various facilities: Dome, Palace,
   Arena — good test diversity; some are 2v2-lane multi battles).
2. Their **Pokémon Emerald ROM** (their own dump; US version — the records came from it).
3. Optionally the **pokeemerald decompilation** (github pret/pokeemerald) — use it to find
   RAM addresses, menu state machines, and the replay entry points; it is reference-only
   (clean-room: don't copy code into the tool, but reading it to find addresses is fine).
4. A **save file only if truly needed** (the user prefers not to need one). Two replay
   strategies, in order of preference:
   - **Strategy B (save-free, preferred): direct state injection.** Use the emulator
     scripting API to write the RecordedBattleSave into the game's memory/flash region
     and invoke the recorded-battle playback path directly (the decomp names the
     functions: the Battle Record menu ultimately calls the recorded-battle setup —
     find the cleanest entry: e.g. set the flash sector, then call/force the same code
     path the "watch record" menu item runs, via breakpoint + register/state setup, or
     by driving the menu with a minimal bootstrapped profile). Harder but zero user save.
   - **Strategy A (fallback): template save + menu driving.** Take the user's `.sav`
     (which has the Frontier Pass unlocked), overwrite sector 31 with the `.rec`, boot,
     and drive the menus with a scripted input sequence to reach the replay. Simple and
     robust; costs asking the user for their save once.
   Start with A to get end-to-end video quickly (it validates everything else), then
   attempt B as the polish that removes the save dependency.

## Deliverable

`rec2mp4 <input.rec | folder> [-o outdir] [--rom path] [--sav path]`
- Batch: every `.rec` in → `<same-basename>.mp4` out (240x160 upscaled 3-4x with integer
  nearest-neighbor, 59.73 fps, with game audio).
- Detects the replay's natural END (the replay returns to the menu — detect via RAM state
  or frame similarity) and trims the video to intro→end plus a small tail.
- Sensible logging; a `--headed` debug mode showing the emulator window.
- README with setup for macOS (brew: mgba/ffmpeg or built bindings) and Windows notes.

## Practical cautions learned already

- Menu-driving must be frame-deterministic: fixed frame-count waits are brittle; prefer
  reading menu-state RAM addresses (from the decomp's symbols) to advance robustly.
- The replay is deterministic, but the PATH to it (menus) can eat inputs during fades —
  poll state, don't blind-fire inputs.
- Validate each `.rec` before spending emulator time (sentinel + checksum, reject junk).
- Multi-battles (4 non-empty input lanes) and Battle Palace (mons act on their own —
  fewer inputs) are all fine: the game replays them; nothing special needed per facility.
- Do not distribute the ROM, the save, or any Nintendo-derived art with the tool; the
  tool takes them as user-supplied inputs.

Work iteratively: first milestone = ONE `.rec` → watchable MP4 end-to-end (Strategy A is
fine); then batch mode; then trimming/quality; then Strategy B if feasible.

---

*(End of prompt. Written 2026-07-27 by the PokeDNA project; format spec verified against
the pret/pokeemerald decomp and real hardware exports — see PokeDNA
docs/analysis-2026-07-17/record-spec.md for the full field-by-field derivation.)*
