#ifndef GEN3_TRAINER_H
#define GEN3_TRAINER_H

#include <stdint.h>
#include <stdbool.h>

/* Trainer-card / game-stats reads. Money + game records are XOR-encrypted with
 * the SaveBlock2 security key in Emerald/FRLG; Ruby/Sapphire store them plaintext.
 * Offsets confirmed from the pret decomps. */

typedef enum { PK_RS = 0, PK_EMERALD, PK_FRLG } PkGame;

/* Game-stat indices (same across Gen 3). gameStats is a u32 array. */
enum {
  PK_STAT_FIRST_HOF_PLAY_TIME = 1,   /* packed h<<16 | m<<8 | s, at first HoF entry */
  PK_STAT_STEPS               = 5,
  PK_STAT_TOTAL_BATTLES       = 7,
  PK_STAT_WILD_BATTLES        = 8,
  PK_STAT_TRAINER_BATTLES     = 9,
  PK_STAT_ENTERED_HOF         = 10,
  PK_STAT_POKEMON_CAPTURES    = 11,
  PK_STAT_HATCHED_EGGS        = 13,
};

uint32_t pk_money(const uint8_t* sb1, const uint8_t* sb2, PkGame g);
uint32_t pk_game_stat(const uint8_t* sb1, const uint8_t* sb2, PkGame g, int stat);

/* Game-Record (counter) editing. The stats array + money live in SaveBlock1,
 * XOR'd with the SaveBlock2 key (Emerald/FRLG; RS plaintext). */
int  pk_game_stat_count(PkGame g);                   /* RS 50, E/FRLG 64 */
void pk_set_game_stat(uint8_t* sb1, const uint8_t* sb2, PkGame g, int stat, uint32_t value);
void pk_set_money(uint8_t* sb1, const uint8_t* sb2, PkGame g, uint32_t money);

/* SaveBlock2 trainer identity (plaintext). Commit via section 0. */
void pk_set_trainer_name(uint8_t* sb2, const char* s);          /* <=7 ASCII chars */
void pk_set_gender(uint8_t* sb2, uint8_t g);                    /* 0 male, 1 female */
void pk_set_trainer_id(uint8_t* sb2, uint16_t tid, uint16_t sid);
void pk_set_playtime(uint8_t* sb2, uint16_t h, uint8_t m, uint8_t s);
const char* pk_game_stat_name(int stat);             /* generated (data_tables.c) */

/* First Hall-of-Fame (Elite Four) clear time. Returns false if never entered. */
bool pk_hof_time(const uint8_t* sb1, const uint8_t* sb2, PkGame g,
                 uint16_t* h, uint8_t* m, uint8_t* s);

/* Pokédex seen/caught counts over national #1..386, + whether the National dex is on. */
void pk_pokedex(const uint8_t* sb2, int* seen, int* caught, bool* national);

#endif /* GEN3_TRAINER_H */
