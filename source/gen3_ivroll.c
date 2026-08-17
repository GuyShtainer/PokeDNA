#include "gen3_ivroll.h"
#include "data_tables.h"

#define IVW_EGG      (1u << 30)
#define IVW_ABILITY  (1u << 31)

/* METLOC_IN_GAME_TRADE. The same constant gen3_legality_hooks.c:26 carries, and the same
 * test it makes: the game stamps 0xFE on every NPC trade (src/trade.c:4577), which is all
 * the identification needed to know the record's PID came from a template rather than an
 * RNG stream. Re-typed rather than shared because gen3_legality_hooks.h drags the whole
 * Legality-V2 report in, and this file is meant to stay a leaf. */
#define IVROLL_MET_TRADE 0xFEu

/* THE LADDER, and why it exists. A shiny PID is 1 in 8,192, so keeping shiny AND nature AND
 * sex AND the ability slot needs ~870,000 candidates. The ladder spends 40k on the full set,
 * 40k without sex, 60k without the ability slot / Unown letter, 60k on shiny alone, then 256
 * unconstrained. Measured over 600 shiny rerolls: sparkle kept 600/600, nature kept 49%,
 * avg 112,929 candidates, max 186,441. Ordinary non-shiny mons win on rung 0 after ~55.
 * RUNGS ARE DE-DUPLICATED at run time: for a genderless one-ability non-Unown species
 * (Ditto, Mewtwo, every legendary) rungs 0/1/2 are IDENTICAL, and running all three burned
 * 140,000 candidates for nothing — measured Ditto avg 106,759 before, ~40k after. */
static const struct { unsigned drop; uint32_t budget; } LADDER[] = {
  { 0,                                                       40000u },
  { 1u /*GEN*/,                                              40000u },
  { 1u | 2u /*GEN|ABI*/ | 4u /*FRM*/,                        60000u },
  { 1u | 2u | 4u | 8u /*NAT*/,                               60000u },
  { 1u | 2u | 4u | 8u | 16u /*SHI*/,                           256u },
};
#define D_GEN 1u
#define D_ABI 2u
#define D_FRM 4u
#define D_NAT 8u
#define D_SHI 16u

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static uint32_t pack_ivs(const uint8_t iv[PK_NSTATS]) {
  return ((uint32_t)(iv[PK_HP]  & 31))       | ((uint32_t)(iv[PK_ATK] & 31) <<  5)
       | ((uint32_t)(iv[PK_DEF] & 31) << 10) | ((uint32_t)(iv[PK_SPE] & 31) << 15)
       | ((uint32_t)(iv[PK_SPA] & 31) << 20) | ((uint32_t)(iv[PK_SPD] & 31) << 25);
}
static int shiny_of(uint32_t pid, uint32_t otId) {
  uint16_t tid = (uint16_t)otId, sid = (uint16_t)(otId >> 16);
  return ((uint16_t)(tid ^ sid ^ (uint16_t)pid ^ (uint16_t)(pid >> 16)) < 8);
}

int iv_roll_step(const EditMon* e, const PkMon* cur, IvRollState* st, uint32_t slice,
                 IvRoll* out) {
  if (!e || !cur || !st || !out) return -1;
  if (cur->isBadEgg) return -1;          /* its PID and IVs are garbage: nothing to roll */

  uint32_t ivw_old = em_get_ivword(e);
  uint16_t species = rd16(e->sub[0]);
  int two_ab = (pk_species_ability(species, 1) != 0);
  uint8_t  ratio = pk_species_gender_ratio(species);
  int is_unown = (species == 201);
  uint8_t exempt = pk_pidiv_exempt_reason(cur);

  if (!st->started) {
    st->started = 1; st->rung = 0; st->rolls = 0; st->rung_tries = 0;
    /* Seeded from the frame the caller pressed on, mixed with the record. Never reset
     * between presses by the caller, so two rolls can never repeat a spread. */
    if (!st->seed) st->seed = e->personality ^ e->otId ^ 0x9E3779B9u;
  }

  PkSpread s;
  PkSpreadWant w;
  pk_spread_want_init(&w);
  w.otId = e->otId;
  w.opts = is_unown ? PK_SPREAD_REVERSED : 0u;

  if (exempt != PK_PIDIV_EX_NONE) {
    /* Exempt: the PID and IVs are legitimately unrelated (gen3_pidiv.c:145), so take the
     * IVs of one draw and throw its PID away. One roll, no search, no chunking. */
    w.cap = 1;
    pk_spread_roll(st->seed, &w, &s);
    st->seed = s.next;
    for (unsigned i = 0; i < sizeof *out; i++) ((uint8_t*)out)[i] = 0;
    out->exempt = exempt; out->pid_locked = false; out->pid = e->personality;
    out->rolls = 1;
    out->ivword = pack_ivs(s.ivs) | (ivw_old & (IVW_EGG | IVW_ABILITY));
    for (int k = 0; k < PK_NSTATS; k++) out->ivs[k] = s.ivs[k];
    st->rung = 0; st->started = 0;
    return 1;
  }

  /* Non-exempt: one slice of the current rung, driving the SHARED pk_spread_roll. */
  unsigned full = D_NAT | (two_ab ? D_ABI : 0u) | (is_unown ? D_FRM : 0u)
                | ((ratio >= 1 && ratio <= 253) ? D_GEN : 0u)
                | (cur->isShiny ? D_SHI : 0u);
  unsigned prev_mask = ~0u;
  for (;;) {
    if (st->rung >= (uint8_t)(sizeof LADDER / sizeof LADDER[0])) { st->rung = 0; return -1; }
    unsigned mask = full & ~LADDER[st->rung].drop;
    if (mask == prev_mask) { st->rung++; st->rung_tries = 0; continue; }  /* identical rung */
    prev_mask = mask;

    w.nature       = (mask & D_NAT) ? (int8_t)cur->nature : -1;
    w.gender       = (mask & D_GEN) ? (int8_t)cur->gender : -1;
    w.gender_ratio = ratio;
    /* An UNASKED-FOR shiny is as much of a surprise as a lost one, so shininess is always
     * constrained on the rungs that keep it, and "refuse" otherwise. */
    w.shiny        = (mask & D_SHI) ? 1 : (cur->isShiny ? -1 : 0);
    w.ability      = ((mask & D_ABI) && two_ab) ? cur->abilityNum : 0xFF;
    w.unown_form   = ((mask & D_FRM) && is_unown) ? pk_unown_form(e->personality)
                                                  : PK_SPREAD_ANY_FORM;
    w.cap          = slice;

    bool got = pk_spread_roll(st->seed, &w, &s);
    st->rolls += s.tries;
    st->rung_tries += s.tries;
    st->seed = s.next;
    if (!got) {
      /* AGAINST THE RUNG'S RUNNING TOTAL, not this slice's. The slice is the caller's frame
       * budget (8,000) and every rung's budget is larger, so testing `s.tries` would mean a
       * rung could never expire and the ladder below it would never run: measured, a shiny
       * reroll then ground on rung 0 for up to 17,880,120 candidates instead of the ~200k
       * the whole ladder is allowed. */
      if (st->rung_tries >= LADDER[st->rung].budget) { st->rung++; st->rung_tries = 0; continue; }
      return 0;                                   /* slice exhausted: come back next frame */
    }

    for (unsigned i = 0; i < sizeof *out; i++) ((uint8_t*)out)[i] = 0;
    out->pid_locked  = true;
    out->pid         = s.pid;
    out->rolls       = st->rolls;
    out->old_nature  = cur->nature;     out->new_nature  = (uint8_t)(s.pid % 25u);
    out->old_gender  = cur->gender;     out->new_gender  = pk_gender_from(s.pid, ratio);
    out->old_ability = cur->abilityNum; out->new_ability = two_ab ? (uint8_t)(s.pid & 1u) : 0;
    out->old_shiny   = cur->isShiny;    out->new_shiny   = shiny_of(s.pid, e->otId) != 0;
    out->old_form    = is_unown ? pk_unown_form(e->personality) : 0;
    out->new_form    = is_unown ? pk_unown_form(s.pid) : 0;
    out->chg_nature  = out->new_nature  != out->old_nature;
    out->chg_gender  = out->new_gender  != out->old_gender;
    out->chg_ability = out->new_ability != out->old_ability;
    out->chg_shiny   = out->new_shiny   != out->old_shiny;
    out->chg_form    = is_unown && (out->new_form != out->old_form);
    /* The NPC trade. Not a comparison of before and after — the value the reroll destroys
     * is the template's, and ANY new PID destroys it, so this is a property of the record
     * rather than of the spread. See gen3_ivroll.h for why it is a warning and not an
     * exemption: the caller must say so before writing, and B never writes. */
    out->chg_trade   = (cur->metLocation == IVROLL_MET_TRADE);
    /* The egg flag is never this function's business, so it is carried across verbatim. The
     * ability bit IS: it must be what CreateBoxMon would have written for the NEW PID. */
    out->ivword = pack_ivs(s.ivs) | (ivw_old & IVW_EGG);
    if (two_ab && (s.pid & 1u)) out->ivword |= IVW_ABILITY;
    for (int k = 0; k < PK_NSTATS; k++) out->ivs[k] = s.ivs[k];
    st->rung = 0; st->started = 0;
    return 1;
  }
}

void iv_roll_apply(EditMon* e, const IvRoll* r) {
  if (!e || !r) return;
  /* PID first: em_set_ivword's stat recompute reads the nature out of the personality. */
  if (r->pid_locked) em_set_pid(e, r->pid);
  em_set_ivword(e, r->ivword);
}

/* ---- history ------------------------------------------------------------------ */
void ivh_reset(IvHistory* h, const EditMon* e) {
  h->e[0].pid = e->personality; h->e[0].ivword = em_get_ivword(e);
  h->species = rd16(e->sub[0]);
  h->n = 1; h->cur = 0;
}
bool ivh_sync(IvHistory* h, const EditMon* e) {
  if (h->n > 0 && h->species == rd16(e->sub[0])
      && h->e[h->cur].pid == e->personality
      && h->e[h->cur].ivword == em_get_ivword(e)) return false;
  ivh_reset(h, e);
  return true;
}
void ivh_push(IvHistory* h, const EditMon* e) {
  if ((int)h->cur + 1 < (int)h->n) h->n = (uint8_t)(h->cur + 1);   /* drop the redo tail */
  if (h->n >= IVH_CAP) {
    /* PIN ENTRY 0: drop the SECOND-oldest so "walk all the way back" is always bit-exactly
     * what the user walked in with — the single most valuable entry in an undo stack. */
    for (int i = 2; i < IVH_CAP; i++) h->e[i - 1] = h->e[i];
    h->n = IVH_CAP - 1;
    if (h->cur > 0) h->cur--;
  }
  h->e[h->n].pid = e->personality;
  h->e[h->n].ivword = em_get_ivword(e);
  h->cur = h->n;
  h->n = (uint8_t)(h->n + 1);
}
bool ivh_step(IvHistory* h, EditMon* e, int dir) {
  int nx = (int)h->cur + dir;
  if (nx < 0 || nx >= (int)h->n) return false;
  h->cur = (uint8_t)nx;
  em_set_pid(e, h->e[nx].pid);
  em_set_ivword(e, h->e[nx].ivword);
  return true;
}
