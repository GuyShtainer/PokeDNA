# gba-pokeviewer

A **PKHeX-for-the-GBA**: a homebrew tool that runs *on the cartridge* (EZ-Flash Omega DE,
EverDrive GBA X5), reads the flashcart microSD, and shows your Generation-III Pokémon saves
(Ruby, Sapphire, Emerald, FireRed, LeafGreen) the way the games never did — the PC boxes and
party with a pixel-accurate UI, plus all the **hidden data**: IVs, the full per-stat EV spread
(and EV sum), nature, ability, shininess, *computed* stats, and trainer/Game-Record stats
(badges, TID/SID, play time, Pokédex seen/caught, money, battle counts).

Read-only first; **edit mode** comes later (gated, Omega-only, behind an immutable backup and a
verified-write round-trip). Built on the proven `gba-toolkit` foundation and the record-mixer's
clean-room Gen-3 save core.

> Note: Gen-3 saves do **not** store a "time to defeat the Elite Four" — the Hall of Fame keeps
> the team but no timestamp. That one stat is unrecoverable; everything else listed above is read.

## Status

Early scaffold (milestone **M0**): builds, mounts the SD, lists `.sav` files, and shows a basic
parse summary. The full hidden-data viewer, PC boxes, trainer/stats screen, sprites, and edit mode
land in M1–M5 (see `docs/PLAN.md` if present, or the project plan).

## Building

Requires devkitPro `gba-dev` (libtonc), or just Docker:

```sh
./build.sh        # Docker (no local toolchain needed)
# or
make rebuild      # local devkitPro
```

Output: `pokeviewer.gba`. Copy it to the flashcart SD next to your `.sav` files and run it.

### Pokémon icons (local, not in the repo)

The box-icon art is **not** committed (it is ripped Game Freak art). To enable in-game icons,
drop the Gen-3 sprite pack under `assets/sprites/Gen 3 Sprite Pack V1/` and regenerate the blob:

```sh
python3 tools/gen_icons.py      # needs Pillow (pip install pillow)
```

This writes `source/mon_icons.{c,h}` (also git-ignored). Without it the tool falls back to text.

## Host tests

The save-format core is pure C and dual-compiles on the PC. Drop real `.sav` files into
`tests/fixtures/` and run the host harness (see `tests/host_test.c`). No hardware needed.

## Licensing

Original source is **MIT** (`LICENSE`). Vendored libs keep their own licenses (flashcartio MIT,
ezfo Apache-2.0, FatFs BSD-1). Pokémon sprite art and pret decomp excerpts are **not** part of
this repository — see `LICENSE` and `.gitignore`. All save-format logic is clean-room.
