# Pokemon Gen-3 save format (Ruby/Sapphire/Emerald)

How a Gen-3 `.sav` is framed, located, checksummed, reassembled, and decrypted — enough to read, validate, and bit-exactly rewrite an RS/Emerald save on a GBA. This is the data-format reference behind the record-mixer; the in-game merge algorithm itself lives in [the record-mix subsystem](#the-record-mix-port).

Most concrete claims below cite the **reference implementation** (`source/gen3_save.*`, `source/record_mix.*`), which is **not vendored** in gba-toolkit — it lives in the separate record-mixer repo (reference impl: `/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`). The portable I/O underneath it (`lib/`, `source/log.*`) *is* vendored here; see [../CAPABILITIES.md](../CAPABILITIES.md).

## Load-bearing gotchas

These are the things that silently produce garbage if you get them wrong.

- **Scan by section id, never by position.** The 14 sectors of a slot are rotated on every save, so physical sector index does NOT equal logical id. You must iterate all 14 and match the footer id at `0xFF4`. `gen3_find_section` does exactly this and additionally skips any sector whose signature `0xFF8 != 0x08012025` (`source/gen3_save.c:48-56`).
- **Current slot = higher save counter.** Read the `u32` counter at `0xFFC` from the *first signature-valid sector* of each slot (not a fixed sector). Pick slot 1 iff `counter[1] > counter[0]`, else slot 0; fall back to whichever slot is valid (`source/gen3_save.c:244-264`).
- **The per-mon decryption key is `personality ^ otId`** — this is the Gen-3 box-mon XOR, and is **separate** from the Emerald SaveBlock security key at `SaveBlock2+0xAC`. Do not confuse them (`source/gen3_save.c:100-112`; gotcha list confirms the security key is used here only as a version tiebreaker).
- **Substruct order = `personality % 24`.** A wrong permutation row (or wrong key) decrypts to plausible-looking bytes that fail the per-mon checksum at `mon+0x1C`. Always verify that checksum and treat a mismatch as corrupt (`source/gen3_save.c:114-130`).
- **Don't C-bitfield the secret-base flags byte.** Bitfield bit-order is implementation-defined; the port stores a raw `uint8_t` and decodes with masks (LSB-first on little-endian ARM). Replicate that for any ported bitfield (`source/record_mix.h:19-22, 61-76`).
- **SecretBase array offset is version-specific:** Emerald `0x1A9C`, RS `0x1A08`. Same 160-byte layout otherwise (`source/gen3_save.h:53-56`).
- **Partial sections (ids 0, 4, 13) checksum over a version-specific size**, not 3968. Verification only covers the full sections; `gen3_write_full_section` only works on full sections (`source/gen3_save.c:201-214, 223-237`).
- **All multi-byte reads/writes are explicit little-endian** via `rd16/rd32/wr16`, even though the GBA is LE — so the code also runs on host-side tests (`source/gen3_save.c:5-15`).

## Container layout

A full `.sav` is `0x20000` (128 KiB): **2 slots × 14 sectors × 4096 bytes**, slot size `0xE000` (`source/gen3_save.h:15-20`). Framing is byte-identical across Ruby, Sapphire, and Emerald; only the *contents* of SaveBlock1/2 differ.

Each 4096-byte sector = **3968 data bytes + 116 unused + 12-byte footer**:

| Footer offset | Size | Field | Citation |
|---|---|---|---|
| `0xFF4` | u16 | logical section id | `source/gen3_save.h:23` |
| `0xFF6` | u16 | checksum | `source/gen3_save.h:24` |
| `0xFF8` | u32 | signature, must equal `0x08012025` | `source/gen3_save.h:25,27` |
| `0xFFC` | u32 | per-slot save counter | `source/gen3_save.h:26` |

Logical section ids: SaveBlock2 = `0`; SaveBlock1 spans `1..4`; PC storage spans `5..13` (`source/gen3_save.h:30-34`). The rest of the 128 KiB image (Hall of Fame `0x1C000`, Mystery Gift `0x1E000`, Recorded Battle `0x1F000`) is outside the two save blocks and untouched here.

## Checksum (fold-add u32 → u16)

Mirrors pokeemerald `CalculateChecksum`: zero a `u32` accumulator, add `size/4` little-endian `u32` words, then fold to u16 as `(u16)((sum >> 16) + sum)` (`source/gen3_save.c:19-28`):

```c
uint16_t gen3_checksum(const void* data, uint16_t size) {  // source/gen3_save.c:19
  uint32_t checksum = 0; uint16_t words = size / 4;
  for (uint16_t i = 0; i < words; i++) { checksum += rd32(p); p += 4; }
  return (uint16_t)((checksum >> 16) + checksum);
}
```

Full (3968-byte) sections checksum over all 3968 data bytes. `gen3_verify_full_checksums` only verifies the full ids `{1,2,3,5,6,7,8,9,10,11,12}` and treats an absent section as a non-failure (`source/gen3_save.c:201-214`).

## SaveBlock1 reassembly

Sections `1,2,3,4` each carry 3968 data bytes. Concatenate them **in id order (not physical order)** into one contiguous `4 × 3968 = 15872`-byte buffer; returns 0 if any of the four is missing (`source/gen3_save.c:58-69`, `G3_SAVEBLOCK1_BYTES` `source/gen3_save.h:36-37`). Note this is a 15872-byte stack buffer in `gen3_parse` — sizeable for the GBA, callers on tight IWRAM should be aware.

SaveBlock2 (trainer info), identical across RSE (`source/gen3_save.h:39-46`):

| SB2 offset | Field |
|---|---|
| `0x00` | player name, 7 chars + `0xFF` terminator |
| `0x08` | gender (0 male / 1 female) |
| `0x0A` | trainer id (TID lo/hi, SID lo/hi) |
| `0x0E` / `0x10` / `0x11` | playtime hours u16 / minutes u8 / seconds u8 |
| `0xAC` | u32 encryption key, **Emerald only** (RS: nothing here) |

### Secret-base array

20 records × 160 bytes, spanning SaveBlock1 sections 2–3. Offset is version-specific: **Emerald `0x1A9C`, RS `0x1A08`** (`source/gen3_save.h:48-56`, selected by `gen3_secret_base_offset` `source/gen3_save.c:149-155`). Within a record: id `+0x00`, party `+0x34`, first-party species u16 `+0x7C`, first-party level u8 `+0x94` (`source/gen3_save.c:159-169`).

**Version detection is a heuristic** (the UI confirms before writing): count sane non-empty bases at each candidate offset and pick the higher; a record is sane if empty (`id==0 && species==0`) or species `1..411` and level `1..100`. On a tie (often both 0), fall back to Emerald iff the `0xAC` key is nonzero, else UNKNOWN (`source/gen3_save.c:157-188, 290-304`).

## The per-mon decryption kernel

The live party lives in reassembled SaveBlock1 (section id 1): count u8 `@0x234`, then 6 × `struct Pokemon` `@0x238`, each 100 bytes (`source/gen3_save.h:63-66`). Identical offsets for RS and Emerald. `gen3_read_live_party` (`source/gen3_save.c:87-147`) does, per mon:

1. **Read flags byte at `mon+0x13`:** bit0 = bad-egg, bit2 = is-egg. These flags live here, NOT in the decrypted substructs (`source/gen3_save.c:96-98`).
2. **Compute key** `= rd32(mon+0x00) ^ rd32(mon+0x04)` (personality ^ otId) (`source/gen3_save.c:100-102`).
3. **Decrypt the 48-byte secure block at `mon+0x20`** (12 `u32` words), XOR each word with the key (`source/gen3_save.c:104-112`).
4. **Validate the per-mon checksum:** sum the 24 decrypted u16 halfwords (mod 2^16), compare to the stored u16 at `mon+0x1C` (`source/gen3_save.c:114-130`).
5. **Locate substructs** via `k_substruct_pos[personality % 24]` → slot (0..3) of Growth, Attacks, EVs within the 48-byte block, each 12 bytes (`source/gen3_save.c:76-122`).
6. **Skip rule:** bad-egg, egg, `species==0`, or checksum mismatch → skip; kept mons are compacted to the front of `out->mon[]` (`source/gen3_save.c:126-145`).

Fields pulled from the decrypted substructs: Growth → species `@+0`, heldItem `@+2`; Attacks → moves[0..3] `@+0,+2,+4,+6`; EVs → 6 EV bytes `@+0..+5` (this code keeps only `avgEV = sum/6`) (`source/gen3_save.c:124-141`).

**Box vs party form:** the boxed (PC) form is 80 bytes ending at the 48-byte data block; the party form adds 20 runtime bytes (status, level `@0x54`, current/max HP and the five battle stats) for 100 bytes total. This kernel reads `level` from `mon[0x54]` (`source/gen3_save.c:139`). PC-box parsing reuses the same decrypt/checksum/order logic over the 80-byte form.

The 24-row permutation table (storage slot per type) is `k_substruct_pos[24][3]` (`source/gen3_save.c:76-85`); the four substruct types are Growth/Attacks/EVs/Misc — this kernel does not read the Misc substruct (where IVs/ability live), which is why a stats viewer must add it (below).

## What gen3_save.c already provides (reusable)

All in `source/gen3_save.c` / `.h` (reference impl). Signatures:

| Function | Line | What it does |
|---|---|---|
| `uint16_t gen3_checksum(const void* data, uint16_t size)` | `gen3_save.c:19` | fold-add u16 checksum |
| `char gen3_decode_char(uint8_t c)` | `gen3_save.c:30` | one Gen-3 charset byte → ASCII |
| `int gen3_find_section(const uint8_t* save, int slot, int section_id)` | `gen3_save.c:48` | scan-by-id locator → sector index or -1 |
| `uint32_t gen3_read_saveblock1(const uint8_t* save, int slot, uint8_t* dst)` | `gen3_save.c:58` | reassemble ids 1..4 → 15872 bytes |
| `int gen3_read_live_party(const uint8_t* sb1, Gen3LiveParty* out)` | `gen3_save.c:87` | decrypt + validate + compact party |
| `uint32_t gen3_secret_base_offset(Gen3Version v)` | `gen3_save.c:149` | `0x1A9C` (EM) / `0x1A08` (RS) / 0 |
| `int gen3_count_secret_bases(const uint8_t* sb1, Gen3Version v)` | `gen3_save.c:171` | sane non-empty count (version detect) |
| `bool gen3_section_checksum_ok(...)` | `gen3_save.c:190` | one section's stored vs computed checksum |
| `bool gen3_verify_full_checksums(const uint8_t* save, int slot, int* fail_id)` | `gen3_save.c:201` | verify all full sections; first fail id |
| `void gen3_sb1_touch_sections(Gen3Version v, int* first_id, int* last_id)` | `gen3_save.c:216` | which SB1 ids the base array spans (2..3) |
| `uint32_t gen3_write_full_section(...)` | `gen3_save.c:223` | **bit-exact** data-region overwrite + recompute checksum; preserves pad + footer; returns abs offset or `0xFFFFFFFF` |
| `bool gen3_parse(const uint8_t* save, uint32_t size, Gen3SaveInfo* out)` | `gen3_save.c:239` | top-level: pick slot, decode SB2, reassemble SB1, guess version |

Key types: `Gen3PartyMon` (`gen3_save.h:98`), `Gen3LiveParty` (`gen3_save.h:107`), `Gen3SaveInfo` (`gen3_save.h:74`). The bit-exact write path feeds the temp+verify+rename safety pipeline — see [./safety-pipeline.md](./safety-pipeline.md).

## What a stats viewer must ADD

This kernel distills each mon to what a SecretBaseParty needs; a full stats/PKHeX-style viewer needs more, all derivable from the same decrypted 48-byte block plus a couple of tables:

- **IVs** — read the **Misc** substruct's IV/Egg/Ability word (`u32` at Misc `+0x04`): bits 0-4 HP, 5-9 Atk, 10-14 Def, 15-19 Spe, 20-24 SpA, 25-29 SpD, bit30 isEgg, bit31 abilityNum. The current kernel never touches Misc.
- **Full 6 EVs** — the EVs substruct holds all six EV bytes at `+0..+5`; the kernel collapses them to `avgEV` (`source/gen3_save.c:140-141`). Keep the spread.
- **Nature** = `personality % 25` (the kernel only computes `personality % 24` for substruct order).
- **Computed stats** — apply the Gen-3 stat formula using species base stats (need a base-stat table), level, IVs, EVs, and nature.
- **Name tables** — `gen3_decode_char` handles the trainer/OT name charset (`source/gen3_save.c:30-46`); a viewer also needs species-name, move-name, item-name tables.
- **Emerald money / items** — XOR-obfuscated by the Security Key (`SaveBlock2+0xAC`): money (Section 1 `@0x0490`) XOR full 32-bit key; coins (`@0x0494`) and item quantities XOR the low 16 bits. **Ruby/Sapphire store these in plaintext** — skip the XOR there. (Confirm exact in-section offsets against Bulbapedia/pokeemerald before writing.)

## The record-mix port

The in-game secret-base merge is ported in `source/record_mix.*` (reference impl). Highlights relevant to the format: the 160-byte `SecretBase` packed struct is cast directly over `sb1 + offset` with a `_Static_assert(sizeof(SecretBase) == 160)` layout guard (`source/record_mix.h:24-43`, assert in `record_mix.c`). The embedded SecretBaseParty (record `+0x34`): `personality[6]@0x34`, `moves[24]@0x4C`, `species[6]@0x7C`, `heldItems[6]@0x88`, `levels[6]@0x94`, `evs[6]@0x9A` (`source/record_mix.h:36-42`). `sb_set_party_from_live` (`source/record_mix.h:132`) rebuilds a base's snapshot from the live decrypted party. Full merge mechanics are out of scope for this format doc.

### Decomp-port methodology

The port keeps the pret/pokeemerald **original C and original symbol names** (`ClearSecretBase`, `SortSecretBasesByRegistryStatus`, etc.) so it can be diffed line-by-line against the decomp, replicates the decomp's quirky loop bounds rather than "fixing" them, decodes bitfields by hand instead of using C bitfields, and stays platform-header-free pure C so it dual-compiles for host tests. See [./build-and-toolchain.md](./build-and-toolchain.md) for the dual-compile setup. **Legal posture** (pret is an unlicensed decompilation of copyrighted code; reusable MIT alternatives like pksav exist) is deferred to [./licensing.md](./licensing.md) — read it before redistributing.

## Sources

External references (pin GitHub links to a commit, not `master`):

- Bulbapedia — Save data structure (Generation III): https://bulbapedia.bulbagarden.net/wiki/Save_data_structure_(Generation_III)
- Bulbapedia — Pokemon data structure (Generation III): https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_data_structure_(Generation_III)
- Bulbapedia — Pokemon data substructures (Generation III): https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_data_substructures_(Generation_III)
- pret/pokeemerald `include/pokemon.h` (canonical structs/bitfields): https://github.com/pret/pokeemerald/blob/master/include/pokemon.h
- pret/pokeemerald `src/pokemon.c` (Encrypt/DecryptBoxMon, CalculateBoxMonChecksum, GetSubstruct mod-24 table): https://github.com/pret/pokeemerald/blob/master/src/pokemon.c
- pksav (MIT, pure C, reusable crypt/checksum kernel) — `lib/gba/crypt.c`, `lib/gba/checksum.c`: https://github.com/savaughn/pksav
- PKHeX (C#, authoritative offset/legality reference, not portable): https://github.com/kwsch/PKHeX

Key source files (reference impl: `/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`):

- `source/gen3_save.h`, `source/gen3_save.c` — container, checksum, reassembly, decryption, bit-exact write
- `source/record_mix.h`, `source/record_mix.c` — SecretBase struct overlay and merge port
- `tests/host_test.c` — host-side regression harness

Related KB docs: [../CAPABILITIES.md](../CAPABILITIES.md) · [./safety-pipeline.md](./safety-pipeline.md) · [./build-and-toolchain.md](./build-and-toolchain.md) · [./licensing.md](./licensing.md)
