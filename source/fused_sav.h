#ifndef FUSED_SAV_H
#define FUSED_SAV_H

#include <stdint.h>
#include <stdbool.h>

/*
 * "Fused" Pokemon SAVE — so the emulator build needs nothing but the .gba.
 *
 * The emulator build (PDNA_DELTA) edits its own 128 KiB flash chip, which means the user
 * has to place their .sav next to the ROM under exactly the right filename before the
 * tool has anything to show. On a phone that is the fiddliest step in the whole setup,
 * and getting it wrong lands on "Cannot read this save / copy your Pokemon .sav over
 * pokedna-delta.sav, then relaunch". Guy asked for the save to travel inside the image
 * so he does not have to pick it.
 *
 * Same mechanism as fused_rom.h — append the payload to the cartridge image and patch a
 * locator record — with a distinct magic so the two can coexist in one file.
 *
 * ---- PRECEDENCE, AND WHY IT MATTERS ------------------------------------------
 * The fused save is a SEED, never an authority. The flash chip always wins when it holds
 * a valid Gen-3 save, because that is where the user's edits live: once they save even
 * once, flash is the real save and this copy is a stale snapshot of the day the image was
 * built. The fused copy is read ONLY when flash is blank or unparseable — i.e. exactly
 * the "I have not copied a .sav in" case it exists to solve.
 *
 * Nothing here ever writes. Booting does not seed, program or erase the flash chip: the
 * image is loaded into RAM and the user's first in-app save writes it to flash through
 * the normal verified path. That keeps the toolkit's never-corrupt-user-data rule intact
 * — a fused save cannot overwrite a real one, because it never reaches the chip on its
 * own.
 *
 * ---- legality ----------------------------------------------------------------
 * A save file is the user's own play data, not Nintendo's code or art, so this carries
 * none of fuse_rom.h's copyright weight. It is still personal data: a fused image is
 * one person's save and should not be shared.
 */

/* Byte-identical in shape to PdnaFuseRec; kept a separate type so neither tool can
 * accidentally patch the other's record. See fused_rom.h for why it is const volatile
 * and why the magic is initialised element-by-element rather than as a string literal. */
typedef struct {
  char     magic[8];
  uint32_t offset;      /* byte offset of the appended save from the start of the file */
  uint32_t size;        /* its length in bytes; 0 => nothing fused                      */
} PdnaSavRec;

extern const volatile PdnaSavRec g_pdna_sav;

/* Is a save appended to this image? Fills *size with its length when so. */
bool fused_sav_present(uint32_t* size);

/* Copy the appended save out of cartridge space. `len` must be the recorded size (use
 * fused_sav_present); a short or over-long request fails rather than truncating. */
bool fused_sav_read(void* dst, uint32_t len);

#endif /* FUSED_SAV_H */
