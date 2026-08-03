#ifndef GEN3_SBMAP_H
#define GEN3_SBMAP_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Secret bases on the overworld map: where the doors are, and which room each opens.
 *
 * ---- how the game actually does it ------------------------------------------
 * A secret-base entrance is a BG EVENT of kind 8 (ROM_BG_SECRET_BASE) whose parameter IS
 * the secretBaseId. That is what SetCurSecretBaseIdFromPosition keys on. The tile's
 * metatile BEHAVIOUR (0x90..0x9D: tree / shrub / red-brown-yellow-blue cave) is only the
 * VISUAL, and must never be used to find entrances — those same behaviour ids mean food,
 * posters, telephones and windows in FireRed/LeafGreen, where a behaviour scan finds 596
 * false hits against 0 real entrances.
 *
 * MEASURED across the real ROMs: exactly 75 entrances on 13 maps, and the sets are
 * BYTE-IDENTICAL in Emerald, Ruby and Sapphire (same ids, maps, coordinates and tiles).
 * FRLG has none at all — secret bases are an RSE feature, so gate the whole thing on RSE.
 *
 * ---- which room ---------------------------------------------------------------
 * secretBaseId / 10 indexes a 24-entry table of interior maps, all in map group 25.
 * The order is NOT sequential (it is column-major over 6 colours x 4 sizes), and it is
 * the same in Ruby and Emerald. Do NOT derive the interior from the entrance's colour:
 * 9 of the 75 entrances genuinely mismatch (id 33 sits on a yellow cave tile but opens
 * RedCave4), because the decomp's constant names are aspirational and id/10 is what the
 * game uses. Several ids also share one room — the room repeats, the decorations differ.
 */

#define SB_INTERIOR_GROUP 25
#define SB_MAX_ENTRANCE   16       /* MEASURED max on one map = 13 (Route 119) */

/* The interior map number in group 25 for a secretBaseId, or -1 if out of range. */
int  sbmap_interior(uint8_t base_id);

/* The entrance tile's "opened" metatile for its closed form, or 0 if `closed` is not one
 * of the seven entrance tiles. This is the swap the game performs at map load for every
 * OCCUPIED base (SetOccupiedSecretBaseEntranceMetatiles) — it is what turns a plain shrub
 * into a shrub with a tunnel mouth. */
uint16_t sbmap_open_metatile(uint16_t closed);

/* VAR_CURRENT_SECRET_BASE (0x4054) — the game's own answer to "which base am I in".
 * It holds the RECORD INDEX (0..19), not the secretBaseId, and it is the only thing
 * InitSecretBaseAppearance() uses to pick decorations, so reading it makes a viewer agree
 * with the game byte for byte.
 *
 * It is NEVER cleared — Guy's Ruby save is standing on map 4.6 and still reads 19, a stale
 * empty slot — so it is only meaningful when the save's own location really is a group-25
 * map. sbmap_cur_base_index() applies that guard and returns -1 otherwise. */
int sbmap_cur_base_index(const uint8_t* sb1, int pk_game);

/* True if this game family has secret bases at all (RSE yes, FRLG no). */
bool sbmap_supported(int pk_game);

#endif /* GEN3_SBMAP_H */
