# PokeDNA — Roadmap & Known Issues

The running list of what's planned and what's currently rough. PokeDNA is at **v1.0** and actively
developed; this is an honest backlog, not a promise of dates or order. If something here matters to
you, feel free to open an issue or a PR.

## Known issues / current bugs

- **Box wallpapers mostly don't render correctly.** Several PC box wallpapers display wrong or
  broken; only some work. Needs a pass over the wallpaper tile/palette decode.
- **The "move Pokémon" grab animation isn't quite right.** The pick-up / carry animation in the PC
  is rough and needs polish to match the real games.
- **Full-screen redraws cause flicker.** Several screens repaint everything on each change; moving
  more of them to partial redraws would make the whole app noticeably smoother (see *Smoother
  rendering* below).

## Planned — editor coverage

- **More editable flags & counters.** There are likely additional meaningful event flags / game
  counters not yet surfaced in the editor — keep expanding the curated set.
- **Full Pokédex editor**, plus **register edited/injected Pokémon in the Pokédex** (seen/caught) —
  right now an edited mon is not added to the dex.
- **Editable Poké Ball and met / caught location** (carried over from the v1.0 known gaps).
- **Secret Base editing** *(big one)* — edit your own base (location + contents/decorations) and,
  most importantly, **view other trainers' / "friends'" Secret Bases and edit the Pokémon teams
  inside them.**
- **Daycare viewer** — see the Pokémon currently in the Daycare.

## Planned — graphics & UI

- **Polish the Pokémon summary cards' UI/layout.**
- **Animated Pokémon sprites** — the per-species idle animation that plays on the summary card and
  in battle.
- **Back-sprite view** — show the rear view of a Pokémon in the summary.
- **Smoother rendering** — reduce full-screen refreshes app-wide in favour of partial redraws.

## Planned — file & save management

- **Bank → "Copy to game".** In the bank, replace the redundant *Export .pk* action with a
  **Copy-to-game** option — the mon there is already exported, so what you actually want is to send
  it into the loaded save.
- **Backup management.** Option to **skip backups** or **overwrite a single rolling backup** so they
  don't pile up, and/or a **"Clear backups"** action to delete all backups once you've confirmed a
  save is good.
- **Built-in file operations.** There's no OS file browser on the cartridge, so duplicating a save,
  renaming it, or deleting backups currently means switching to a separate SD browser. Fold the
  essential file ops in (at minimum: duplicate save, rename, delete backups). TBD whether to bring
  the *full* file-browser feature set or just the essentials.

## Testing / validation

- **Cross-game bank only tested Emerald → Emerald.** Verify the bank and cross-game transfers with
  **Ruby/Sapphire** and **FireRed/LeafGreen**, including moving Pokémon **between** different games.

---

*PokeDNA is unofficial and not affiliated with Nintendo / Game Freak / The Pokémon Company. See the
[README](README.md) for the full disclaimer and license (GPLv3).*
