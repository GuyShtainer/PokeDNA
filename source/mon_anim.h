#ifndef MON_ANIM_H
#define MON_ANIM_H
#include <stdint.h>
/* Emerald front-animation family (0..9) for an internal species id — the real
 * per-species summary intro animation, played procedurally (no sprite frames).
 * See mon_anim.c for the family legend. */
uint8_t mon_anim_family(uint16_t internal_species);
#endif
