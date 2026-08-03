#ifndef FLASHSAVE_H
#define FLASHSAVE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 128 KiB GBA FLASH save I/O — the storage backend for the EMULATOR build
 * (pokedna-delta.gba), which has no flashcart and therefore no microSD.
 *
 * Idea: an emulator gives every ROM its own save file. If the user drops their
 * Pokemon save in as PokeDNA's save, PokeDNA is editing it directly. So this build
 * reads and writes ITS OWN 128 KiB flash save instead of a .sav on a card:
 *
 *   Delta / RetroArch:  copy  "Pokemon Emerald.sav"  ->  "pokedna-delta.sav"
 *                       edit on the phone, then copy it back.
 *
 * ---- what this costs (accepted by Guy) --------------------------------------
 * NO backups. The verified-write pipeline on the SD build writes a .tmp, byte-compares
 * it, and keeps an immutable .bak; none of that is possible in a single fixed save
 * slot. This build verifies the write by reading it back, but if that fails the
 * original is already gone. The user's own copy of the .sav is the backup.
 *
 * ---- hardware notes ----------------------------------------------------------
 * 128 KiB flash is two 64 KiB banks sharing one 64 KiB window at 0x0E000000, selected
 * by a bank-switch command. The chip is BYTE-wide: every access must be u8, and the
 * cart bus must be in 8-bit SRAM wait mode (REG_WAITCNT). Reads are plain loads;
 * writes need Atmel/Sanyo-style command sequences to 0x0E005555 / 0x0E002AAA.
 *
 * Erase-then-program is per 4 KiB sector. Both VBA-M (Delta's core) and mGBA
 * implement this command set, so the same code works on real hardware and in the
 * emulators — but the emulator is the only target that matters here.
 */

#define FLASHSAVE_SIZE      0x20000u   /* 128 KiB = Gen-3 save size          */
#define FLASHSAVE_SECTOR    0x1000u    /* 4 KiB erase granularity            */
#define FLASHSAVE_BANK_SIZE 0x10000u   /* 64 KiB window per bank             */

/* Probe the chip (reads its manufacturer/device id). False means no flash save was
 * allocated — usually the emulator failed to detect the save type, i.e. the
 * FLASH1M_V signature is missing from the ROM image. */
bool flashsave_probe(uint16_t* out_id);

/* Read the whole 128 KiB save into dst. */
bool flashsave_read(uint8_t* dst, uint32_t len);

/* Erase + program the whole 128 KiB save from src, then read back and byte-compare.
 * Returns false if any sector failed to verify — the save is then in an UNKNOWN
 * state, which is why the UI must tell the user to restore their own copy. */
bool flashsave_write(const uint8_t* src, uint32_t len);

#endif /* FLASHSAVE_H */
