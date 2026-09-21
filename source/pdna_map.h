#ifndef PDNA_MAP_H
#define PDNA_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/* Overworld map screen. Reads map data live out of the user's OWN Pokemon ROM on the
 * microSD (PokeDNA ships no Nintendo map data), cross-referenced with the open save
 * for the player's position and NPC visibility flags.
 *
 * `sb1`/`sb2` are the open save's blocks (sb1 for the player position and event
 * flags; sb2 for the teleport's specialSaveWarpFlags). B returns. */
void pdna_map(uint8_t* sb1, uint8_t* sb2, PkGame game);

/* ---- the ONE file browser (BACKLOG #186) ---------------------------------------
 * The launch (.sav) browser and the three pickers below (app_pick_rom/app_pick_gb_save/
 * app_pick_gb_rom) are the SAME code now: same chrome, same sort/filter/hidden toggles,
 * same file ops, same detail line and footer -- only the title, the extension filter,
 * the remembered-folder config key, and whether the START menu's Verify-ROM/Reboot
 * rows apply differ per kind. `browse_pick_spec()` (implemented in pdna_main.c, which
 * owns the shared chrome + g_cwd/g_sort/... state) is the one core; pdna_main.c's own
 * private browse_pick() is just its ".sav" instance. */

#define BR_NAME_MAX 64   /* one strncpy cap for every entry's FatFs LFN, every kind    */

typedef struct {
  char     name[BR_NAME_MAX];
  uint32_t size;                  /* file size in bytes (0 for folders)              */
  uint32_t dosdt;                 /* (fdate<<16)|ftime, for the date sort            */
  bool     is_dir;
} BrowseEntry;

typedef enum {
  BR_MATCH_SAV,      /* has_sav_ext(): any name CONTAINING ".sav" (backups included) */
  BR_MATCH_SUFFIX,   /* plain case-insensitive suffix match against `exts`           */
} BrMatchMode;

/* Every field is the CALLER's: `entries`/`cap` size the listing buffer (A3 -- the
 * launch browser hands in its own resident g_entries/MAX_ENTRIES; the three pdna_map.c
 * pickers below hand in an arena- or mon_decomp-borrowed buffer, because g_entries is
 * ALSO used as box_oam.c's icon pose-swap cache and the GB reconcile/migration
 * scratch (app_box_swap_acquire, pdna_app.h) whenever a box screen or GB session is
 * live -- exactly the window app_pick_rom/app_pick_gb_save/app_pick_gb_rom are
 * reachable from, so they may NOT alias it). `exts` is a NULL-terminated suffix list,
 * unused when match_mode is BR_MATCH_SAV. */
typedef struct {
  const char*        title;          /* e.g. "Pick .sav" -- the browser's title prefix   */
  const char*        filter_label;   /* e.g. ".sav" -- the status line's filter tag       */
  BrMatchMode        match_mode;
  const char* const* exts;
  const char*        cfg_key;        /* "dir" | "dir_rom" | "dir_gb" | "dir_gbsav"        */
  bool                menu_extra;    /* Verify ROM / Reboot rows in the START menu         */
  BrowseEntry*        entries;
  int                 cap;
} BrowseSpec;

bool browse_pick_spec(const BrowseSpec* spec, char* out, int cap);

/* Browse the SD for a .gba (the map's own picker, arena-backed). Returns true with
 * the full path in out. Fails (false) when the arena is unavailable — i.e. unsaved
 * box moves are pending — as well as on cancel. Used by Settings > Game ROM. */
bool app_pick_rom(char* out, int out_cap);
/* The same browser filtered to Game Boy battery files (.sav/.srm) for GB import. */
bool app_pick_gb_save(char* out, int out_cap);
/* The same browser filtered to Game Boy cartridge dumps (.gb/.gbc) — the runtime source
 * of Gen-1/2 sprite art, so an imported mon can wear the art of the game it came from.
 * The extension only narrows the list; the caller must identify the ROM by its header. */
bool app_pick_gb_rom(char* out, int out_cap);

#endif /* PDNA_MAP_H */
