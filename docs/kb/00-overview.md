# Knowledge base — overview & map

This is the index for the gba-toolkit knowledge base. Each doc is a focused, cited
reference for one subsystem, written so a future tool reuses what was learned **without
re-deriving it from source**. Read this first, then jump to what you need.

## The tool family

All tools share one hardware substrate (a GBA running from a flashcart, reading/writing
its microSD), one filesystem layer (FatFs), one safety discipline (verified writes), and —
for the Pokémon tools — one save-format/crypto kernel.

| Tool | Status | Adds |
|---|---|---|
| **Pokémon record-mixer** | shipped / reference impl | secret-base merge, live-party regen, bidirectional mix |
| **SD file browser** | planned (Project A) | list/copy/paste/delete/mkdir/rename, optional hex editor |
| **Gen-3 SAV reader** | planned (Project B) | PKHeX-like stats viewer (IVs/EVs/nature/stats), later edit |

See `../ROADMAP.md` for the phased plans and feasibility verdicts.

## The two facts that shape everything

1. **OS-mode / ROM disappears.** During any EZ-Flash SD transfer the game ROM is unmapped;
   I/O code and the data it touches must live in EWRAM (`EWRAM_CODE`/`EWRAM_BSS`) with IRQs
   off. → `flashcart-io.md`, `build-and-toolchain.md`.
2. **Never corrupt the original.** Writes have no retry; always back up + write-temp +
   byte-verify + rename. → `safety-pipeline.md`.

## Map of the docs

| Doc | Read it when you… |
|---|---|
| [flashcart-io.md](flashcart-io.md) | touch SD sector I/O, cart detection, OS-mode/IRQ/DMA, EZFO registers |
| [fatfs-fileops.md](fatfs-fileops.md) | list/copy/move/delete/create files, configure ffconf, build the file browser |
| [gen3-save-format.md](gen3-save-format.md) | parse/decrypt/edit a Gen-3 `.sav`; need offsets, the crypt kernel, derived stats |
| [rtc.md](rtc.md) | read the cartridge clock / stamp file timestamps |
| [safety-pipeline.md](safety-pipeline.md) | write any file safely (backup → temp → verify → rename) |
| [ui-and-logging.md](ui-and-logging.md) | build menus/input, or wire up the screen+mGBA+SD logger |
| [build-and-toolchain.md](build-and-toolchain.md) | build, place code in EWRAM/IWRAM, start a new tool |
| [hardware-validation.md](hardware-validation.md) | get a feature signed off on real hardware |
| [licensing.md](licensing.md) | **before** writing any save-parser code (PKHeX/pret/pksav posture) |

Plus the at-a-glance [../CAPABILITIES.md](../CAPABILITIES.md) (proven-possible table + all
external references).

## Where the code lives

- **Vendored in this repo:** everything under `lib/`, plus `source/gba_rtc.*` and `source/log.*`.
- **Reference implementation** (`/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`):
  `source/gen3_save.*`, `source/savefile.*`, `source/main.c`, `source/record_mix.*`,
  `reference/` (pret decomps), `tests/host_test.c`. KB docs cite these by relative path and
  note the reference repo. They get extracted into this repo's `source/` when Project B starts.

## Conventions (full list in [../../CLAUDE.md](../../CLAUDE.md))

Pure-C cores for host tests · `EWRAM_BSS` static buffers, never big stack buffers ·
write features Omega-only · clean-room save-format code · `hardware-testing-protocol`
sign-off for anything touching the card, the RTC, or user data.
