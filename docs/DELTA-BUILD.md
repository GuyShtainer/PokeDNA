# pokedna-delta.gba — the emulator build

A second build of PokeDNA for **Delta / RetroArch on a phone**, where there is no
flashcart and therefore no microSD.

**Build it only when asked** (Guy, 2026-07-29): `make delta` is not part of the normal
edit/test loop. Editing shared code and rebuilding the main `PokeDNA.gba` is unaffected.

```sh
make delta        # -> pokedna-delta.gba   (main build stays `make rebuild`)
```

## How it works

The SD build browses for a `.sav` on the card. This build has no filesystem, so instead it
reads and writes **its own 128 KiB flash save**. If you drop your Pokémon save in as this
ROM's save, PokeDNA is editing it directly.

```
Delta / RetroArch:
  1. add pokedna-delta.gba
  2. copy   "Pokemon Emerald.sav"  ->  "pokedna-delta.sav"
  3. edit on the phone, save in-app
  4. copy pokedna-delta.sav back over your Pokémon save
```

Save type must be **Flash 1Mbit (128K)**. The ROM carries a `FLASH1M_V103` marker so
VBA-M/mGBA auto-detect it — emulators pick save type by scanning the image for strings like
`SRAM_V` / `FLASH_V` / `FLASH1M_V`.

> ⚠️ **The marker must survive `--gc-sections`** (gba.specs enables it). It is anchored in
> `flashsave.c` by a `static const char* volatile` store. Note the placement of `volatile`:
> it qualifies the POINTER. Written as `volatile const char*` it is a pointer-to-volatile,
> the store gets optimised away, and the string silently vanishes from the image — which is
> exactly what happened on the first build. **Verify after every delta build:**
> ```sh
> python3 -c "print(hex(open('pokedna-delta.gba','rb').read().find(b'FLASH1M_V103')))"
> ```
> Anything other than a real offset means the emulator will give the ROM no save at all.

## What you lose

- **No backups.** The SD build writes a `.tmp`, byte-compares it, and keeps an immutable
  `.bak`. A single fixed save slot cannot do that. `flashsave_write()` erases, programs and
  then reads back and byte-compares the whole 128 KiB, and refuses to claim success if the
  compare fails — but if it does fail, the original is already gone. **Your own copy of the
  `.sav` is the backup.** Accepted by Guy.
- **No file browser** — there is only one save, so the tool boots straight into it.
- **No Map / teleport.** It reads a *second* file (your Pokémon ROM) off the SD, and an
  emulated GBA has no filesystem. The menu entry is compiled out entirely rather than left
  as a dead option (Guy: *"Just dont leave that in as an option"*).
- **No `.rec` battle export/import, no sprite streaming, no SD logs.**

Everything else works: box/party editing, frontier streaks, Fly destinations, trainer card,
bag, Pokédex, daycare, secret bases, clock fix.

## Implementation

| Piece | Where |
|---|---|
| Flash driver (bank switch, sector erase, byte program, verify) | `source/flashsave.{c,h}` |
| Build switch | `Makefile` — `PDNA_TARGET=delta` sets `-DPDNA_DELTA`, `PROJ=pokedna-delta`, ROM title `PokeDNADLT` |
| Boot straight into the flash save | `pdna_main.c` `main()`, `#ifdef PDNA_DELTA` |
| Load / commit swapped to flash | `pdna_main.c` `view_save()` and `app_save_finalize()` |
| Writes always allowed (no Omega gate) | `pdna_main.c` `app_can_edit()` |
| Map entry compiled out | `pdna_main.c` nav enum + labels + switch |

The Gen-3 cores (`gen3_*.c`) are untouched — they were already pure C with no I/O, which is
why this variant was cheap.

## Not yet verified

**Nothing here has been run in an emulator yet.** In particular: whether VBA-M's flash
implementation accepts this exact command sequence and bank-switch order, and whether
Delta's save-type auto-detection picks up the marker in a 12 MB ROM. Both are the first
things to check.
