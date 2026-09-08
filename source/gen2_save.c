/* Generation-II (Gold/Silver/Crystal) save reader — pure C, host-testable.
 * Read-only: PokeDNA never writes a Game Boy save (docs/research-gen12.md §0).
 *
 * Offsets/checksum ranges transcribed from Bulbapedia "Save data structure
 * (Generation II)" raw wikitext (fetched 2026-08) and cross-checked against
 * pret/pokecrystal ram/sram.asm for Crystal; record layout from Bulbapedia
 * "Pokemon data structure (Generation II)"; the derived DV rules from
 * Bulbapedia "Individual values". Decomps are reference-only per
 * docs/kb/licensing.md — nothing here is copied from them.
 */
#include "gen2_save.h"
#include <string.h>

/* GB saves are big-endian; the two stored checksums are the exception. */
static uint16_t rd16be(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t rd24be(const uint8_t* p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* ---------------------------------------------------------------- checksums */
/* "The checksums are simply the 16-bit sum of all byte values of the
 * corresponding byte regions. Checksums are stored as little-endian."
 *
 * G/S:     sum 0x2009-0x2D68                                   -> 0x2D69
 *          sum 0x0C6B-0x17EC + 0x3D96-0x3F3F + 0x7E39-0x7E6C   -> 0x7E6D
 *          (0x3D96, NOT 0x3D69 — a PREVIOUS pass here got this backwards; see
 *           the note on k_gs_mirror below, BACKLOG #49 P0)
 * Crystal: sum 0x2009-0x2B82                                   -> 0x2D0D
 *          sum 0x1209-0x1D82                                   -> 0x1F0D
 * The G/S backup regions are the *destinations* of the five-way scatter map
 * below (three of them are adjacent, which is why the wiki states the backup
 * sum as one 0x0C6B-0x17EC run); g2_mirror_map() exposes the same map so a test
 * can build a save whose backup validates. Japanese ranges are listed too, so a
 * JP save is recognised and refused instead of silently misparsed. */
enum {
  SUM_GS_P = 0, SUM_GS_B, SUM_C_P, SUM_C_B,
  SUM_JGS_P, SUM_JGS_B, SUM_JC_P, SUM_JC_B, SUM_N
};
enum { ST_GS_P = 0, ST_GS_B, ST_C_P, ST_C_B, ST_JP_B, ST_N };

static const uint32_t k_stored_off[ST_N] = {
  0x2D69u,  /* G/S checksum 1                     */
  0x7E6Du,  /* G/S checksum 2                     */
  0x2D0Du,  /* Crystal checksum 1 (JP GS/C too)   */
  0x1F0Du,  /* Crystal checksum 2                 */
  0x7F0Du,  /* JP checksum 2                      */
};

static const struct { uint8_t sum; uint32_t from, to; } k_spans[] = {
  { SUM_GS_P,  0x2009u, 0x2D68u },
  { SUM_GS_B,  0x0C6Bu, 0x17ECu }, { SUM_GS_B, 0x3D96u, 0x3F3Fu }, { SUM_GS_B, 0x7E39u, 0x7E6Cu },
  { SUM_C_P,   0x2009u, 0x2B82u },
  { SUM_C_B,   0x1209u, 0x1D82u },
  { SUM_JGS_P, 0x2009u, 0x2C8Bu }, { SUM_JGS_B, 0x7209u, 0x7E8Bu },
  { SUM_JC_P,  0x2009u, 0x2AE2u }, { SUM_JC_B,  0x7209u, 0x7CE2u },
};
#define NSPANS ((int)(sizeof k_spans / sizeof k_spans[0]))

/* The G/S five-way scatter map. Region 2's destination (sBackupPlayerData2) is 0x3D96 —
 * BACKLOG #49 P0 (docs/GEN12-PARITY-DESIGN.md §1.9/§2.4) found this file previously said
 * 0x3D69 here, with a comment claiming a real Gold cartridge save had proved it. That
 * comment had the direction backwards. Four independent witnesses now agree on 0x3D96:
 *
 *   1. pokegold's own compiled symbol table (pinned a0dad09, docs/GEN12-PARITY-DESIGN.md's
 *      pin): `01:bd96 sBackupPlayerData2` -> file 0x2000 + (0xBD96-0xA000) = 0x3D96.
 *   2. pokegold/ram/sram.asm (same pin, `git show a0dad09:ram/sram.asm`): sBackupPlayerData2
 *      sits alone in `SECTION "Backup Save 2", SRAM`, physically separate from the other
 *      four backup regions (which cluster in "Backup Save 1" right after sPlayerData3) —
 *      exactly why its address looks "far away" and easy to mistranscribe.
 *   3. pokegold/engine/menus/save.asm's SaveBackupChecksum (~line 495) sums FIVE regions
 *      into sBackupChecksum, one of them `ld hl, sBackupPlayerData2 / ld bc,
 *      wPlayerData2End - wPlayerData2`, matching this file's SUM_GS_B span.
 *   4. THE DECIDING VOTE — a ROM boot, not a byte compare (this module's own
 *      tools/gb_roundtrip.py rule, since a fixture built from the same constant as the
 *      parser can never falsify it): break Gold.sav's primary checksum, rebuild the
 *      backup at each candidate address, recompute that candidate's checksum, boot under
 *      libmgba. `tools/gb_roundtrip.py --selftest` (re-run 2026-09-07):
 *        backup rebuilt at 0x3D96 -> ACCEPT, PLAYER=MattiaP  ("the measured G/S mirror
 *          map rescues the save")
 *        backup rebuilt at 0x3D69 -> REJECT, "The save file is / corrupted!"  ("k_gs_
 *          mirror's 0x3D69 does not -- the ROM's verdict on the map")
 *
 * Why 0x3D69 looked right for as long as it did: sPlayerData2 opens with 45 zero bytes,
 * so the window slid back 45 bytes still compares byte-for-byte against the primary on
 * Guy's own Gold.sav (0/426 differ) AND still sums to the stored backup checksum 0xAEF9
 * — a fixture rebuilt from that same wrong constant agrees with itself, and even a real
 * save's RAW BYTES can't break the tie, because the wrong window is a genuine, if
 * coincidental, byte-perfect copy. The TRUE window at 0x3D96 disagrees with the primary
 * in 253 of 426 bytes on that same file (computed sum 0xC03D vs the stored 0xAEF9) —
 * which only means Guy's Gold.sav itself carries a stale G/S backup (§1.9's CONFLICT
 * note), not that 0x3D96 is wrong. Only booting the actual ROM tells the two apart.
 *
 * The lesson worth keeping: a byte compare against one real save narrowed this to two
 * candidates that both look locally consistent; only the running game could pick between
 * them. */
static const G2MirrorRegion k_gs_mirror[5] = {
  { 0x2009u, 0x222Eu, 0x15C7u },
  { 0x222Fu, 0x23D8u, 0x3D96u },
  { 0x23D9u, 0x2855u, 0x0C6Bu },
  { 0x2856u, 0x2889u, 0x7E39u },
  { 0x288Au, 0x2D68u, 0x10E8u },
};
static const G2MirrorRegion k_c_mirror[1] = { { 0x2009u, 0x2B82u, 0x1209u } };

int g2_mirror_map(G2Version ver, const G2MirrorRegion** out) {
  if (!out) return 0;
  if (ver == G2_VER_GS)      { *out = k_gs_mirror; return 5; }
  if (ver == G2_VER_CRYSTAL) { *out = k_c_mirror;  return 1; }
  *out = 0;
  return 0;
}

uint32_t g2_checksum_primary_off(G2Version ver) {
  if (ver == G2_VER_GS) return k_stored_off[ST_GS_P];
  if (ver == G2_VER_CRYSTAL || ver == G2_VER_JP_GS || ver == G2_VER_JP_CRYSTAL)
    return k_stored_off[ST_C_P];
  return 0;
}
uint32_t g2_checksum_backup_off(G2Version ver) {
  if (ver == G2_VER_GS) return k_stored_off[ST_GS_B];
  if (ver == G2_VER_CRYSTAL) return k_stored_off[ST_C_B];
  if (ver == G2_VER_JP_GS || ver == G2_VER_JP_CRYSTAL) return k_stored_off[ST_JP_B];
  return 0;
}

static uint16_t sum_of(const uint8_t* sav, int which) {
  uint32_t acc = 0;
  for (int i = 0; i < NSPANS; i++) {
    if (k_spans[i].sum != which) continue;
    for (uint32_t p = k_spans[i].from; p <= k_spans[i].to; p++) acc += sav[p];
  }
  return (uint16_t)acc;
}

static int primary_sum_index(G2Version v) {
  switch (v) {
    case G2_VER_GS:         return SUM_GS_P;
    case G2_VER_CRYSTAL:    return SUM_C_P;
    case G2_VER_JP_GS:      return SUM_JGS_P;
    case G2_VER_JP_CRYSTAL: return SUM_JC_P;
    default:                return -1;
  }
}
static int backup_sum_index(G2Version v) {
  switch (v) {
    case G2_VER_GS:         return SUM_GS_B;
    case G2_VER_CRYSTAL:    return SUM_C_B;
    case G2_VER_JP_GS:      return SUM_JGS_B;
    case G2_VER_JP_CRYSTAL: return SUM_JC_B;
    default:                return -1;
  }
}

uint16_t g2_checksum_primary(const uint8_t* sav, G2Version ver) {
  int i = primary_sum_index(ver);
  return (!sav || i < 0) ? 0 : sum_of(sav, i);
}
uint16_t g2_checksum_backup(const uint8_t* sav, G2Version ver) {
  int i = backup_sum_index(ver);
  return (!sav || i < 0) ? 0 : sum_of(sav, i);
}

/* ---------------------------------------------------------------- detection */

void g2_scan_begin(G2Scan* s) { if (s) memset(s, 0, sizeof *s); }

void g2_scan_feed(G2Scan* s, uint32_t off, const uint8_t* buf, uint32_t len) {
  if (!s || !buf || !len || off >= G2_SAVE_SIZE) return;
  uint32_t end = off + len;
  if (end > G2_SAVE_SIZE) end = G2_SAVE_SIZE;   /* the RTC footer is not save data */
  s->fed += end - off;

  for (int i = 0; i < NSPANS; i++) {
    uint32_t a = k_spans[i].from > off ? k_spans[i].from : off;
    uint32_t b = (k_spans[i].to + 1u) < end ? (k_spans[i].to + 1u) : end;
    uint32_t acc = 0, orr = 0;
    for (uint32_t p = a; p < b; p++) { acc += buf[p - off]; orr |= buf[p - off]; }
    s->sum[k_spans[i].sum] += acc;
    if (orr) s->nonzero |= (uint8_t)(1u << k_spans[i].sum);
  }
  for (int i = 0; i < ST_N; i++) {
    for (uint32_t k = 0; k < 2; k++) {
      uint32_t p = k_stored_off[i] + k;
      if (p >= off && p < end) { s->stored[i][k] = buf[p - off]; s->have[i] |= (uint8_t)(1u << k); }
    }
  }
}

static bool scan_match(const G2Scan* s, int sumi, int sti) {
  if (s->have[sti] != 3) return false;
  /* An all-zero region sums to 0 and, in unused save space, is stored next to a
   * zero word — a match there proves nothing. Erased images and the empty high
   * boxes that alias the Japanese checksum ranges both land here. */
  if (!(s->nonzero & (1u << sumi))) return false;
  uint16_t stored = (uint16_t)(s->stored[sti][0] | ((uint16_t)s->stored[sti][1] << 8));
  return (uint16_t)s->sum[sumi] == stored;
}

bool g2_scan_finish(const G2Scan* s, uint32_t total, G2Save* out) {
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!s) return false;
  out->tail = total > G2_SAVE_SIZE ? total - G2_SAVE_SIZE : 0;
  if (s->fed < G2_SAVE_SIZE) { out->short_file = true; return false; }

  const struct { G2Version v; int psum, pst, bsum, bst; } cand[] = {
    { G2_VER_GS,         SUM_GS_P,  ST_GS_P, SUM_GS_B,  ST_GS_B },
    { G2_VER_CRYSTAL,    SUM_C_P,   ST_C_P,  SUM_C_B,   ST_C_B  },
    { G2_VER_JP_GS,      SUM_JGS_P, ST_C_P,  SUM_JGS_B, ST_JP_B },
    { G2_VER_JP_CRYSTAL, SUM_JC_P,  ST_C_P,  SUM_JC_B,  ST_JP_B },
  };

  /* Score both copies rather than taking the first primary that matches: the
   * Japanese ranges start at the same 0x2009 and store checksum 1 in the same
   * slot as Crystal, so a western save whose data happens to end in zeros makes
   * a JP primary match too. Requiring the pair (primary + backup) to agree
   * separates them, and on a real tie the western layout — the only one we can
   * parse — wins. */
  int best = -1, best_score = 0;
  for (int i = 0; i < 4; i++) {
    int score = (scan_match(s, cand[i].psum, cand[i].pst) ? 2 : 0)
              + (scan_match(s, cand[i].bsum, cand[i].bst) ? 1 : 0);
    if (score == 0) continue;
    if (score > best_score) { best = i; best_score = score; out->ambiguous = false; }
    else if (score == best_score) out->ambiguous = true;
  }
  if (best < 0) return false;

  out->version    = cand[best].v;
  out->primary_ok = (best_score & 2) != 0;
  out->backup_ok  = (best_score & 1) != 0;
  /* PRIMARY-ONLY, DELIBERATELY. Every read path below (g2_read_header, g2_offsets,
   * g2_box_*_at) addresses the PRIMARY copy, so a save whose primary block is corrupt
   * but whose backup survives must NOT be reported as usable: it would be parsed out
   * of the damaged copy while looking healthy. Gen 2 keeps that backup precisely
   * because the primary can be torn mid-write, and this is a READ-ONLY importer — the
   * honest move is to refuse and say why, not to quietly read rubbish. (Reading FROM
   * the backup is a legitimate future feature; it needs its own offset set, so it is
   * not something to bolt on here.) */
  out->supported  = (out->version == G2_VER_GS || out->version == G2_VER_CRYSTAL)
                 && out->primary_ok;
  return out->supported;
}

bool g2_detect(const uint8_t* sav, uint32_t size, G2Save* out) {
  G2Scan s;
  g2_scan_begin(&s);
  if (sav) g2_scan_feed(&s, 0, sav, size);
  return g2_scan_finish(&s, size, out);
}

bool g2_detect_ranged(G2ReadFn rd, void* ctx, uint32_t len,
                      uint8_t* scratch, uint32_t scratch_len, G2Save* out) {
  G2Scan s;
  g2_scan_begin(&s);
  if (!rd || !scratch || scratch_len < 64) return g2_scan_finish(&s, 0, out);
  uint32_t want = len < G2_SAVE_SIZE ? len : G2_SAVE_SIZE;
  for (uint32_t off = 0; off < want; ) {
    uint32_t n = want - off < scratch_len ? want - off : scratch_len;
    if (!rd(ctx, off, scratch, n)) break;   /* a short read fails the size gate */
    g2_scan_feed(&s, off, scratch, n);
    off += n;
  }
  return g2_scan_finish(&s, len, out);
}

const char* g2_version_name(G2Version v) {
  switch (v) {
    case G2_VER_GS:         return "Gold/Silver";
    case G2_VER_CRYSTAL:    return "Crystal";
    case G2_VER_JP_GS:      return "Gold/Silver (JP)";
    case G2_VER_JP_CRYSTAL: return "Crystal (JP)";
    default:                return "?";
  }
}

const char* g2_reject_reason(const G2Save* sv) {
  if (!sv) return "no save";
  if (sv->supported) return 0;
  if (sv->short_file) return "file is smaller than a 32 KiB Gen-2 save";
  if (sv->version == G2_VER_JP_GS || sv->version == G2_VER_JP_CRYSTAL)
    return "Japanese Gen-2 saves are not supported yet";
  if (sv->backup_ok && !sv->primary_ok)
    return "main save block is damaged (only the backup copy is intact)";
  return "not a Gen-2 save (no checksum matched)";
}

/* ------------------------------------------------------------------- layout */

bool g2_offsets(G2Version ver, G2Offsets* out) {
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (ver == G2_VER_GS) {
    out->tid = 0x2009u; out->player_name = 0x200Bu;
    out->money = 0x23DBu; out->johto_badges = 0x23E4u; out->kanto_badges = 0x23E5u;
    out->dex_owned = 0x2A4Cu; out->dex_seen = 0x2A6Cu;
    out->current_box_no = 0x2724u; out->box_names = 0x2727u;
    out->party_list = 0x288Au; out->current_box_list = 0x2D6Cu;
    out->player_gender = 0;    /* G/S has one player character */
    return true;
  }
  if (ver == G2_VER_CRYSTAL) {
    out->tid = 0x2009u; out->player_name = 0x200Bu;
    out->money = 0x23DCu; out->johto_badges = 0x23E5u; out->kanto_badges = 0x23E6u;
    out->dex_owned = 0x2A27u; out->dex_seen = 0x2A47u;
    out->current_box_no = 0x2700u; out->box_names = 0x2703u;
    out->party_list = 0x2865u; out->current_box_list = 0x2D10u;
    out->player_gender = 0x3E3Du;
    return true;
  }
  return false;
}

/* Boxes 1-7 in SRAM bank 2, 8-14 in bank 3; the stride is 0x450 (the 1102-byte
 * list plus the two FF 00 bytes that follow every list). Same for both versions. */
static const uint32_t k_box_off[G2_NUM_BOXES] = {
  0x4000u, 0x4450u, 0x48A0u, 0x4CF0u, 0x5140u, 0x5590u, 0x59E0u,
  0x6000u, 0x6450u, 0x68A0u, 0x6CF0u, 0x7140u, 0x7590u, 0x79E0u,
};

int g2_list_size(int box)       { return box == G2_BOX_PARTY ? G2_PARTY_LIST_SIZE : G2_BOX_LIST_SIZE; }
int g2_list_capacity(int box)   { return box == G2_BOX_PARTY ? G2_PARTY_CAPACITY : G2_BOX_CAPACITY; }
int g2_list_entry_size(int box) { return box == G2_BOX_PARTY ? G2_PARTY_ENTRY    : G2_BOX_ENTRY; }

uint32_t g2_list_offset(const G2Save* sv, int box, int current_box) {
  G2Offsets o;
  if (!sv || !g2_offsets(sv->version, &o)) return 0;
  if (box == G2_BOX_PARTY) return o.party_list;
  if (box < 0 || box >= G2_NUM_BOXES) return 0;
  /* The game copies the open box into main data and only writes it back to the
   * bank on a box switch, so the banked copy of the current box is stale. */
  if (box == current_box) return o.current_box_list;
  return k_box_off[box];
}

/* --------------------------------------------------------------- list decode */
/* Layout of a Pokemon list (capacity C, entry size E):
 *   +0            count
 *   +1            C+1 species bytes, 0xFF after the last real one (0xFD = Egg)
 *   +2+C          C records of E bytes
 *   +2+C+C*E      C OT names, 11 bytes each
 *   +2+C+C*E+11C  C nicknames, 11 bytes each
 * Total 2 + C*(E+23) — 1102 for a box, 428 for the party. */
static int off_species(int box)  { (void)box; return 1; }
static int off_mons(int box)     { return 2 + g2_list_capacity(box); }
static int off_otnames(int box)  { return off_mons(box) + g2_list_capacity(box) * g2_list_entry_size(box); }
static int off_nicknames(int box){ return off_otnames(box) + g2_list_capacity(box) * 11; }

int g2_off_species_area(int box)      { return off_species(box); }
int g2_off_record(int box, int slot)  { return off_mons(box) + slot * g2_list_entry_size(box); }
int g2_off_otname(int box, int slot)  { return off_otnames(box) + slot * 11; }
int g2_off_nickname(int box, int slot){ return off_nicknames(box) + slot * 11; }

int g2_list_count(const uint8_t* list, int box) {
  if (!list) return -1;
  int cap = g2_list_capacity(box);
  int n = list[0];
  if (n > cap) return -1;
  if (list[off_species(box) + n] != G2_LIST_TERMINATOR) return -1;
  return n;
}

bool g2_list_plausible(const uint8_t* list, int box) {
  int n = g2_list_count(list, box);
  if (n < 0) return false;
  for (int i = 0; i < n; i++) {
    uint8_t sp = list[off_species(box) + i];
    if (sp == G2_LIST_EGG) continue;
    if (sp == 0 || sp > G2_SPECIES_MAX) return false;
  }
  return true;
}

bool g2_list_species(const uint8_t* list, int box, int slot, uint8_t* species, bool* is_egg) {
  int n = g2_list_count(list, box);
  if (n < 0 || slot < 0 || slot >= n) return false;
  uint8_t sp = list[off_species(box) + slot];
  if (is_egg)  *is_egg = (sp == G2_LIST_EGG);
  if (species) *species = sp;
  return true;
}

/* ------------------------------------------------------------------- charset */
/* English Gen-2 map, transcribed from Bulbapedia "Character encoding
 * (Generation II)". The letters/digits/space sit where Gen 1 puts them, but the
 * two generations are NOT the same table: Gen 1 has 'e-acute at 0xBA and the
 * apostrophe-letter glyphs at 0xBB-0xBF / 0xE4 / 0xE5, where Gen 2 has blanks,
 * umlauts at 0xC0-0xC5 and the apostrophe glyphs at 0xD0-0xD6. Gen 1 also has
 * katakana at 0xE9-0xEB where Gen 2 has '&', 'e-acute and an arrow. So this
 * table decodes Gen-2 text only.
 *   0x50   string terminator
 *   0x7F   space
 *   0x80-0x99 A-Z   0x9A-0x9F ( ) : ; [ ]
 *   0xA0-0xB9 a-z
 *   0xC0-0xC5 A O U a o u with umlauts   0xD0-0xD6 'd 'l 'm 'r 's 't 'v
 *   0xE0 '  0xE3 -  0xE6 ?  0xE7 !  0xE8 .  0xE9 &  0xEA e-acute  0xEF male
 *   0xF0 currency  0xF1 x  0xF2 .  0xF3 /  0xF4 ,  0xF5 female  0xF6-0xFF 0-9
 * Untranslatable glyphs become spaces, the policy gen3_decode_char uses. */
static int put(char* out, int cap, int n, const char* s) {
  int len = (int)strlen(s);
  if (n + len >= cap) return -1;         /* leave room for the NUL */
  memcpy(out + n, s, (size_t)len);
  return n + len;
}

/* `tmp` is caller scratch (>=2 bytes) so this stays re-entrant and static-free. */
static const char* g2_glyph(uint8_t c, char* tmp) {
  if (c >= 0x80 && c <= 0x99) { tmp[0] = (char)('A' + (c - 0x80)); tmp[1] = 0; return tmp; }
  if (c >= 0xA0 && c <= 0xB9) { tmp[0] = (char)('a' + (c - 0xA0)); tmp[1] = 0; return tmp; }
  if (c >= 0xF6)              { tmp[0] = (char)('0' + (c - 0xF6)); tmp[1] = 0; return tmp; }
  switch (c) {
    case 0x7F: return " ";
    case 0x9A: return "("; case 0x9B: return ")"; case 0x9C: return ":";
    case 0x9D: return ";"; case 0x9E: return "["; case 0x9F: return "]";
    case 0xC0: return "A"; case 0xC1: return "O"; case 0xC2: return "U";
    case 0xC3: return "a"; case 0xC4: return "o"; case 0xC5: return "u";
    case 0xD0: return "'d"; case 0xD1: return "'l"; case 0xD2: return "'m";
    case 0xD3: return "'r"; case 0xD4: return "'s"; case 0xD5: return "'t";
    case 0xD6: return "'v";
    case 0xE0: return "'";  case 0xE3: return "-";
    case 0xE6: return "?";  case 0xE7: return "!";  case 0xE8: return ".";
    case 0xE9: return "&";  case 0xEA: return "e";
    case 0xEF: return "\xE2\x99\x82";   /* male sign, spelled as in data_tables.c   */
    case 0xF0: return "$";  case 0xF1: return "x";  case 0xF2: return ".";
    case 0xF3: return "/";  case 0xF4: return ",";
    case 0xF5: return "\xE2\x99\x80";   /* female sign */
    default:   return " ";
  }
}

int g2_decode_text(const uint8_t* src, int max, char* out, int cap) {
  if (!out || cap <= 0) return 0;
  out[0] = 0;
  if (!src) return 0;
  int n = 0;
  char tmp[2];
  for (int i = 0; i < max; i++) {
    uint8_t c = src[i];
    if (c == 0x50) break;                /* string terminator */
    int m = put(out, cap, n, g2_glyph(c, tmp));
    if (m < 0) break;                    /* the rest does not fit */
    n = m;
  }
  /* No trimming: Gen 2 pads with the 0x50 terminator, so a trailing space is a
   * space the player actually entered. */
  out[n] = 0;
  return n;
}

bool g2_box_name(const uint8_t* names, int box, char* out, int cap) {
  if (!out || cap <= 0) return false;
  out[0] = 0;
  if (!names || box < 0 || box >= G2_NUM_BOXES) return false;
  g2_decode_text(names + box * 9, G2_BOXNAME_CHARS, out, cap);
  return true;
}

/* ------------------------------------------------------- derived properties */

uint8_t g2_hp_dv(const uint8_t dv[4]) {
  if (!dv) return 0;
  return (uint8_t)(((dv[0] & 1) << 3) | ((dv[1] & 1) << 2) | ((dv[2] & 1) << 1) | (dv[3] & 1));
}

bool g2_dv_shiny(const uint8_t dv[4]) {
  if (!dv) return false;
  if (dv[1] != 10 || dv[2] != 10 || dv[3] != 10) return false;
  return (dv[0] & 2) != 0;   /* 2,3,6,7,10,11,14,15 == bit 1 set */
}

int g2_unown_letter(const uint8_t dv[4]) {
  if (!dv) return 0;
  int v = (((dv[0] >> 1) & 3) << 6) | (((dv[1] >> 1) & 3) << 4)
        | (((dv[2] >> 1) & 3) << 2) |  ((dv[3] >> 1) & 3);
  return v / 10;             /* 0..25 = A..Z; 255/10 == 25, so never out of range */
}

bool g2_unown_dv_for_letter(uint8_t letter, uint8_t dv[4]) {
  if (!dv || letter > 25u) return false;
  /* The lowest v in this letter's own 10-wide range (25's own range is only
   * 250..255, six wide, since v tops out at 255 -- 25*10 = 250 is still the
   * right low end, same as g2_unown_letter's own "never out of range" note). */
  int v = (int)letter * 10;
  dv[0] = (uint8_t)(((v >> 6) & 3) << 1);   /* Atk: bits 1-2 = this field, bit 0 = 0 */
  dv[1] = (uint8_t)(((v >> 4) & 3) << 1);   /* Def */
  dv[2] = (uint8_t)(((v >> 2) & 3) << 1);   /* Spd */
  dv[3] = (uint8_t)(( v       & 3) << 1);   /* Spc */
  return true;
}

int g2_gender_from_dv(uint8_t atk_dv, uint8_t gender_ratio) {
  if (gender_ratio == 0xFF) return 2;                 /* genderless */
  if (gender_ratio == 0xFE) return 1;                 /* always female */
  if (gender_ratio == 0x00) return 0;                 /* always male   */
  /* Gen 2 compares the Attack DV against the species' ratio; the Gen-3 ratio
   * byte holds the same five values (31/63/127/191 = 12.5/25/50/75% female), so
   * scaling the DV into 256ths reproduces Gen 2's thresholds exactly:
   * DV<=1 for 7:1, <=3 for 3:1, <=7 for 1:1, <=11 for 1:3. */
  return (((int)(atk_dv & 0x0F) << 4) | 0x0F) <= (int)gender_ratio ? 1 : 0;
}

/* -------------------------------------------------------------- mon decoding */

bool g2_decode_mon(const uint8_t* rec, bool is_party, G2Mon* out) {
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!rec) return false;

  out->is_party  = is_party;
  out->species   = rec[0x00];
  out->held_item = rec[0x01];
  for (int i = 0; i < 4; i++) {
    out->moves[i] = rec[0x02 + i];
    out->pp[i]    = (uint8_t)(rec[0x17 + i] & 0x3F);
    out->pp_up[i] = (uint8_t)(rec[0x17 + i] >> 6);
  }
  out->otid = rd16be(rec + 0x06);
  out->exp  = rd24be(rec + 0x08);
  for (int i = 0; i < 5; i++) out->statexp[i] = rd16be(rec + 0x0B + i * 2);
  out->dv[0] = (uint8_t)(rec[0x15] >> 4);   /* Attack  */
  out->dv[1] = (uint8_t)(rec[0x15] & 0x0F); /* Defense */
  out->dv[2] = (uint8_t)(rec[0x16] >> 4);   /* Speed   */
  out->dv[3] = (uint8_t)(rec[0x16] & 0x0F); /* Special */
  out->hp_dv       = g2_hp_dv(out->dv);
  out->is_shiny    = g2_dv_shiny(out->dv);
  out->unown_letter = (uint8_t)g2_unown_letter(out->dv);
  out->friendship  = rec[0x1B];
  out->pokerus     = rec[0x1C];
  /* Caught data is written by Crystal only; G/S leave it zero (a Crystal mon
   * traded to G/S keeps it). */
  out->caught_time  = (uint8_t)(rec[0x1D] >> 6);
  out->caught_level = (uint8_t)(rec[0x1D] & 0x3F);
  out->ot_gender    = (uint8_t)(rec[0x1E] >> 7);
  out->caught_loc   = (uint8_t)(rec[0x1E] & 0x7F);
  out->caught_valid = (rec[0x1D] | rec[0x1E]) != 0;
  out->level        = rec[0x1F];

  if (is_party) {
    out->status = rec[0x20];
    out->cur_hp = rd16be(rec + 0x22);
    for (int i = 0; i < 6; i++) out->stats[i] = rd16be(rec + 0x24 + i * 2);
  }
  return true;
}

bool g2_list_mon(const uint8_t* list, int box, int slot, G2Mon* out) {
  if (!out) return false;
  memset(out, 0, sizeof *out);
  uint8_t listed = 0;
  bool egg = false;
  if (!g2_list_species(list, box, slot, &listed, &egg)) return false;

  const uint8_t* rec = list + off_mons(box) + slot * g2_list_entry_size(box);
  if (!g2_decode_mon(rec, box == G2_BOX_PARTY, out)) return false;
  out->is_egg = egg;
  /* The record's own species byte is the mon's real identity — the species list
   * is the menu's cache, and for an Egg it holds 0xFD instead. */
  if (out->species == 0 || out->species > G2_SPECIES_MAX) return false;

  g2_decode_text(list + off_otnames(box)   + slot * 11, G2_NAME_CHARS,
                 out->otname,   (int)sizeof out->otname);
  g2_decode_text(list + off_nicknames(box) + slot * 11, G2_NAME_CHARS,
                 out->nickname, (int)sizeof out->nickname);
  return true;
}

/* --------------------------------------------- whole-image helpers (tests) */

static int popcount_dex(const uint8_t* bits) {
  int n = 0;
  for (int dex = 1; dex <= G2_SPECIES_MAX; dex++)
    if (bits[(dex - 1) >> 3] & (1u << ((dex - 1) & 7))) n++;
  return n;
}

bool g2_read_header(const uint8_t* sav, const G2Save* sv, G2Header* out) {
  G2Offsets o;
  if (!out) return false;
  memset(out, 0, sizeof *out);
  out->player_gender = -1;
  out->current_box = -1;
  if (!sav || !sv || !g2_offsets(sv->version, &o)) return false;

  out->tid = rd16be(sav + o.tid);
  g2_decode_text(sav + o.player_name, 7, out->player, (int)sizeof out->player);
  out->money = rd24be(sav + o.money);
  out->johto_badges = sav[o.johto_badges];
  out->kanto_badges = sav[o.kanto_badges];
  out->dex_owned = popcount_dex(sav + o.dex_owned);
  out->dex_seen  = popcount_dex(sav + o.dex_seen);
  int cur = sav[o.current_box_no] & 0x0F;      /* low 4 bits = the open box     */
  out->current_box = cur < G2_NUM_BOXES ? cur : -1;
  if (o.player_gender) out->player_gender = sav[o.player_gender] & 1;
  return true;
}

int g2_box_count_at(const uint8_t* sav, const G2Save* sv, const G2Header* hd, int box) {
  uint32_t off = g2_list_offset(sv, box, hd ? hd->current_box : -1);
  if (!sav || !off) return -1;
  return g2_list_count(sav + off, box);
}

bool g2_box_mon_at(const uint8_t* sav, const G2Save* sv, const G2Header* hd,
                   int box, int slot, G2Mon* out) {
  uint32_t off = g2_list_offset(sv, box, hd ? hd->current_box : -1);
  if (!sav || !off) return false;
  return g2_list_mon(sav + off, box, slot, out);
}

bool g2_box_name_at(const uint8_t* sav, const G2Save* sv, int box, char* out, int cap) {
  G2Offsets o;
  if (!out || cap <= 0) return false;
  out[0] = 0;
  if (!sav || !sv || !g2_offsets(sv->version, &o)) return false;
  return g2_box_name(sav + o.box_names, box, out, cap);
}
