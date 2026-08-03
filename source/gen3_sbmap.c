/*
 * Secret-base map tables. Pure C, no tonc, no GBA headers — see gen3_sbmap.h for why the
 * entrance lookup is a BG-event read and never a metatile-behaviour scan.
 */
#include "gen3_sbmap.h"
#include "gen3_trainer.h"      /* PkGame */

/* sSecretBaseEntrancePositions (pokeemerald) == gUnknown_083D1374 (pokeruby), identical.
 * Index = secretBaseId / 10; value = map number within group 25. Column-major over the
 * six entrance colours and four room sizes, which is why it looks scrambled. VERIFIED
 * against both decomps and against the layouts in the real Emerald and Ruby ROMs. */
static const uint8_t k_interior[24] = {
   0,  6, 12, 18,
   1,  7, 13, 19,
   2,  8, 14, 20,
   3,  9, 15, 21,
   4, 10, 16, 22,
   5, 11, 17, 23,
};

int sbmap_interior(uint8_t base_id) {
  int g = base_id / 10;
  if (g < 0 || g >= 24) return -1;
  return (int)k_interior[g];
}

/* sSecretBaseEntranceMetatiles. Seven closed/open pairs — tree left/right, the four cave
 * colours, and the Fortree shrub. All 75 entrance cells in the ROM hold a CLOSED id. */
static const uint16_t k_swap[7][2] = {
  { 0x026, 0x036 },   /* tree, left half         */
  { 0x027, 0x037 },   /* tree, right half        */
  { 0x1A0, 0x1A1 },   /* red cave                */
  { 0x1A8, 0x1A9 },   /* yellow cave             */
  { 0x1B0, 0x1B1 },   /* blue cave               */
  { 0x208, 0x210 },   /* brown cave (Fallarbor)  */
  { 0x271, 0x278 },   /* shrub (Fortree)         */
};

uint16_t sbmap_open_metatile(uint16_t closed) {
  for (int i = 0; i < 7; i++) if (k_swap[i][0] == closed) return k_swap[i][1];
  return 0;
}

/* vars[] sits immediately after flags[]: Emerald 0x1270+300 = 0x139C, RS 0x1220+288 = 0x1340.
 * VAR_CURRENT_SECRET_BASE is var 0x54 past VARS_START, i.e. +0xA8 bytes. Plaintext — Emerald's
 * security key does not obfuscate vars[]. */
#define SBD_VAR_CUR_BASE_EM 0x1444u
#define SBD_VAR_CUR_BASE_RS 0x13E8u

int sbmap_cur_base_index(const uint8_t* sb1, int pk_game) {
  if (!sb1 || !sbmap_supported(pk_game)) return -1;
  /* CurMapIsSecretBase(): location.mapGroup/mapNum live at SaveBlock1 +0x04/+0x05. */
  if ((int8_t)sb1[0x04] != SB_INTERIOR_GROUP || sb1[0x05] > 23) return -1;
  uint32_t off = (pk_game == PK_EMERALD) ? SBD_VAR_CUR_BASE_EM : SBD_VAR_CUR_BASE_RS;
  int idx = (int)(sb1[off] | (sb1[off + 1] << 8));
  return (idx >= 0 && idx < 20) ? idx : -1;
}

bool sbmap_supported(int pk_game) {
  /* Ruby/Sapphire and Emerald only. FRLG has no secret bases; its BG events never carry
   * kind 8, so this is belt-and-braces rather than the load-bearing check. */
  return pk_game == PK_RS || pk_game == PK_EMERALD;
}
