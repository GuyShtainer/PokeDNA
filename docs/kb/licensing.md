# Licensing posture for GBA Pokemon tooling

**Purpose.** This is the MANDATORY-reading doc before you touch any save-parser
work in `gba-toolkit`. It records which upstream projects you may *ship* code
from, which are *reference-only*, and the clean-room rule that keeps every shipped
byte of save-format logic free of GPL and unlicensed-decompilation
contamination.

> First save-format work? Read **## Load-bearing gotchas** and the **DO / DON'T
> table** below, then come back for the per-project detail.

---

## Load-bearing gotchas

These are the things that bite you. None of them are obvious from a casual
"is the license permissive?" glance.

1. **PKHeX is GPLv3, not MIT — and it cannot run on a GBA anyway.** It is 100%
   C#/.NET (PKHeX.Core targets .NET 10, no native code). Porting (translating)
   its source to C creates a GPLv3 derivative work that would force the *entire*
   toolkit under GPLv3. Treat it as a spec, never as code. (One of our own
   research findings mislabels PKHeX "MIT" — that is wrong; the repo `LICENSE`
   file and the `PKHeX.Core` NuGet package both say GPL-3.0-or-later.)

2. **pret decomps (pokeemerald / pokeruby / pokefirered) are UNLICENSED.** A
   decompilation of copyrighted Game Freak code with no license file is *not*
   safe to ship verbatim, regardless of how authoritative it is. Read it to
   confirm byte layout; do not paste it. If you cite it, **pin to a commit
   hash**, never `/blob/master/` — `master` moves and your citation rots.

3. **The shippable basis is `savaughn/pksav` (MIT), not the archived
   `ncorgan/pksav`.** Same MIT license, same buffer-level no-malloc core, but
   actively maintained. Caveat that bites: **US-region saves only** — both forks
   only handle American string/section layouts. EU/JP needs your own work.

4. **Name-confusion traps.** `ScoreUnder/pksv` is a **GPL-3.0 ROM script
   editor**, not a save parser — wrong tool *and* a copyleft you must avoid.
   `PKMDS` is C#/C++ with bundled SQLite + the veekun Pokedex DB (won't fit a
   GBA, Gen 3 only partial). `ads04r/Gen3Save` is Python (Unlicense) — useful as
   a third cross-check, never shippable C.

5. **"Facts aren't copyrightable" is the whole game.** Byte offsets, the
   personality%24 substruct permutation table, the XOR-key formula, the
   fold-add section checksum — these are *facts/formats*. You may learn them
   from GPL or unlicensed sources and re-express them in your own clean C. What
   you may **not** do is copy their *expression* (variable names, control flow,
   comment-for-comment structure).

6. **Each NEW tool in this repo must pick its own license early.** If any
   GPLv3-derived line ever lands, the whole tool is GPLv3. The vendored
   flashcart/FS libs are already MIT / Apache-2.0 / FatFs-BSD-ish (see below);
   your new tool's license must be compatible with all of them.

See also: [../CAPABILITIES.md](../CAPABILITIES.md) (what the toolkit ships) and
[../ROADMAP.md](../ROADMAP.md) (what's planned).

---

## DO / DON'T table

| Project | License | Lang / runtime | Gen-3 GBA | Verdict |
|---|---|---|---|---|
| **savaughn/pksav** | MIT | pure C, no deps | complete (US only) | **SHIP** — vendor `lib/gba/` + `include/pksav/gba/`; cross-check our crypt/checksum against it |
| ncorgan/pksav | MIT | pure C | complete (US only) | reference / archived — prefer the savaughn fork |
| **PKHeX (kwsch)** | **GPLv3** | C# / .NET 10 | complete, authoritative | **SPEC ONLY** — never copy/port; can't run on GBA |
| **pret/pokeemerald, pokeruby, pokefirered** | **UNLICENSED** | C | byte-exact ground truth | **REFERENCE ONLY** — do NOT ship verbatim; pin to a commit hash |
| libspec (Chase-san) | MIT | C11 | RSE/FRLG, **self-declared unfinished** | reference only — not a stable dependency |
| ScoreUnder/pksv | GPL-3.0 | C | n/a (ROM *script* editor) | **AVOID** — wrong tool + copyleft. Name clash with `pksav` |
| PKMDS | C# Unlicense / C++ | C#/C++ + SQLite + veekun DB | partial | **AVOID** — too heavy for GBA |
| ads04r/Gen3Save | Unlicense | Python | read-only | reference cross-check only — not shippable C |
| Bulbapedia Gen-III pages | (facts) | docs | spec | **PRIMARY SPEC** — pair with pret for byte-exactness |

---

## The clean-room rule

> **Every shipped byte of save-format logic in this repo is independently
> authored from non-GPL, non-unlicensed specifications.**

Concretely:

- **Authoritative specs you may author from freely:** the three Bulbapedia
  "(Generation III)" pages (Pokemon data structure, Pokemon data substructures,
  Save data structure) and the MIT-licensed `savaughn/pksav` source.
- **Reference-but-don't-copy:** PKHeX (read it for offsets/edge cases), pret
  decomps (confirm byte layout, then re-express).
- Keep a mental (or commit-message) wall between *"I read their code to learn
  the format"* and *"I wrote this implementation."* Where two sources disagree,
  Bulbapedia is the human-readable spec and pret is byte-exact ground truth —
  but the *shipped* expression should match `pksav` (MIT) or your own.
- The Gen-3 facts that recur — XOR key = `personality ^ otId` per 32-bit word;
  substruct order = `personality % 24`; per-mon u16 truncating-sum checksum at
  `0x1C`; section footer signature `0x08012025` at `0xFF8` and folded
  32→16-bit section checksum at `0xFF6` — are formats, not copyrightable
  expression. (Detail lives in `./gen3-save-format.md` once written; the
  reference implementation is in `gen3_save.c` / `record_mix.c`, **reference
  impl: `/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`**,
  which is NOT vendored here.)

### What is vendored here vs. the reference-impl repo

| Vendored in `gba-toolkit` | NOT vendored (lives in reference-impl repo) |
|---|---|
| everything under `lib/` (flashcart IO, FatFs) | `source/gen3_save.{c,h}` |
| `source/gba_rtc.{c,h}` | `source/savefile.{c,h}` |
| `source/log.{c,h}` | `source/main.c`, `source/record_mix.{c,h}` |
| | `reference/`, `tests/host_test.c` |

The save-format code is exactly the code most exposed to the clean-room rule, so
it stays in the reference-impl repo until it has been independently authored and
license-cleared for sharing.

---

## Vendored-library licenses (already in this repo)

Two `LICENSE.*` files sit at the repo root, one per vendored flashcart backend:

| Vendored unit | License | Evidence |
|---|---|---|
| gba-flashcartio (Everdrive GBA X5 backend) | **MIT**, © 2024 Rodrigo Alfonso | [`LICENSE.gba-flashcartio:1-3`](../../LICENSE.gba-flashcartio) |
| ezfo-disc_io (EZ-Flash Omega backend) | **Apache License 2.0** | [`LICENSE.ezfo-disc_io:1-3`](../../LICENSE.ezfo-disc_io) |
| FatFs (Generic FAT FS module R0.15) | **FatFs license** (1-clause BSD-style, attribution-only) | `lib/fatfs/ff.c:5-17` |

Provenance notes that matter for attribution:

- The EZ-Flash Omega driver is derived from the official EZ-Flash kernel:
  `lib/ezflashomega/io_ezfo.c:8-9` points at
  `github.com/ez-flash/omega-de-kernel/.../Ezcard_OP.c`. The Apache-2.0 terms
  (notably the NOTICE/attribution requirements) apply to this backend.
- The Everdrive GBA X5 driver header is authored "krik" (krikzz), dated 2015:
  `lib/everdrivegbax5/everdrive.h:2-5`. Covered by the MIT
  `LICENSE.gba-flashcartio`.
- `lib/flashcartio.c:5-12` compile-gates both backends; whichever you build, its
  attribution travels with the binary.
- `source/gba_rtc.{c,h}` and `source/log.{c,h}` carry no upstream license header
  (`source/gba_rtc.h:1-14`, `source/log.h:1-20`) — they are first-party to this
  project. Add a license header when this repo picks its top-level license.

All three vendored licenses are permissive and mutually compatible; none is
copyleft. **MIT + Apache-2.0** is the natural license target for a new tool that
links them. Do not adopt GPL for a new tool unless you have *deliberately*
decided to pull in GPL code — and you have not, because nothing shippable here
is GPL.

See [./flashcart-io.md](./flashcart-io.md) and [./fatfs-fileops.md](./fatfs-fileops.md)
for the technical detail on these vendored backends.

---

## Per-project detail

**PKHeX (kwsch/PKHeX) — GPLv3, C#/.NET — SPEC ONLY.**
Repo `LICENSE` is the full GPLv3 text; the `PKHeX.Core` NuGet package declares
`GPL-3.0-or-later`. It is the most authoritative Gen 1–9 parser and *does*
contain Gen-3 (`SAV3`) and Battle-Tower / mixed-records logic relevant to record
mixing — but it is managed C# with no native code and cannot be compiled into or
linked against a devkitARM/bare-metal GBA binary. Two reasons it's spec-only:
(1) technical — no .NET runtime/GC/JIT on a GBA; (2) legal — porting C# to C is
translation, producing a GPLv3 derivative that would relicense the whole
toolkit. Use it to confirm offsets, checksums, and edge cases; author your own C.

**pret/pokeemerald, pokeruby, pokefirered — UNLICENSED — REFERENCE ONLY.**
Byte-exact ground truth for the struct/bitfield layout (`include/pokemon.h`), the
`EncryptBoxMon`/`DecryptBoxMon`/`CalculateBoxMonChecksum`/`GetSubstruct` mod-24
table (`src/pokemon.c`), and per-game `SaveBlock1`/`SaveBlock2` offsets
(`include/global.h`). But it is a decompilation of copyrighted code with **no
license file** — shipping it verbatim is not safe. Read → confirm → re-express
in clean C. **Pin every citation to an immutable commit hash**, e.g.
`github.com/pret/pokeemerald/blob/<sha>/src/pokemon.c`.

**savaughn/pksav — MIT, pure C — THE SHIPPABLE BASIS.**
Actively maintained (v2.2, May 2024) MIT fork of the archived `ncorgan/pksav`.
The load-bearing parts already operate on caller-provided in-memory buffers with
**no malloc and no file I/O**: block decrypt/unshuffle (`lib/gba/crypt.c`),
section checksums (`lib/gba/checksum.c`), and the struct headers
(`include/pksav/gba/pokemon.h`). The only porting work is replacing the thin
`fopen`/`fread`/`malloc` wrappers (`pksav_gba_save_load`/`_save`) with our own SD
sector reads (via `lib/flashcartio.c`) into a static buffer, then calling
`pksav_buffer_is_gba_save()` and the buffer-level functions directly. Vendor only
`lib/gba/` + `include/pksav/gba/` and the shared helpers they depend on. **Caveat:
US-region saves only** (regional string-layout differences); EU/JP needs your own
extension. High-value even if not fully adopted: diff our `gen3_save.c`
checksum/decrypt routines against pksav's to catch correctness bugs.

**libspec (Chase-san/libspec) — MIT, C11 — REFERENCE ONLY.**
Right idea (C11, MIT, Gen-3 RSE/FRLG + GB/DS, little-endian, GNU Make + modern C)
but the README explicitly warns it is **UNFINISHED, expect massive breaking
changes**. Usable as a secondary reference, not as a stable dependency.

**Avoid these (name confusion / unfit):**
- **ScoreUnder/pksv** — GPL-3.0 ROM *script* viewer/editor, not a save parser.
  The name collides with `pksav`; do not confuse them. Copyleft is incompatible
  with our permissive posture anyway.
- **PKMDS** — editor is C# (Unlicense); the C++ lib bundles SQLite + the veekun
  Pokedex DB and only partially covers Gen 3. Archived. Won't fit a GBA.
- **ads04r/Gen3Save** — correct read-only Gen-3 parser but 100% Python
  (Unlicense). Fine as a third cross-check fixture, not shippable C.

---

## Sources

**External (research spine):**
- PKHeX repo & LICENSE (GPLv3): https://github.com/kwsch/PKHeX/blob/master/LICENSE
- PKHeX.Core NuGet (GPL-3.0-or-later): https://www.nuget.org/packages/PKHeX.Core
- savaughn/pksav (MIT, maintained fork): https://github.com/savaughn/pksav
- ncorgan/pksav (MIT, archived): https://github.com/ncorgan/pksav
- pksav GBA crypt: https://raw.githubusercontent.com/ncorgan/pksav/master/lib/gba/crypt.c
- pksav GBA checksum: https://raw.githubusercontent.com/ncorgan/pksav/master/lib/gba/checksum.c
- pksav GBA save (buffer vs file I/O): https://raw.githubusercontent.com/ncorgan/pksav/master/lib/gba/save.c
- libspec (MIT, unfinished): https://github.com/Chase-san/libspec
- ScoreUnder/pksv (GPL-3.0 ROM script editor): https://github.com/ScoreUnder/pksv
- PKMDS (C#/C++ + SQLite): https://github.com/codemonkey85/PKMDS-Save-Editor
- ads04r/Gen3Save (Python, Unlicense): https://github.com/ads04r/Gen3Save
- pret/pokeemerald (unlicensed decomp, ground truth): https://github.com/pret/pokeemerald
- Bulbapedia, Pokemon data structure (Gen III): https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_data_structure_(Generation_III)
- Bulbapedia, Save data structure (Gen III): https://bulbapedia.bulbagarden.net/wiki/Save_data_structure_(Generation_III)

**Key source files in this repo:**
- `LICENSE.gba-flashcartio` (MIT, Everdrive backend)
- `LICENSE.ezfo-disc_io` (Apache-2.0, EZ-Flash Omega backend)
- `lib/fatfs/ff.c:5-17` (FatFs license header)
- `lib/ezflashomega/io_ezfo.c:8-9` (upstream provenance)
- `lib/everdrivegbax5/everdrive.h:2-5` (author/provenance)
- `lib/flashcartio.c:5-12` (backend compile-gating)
- `source/gba_rtc.h:1-14`, `source/log.h:1-20` (first-party, no upstream header)
