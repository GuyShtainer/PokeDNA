# PokeDNA — future ideas (parking lot)

## Export a battle "record" from Pokémon Emerald (→ replay/render on PC) — Guy, 2026-07-12

The GBA SP can't render/play an MP4 of a battle. Idea: **export a data file that a PC tool can
later turn into a video/replay** of the battle.

### Feasibility — RESOLVED (researched 2026-07-12): YES, Emerald actually stores a replayable battle.

The earlier note here guessed "the battle isn't logged, so a true replay export is probably not
possible." **That was wrong.** Emerald *does* have a deterministic battle recorder (the **Battle
Record** on the **Frontier Pass** — replay a Battle-Frontier or link battle). It is **not a video** —
it's a **seed + inputs** record the game re-simulates. And it lives at a **fixed, known place in the
`.sav`**, so a save tool like PokeDNA can read it directly. (Verified against the pret/pokeemerald
decomp — reference only, clean-room anything shipped.)

**Where it lives (verified):**
- Flash **sector 31** = `SECTOR_ID_RECORDED_BATTLE` → byte offset **`0x1F000`** in the 128 KB save
  (`31 * 0x1000`). One special sector, not double-buffered; it holds **the single most-recently
  recorded battle** (overwritten each time you record a new one).
- Sector framing: 3968 bytes of data (`SECTOR_DATA_SIZE`) + a 128-byte footer holding
  `u16 id; u16 checksum; u32 signature(=0x08012025 when valid); u32 counter`. Validate the
  signature + checksum before trusting the blob.

**What the record contains — `struct RecordedBattleSave` (fills the 3968-byte sector exactly):**
- `struct Pokemon playerParty[6]` and `struct Pokemon opponentParty[6]` — **both full teams**, the
  same 100-byte encrypted mon format PokeDNA already decrypts (600 B each, at `0x1F000` and
  `0x1F000+600`).
- `u32 rngSeed` — **the RNG state at battle start** (this is what makes replay deterministic).
- `u32 battleFlags`, `u32 AI_scripts`, `u8 lvlMode`, `u8 frontierFacility`, opponent/partner IDs,
  both trainers' names/genders/IDs/languages, apprentice/record-mix fields, easy-chat speech.
- `u8 battleRecord[MAX_BATTLERS_COUNT][664]` — **the turn-by-turn recorded inputs** (each battler's
  actions/move selections, 664 bytes per battler). Seed + parties + this input stream = the whole
  battle, reproducible move-for-move.
- `u32 checksum`.

### Two realistic ways to actually get a video (pick per effort budget)

1. **Trivial, zero custom engine — in-game replay + screen capture.** The game already replays the
   recorded battle (Frontier Pass → Battle Record). So on a PC: load the `.sav` in mGBA, watch the
   in-game replay, screen-record it. That *is* "render the battle to video on the PC," using the
   game's own deterministic replay. Works because the emulator runs the exact same engine + seed.
2. **Clean render — headless replayer.** Port pokeemerald's battle engine (the decomp is complete C)
   into a headless replayer that consumes a `RecordedBattleSave` (seed + parties + input stream) and
   renders frames → video. Bounded but large; the payoff is a shareable/archivable battle-video
   pipeline independent of an emulator UI. **Do not** use a generic sim (Pokémon Showdown) as the
   replayer — its RNG/mechanics aren't byte-identical to Gen-3, so it desyncs; determinism only
   holds with a true Gen-3 engine (the real ROM or the decomp).

### PokeDNA's concrete, easy piece (the part that belongs in this tool)

- **Read + export sector 31.** PokeDNA can, from any Emerald `.sav`, validate the footer and dump
  the `RecordedBattleSave` as JSON + the two teams as PKM/summary — reusing the existing mon parser
  verbatim (same 100-byte `struct Pokemon`). This gives an **archive/share** artifact for the last
  recorded battle without touching the game.
- **Round-trip (optional):** write an exported blob back into a fresh Emerald save's sector 31 (with
  the checksum recomputed) so it replays in an emulator — lets you archive/transplant recorded
  battles. Same verified-write discipline as every other PokeDNA edit (backup → `.tmp` → verify →
  rename); the checksum is over `SECTOR_DATA_SIZE` bytes.

### Caveats / non-goals
- **One battle at a time** (sector 31 is overwritten on each new recording) — it's the *current*
  record, not a library. Export it before the player records over it.
- **Only recordable battles are logged** — Battle-Frontier-facility battles and link battles the
  player chose to record. Ordinary wild/overworld-trainer battles are never in the record.
- Emerald only. RS/FRLG have no equivalent Frontier Battle Record (FRLG has no Frontier; RS predate
  it). This is an **Emerald-specific** feature.

(Cross-check before building: toolkit KB `docs/kb/pokemon/` + `docs/CAPABILITIES.md` — on-GBA mp4
render is INFEASIBLE; **the export is feasible on-cart, the video render is a PC-side project**.)
