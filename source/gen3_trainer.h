#ifndef GEN3_TRAINER_H
#define GEN3_TRAINER_H

#include <stdint.h>
#include <stdbool.h>

/* Trainer-card / game-stats reads. Money + game records are XOR-encrypted with
 * the SaveBlock2 security key in Emerald/FRLG; Ruby/Sapphire store them plaintext.
 * Offsets confirmed from the pret decomps. */

typedef enum { PK_RS = 0, PK_EMERALD, PK_FRLG } PkGame;

/* Game-stat indices (same across Gen 3 — verified in all three decomps'
 * include/constants/game_stat.h). gameStats is a u32 array. */
enum {
  PK_STAT_FIRST_HOF_PLAY_TIME = 1,   /* packed h<<16 | m<<8 | s, at first HoF entry */
  PK_STAT_STEPS               = 5,
  PK_STAT_TOTAL_BATTLES       = 7,
  PK_STAT_WILD_BATTLES        = 8,
  PK_STAT_TRAINER_BATTLES     = 9,
  PK_STAT_ENTERED_HOF         = 10,
  PK_STAT_POKEMON_CAPTURES    = 11,
  PK_STAT_HATCHED_EGGS        = 13,
  /* trainer-card BACK page rows (game_stat.h:25-28,38-39,54-55) */
  PK_STAT_POKEMON_TRADES      = 21,
  PK_STAT_LINK_BATTLE_WINS    = 23,
  PK_STAT_LINK_BATTLE_LOSSES  = 24,
  PK_STAT_POKEBLOCKS_W_FRIENDS= 34,
  PK_STAT_WON_LINK_CONTEST    = 35,
  PK_STAT_UNION_ROOM_BATTLES  = 50,  /* FRLG only (their stat count is 64) */
  PK_STAT_BERRY_CRUSH_POINTS  = 51,  /* FRLG only */
};

uint32_t pk_money(const uint8_t* sb1, const uint8_t* sb2, PkGame g);
uint32_t pk_game_stat(const uint8_t* sb1, const uint8_t* sb2, PkGame g, int stat);

/* Game-Record (counter) editing. The stats array + money live in SaveBlock1,
 * XOR'd with the SaveBlock2 key (Emerald/FRLG; RS plaintext). */
int  pk_game_stat_count(PkGame g);                   /* RS 50, E/FRLG 64 */
void pk_set_game_stat(uint8_t* sb1, const uint8_t* sb2, PkGame g, int stat, uint32_t value);
void pk_set_money(uint8_t* sb1, const uint8_t* sb2, PkGame g, uint32_t money);

/* Game-Corner coins (SaveBlock1, key-XOR'd like money; max 9999). */
uint16_t pk_coins(const uint8_t* sb1, const uint8_t* sb2, PkGame g);
void     pk_set_coins(uint8_t* sb1, const uint8_t* sb2, PkGame g, uint16_t coins);

/* SaveBlock2 trainer identity (plaintext). Commit via section 0. */
void pk_set_trainer_name(uint8_t* sb2, const char* s);          /* <=7 ASCII chars */
void pk_set_gender(uint8_t* sb2, uint8_t g);                    /* 0 male, 1 female */
void pk_set_trainer_id(uint8_t* sb2, uint16_t tid, uint16_t sid);
void pk_set_playtime(uint8_t* sb2, uint16_t h, uint8_t m, uint8_t s);
const char* pk_game_stat_name(int stat);             /* generated (data_tables.c) */

/* Trainer-card BACK facility records that do NOT live in gameStats (both
 * plaintext u16, SaveBlock2; commit via section 0):
 *  RS Battle Tower — totalBattleTowerWins @0x0570 / bestBattleTowerWinStreak
 *  @0x0572 (pokeruby include/global.h:834-835 + trainer_card.c:408-409);
 *  Emerald card Battle Points — frontier.cardBattlePoints @0xEBA
 *  (pokeemerald include/global.h:450 in struct BattleFrontier @0x64C:541,
 *  trainer_card.c:766). */
uint16_t pk_rs_tower(const uint8_t* sb2, int streak);            /* 0 wins, 1 streak */
void     pk_set_rs_tower(uint8_t* sb2, int streak, uint16_t v);
uint16_t pk_e_card_bp(const uint8_t* sb2);
void     pk_set_e_card_bp(uint8_t* sb2, uint16_t v);

/* First Hall-of-Fame (Elite Four) clear time. Returns false if never entered. */
bool pk_hof_time(const uint8_t* sb1, const uint8_t* sb2, PkGame g,
                 uint16_t* h, uint8_t* m, uint8_t* s);

/* Pokédex seen/caught counts over national #1..386, + whether the National dex is on. */
void pk_pokedex(const uint8_t* sb2, int* seen, int* caught, bool* national);

#endif /* GEN3_TRAINER_H */
