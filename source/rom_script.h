#ifndef ROM_SCRIPT_H
#define ROM_SCRIPT_H

#include <stdint.h>
#include <stdbool.h>
#include "rom_map.h"

/*
 * Gen-3 event-script walker + dialog-text decoder, read live out of the user's own ROM.
 *
 * Pure C: only <stdint.h>/<string.h> and rom_map.h's RomReadFn. Host-testable.
 *
 * ---- what this does ---------------------------------------------------------
 * RomObjectEvent.script points at bytecode for the Gen-3 script VM. This walks a
 * BOUNDED slice of that bytecode and returns the ROM address of the first dialog
 * string the NPC would show, then decodes it out of the game's own character set.
 *
 * ---- what it deliberately does NOT do ---------------------------------------
 * It is not an interpreter. It never evaluates a flag or a var, because those live
 * in the player's RAM/save and the answer is "it depends on where you are in the
 * story". When a script branches, this reports WHICH KIND of answer it found:
 *
 *   ROM_DLG_CERTAIN     no conditional was passed on the way to the text.
 *                       The NPC always says this. (MEASURED: 68% of Emerald NPCs,
 *                       63% of FireRed, and 1278/1278 of them agree with an
 *                       independent walker run over pret's source.)
 *   ROM_DLG_FIRST_OF_N  reached by falling through >=1 conditional. This is the
 *                       "default"/else line; the NPC may say something else first.
 *   ROM_DLG_BRANCH      only reachable by taking a conditional branch.
 *
 * The UI must surface that: label the box "says" vs "may say".
 */

#define ROM_DLG_CERTAIN     0
#define ROM_DLG_FIRST_OF_N  1
#define ROM_DLG_BRANCH      2

typedef struct {
  uint32_t text_addr;     /* ROM address of the Gen-3 string                       */
  uint8_t  certainty;     /* ROM_DLG_*                                             */
  uint8_t  via;           /* ROM_DLG_VIA_*, for the log only                       */
} RomDialogRef;

#define ROM_DLG_VIA_MSGBOX        0   /* loadword 0,X + callstd <msgbox std>        */
#define ROM_DLG_VIA_MESSAGE       1   /* message X / messageautoscroll / instant    */
#define ROM_DLG_VIA_TRAINER       2   /* trainerbattle intro text                   */
#define ROM_DLG_VIA_ITEM          3   /* ground item ball (no dialog; item id only) */

/* Walk from `script`. Returns false when nothing resolves inside the budget
 * (a ground item, a special-driven script, a runtime-built string in EWRAM).
 * Budgets: <=24 commands per straight-line path, <=16 alternate entry points,
 * call depth <=4. MEASURED: that already resolves 99.5% of talkable NPCs. */
bool rom_script_find_dialog(const RomCtx* c, uint32_t script, RomDialogRef* out);

/* Ground-item balls carry no dialog at all; they are
 *     setorcopyvar VAR_0x8000, <item> ; setorcopyvar VAR_0x8001, <qty> ; callstd 0|1
 * Returns the item id, or 0 if `script` is not that shape. */
uint16_t rom_script_item_ball(const RomCtx* c, uint32_t script);

/* Decode a Gen-3 string at `addr` into ASCII-safe text.
 *
 * Output is folded to what the GBA HUD font (libtonc sys8, ASCII 32..127) can draw:
 *   e-acute -> 'e'  (so "POKeMON"), curly quotes -> ' and ", ellipsis -> "...",
 *   male/female -> "(M)"/"(F)", Pokedollar -> '$'.
 * Control bytes:
 *   0xFE newline, 0xFA/0xFB prompt-scroll / prompt-clear -> newline (page break),
 *   0xFC ext-control + its operand bytes -> dropped,
 *   0xFD placeholder -> "<PLAYER>" / "<VAR1>" ... (the game substitutes at runtime),
 *   0xF7/0xF8/0xF9 -> 1 operand byte each, 0xFF terminates.
 * Multi-byte ligatures 53 54 -> "POKeMON" and 55 56 57 58 59 -> "POKeBLOCK".
 *
 * Returns the number of characters written (excluding NUL), or -1 if the bytes are
 * not a valid string (unknown byte / unknown ext code / no terminator in `cap`).
 * `*out_lines` (may be NULL) receives the line count. */
int rom_text_decode(const RomCtx* c, uint32_t addr, char* dst, int cap, int* out_lines);

#endif /* ROM_SCRIPT_H */
