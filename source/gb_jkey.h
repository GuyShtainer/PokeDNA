/* SPDX-License-Identifier: GPL-3.0-or-later
 * gb_jkey.h -- BACKLOG #234 slice 4: what a Game Boy save's undo journal is keyed on, and what its steps are called.
 * PURE C (gb_session + journal only; no tonc/FatFs) so tests/host_gbjkey_test.c pins the real code on the PC.
 *
 * THE KEY. journal.h's jrn_key64 is FNV over a canonical name + TID + SID + two booleans. A Game Boy save has no SID and
 * no gender byte in the Gen-1 layout, so the key is: the trainer name (up to its 0x50 terminator, re-terminated with the
 * 0xFF the engine canonicalizes on), the trainer ID (u16, big-endian on the cart), and a fixed marker in the SID slot
 * (0x4740 + the field-table game) that keeps a Game Boy journal from ever sharing a directory with a Gen-3 save of the
 * same name and ID. Identity edits (trainer rename / ID) move the key; jrn_app writes the redirect. */
#ifndef GB_JKEY_H
#define GB_JKEY_H

#include <stdint.h>

#include "gb_session.h"

/* The journal key of the OPEN resident session `s` as its image reads NOW; 0 when the session is not a resident image
 * (streamed / not open) or the identity fields cannot be located. */
uint64_t gb_journal_key(const GbSession* s);

/* The history name (ASCII, <= 24 chars) of the step a gb_persist/gb_hold_commit tag records ("move" -> "Box move"); an
 * unknown tag reads "Edit". Never NULL. */
const char* gb_step_name(const char* tag);

#endif /* GB_JKEY_H */
