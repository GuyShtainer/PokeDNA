/* GENERATED: per-species Emerald front-animation family (pure C). Extracted from
 * pokeemerald sMonFrontAnimIdsTable and bucketed into procedural transform families
 * (the real per-species intro animation, no sprite assets):
 *   0 squish&bounce  1 stretch  2 v-shake  3 h-shake  4 grow
 *   5 shrink-grow    6 v-slide  7 h-slide  8 jumps    9 wobble (rotate/swing approx) */
#include "mon_anim.h"

static const uint8_t s_fam[413] = {  /* [internal species id] */
  0,8,1,8,8,8,2,9,5,2,9,9,7,7,0,3,6,1,9,8,9,6,9,1,
  1,0,1,9,1,0,1,3,4,5,3,0,9,1,2,9,8,0,7,0,0,9,7,3,
  8,3,2,3,8,1,8,3,8,9,8,3,0,8,2,8,4,1,0,2,8,1,9,8,
  0,0,9,9,8,0,1,0,9,9,0,9,3,4,9,0,7,0,9,7,0,0,4,8,
  9,4,7,3,0,0,7,8,9,9,1,4,0,5,6,2,5,0,9,1,9,6,9,6,
  9,9,7,3,1,0,3,4,2,9,9,1,1,1,1,0,1,8,6,4,7,3,6,9,
  4,0,0,0,2,6,4,9,0,0,1,8,1,2,8,3,3,0,8,6,1,8,6,9,
  3,0,0,6,8,8,9,9,0,8,4,0,8,0,9,0,9,7,8,6,6,6,8,8,
  0,9,0,1,4,2,0,5,6,3,0,8,9,2,0,5,3,1,2,4,3,9,4,1,
  1,2,1,1,0,3,7,8,1,8,9,1,1,2,9,8,2,0,0,8,1,3,4,0,
  0,0,0,0,4,2,8,2,3,4,4,6,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,8,3,1,6,8,2,2,
  7,4,0,0,6,6,8,0,0,9,9,0,3,0,7,6,8,1,0,0,8,9,6,0,
  0,1,6,0,1,0,7,6,9,1,0,9,9,7,2,4,9,0,9,8,2,3,3,9,
  9,0,0,6,2,9,1,3,9,6,0,3,9,9,0,8,8,0,1,0,9,0,6,1,
  8,3,3,0,0,8,0,0,0,2,7,9,2,9,4,6,1,6,9,1,4,8,0,0,
  2,7,0,9,1,2,9,2,0,0,0,2,6,3,3,6,2,1,9,4,9,2,3,9,
  2,9,9,7,0,
};

#define FAM_COUNT (int)(sizeof s_fam / sizeof s_fam[0])
uint8_t mon_anim_family(uint16_t s) { return (s < FAM_COUNT) ? s_fam[s] : 0; }
