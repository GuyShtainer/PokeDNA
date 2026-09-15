#ifndef ROM_MAP_H
#define ROM_MAP_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Gen-3 overworld map data, read live out of the USER'S OWN Pokemon ROM.
 *
 * PokeDNA ships NO Nintendo map data. The map viewer opens the .gba file the user
 * already has on the same microSD they play from, and parses the map structures out
 * of it at runtime. That keeps the ROM size unchanged and means nothing copyrighted
 * is ever redistributed. Nothing in here decrypts, defeats a header check, or
 * bypasses any protection — it is a plain read of a plaintext file.
 *
 * ---- host-testability -------------------------------------------------------
 * All I/O goes through a caller-supplied RomReadFn. On the GBA that is FatFs
 * (f_lseek + f_read, under the OS-mode rules); on the PC it is fread. So the exact
 * same parser is exercised by tests/host_rommap_test.c against real retail ROMs.
 * This file therefore includes NO tonc, NO FatFs and NO GBA headers.
 *
 * ---- addresses --------------------------------------------------------------
 * Gen-3 has no runtime-discoverable pointer to gMapGroups, so its address is a
 * per-version constant (from pret's byte-matching .sym files, cross-checked against
 * the Advance Map / PGE "map bank table" address the romhacking community has used
 * for twenty years). Everything else is reached by following pointers from there.
 *
 * ---- the per-game divergences that WILL bite -------------------------------
 * FireRed/LeafGreen are not "Emerald with different addresses". They differ in four
 * separate ways, and a parser that assumes the Emerald layout reads NULL attribute
 * pointers and renders garbage:
 *   - MapHeader tail: RSE has u8 filler[2] at 0x18; FRLG has flags at 0x19 and
 *     s8 floorNum at 0x1A instead.
 *   - MapLayout is 24 bytes on RSE, 28 on FRLG (borderWidth/borderHeight at 0x18).
 *   - Tileset: RSE has metatileAttributes at 0x10 and callback at 0x14; FRLG has
 *     them SWAPPED, and its attributes are u32 (4 bytes each), not u16.
 *   - NUM_METATILES_IN_PRIMARY is 512 on RSE but 640 on FRLG (likewise tiles, and
 *     6 vs 7 palettes in the primary tileset).
 * All of that is captured in RomFmt so the readers stay branch-free.
 */

#define ROM_BASE 0x08000000u        /* GBA cart window; file offset = addr - this */

/* Read `len` bytes at FILE offset `off` into dst. Returns false on any short read.
 * The GBA implementation must respect the OS-mode rule (no ROM access / rendering
 * while the transfer is in flight). */
typedef bool (*RomReadFn)(void* ctx, uint32_t off, void* dst, uint32_t len);

typedef enum {
  ROM_NONE = 0,
  ROM_EMERALD,          /* BPEE rev 0            */
  ROM_RUBY,             /* AXVE rev 0/1/2        */
  ROM_SAPPHIRE,         /* AXPE rev 0/1/2        */
  ROM_FIRERED,          /* BPRE rev 0/1          */
  ROM_LEAFGREEN,        /* BPGE rev 0/1          */
} RomKind;

/* ---- ROM-hack detection (BACKLOG #54, tier T0) ------------------------------
 * The reader tables above are address-pinned to 11 retail builds; a binary hack that
 * keeps the same (code, version) pair and the same 16 MiB size but relocates internal
 * tables used to register as that retail game, silently. RomIdent is the honest
 * verdict rom_open() now derives from the 0xC0 header it already reads, at zero extra
 * I/O cost (no whole-ROM hash, no fingerprint window — see rom_identify()'s comment
 * for why that is out of scope here; T2's per-hack SD profile is the follow-on that
 * would let a verified hack unlock editing, and stays in BACKLOG). None of this loosens
 * rom_open()'s refusal of anything but a bit-identical retail build: T1's honesty is
 * limited to WHAT gets reported and WHETHER the save opens read-only, not to widening
 * what rom_open() accepts. */
typedef enum {
  ROM_ID_RETAIL = 0,    /* code+version pinned, size == 16 MiB, title matches retail */
  ROM_ID_HACK,          /* pinned code+version but title/size/structure diverges, OR
                          * no pin match but the title starts "POKEMON" (base unknown) */
  ROM_ID_NOT_POKEMON,   /* a GBA ROM, but neither a pinned build nor "POKEMON..." titled */
  ROM_ID_NOT_GBA,       /* hdr[0xB2] != 0x96 — not even a GBA cart image               */
} RomIdent;

/* Per-game structural constants (the FRLG divergences above). */
typedef struct {
  uint8_t  layout_size;        /* 24 RSE / 28 FRLG                                */
  uint8_t  ts_attr_off;        /* Tileset.metatileAttributes: 0x10 RSE / 0x14 FRLG */
  uint8_t  ts_cb_off;          /* Tileset.callback:           0x14 RSE / 0x10 FRLG */
  uint8_t  attr_bytes;         /* bytes per metatile attribute: 2 RSE / 4 FRLG     */
  uint16_t metatiles_primary;  /* 512 RSE / 640 FRLG                               */
  uint16_t tiles_primary;      /* 512 RSE / 640 FRLG                               */
  uint8_t  pals_primary;       /* 6 RSE / 7 FRLG                                   */
  uint8_t  frlg_header;        /* 1 = MapHeader has floorNum instead of filler[2]  */
} RomFmt;

#define ROM_MAX_GROUPS 48

typedef struct {
  RomReadFn read;
  void*     ctx;
  uint32_t  size;              /* file size in bytes                               */
  RomKind   kind;
  uint8_t   version;           /* header revision byte at 0xBC                     */
  char      code[5];           /* 4-char game code at 0xAC, NUL-terminated         */
  char      title[13];         /* 12-char internal title at 0xA0                   */
  RomFmt    fmt;

  uint32_t  map_groups;        /* ROM address of gMapGroups                        */
  uint32_t  map_layouts;       /* ROM address of gMapLayouts                       */
  uint32_t  gfx_info_ptrs;     /* gObjectEventGraphicsInfoPointers                 */
  uint8_t   group_count;
  uint8_t   ident;              /* RomIdent — the hack/retail verdict (BACKLOG #54).
                                  * Placed here, not appended at the tail: 3 bytes of
                                  * padding already sit between group_count and the
                                  * 4-aligned group_ptr[] below, so this byte spends
                                  * padding rather than growing sizeof(RomCtx) — see
                                  * the _Static_assert beside the struct's close brace. */
  uint32_t  group_ptr[ROM_MAX_GROUPS];   /* gMapGroups[i]                          */
  uint16_t  group_size[ROM_MAX_GROUPS];  /* derived from consecutive differences    */
  uint16_t  total_maps;
} RomCtx;

/* The padding argument above holds regardless of pointer width (both `read` and `ctx`
 * are already aligned to their own size, so the fields between them and group_count
 * shift together with no net change to the gap before group_ptr[]) — but pointer width
 * itself differs: 4 bytes on the GBA target, 8 on every host build that compiles this
 * header for tests/host_romident_test.c and friends. Measured on main before this
 * lane (both values unchanged by adding `ident`): 352 on a 32-bit-pointer target, 368
 * on a 64-bit-pointer host. */
#if UINTPTR_MAX == 0xFFFFFFFFu
_Static_assert(sizeof(RomCtx) == 352, "RomCtx grew on the 32-bit (GBA) target");
#else
_Static_assert(sizeof(RomCtx) == 368, "RomCtx grew on the 64-bit (host) target");
#endif

/* ---- structures, already byte-swapped and per-game-normalised --------------- */

typedef struct {
  uint32_t layout;             /* -> RomLayout                                     */
  uint32_t events;             /* -> MapEvents (may be 0, and may be SHARED)       */
  uint32_t scripts;
  uint32_t connections;        /* 0 for most maps (only 64 of Emerald's 519)       */
  uint16_t music;
  uint16_t layout_id;          /* 1-based index into gMapLayouts                   */
  uint8_t  mapsec;             /* regionMapSectionId -> the town/route name        */
  uint8_t  cave;
  uint8_t  weather;
  uint8_t  map_type;
  uint8_t  flags;              /* bit0 cycling, bit1 escaping, bit2 running        */
  uint8_t  battle_type;
  uint8_t  floor_num;          /* FRLG only; 0 elsewhere                           */
} RomMapHeader;

typedef struct {
  int32_t  width, height;      /* in metatiles ("blocks")                          */
  uint32_t border;             /* u16[borderW*borderH]                             */
  uint32_t blocks;             /* u16[width*height], row-major, UNCOMPRESSED       */
  uint32_t tileset_primary;
  uint32_t tileset_secondary;
  uint8_t  border_w, border_h; /* FRLG stores these; RSE is fixed 2x2              */
} RomLayout;

typedef struct {
  uint8_t  compressed;         /* tiles are LZ77 when set                          */
  uint8_t  secondary;
  uint32_t tiles;              /* 4bpp char data                                   */
  uint32_t palettes;           /* 16 palettes x 16 BGR555 entries                  */
  uint32_t metatiles;          /* 8 u16 per metatile                               */
  uint32_t attributes;         /* 1 u16 (RSE) or u32 (FRLG) per metatile           */
} RomTileset;

typedef struct {
  uint8_t  object_count, warp_count, coord_count, bg_count;
  uint32_t objects, warps, coords, bgs;
} RomMapEvents;

/* ObjectEventTemplate — 24 bytes. These are ROM TEMPLATES (where an NPC spawns),
 * not live state: the game's runtime positions live in RAM and are not in the save.
 * An NPC is present iff its flag is CLEAR; flag_id == 0 means always present. */
typedef struct {
  uint8_t  local_id, graphics_id, kind;
  int16_t  x, y;
  uint8_t  elevation, movement_type, range_x, range_y;
  uint16_t trainer_type, sight;
  uint32_t script;
  uint16_t flag_id;
} RomObjectEvent;

/* BgEvent — 12 bytes. Signs, hidden items, and SECRET BASE ENTRANCES.
 * kind 8 (BG_EVENT_SECRET_BASE) carries the secretBaseId in `param`, which is what the
 * game itself keys on (SetCurSecretBaseIdFromPosition). Do NOT identify entrances by
 * metatile behaviour: the RSE behaviour ids 0x90..0x9D mean something completely different
 * in FRLG (food, posters, windows...), and a behaviour scan finds 596 false hits per FRLG
 * ROM against 0 real entrances. */
#define ROM_BG_SECRET_BASE 8
typedef struct {
  int16_t  x, y;
  uint8_t  elevation;
  uint8_t  kind;
  uint32_t param;              /* kind 8: secretBaseId                             */
} RomBgEvent;

typedef struct {
  uint8_t  warp_id;
  int16_t  x, y;
  uint8_t  elevation;
  uint8_t  dest_warp_id, dest_map, dest_group;
} RomWarp;

/* MapConnection — how the outdoor maps stitch into one world.
 * direction: 1 south, 2 north, 3 west, 4 east, 5 dive, 6 emerge. */
#define ROM_CONN_SOUTH  1
#define ROM_CONN_NORTH  2
#define ROM_CONN_WEST   3
#define ROM_CONN_EAST   4
#define ROM_CONN_DIVE   5
#define ROM_CONN_EMERGE 6
typedef struct {
  uint32_t direction;
  int32_t  offset;             /* in metatiles, along the shared edge              */
  uint8_t  group, num;
} RomConnection;

/* Blockdata cell bit split (include/global.fieldmap.h). */
#define ROM_CELL_METATILE(c)  ((uint16_t)((c) & 0x03FFu))
#define ROM_CELL_COLLISION(c) ((uint8_t)(((c) >> 10) & 0x3u))
#define ROM_CELL_ELEVATION(c) ((uint8_t)(((c) >> 12) & 0xFu))
#define ROM_MAPGRID_UNDEFINED 0x03FFu

/* ---- API ------------------------------------------------------------------- */

/* Identify the ROM and fill *c (including the derived per-group map counts).
 * `size` is the file's byte length. Returns false for anything but a retail build
 * (`c->ident != ROM_ID_RETAIL`) — a ROM hack that relocated the tables, a truncated
 * dump, a wrong-region build, or a structurally-intact hack the sanity walk cannot
 * see (rule 1c downgrades that case from RETAIL to ROM_ID_HACK after the walk).
 * `c->kind`/`c->ident` are set BEFORE every return, including the false ones, so a
 * caller can still attribute a HACK verdict to the base game it impersonates — `kind`
 * is no longer ROM_NONE on that path. A false return must be reported to the user,
 * never worked around. */
bool rom_open(RomCtx* c, RomReadFn read, void* ctx, uint32_t size);

/* Pure classification, no I/O: given the 0xC0 header bytes rom_open() already read
 * and the file size, which of the four RomIdent verdicts applies (decision 1 in
 * BACKLOG #54's brief), and — for a pinned (code, version) match, retail or hack —
 * which RomKind it impersonates. `*base_kind` is ROM_NONE for ROM_ID_NOT_GBA,
 * ROM_ID_NOT_POKEMON, and the "unknown base" flavour of ROM_ID_HACK (rule 1d): a
 * hack whose header doesn't even match a pinned code+version cannot be attributed to
 * a specific game, and guessing one would risk locking a retail save of that game. */
RomIdent rom_identify(const uint8_t hdr[0xC0], uint32_t size, RomKind* base_kind);

/* Raw read at a ROM ADDRESS (0x08xxxxxx), bounds-checked against the file. */
bool rom_read_at(const RomCtx* c, uint32_t addr, void* dst, uint32_t len);

/* True iff addr is a plausible pointer into this ROM image. Every pointer followed
 * out of ROM data is checked with this — a corrupt/hacked table must fail cleanly,
 * never send a read off into nowhere. */
bool rom_ptr_ok(const RomCtx* c, uint32_t addr);

int  rom_maps_in_group(const RomCtx* c, int group);      /* -1 if group invalid    */

bool rom_map_header(const RomCtx* c, int group, int num, RomMapHeader* out);
bool rom_layout(const RomCtx* c, uint32_t layout_addr, RomLayout* out);
bool rom_tileset(const RomCtx* c, uint32_t ts_addr, RomTileset* out);
bool rom_map_events(const RomCtx* c, uint32_t events_addr, RomMapEvents* out);
bool rom_object_event(const RomCtx* c, const RomMapEvents* ev, int i, RomObjectEvent* out);
bool rom_warp(const RomCtx* c, const RomMapEvents* ev, int i, RomWarp* out);
bool rom_bg_event(const RomCtx* c, const RomMapEvents* ev, int i, RomBgEvent* out);

/* Connections are a { s32 count; const MapConnection *list; } pair. Returns the
 * count (0 when the map has none, which is the common case). */
int  rom_connection_count(const RomCtx* c, uint32_t connections_addr);
bool rom_connection(const RomCtx* c, uint32_t connections_addr, int i, RomConnection* out);

/* Read `n` blockdata cells starting at cell index `first` (row-major). */
bool rom_blocks(const RomCtx* c, const RomLayout* l, uint32_t first, uint16_t* dst, uint32_t n);

/* One metatile's 8 tilemap entries (2 layers x 4 tiles). Resolves primary vs
 * secondary tileset by metatile id, using the per-game NUM_METATILES_IN_PRIMARY. */
bool rom_metatile(const RomCtx* c, const RomLayout* l, uint16_t metatile_id, uint16_t out[8]);

/* Metatile layer types (how the two 4-tile halves map onto background layers).
 * For a FLAT top-down render all three composite identically — measured across every
 * Emerald metatile, the bottom half is opaque in all but 0.6% of COVERED tiles — so the
 * layer type only decides which BG numbers the halves occupy, i.e. how sprites
 * interleave with them. It matters for the Mode-0 BG assignment, not for the pixels. */
#define ROM_LAYER_NORMAL  0
#define ROM_LAYER_COVERED 1
#define ROM_LAYER_SPLIT   2

/* One metatile's attributes, NORMALISED across the per-game encodings.
 *
 * These are stored completely differently and the difference is invisible unless you
 * check (MEASURED over every attribute in both ROMs):
 *   RSE  = u16: behavior bits 0-7,  layer type bits 12-15  (bits 29-30 always 0)
 *   FRLG = u32: behavior bits 0-8,  layer type bits 29-30  (bits 12-15 always 0)
 * Reading FRLG's u32 and truncating to u16 — the obvious thing — silently reports
 * layer type NORMAL for all 10,155 FireRed metatiles. */
typedef struct {
  uint32_t raw;        /* the word as stored (u16-widened on RSE)                  */
  uint16_t behavior;   /* tall grass / water / door / warp / ... (game-specific id) */
  uint8_t  layer_type; /* ROM_LAYER_*                                              */
} RomMetatileAttr;

bool rom_metatile_attr(const RomCtx* c, const RomLayout* l, uint16_t metatile_id,
                       RomMetatileAttr* out);

/* ---- connection geometry ----------------------------------------------------
 * Where the neighbour's cell (0,0) sits, expressed in the CURRENT map's local frame:
 *     SOUTH (off,  H)    NORTH (off, -Hn)    WEST (-Wn, off)    EAST (W, off)
 * so a current-map cell maps to the neighbour by SUBTRACTING the origin.
 *
 * NOTE WHICH SIZE EACH CASE USES: SOUTH/EAST take the CURRENT map's H/W, NORTH/WEST take
 * the NEIGHBOUR's Hn/Wn. Mixing those up is invisible whenever the two maps happen to be
 * the same size, which on the Hoenn water routes is most of the time.
 *
 * Transcribed from pret/pokeemerald src/fieldmap.c Fill{South,North,West,East}Connection. */
bool rom_conn_origin(int dir, int32_t off, int32_t w, int32_t h,
                     int32_t nb_w, int32_t nb_h, int32_t* ox, int32_t* oy);

/* Does the neighbour actually cover this position along the shared edge? Uses the CLEAN
 * containment test (pokeemerald IsCoordInConnectingMap), not the sloppy inclusive one the
 * game uses at a crossing, so the cell the cursor stands on and the map it hands off to
 * can never disagree. `along` is x for NORTH/SOUTH, y for WEST/EAST. */
bool rom_conn_contains(int dir, int32_t off, int32_t nb_w, int32_t nb_h, int32_t along);

/* ---- warp destinations ------------------------------------------------------ */
enum { ROM_WARP_OK = 0, ROM_WARP_DYNAMIC, ROM_WARP_DUMMY, ROM_WARP_BAD_MAP,
       ROM_WARP_UNREADABLE };

/* Resolve where warp `w` lands, the way pokeemerald's SetWarpDestination +
 * SetPlayerCoordsFromWarp do. The player lands ON the destination warp tile; stepping out
 * of a door is a separate cosmetic task in the game and is deliberately not baked in here.
 * Returns one of the ROM_WARP_* codes; only ROM_WARP_OK fills the outputs. */
int rom_warp_dest(const RomCtx* c, const RomWarp* w,
                  uint8_t* out_group, uint8_t* out_num, int32_t* out_x, int32_t* out_y);

/* Find a map belonging to a region-map SECTION (mapsec). The region map knows only
 * sections — "Slateport City" — while the tile view needs a concrete (group, num). This
 * is what lets zooming in from the region map land where the cursor is pointing.
 *
 * OUTDOOR maps are preferred (town/city/route/ocean): a section's first map by index is
 * often an interior, and dropping the user inside a random house when they pointed at a
 * city is not what they asked for. Falls back to any map with that section.
 *
 * Walks the map headers, so it does SD reads — call it with the screen blanked or behind
 * a loading frame, never from a render loop. Returns false if no map claims the section
 * (real: several sections exist only as region-map artwork). */
bool rom_find_map_by_mapsec(const RomCtx* c, uint8_t mapsec, uint8_t* out_group, uint8_t* out_num);

/* Human-readable, for the "which ROM is this?" screen. */
const char* rom_kind_name(RomKind k);

#endif /* ROM_MAP_H */
