#ifndef DC_LEARN_H
#define DC_LEARN_H

#include <stdint.h>

/* One move-learning event during Day-Care growth (shared by the Gen-3 core in
 * gen3_daycare.c and the Gen-1/2 core in rom_gblearn.c, so neither header has to
 * pull in the other's world). */
typedef struct {
  uint16_t newmove;      /* the move learned                                      */
  uint16_t replaced;     /* the move pushed out (oldest first); 0 = filled a free slot */
  uint8_t  at_level;     /* the level that taught it                              */
} DcLearn;

#define DC_LEARN_MAX 8   /* events recorded; beyond this only the count grows     */

#endif /* DC_LEARN_H */
