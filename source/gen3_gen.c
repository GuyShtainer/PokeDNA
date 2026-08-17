#include "gen3_gen.h"
#include "gen3_edit.h"
#include "gen3_mon.h"
#include <string.h>

uint32_t gen3_spread_opts(uint16_t species) {
  /* Unown is the one species whose PID halves come out of the stream the other way round.
   * Not a guess: over Guy's five saves the reversed order matched 23 Pokemon and every one
   * was an Unown, while no Unown matched the normal order (gen3_pidiv.c:168-173). Emitting
   * a normal-order Unown would make PokeDNA's own report read "Method 1" where every real
   * one reads "Method 1 (Unown)" — a tell, written by the tool itself. */
  return (species == 201) ? PK_SPREAD_REVERSED : 0u;
}

bool gen3_build_mon_spread(uint16_t species, uint8_t lvl, uint32_t seed, uint32_t otId,
                           const char* otName, uint8_t metgame,
                           const PkSpreadWant* want, uint8_t out[80], Gen3BuildInfo* info) {
  PkSpreadWant w;
  if (want) w = *want; else pk_spread_want_init(&w);
  w.otId = otId;                              /* shiny is a property of THIS trainer */
  w.opts = (w.opts & ~PK_SPREAD_REVERSED) | gen3_spread_opts(species);

  PkSpread s;
  bool ok = pk_spread_roll(seed, &w, &s);

  gen3_build_mon(species, lvl, s.pid, otId, otName, metgame, out);

  /* The IVs of the seed that produced that PID, written into the record that PID went
   * into. SIX single-stat setters and not one word write: em_set_iv is a read-modify-write
   * on the 32-bit IV/egg word, so bit 30 (is-egg) and bit 31 (the ability slot
   * gen3_build_mon just derived from this same PID) survive it. */
  EditMon e;
  gen3_edit_load(out, false, &e);
  for (int i = 0; i < PK_NSTATS; i++) em_set_iv(&e, i, s.ivs[i]);
  gen3_edit_commit(&e, out);

  if (info) {
    memset(info, 0, sizeof *info);
    info->spread    = s;
    info->spread_ok = ok ? 1u : 0u;
    info->level     = lvl ? (lvl > 100 ? 100u : lvl) : gen3_build_level(species);
    info->hatched   = gen3_species_can_hatch(species) ? 1u : 0u;
  }
  return ok;
}
