/*
 * Player teleport — see gen3_warp.h for why this writes continueGameWarp rather
 * than pos/location, and for the coordinate convention.
 *
 * Pure C — no tonc, no GBA headers — so tests/host_warp_test.c runs it on the PC.
 */
#include <string.h>
#include "gen3_warp.h"

static void wr16(uint8_t* p, int16_t v) {
  p[0] = (uint8_t)((uint16_t)v & 0xFF);
  p[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFF);
}
static int16_t rd16s(const uint8_t* p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }

/* Emerald map groups whose contents are driven by save state a teleporting player
 * will not have. Group 25 holds the 24 secret bases (n 0..23) and the link rooms;
 * 26 is the Battle Frontier proper; 27 the Battle Pyramid floors; 28 Trainer Hill.
 * These are advisory — the UI warns, it does not forbid, because Guy explicitly
 * wants to be able to go anywhere. */
bool g3warp_risky_map(PkGame game, uint8_t group, uint8_t num) {
  (void)num;
  if (game != PK_EMERALD) return false;
  return group == 25 || group == 26 || group == 27 || group == 28;
}

int g3warp_check(const G3MapBounds* b, const G3Warp* w) {
  if (!b || !w) return G3W_BAD_MAP;
  /* An unknown (group,num) must NEVER be written: the game dereferences
   * gMapGroups[group][num] with no bounds check at all. */
  if (!b->known || b->group != w->group || b->num != w->num) return G3W_BAD_MAP;
  if (b->width == 0 || b->height == 0) return G3W_BAD_MAP;
  /* "Centre of map" is always in bounds by construction, so it skips the range
   * check — that is precisely why it is the safe fallback we offer on failure. */
  if (!w->centre) {
    if (w->x < 0 || w->y < 0) return G3W_OUT_OF_BOUNDS;
    if ((uint16_t)w->x >= b->width || (uint16_t)w->y >= b->height) return G3W_OUT_OF_BOUNDS;
  }
  return G3W_OK;
}

bool g3warp_apply(uint8_t* sb1, uint8_t* sb2, PkGame game, const G3Warp* w) {
  if (!sb1 || !sb2 || !w) return false;

  uint8_t* d = sb1 + G3W_CONTINUE_OFF;
  d[0] = w->group;
  d[1] = w->num;
  d[2] = G3W_WARP_ID_NONE;    /* -1: use the x/y below, not a warp-event index */
  d[3] = 0;                   /* pad */
  if (w->centre) {
    /* SetPlayerCoordsFromWarp: a negative coord means "centre of the map", which is
     * always in bounds whatever the destination's size. */
    wr16(d + 4, (int16_t)G3W_CENTRE);
    wr16(d + 6, (int16_t)G3W_CENTRE);
  } else {
    wr16(d + 4, w->x);
    wr16(d + 6, w->y);
  }

  /* Ruby/Sapphire compare the WHOLE byte with == 1 (overworld.c), so an
   * Emerald-style |= on a save whose byte held something else would silently not
   * warp. Emerald/FRLG treat it as a bitfield and must not clobber the other bits
   * (POKECENTER_SAVEWARP etc.). */
  if (game == PK_RS) sb2[G3W_SPECIAL_WARP_OFF] = G3W_CONTINUE_BIT;
  else               sb2[G3W_SPECIAL_WARP_OFF] |= G3W_CONTINUE_BIT;
  return true;
}

bool g3warp_get_pending(const uint8_t* sb1, const uint8_t* sb2, PkGame game, G3Warp* out) {
  if (!sb1 || !sb2 || !out) return false;
  bool armed = (game == PK_RS) ? (sb2[G3W_SPECIAL_WARP_OFF] == G3W_CONTINUE_BIT)
                               : ((sb2[G3W_SPECIAL_WARP_OFF] & G3W_CONTINUE_BIT) != 0);
  const uint8_t* d = sb1 + G3W_CONTINUE_OFF;
  out->group  = d[0];
  out->num    = d[1];
  out->x      = rd16s(d + 4);
  out->y      = rd16s(d + 6);
  out->centre = (out->x < 0 || out->y < 0);
  return armed;
}

bool g3warp_get_current(const uint8_t* sb1, G3Warp* out) {
  if (!sb1 || !out) return false;
  const uint8_t* loc = sb1 + G3W_LOCATION_OFF;
  out->group  = loc[0];
  out->num    = loc[1];
  out->x      = rd16s(sb1 + G3W_POS_OFF);      /* live position, not the warp's x/y */
  out->y      = rd16s(sb1 + G3W_POS_OFF + 2);
  out->centre = false;
  return true;
}

void g3warp_snapshot(const uint8_t* sb1, const uint8_t* sb2, uint8_t out[G3W_SNAPSHOT_SIZE]) {
  if (!sb1 || !sb2 || !out) return;
  memcpy(out, sb1, 0x34);                       /* the whole location block */
  out[0x34] = sb2[G3W_SPECIAL_WARP_OFF];
}

bool g3warp_restore(uint8_t* sb1, uint8_t* sb2, const uint8_t snap[G3W_SNAPSHOT_SIZE]) {
  if (!sb1 || !sb2 || !snap) return false;
  memcpy(sb1, snap, 0x34);
  sb2[G3W_SPECIAL_WARP_OFF] = snap[0x34];
  return true;
}
