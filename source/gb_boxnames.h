#ifndef GB_BOXNAMES_H
#define GB_BOXNAMES_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"    /* GbGame, GbField, gbf_off/len                              */
#include "gb_session.h"   /* GbSession, GbsStatus, gbs_read_field/gbs_write_field/finish */
#include "gen2_save.h"    /* G2_NUM_BOXES, G2_BOXNAME_CHARS                            */

/* gb_boxnames — pure-C Gen-2 box-name core (BACKLOG #94, docs/GEN12-PARITY-DESIGN.md
 * §1.0's own region table).
 *
 * GEN 1 IS NOT APPLICABLE: pokered/pokeyellow have no per-box name table at all (every
 * box banner is always literally "BOX n") — gbbn_read()/gbbn_rename() both refuse a
 * Gen-1 session outright (false / GBS_ERR_ARG) rather than inventing a field. A caller
 * asks gbbn_supported(session) BEFORE it ever offers the rename row, the same shape
 * gbt_field_present()/gbc_field_present() use elsewhere in this family.
 *
 * P1a review D4 (this replaces an earlier "thin wrapper over gen2_save.c/gen2_write.c"
 * design): the earlier gbbn_read went through g2_box_name_at(), which decodes with the
 * LOSSY g2_decode_text() (the display decoder — collapses <PK>/<MN>/the accented
 * letters/multiplication sign to spaces, gb_edit.h's own NAMES block), and gbbn_rename's
 * "unchanged" guard compared against THAT lossy read — so a name containing one of
 * those glyphs always looked "changed" (or, on a re-save of an unmodified name, quietly
 * dropped the special glyph). And the write went through g2w_set_box_name(), whose own
 * encode_checked() calls g2w_encode_text(), which has no "{XX}" hex-escape support at
 * all (gen2_write.c's enc_step() has a fixed multi-char table, not gb_char_encode's
 * escape mechanism) — a name containing an escaped byte could never be spelled at all.
 *
 * This core now reads/writes the 9 raw bytes at gbf_off(GBF_BOXNAMES) + box*9 directly
 * (the generic field primitive, gbs_read_field/gbs_write_field/gbs_finish — the same
 * shape gb_trainer.c's set_name()/get_name() use for every other name field in this
 * tree) and decodes/encodes with gb_name_decode/gb_name_encode, the SEQUENCE-SAFE pair
 * that adds the "{XX}" escaping needed to make Gen 2's charset genuinely reversible
 * (gb_edit.h's own NAMES block: "decode whole fields with gb_name_decode... USE
 * gb_name_decode, NOT a loop over gb_char_decode"). gbf_off(GBF_BOXNAMES) is
 * cross-checked against gen2_save.c's own g2_offsets().box_names by this core's host
 * test, so the two table sources cannot silently drift apart even though this core no
 * longer calls into gen2_save.c/gen2_write.c for the box-name path itself.
 */

#define GB_BOXNAME_GLYPHS G2_BOXNAME_CHARS   /* 8 -- BOX_NAME_LENGTH(9) minus the
                                              * terminator, same convention as gb_trainer.h's
                                              * GB_OT_GLYPHS */
#define GB_BOXNAME_BYTES  (GB_BOXNAME_GLYPHS + 1)   /* 9, BOX_NAME_LENGTH */

/* Is this session's generation one Gen 2 saves ever have box names for at all? False
 * for a Gen-1 session or a malformed one. */
bool gbbn_supported(const GbSession* s);

/* Read box `box`'s (0..G2_NUM_BOXES-1) name into `out` (UTF-8, `cap` >= GB_TEXT_MAX to
 * be safe, same convention as gb_trainer.h's name fields), via gb_name_decode — so a
 * name holding <PK>/<MN>/an accented letter/an escaped byte reads back exactly, never
 * as blank spaces. False on a Gen-1 session, a bad box index, or a malformed session --
 * `out[0]` is left 0 in every false case. */
bool gbbn_read(const GbSession* s, int box, char* out, int cap);

/* Rename box `box`. Refuses (GBS_ERR_ARG) before a single byte moves when: the session
 * is Gen 1 (gbbn_supported false), `box` is out of range, `utf8` is empty or all spaces
 * (P1a review D5 -- retail's own home/string.asm _InitString restores the OLD name
 * rather than ever storing a blank one, so accepting an empty string here would quietly
 * diverge from what the game itself does), `utf8` is over GB_BOXNAME_GLYPHS glyphs, or
 * this generation's charset cannot spell it exactly (gb_text_lossy(), the same charset
 * gate gb_trainer.c's set_name() uses).
 *
 * UNCHANGED IS UNTOUCHED: decodes the CURRENT raw bytes with gb_name_decode and compares
 * against `utf8` first — the sequence-safe decoder, not the lossy display one — and
 * skips the write, including the checksum refresh, when they already match. Otherwise
 * encodes with gb_name_encode (which supports the "{XX}" escape, unlike
 * g2w_encode_text) and writes through gbs_write_field + gbs_finish, the same generic
 * field-write primitive every other core in this family uses. */
GbsStatus gbbn_rename(GbSession* s, int box, const char* utf8);

#endif /* GB_BOXNAMES_H */
