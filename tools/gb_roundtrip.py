#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""gb_roundtrip.py — boot a REAL Game Boy Pokemon game on a save and ask the
game itself whether it accepts it.

Every other test in this tree checks a save against *our* parser. This one
checks it against Game Freak's: it runs the retail ROM headlessly in mGBA,
drives past the title screen, picks CONTINUE, and reports what the game says —
the player's name, badges, Pokedex count, play time and party — or the fact
that it refused the save ("The file data is destroyed!" / "The save file is
corrupted!" / no CONTINUE option at all).

Why the game and not a parser: Gen 1/2 boxes are four parallel structures
(count, species list, records, OT names, nicknames) that must agree. A parser
we wrote cannot falsify a layout we wrote. The ROM can.

How it reads the screen
-----------------------
Not OCR. Gen 1 and Gen 2 place *character codes* straight into the BG/window
tilemap (the font is loaded so tile id == charmap value; see
pokered/constants/charmap.asm and pokecrystal/constants/charmap.asm), so
composing the visible 20x18 tiles out of VRAM and running them back through the
charmap yields exactly the string the game drew.

Requirements
------------
The mGBA Python bindings (libmgba-py). mGBA's GB/GBC core is used, not the GBA
core: `mgba.core.load_path()` returns a GB core for .gb/.gbc images. Point the
tool at a vendored bindings directory with --mgba-vendor or $GB_ROUNDTRIP_MGBA,
or just run it with an interpreter that can already `import mgba`.

Usage
-----
    gb_roundtrip.py --rom Gold.gbc --sav Gold.sav --out shots/
    gb_roundtrip.py --rom Red.gb  --sav edited.sav --expect accept   # gate
    gb_roundtrip.py --rom Red.gb  --sav Red.sav --json report.json

    # a gate that defends the DATA and not just the boot:
    gb_roundtrip.py --rom Crystal.gbc --sav edited.sav \
        --expect-name MattiaP --expect-party HACKED --expect-party-count 6

    # this tool's own regression tests (~3 s, needs the three ROMs + saves):
    gb_roundtrip.py --selftest --corpus /path/to/roms/gb

Exit status: 0 every assertion held, 1 an assertion failed, 2 harness/driver
failure (mGBA missing, ROM unreadable, the emulated SRAM changed length, or an
--expect combination that can never be satisfied).

--selftest builds the known data-loss saves from the corpus (in a scratch dir;
the corpus is never written), boots each, and asserts on this tool's own exit
code — so every claim in the sections below is a test that fails if the claim
stops being true, including the 0x3D96 backup address and the 256-byte figure.

WHY THE CONTENT ASSERTIONS EXIST — read this before writing a gate
------------------------------------------------------------------
--expect and --expect-name alone are NOT a data-loss gate. They ask "did the
game boot, and is this still the same trainer" — which is true of almost every
way an edit can be lost. Two real failures, both of which a verdict+name gate
waves through with exit 0 while the game silently throws the edit away:

  * Crystal, a party nickname rewritten with the primary checksum left stale.
    The game rejects the primary, boots the BACKUP copy, and draws the OLD
    nickname. Verdict accept, PLAYER still right, edit gone.
        --expect-party HACKED        <- this is what fails it
  * Gen 1, a party whose count byte says 6 while the species terminator sits at
    index 2. The game lists two Pokemon; four are gone. Verdict accept, PLAYER
    right, party silently four short.
        --expect-party-count 6       <- this is what fails it

So assert on what you WROTE, every time. --expect-party / --expect-no-party /
--expect-party-count read the party screen the game drew; --expect-screen /
--expect-no-screen search every screen the run recorded (boot transcript, main
menu, continue screen, overworld, party, refusal). All of them are evaluated
together with --expect and --expect-name, and every failure is printed, not
just the first.

Prefer the party assertions when the thing you wrote is in the party: the
screen ones search a wide net that includes the boot screens, and Gen 2 draws a
whole alphabet row while loading its font, so a needle of one or two letters
can match something that is not your data. Nicknames and names are long enough
not to care; "A" is not.

NOTE ON SAFETY: the .sav is read once into an in-memory VFile. The file on disk
is never written, so pointing this at an original cartridge dump cannot damage
it.

--dump-save writes out what the emulated cartridge SRAM looks like *after* the
game has loaded it. A dump is EVIDENCE, NOT A SAVE YOU CAN RESTORE. On an
MBC3+RTC cart (Gold/Silver/Crystal) mGBA keeps a 48-byte clock footer past the
end of SRAM and writes it from the HOST clock at load, so:
  * Gold.sav arrives with a footer and the dump's differs from it (12 of 48
    bytes, measured);
  * Crystal.sav arrives with none and the dump GROWS one — 32768 bytes in,
    32816 out.
Either way the dump's clock is the emulator's. Restoring one to a cartridge
sets the in-game clock to whenever the emulator happened to run, which in Gen 2
reschedules berries, phone calls and every daily event. Pass --dump-keep-rtc
for a dump whose footer is the input's (or absent, if the input had none). The
footer is excluded from the SRAM diff for the same reason (see _report_sram),
and an SRAM length change — as opposed to a footer appearing — is a hard error.

Measured recipe (2026-08-09, Guy's own cartridge dumps, --rtc pinned)
--------------------------------------------------------------------
Nothing here is hardcoded — the driver watches the screen — but these are the
costs, so a caller can budget:

    game     frames to the main menu   frames to the overworld   buttons
    Red             1004                       1352              ~11x START, 2x A
    Gold             360                        672              ~4x START, 2x A
    Crystal          468                        836              ~4x START, 2x A

A whole run including the party read is ~1.5 s of wall clock. Eleven runs
(3 baselines + 8 deliberately broken saves) take about 2.7 s total, so this is
cheap enough to gate every single write a test produces.

What this gate already caught
-----------------------------
Both generations refuse a bad save, but NOT the same way, and the difference is
the reason a writer needs this and not just a checksum unit test:

  * Gold/Silver — a stale primary checksum is fatal and LOUD: "The save file is
    corrupted!", no load. The backup is consulted, but only rescues the save if
    the backup itself validates (pokegold/engine/menus/save.asm:554-556), and
    on Guy's Gold.sav it does not — see the next section, which is the whole of
    the "a valid backup does not rescue a bad primary" puzzle.
  * Crystal — a stale primary checksum is SILENT: the game loads the backup
    copy instead and boots normally, showing the OLD data
    (pokecrystal/engine/menus/save.asm:613-627). An edit written without
    recomputing the checksum simply vanishes, with no error anywhere. Nothing
    but booting the real game reveals that.

The G/S backup copy lives at 0x3D96, not 0x3D69   (measured 2026-08-09)
-----------------------------------------------------------------------
Load-bearing for anyone reading this tool's Gen-2 output, because the toolkit's
own k_gs_mirror (source/gen2_save.c) currently says 0x3D69 and calls 0x3D96 a
transposed digit. Measured against the running ROM, it is the other way round.

Method — let the GAME do the transcribing. Tag the primary save block at 20
offsets with unique 8-byte patterns, repair the primary checksum so the game
takes the primary path, boot Gold, dump SRAM, and look up where each tag was
copied to. TryLoadSaveFile rewrites the backup on every successful load, so the
destination is measured, not read off a wiki:

    primary 0x2009..0x222E -> 0x15C7   sBackupPlayerData1   550 B
    primary 0x222F..0x23D8 -> 0x3D96   sBackupPlayerData2   426 B   <-- 0x3D96
    primary 0x23D9..0x2855 -> 0x0C6B   sBackupPlayerData3  1149 B
    primary 0x2856..0x2889 -> 0x7E39   sBackupCurMapData     52 B
    primary 0x288A..0x2D68 -> 0x10E8   sBackupPokemonData  1247 B

(section names and order from pokegold/ram/sram.asm "Backup Save 1/2/3" and
pokegold/layout.link; the four unchanged rows agree with k_gs_mirror.)

Confirmation, because a measurement can be misread as easily as a wiki: break
Gold.sav's primary checksum, rebuild the backup from the primary using each
candidate map, recompute the backup checksum over that map's own spans, boot.
    backup rebuilt at 0x3D69 -> "The save file is / corrupted!"
    backup rebuilt at 0x3D96 -> loads, PLAYER=MattiaP
That is the ROM casting the deciding vote, which is the only vote this file
accepts.

Why 0x3D69 looked convincing: sPlayerData2 opens with 45 zero bytes (232 of its
426 bytes are zero). Slide the 426-byte window back by 45 and it still matches
the primary byte-for-byte — so the REAL backup at 0x3D96 also produces a
"byte-perfect mirror" at 0x3D69, and the stored backup checksum agrees with the
wrong window as well, because the 45 bytes that window adds are zero and the 45
it drops are a tail it never covers. Gold.sav sums to 0xAEF9 over 0x3D69..
0x3F12 and 0xC03D over the real 0x3D96..0x3F3F, and the file stores 0xAEF9. A
real cartridge save caught the first transposition; only the running ROM could
catch the second, because the wrong constant agrees with real data too.

Consequence: Guy's Gold.sav has an INVALID BACKUP (stored 0xAEF9, true 0xC03D).
There is no unexplained refusal and no rescue failure — the backup was never
valid. A G/S save with a broken primary and a genuinely valid backup boots.

What an untouched load actually writes to SRAM   (measured, Gold.sav)
---------------------------------------------------------------------
The old claim — "Gold.sav ships with a STALE backup, and an untouched load
rewrites 285-604 bytes healing it" — was half right, self-contradicted by the
same report's "the backup is byte-perfect, correctly-checksummed", and quoted a
number that is not a number. Measured:

  * The backup IS stale, and the contradiction was an artefact of the wrong
    address. At the real 0x3D96, Gold.sav's backup differs from its primary in
    253 of sBackupPlayerData2's 426 bytes and its stored checksum is wrong
    (0xAEF9 stored, 0xC03D true). At 0x3D69 the same backup looks byte-perfect
    AND correctly-checksummed. Both halves of the contradiction came from one
    bad constant.
  * But the load is NOT a repair, and "healing" is the wrong word. Gen 2
    rewrites the backup from live WRAM on EVERY successful load, stale or not
    (pokegold/engine/menus/save.asm:538-552 ValidateBackupSave,
    SaveBackupOptions, SaveBackupPlayerData, SaveBackupPokemonData,
    SaveBackupChecksum; pokecrystal/engine/menus/save.asm:596-611 is the same
    list plus RestoreGSBallFlag). WRAM at that moment is the primary as just
    loaded, so afterwards the backup is byte-EQUAL to the primary (measured:
    253 of 426 differ before, 0 of 426 after). Guy's Crystal.sav, whose backup
    already equals its primary, runs the identical code path and changes ZERO
    bytes. So the byte count measures how stale the INPUT's backup was — it is
    not a property of the game, and a Gen-2 writer cannot read anything into it
    beyond "this file's two copies disagreed by this much".

Measured on Gold.sav, one load changes:

    256 bytes of save data, all of it in the BACKUP copy
          1 B  sBackupPlayerData1        253 B  sBackupPlayerData2
          1 B  sBackupOptions              1 B  sBackupChecksum
      (Crystal.sav, same code path, backup already current: 0 bytes.)
    17-336 bytes of SRAM SCRATCH, depending only on where the run stops:
          the G/S SRAM window stack, $B800-$BFFF = file 0x1800-0x1FFF
          (pokegold/ram/sram.asm "SRAM Window Stack", layout.link SRAM $00).
          It is a stack, not save data. "285-604" was this and nothing else,
          and it was never a range: 273+12 footer with the party read,
          592+12 without it — two stopping points of one run.
    12 of the 48 RTC-footer bytes, which are mGBA's and not the game's.

So the figures to regress against are: Gold.sav 256 bytes, Crystal.sav 0, both
entirely backup-side, with scratch and footer counted separately. The signal
worth alarming on is a changed PRIMARY (sram_changed_primary_bytes): on the
good-primary path the game never writes it, so a primary that moved means the
game rejected it and booted the backup.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

# --------------------------------------------------------------------------
# Game Boy hardware constants (Pan Docs). The GB screen is 20x18 tiles.
# --------------------------------------------------------------------------
SCREEN_COLS = 20
SCREEN_ROWS = 18
GB_W, GB_H = 160, 144

# Battery-backed SRAM sizes an MBC can present (Pan Docs, cartridge header
# byte 0x149). A .sav is SRAM followed by an OPTIONAL RTC footer, so the SRAM
# length is the largest of these that fits and the remainder is the footer.
GB_SRAM_SIZES = (0x200, 0x800, 0x2000, 0x8000, 0x10000, 0x20000)

# Where a *loaded* game is allowed to scribble without it meaning anything.
# Gen 1 (pokered/ram/sram.asm "Sprite Buffers"): sSpriteBuffer0..2, three
#   SPRITEBUFFERSIZE ($188) blocks at the start of bank 0, used as
#   sprite-decompression scratch (pokered/home/pics.asm:95-97).
# Gen 2 (pokegold + pokecrystal ram/sram.asm, layout.link SRAM $00):
#   "Scratch"/sDecompressScratch is $60 tiles = 0x600 at $A000, and G/S also
#   parks its window STACK at $B800-$BFFF. A stack is not save data; leaving it
#   in the diff is what turned 273 bytes into 592 between two runs of the same
#   save and got called "the game healing one copy from the other".
SCRATCH_RANGES = {
    1: (("sprite-decompression buffers", 0x0000, 0x0497),),
    2: (("sDecompressScratch", 0x0000, 0x05FF),),
}
GS_SCRATCH_EXTRA = ("SRAM window stack", 0x1800, 0x1FFF)

# The Gen-2 save copies, so a diff can say WHICH copy the game touched. These
# are whole SECTIONS, not just the checksummed spans — the options block, the
# two SAVE_CHECK_VALUEs and the stored checksum belong to the copy that owns
# them, and a diff that files them under "other" is a diff that hides which
# copy the game rewrote.
#   G/S     "Save" $A000-$AD6B in bank 1 -> 0x2000-0x2D6B (sOptions,
#           sCheckValue1, sGameData 0x2009-0x2D68, sChecksum, sCheckValue2);
#           "Backup Save 1" 0x0C6B-0x17EC, "Backup Save 2" 0x3D96-0x3F3F (the
#           MEASURED base, see the docstring), "Backup Save 3" 0x7E30-0x7E6F.
#           pokegold/ram/sram.asm + layout.link.
#   Crystal pokecrystal/ram/sram.asm:58-103: "Backup Save" at $B200 -> 0x1200,
#           sBackupGameData 0x1209-0x1D82, ds $18a, sBackupChecksum 0x1F0D,
#           sBackupCheckValue2 0x1F0F; "Save" 0x2000, sGameData 0x2009-0x2B82,
#           ds $18a, sChecksum 0x2D0D, sCheckValue2 0x2D0F. Both stored
#           checksums on Guy's Crystal.sav compute to 0xADEF over those spans.
GEN2_LAYOUT = {
    "gs": {"primary": ((0x2000, 0x2D6B),),
           "backup": ((0x0C6B, 0x17EC), (0x3D96, 0x3F3F), (0x7E30, 0x7E6F))},
    "crystal": {"primary": ((0x2000, 0x2D0F),),
                "backup": ((0x1200, 0x1F0F),)},
}

# Both generations draw every party member's HP as "cur/max" under its name, so
# counting those is how many Pokemon the GAME thinks are in the party — which
# is the number a party edit has to be checked against, not the count byte the
# editor wrote. Gen 1 puts the HP on its own row, Gen 2 on the name row; the
# regex does not care.
PARTY_HP_RE = re.compile(r"\d+\s*/\s*\d+")

rLCDC = 0xFF40
rSCY = 0xFF42
rSCX = 0xFF43
rWY = 0xFF4A
rWX = 0xFF4B

# Button bit indices. mgba/gb.py aliases KEY_A..KEY_RIGHT to the GBA_KEY_*
# values, so the low 8 bits of the GBA order are also the GB order.
BUTTONS = {"A": 0, "B": 1, "SELECT": 2, "START": 3,
           "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7}

# --------------------------------------------------------------------------
# Charmap: tile id -> character.
#
# Transcribed from pokered/constants/charmap.asm (the $80.. block, lines
# ~148-200 "Actual characters (from gfx/font/font.png)"); pokecrystal's
# charmap.asm is identical over this range. Only the letter/digit/punctuation
# block is decoded — $60-$7E are box-drawing and battle glyphs that differ
# between the two games and carry no text.
# --------------------------------------------------------------------------


def _build_charmap():
    cm = {}
    for i, c in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
        cm[0x80 + i] = c                       # charmap "A", $80
    for i, c in enumerate("abcdefghijklmnopqrstuvwxyz"):
        cm[0xA0 + i] = c                       # charmap "a", $a0
    for i, c in enumerate("0123456789"):
        cm[0xF6 + i] = c                       # charmap "0", $f6
    cm.update({
        0x7F: " ",   # charmap " ",  $7f
        0x9A: "(", 0x9B: ")", 0x9C: ":", 0x9D: ";", 0x9E: "[", 0x9F: "]",
        # "é": $ba in Gen 1 (pokered charmap.asm), $ea in Gen 2 (pokecrystal
        # charmap.asm:188). Neither code is a text glyph in the other game
        # ($ea is an unused katakana in Gen 1, $ba is unassigned in Gen 2), so
        # both can be decoded unconditionally. Flattened to "e" so "POKéMON"
        # greps as "POKeMON".
        0xBA: "e", 0xEA: "e",
        0x6D: ":",   # <COLON> — the narrow colon used in clock/time fields
        0xE0: "'", 0xE3: "-", 0xE6: "?", 0xE7: "!", 0xE8: ".",
        0xE9: "&",   # Gen 2 only; unused katakana in Gen 1
        0xF3: "/", 0xF4: ",",
        0xF1: "x",   # "×"
        0xEF: "M",   # "♂"
        0xF5: "F",   # "♀"
        0xE1: "P", 0xE2: "M",   # <PK><MN>: the two tiles that spell "PkMn"
        0x00: " ",   # blank tile on a cleared screen
        0x50: " ",   # "@" string terminator, drawn as blank filler in Gen 2
    })
    return cm


CHARMAP = _build_charmap()
UNKNOWN = "."      # any tile that is graphics, not text

# Phrases the games print when they refuse a save.
#   pokered/data/text/text_3.asm:1     "The file data is / destroyed!"
#   pokecrystal/data/text/common_3.asm:213  "The save file is / corrupted!"
REFUSAL_PHRASES = ("destroyed", "corrupted")


class RoundtripError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------
class GbDriver:
    """A booted GB/GBC core you can step, press buttons on, and read.

    Ordering rules are the same ones drive.py documents for the GBA core and
    they are just as load-bearing here:
      1. mgba.log.silence()      before creating a core
      2. set_video_buffer()      before reset()
      3. load_save()             before reset()
      4. keep a Python reference to the save VFile or the GC frees it
    """

    # Cartridge header titles (0x134..0x142). "POKEMON RED", "POKEMON YEL",
    # "POKEMON_GLD" and "PM_CRYSTAL" were read off Guy's own dumps; the other
    # two are the documented siblings. The generation only annotates the SRAM
    # diff, so an unknown title degrades to generation 0 and loses nothing else.
    GEN1_TITLES = ("POKEMON RED", "POKEMON BLUE", "POKEMON YEL")
    GEN2_TITLES = ("POKEMON_GLD", "POKEMON_SLV", "PM_CRYSTAL")

    def __init__(self, rom_path, save_path=None, vendor=None, log=print,
                 rtc=None):
        self._log_fn = log or (lambda *a, **k: None)
        self.rom_path = str(rom_path)
        self.frames_run = 0
        self.save_in = None
        self.rtc_pinned = None

        for cand in (vendor, os.environ.get("GB_ROUNDTRIP_MGBA")):
            if cand and Path(cand).is_dir() and str(cand) not in sys.path:
                sys.path.insert(0, str(cand))
        try:
            import mgba.core, mgba.image, mgba.log, mgba.vfs
            import mgba.gb          # noqa: F401 — registers the GB subclass
            from mgba import ffi
        except Exception as exc:                       # pragma: no cover
            raise RoundtripError(
                f"the mGBA Python bindings are not importable ({exc!r}).\n"
                "Pass --mgba-vendor /path/to/dir-containing-mgba, set "
                "$GB_ROUNDTRIP_MGBA, or run this with an interpreter that can "
                "import mgba (e.g. the harness venv).") from exc
        self._mgba, self._ffi = mgba, ffi

        mgba.log.silence()                              # RULE 1
        if not os.path.isfile(self.rom_path):
            raise RoundtripError(f"ROM not found: {self.rom_path}")
        core = mgba.core.load_path(self.rom_path)
        if core is None:
            raise RoundtripError(f"mGBA could not load ROM: {self.rom_path}")
        if type(core).__name__ != "GB":
            raise RoundtripError(
                f"{os.path.basename(self.rom_path)} loaded as a "
                f"{type(core).__name__} core, not GB — this tool drives Game "
                "Boy / Game Boy Color games only.")
        self._core = core
        try:
            self.game_title = core.game_title.rstrip("\0 ")
        except Exception:
            self.game_title = "?"
        self.generation = (1 if self.game_title in self.GEN1_TITLES else
                           2 if self.game_title in self.GEN2_TITLES else 0)

        # mGBA always hands out a 256x224 buffer for the GB core (the Super
        # Game Boy border size) but only *uses* the whole thing in SGB mode.
        # Where the 160x144 screen lands depends on the emulated model:
        #   SGB (Red is detected as GB_MODEL_SGB, 0x20): centred at (48,40),
        #     surrounded by the SGB border artwork;
        #   CGB (Yellow/Gold/Crystal are all detected as GB_MODEL_CGB, 0x80):
        #     top-left at (0,0), the rest of the buffer left black.
        # Cropping at the wrong origin silently produces screenshots of the
        # wrong 160x144 window — the verdict still reads correctly (it comes
        # from the tilemap) but the human evidence is a lie. The model is only
        # known after reset(), so the crop is chosen there.
        w, h = core.desired_video_dimensions()
        self.width, self.height = w, h
        self.crop = (0, 0)
        self._screen = mgba.image.Image(w, h)
        core.set_video_buffer(self._screen)             # RULE 2

        self._save_vf = None
        if save_path is not None:
            if not os.path.isfile(save_path):
                raise RoundtripError(f"save file not found: {save_path}")
            data = Path(save_path).read_bytes()
            self.save_in = data
            vf = mgba.vfs.VFile.fromEmpty()
            vf.write(data, len(data))
            vf.seek(0, whence=0)
            core.load_save(vf)                          # RULE 3
            self._save_vf = vf                          # RULE 4
            self._log(f"attached save: {len(data)} bytes")

        # Gold/Silver/Crystal are MBC3+RTC carts and print the day and time of
        # day on the main menu, so with mGBA's default (host wall clock) the
        # screen text — and therefore any screenshot comparison — changes every
        # run. Pin it to make a run reproducible.
        if rtc is not None:
            try:
                core.rtc.use_fixed(rtc)
                self.rtc_pinned = rtc
            except Exception as exc:
                self._log(f"WARNING: could not pin the RTC ({exc!r})")
        core.reset()

        # GB_MODEL_SGB is 0x20 and GB_MODEL_SGB2 is 0x60 (mgba lib.GB_MODEL_*),
        # so bit 5 means "an SGB is drawing a border round the screen".
        try:
            self.model = int(core._native.model)
        except Exception:
            self.model = 0
        if (w, h) == (256, 224) and (self.model & 0x20):
            self.crop = (48, 40)

        try:
            self._audio = core.get_audio_channels()
            self._audio.set_rate(32768)
        except Exception:
            self._audio = None
        self._log(f"loaded {os.path.basename(self.rom_path)}: "
                  f"title={self.game_title!r} video={w}x{h} "
                  f"model=0x{self.model:02X} crop={self.crop}")

    def _log(self, msg):
        self._log_fn(f"[gb] {msg}")

    # ---------------------------------------------------------- stepping --
    def step(self, frames=1, buttons=None):
        mask = 0
        if isinstance(buttons, int):
            mask = buttons
        elif buttons:
            for b in str(buttons).replace(",", "+").split("+"):
                b = b.strip().upper()
                if b not in BUTTONS:
                    raise ValueError(f"unknown button {b!r}")
                mask |= 1 << BUTTONS[b]
        for _ in range(int(frames)):
            self._core.set_keys(raw=mask)
            self._core.run_frame()
            self.frames_run += 1
            if self._audio is not None:
                try:
                    self._audio.clear()
                except Exception:
                    pass
        return self

    def tap(self, button, hold=8, release=8):
        """Hold then release. A one-frame press is not reliably seen: menus
        latch on the 0->1 edge and debounce (same 8/8 rule as drive.py)."""
        self.step(hold, button)
        self.step(release)
        return self

    # ------------------------------------------------------------ memory --
    def read_mem(self, addr, length=1):
        """Bytes from the emulated GB bus (0x0000-0xFFFF)."""
        m = self._core.memory.u8
        return bytes(m[addr + i] for i in range(length))

    # ------------------------------------------------------------ screen --
    def screen_rows(self):
        """The 18 visible text rows, composed from BG + window per LCDC.

        Gen 1/2 draw menus and text boxes on the *window* layer (LCDC bit 5,
        map selected by bit 6) and the map on the BG layer, so reading one map
        only gets you half the screen. WX is offset by 7 in hardware.
        """
        m = self._core.memory.u8
        lcdc = m[rLCDC]
        scy, scx = m[rSCY], m[rSCX]
        wy, wx = m[rWY], m[rWX]
        bg_map = 0x9C00 if lcdc & 0x08 else 0x9800
        win_map = 0x9C00 if lcdc & 0x40 else 0x9800
        win_on = bool(lcdc & 0x20) and wy <= 143 and wx <= 166
        rows = []
        for y in range(SCREEN_ROWS):
            out = []
            py = y * 8
            for x in range(SCREEN_COLS):
                px = x * 8
                if win_on and py >= wy and px + 7 >= wx:
                    ty = (py - wy) // 8
                    tx = (px + 7 - wx) // 8
                    v = m[win_map + (ty & 31) * 32 + (tx & 31)]
                else:
                    ty = ((scy + py) // 8) & 31
                    tx = ((scx + px) // 8) & 31
                    v = m[bg_map + ty * 32 + tx]
                out.append(CHARMAP.get(v, UNKNOWN))
            rows.append("".join(out).rstrip())
        return rows

    def screen_text(self):
        return "\n".join(self.screen_rows())

    # ------------------------------------------------------------- pixels --
    def rgb_array(self):
        import numpy as np
        buf = np.frombuffer(bytes(self._ffi.buffer(self._screen.buffer)),
                            dtype=np.uint8)
        buf = buf.reshape(self.height, self._screen.stride, 4)
        a = buf[:, :self.width, :3]
        cx, cy = self.crop
        # Always crop to the real 160x144 screen — a (0,0) crop still has to
        # cut away the unused right/bottom of mGBA's 256x224 GB buffer.
        return a[cy:cy + GB_H, cx:cx + GB_W].copy()

    def screenshot(self, path, scale=2):
        from PIL import Image
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        im = Image.fromarray(self.rgb_array(), "RGB")
        if scale > 1:
            im = im.resize((im.width * scale, im.height * scale),
                           Image.NEAREST)
        im.save(p)
        return str(p)

    # --------------------------------------------------------------- save --
    def save_bytes(self):
        if self._save_vf is None:
            return None
        self._save_vf.seek(0, whence=0)
        return self._save_vf.read_all()

    # ------------------------------------------------------------ waiting --
    def wait_for(self, predicate, timeout_frames=1800, sample=4, buttons=None,
                 what="a screen"):
        """Advance until predicate(rows) is true. Returns (rows, frames)."""
        spent = 0
        while spent <= timeout_frames:
            rows = self.screen_rows()
            if predicate(rows):
                return rows, spent
            self.step(sample, buttons)
            spent += sample
        raise RoundtripError(
            f"timed out after {timeout_frames} frames waiting for {what}")

    def mash(self, button, predicate, tries=12, gap=30, what="a screen"):
        """Tap `button` up to `tries` times until predicate(rows) holds.

        Needed because these games ignore input during fixed delays — e.g.
        DisplayContinueGameInfo ends with `ld c, 30 / jp DelayFrames`
        (pokered/engine/menus/main_menu.asm:378-379) before it starts polling
        the joypad, so a single well-timed press is silently eaten.
        """
        for _ in range(tries):
            rows = self.screen_rows()
            if predicate(rows):
                return rows, True
            self.tap(button)
            self.step(gap)
        rows = self.screen_rows()
        return rows, bool(predicate(rows))

    def settle(self, quiet=4, sample=4, timeout_frames=300):
        """Run until the composed text stops changing, then return it.

        Gen 2 slides its text boxes in from the edge of the screen over a dozen
        frames, so sampling the instant a keyword appears captures a
        half-drawn box (the player-name row is simply missing). Everything that
        reads a screen must settle first.
        """
        prev = None
        same = 0
        spent = 0
        while spent <= timeout_frames:
            rows = self.screen_rows()
            cur = "\n".join(rows)
            same = same + 1 if cur == prev else 0
            if same >= quiet:
                return rows
            prev = cur
            self.step(sample)
            spent += sample
        return self.screen_rows()


# --------------------------------------------------------------------------
# Screen helpers
# --------------------------------------------------------------------------
def has(rows, needle):
    return any(needle in r for r in rows)


def clean_rows(rows):
    """Rows with the box-drawing/graphic tiles stripped, blanks dropped."""
    out = []
    for r in rows:
        s = r.strip(" " + UNKNOWN).strip()
        if s:
            out.append(s)
    return out


def clean_lines(rows):
    return " / ".join(clean_rows(rows))


def find_row(rows, needle):
    for i, r in enumerate(rows):
        if needle in r:
            return i
    return -1


def party_count(rows):
    """How many Pokemon the game just drew on the party screen."""
    return sum(1 for r in rows if PARTY_HP_RE.search(r))


def evidence_text(rep):
    """Every screen this run recorded, as one searchable blob.

    --expect-screen has to see the same thing a human reviewing the run would:
    the boot transcript included, because a refusal or a "no save" screen can
    appear for ~100 frames and be gone before the main menu is drawn.
    """
    parts = []
    for entry in rep.get("boot_transcript", []):
        parts.extend(entry.get("screen", []))
    for key in ("main_menu", "continue_screen", "overworld_screen",
                "party_screen"):
        parts.extend(rep.get(key) or [])
    for key in ("refusal_text", "player_name", "badges", "pokedex",
                "play_time"):
        if rep.get(key):
            parts.append(str(rep[key]))
    return "\n".join(parts)


def label_value(rows, label):
    """Text that follows `label` on the same row.

    Gen 1/2 print the label and the value as separate PlaceString calls into
    the same tilemap row (e.g. main_menu.asm DisplayContinueGameInfo puts
    "PLAYER" at column 5 and wPlayerName at column 12), so the value is simply
    the rest of the row.
    """
    i = find_row(rows, label)
    if i < 0:
        return None
    r = rows[i]
    start = r.index(label) + len(label)
    return r[start:].strip(" " + UNKNOWN).strip() or None


# --------------------------------------------------------------------------
# The flow
# --------------------------------------------------------------------------
class Report(dict):
    def add_shot(self, path):
        self.setdefault("screenshots", []).append(path)

    def note(self, msg):
        self.setdefault("notes", []).append(msg)


def boot_to_main_menu(gb, rep, out, max_frames=4000, log=print):
    """Mash START past the intro/title until the main menu is on screen.

    The screen is sampled every 4 frames throughout, because Gen 1 prints
    "The file data is destroyed!" for only ~100 frames and then continues on
    its own (pokered/engine/menus/save.asm:17-23 — PrintText then
    `ld c, 100 / DelayFrames`, no button wait). Miss that window and a refused
    save looks like a save that was simply never there.
    """
    seen = []
    last = None
    since_tap = 0
    since_change = 0
    refusal_shot = None
    while gb.frames_run < max_frames:
        rows = gb.screen_rows()
        text = "\n".join(rows).strip()
        refusing = False
        if text and text != last:
            seen.append((gb.frames_run, rows))
            last = text
            since_change = 0
            low = text.lower()
            for phrase in REFUSAL_PHRASES:
                if phrase in low:
                    refusing = True
                    if refusal_shot is None:
                        refusal_shot = gb.screenshot(
                            Path(out) / "03-refused.png")
                        rep["refusal_text"] = clean_lines(rows)
                        rep.add_shot(refusal_shot)
                        log(f"  !! the game printed a refusal at frame "
                            f"{gb.frames_run}: {rep['refusal_text']}")
        elif any(p in text.lower() for p in REFUSAL_PHRASES):
            refusing = True
        if has(rows, "NEW GAME"):
            return rows, seen
        if refusing:
            # "The file data is destroyed!" ends with `prompt`
            # (pokered/data/text/text_3.asm:1-4), which blocks on A/B — START
            # does not dismiss it, so without this the run hangs on the very
            # screen it was built to detect.
            gb.tap("A")
            since_tap = 0
        elif since_tap >= 90:
            gb.tap("START")
            since_tap = 0
        elif since_change >= 600:
            # Nothing has moved for 10 s of emulated time and START is not
            # helping: some other prompt is waiting on A.
            gb.tap("A")
            since_change = 0
        gb.step(4)
        since_tap += 4
        since_change += 4
    raise RoundtripError(
        f"never reached the main menu in {max_frames} frames (last screen:\n"
        + (last or "<blank>") + "\n)")


def read_party(gb, rep, out, log=print):
    """Open the start menu, pick the POKeMON entry, and read the party list.

    Navigating menus rather than reading WRAM keeps this version-agnostic:
    Gold/Silver and Crystal do not share WRAM addresses, but they all draw the
    party list the same way.
    """
    rows, ok = gb.mash("START", lambda r: has(r, "SAVE") and has(r, "MON"),
                       tries=8, gap=40, what="the start menu")
    if not ok:
        rep.note("could not open the start menu; party not read")
        rep.add_shot(gb.screenshot(Path(out) / "06-no-start-menu.png"))
        return None
    rows = gb.settle()
    rep.add_shot(gb.screenshot(Path(out) / "06-start-menu.png"))

    # The start menu is a vertical list; its entries are the non-blank rows of
    # the menu box. Find POKeMON (not POKeDEX / POKeGEAR) and count how many
    # entries sit above it — that is how many DOWN presses the cursor needs.
    order = [s for s in clean_rows(rows) if any(c.isalpha() for c in s)]
    target = None
    for idx, s in enumerate(order):
        if "MON" in s and "DEX" not in s and "GEAR" not in s:
            target = idx
            break
    if target is None:
        rep.note(f"no POKeMON entry in the start menu: {order}")
        return None
    for _ in range(target):
        gb.tap("DOWN", hold=6, release=6)
    gb.tap("A")
    try:
        rows, _ = gb.wait_for(
            # Both generations draw current/max HP as "nnn/nnn" under each
            # party member; the level glyph is a graphic tile, not text, so
            # "LV" is not reliably present.
            lambda r: has(r, "/"),
            timeout_frames=400, what="the party list")
    except RoundtripError:
        rep.note("party screen showed no HP numbers; reporting the raw screen")
    rows = gb.settle()
    rep.add_shot(gb.screenshot(Path(out) / "07-party.png"))
    return clean_rows(rows)


def roundtrip(rom, sav, out, want_party=True, dump_save=None,
              max_frames=4000, log=print, vendor=None, rtc=None,
              dump_keep_rtc=False, read_mem=None):
    """Drive one save through one game and return (report, driver).

    The SRAM diff and the optional dump run on every path, refusals included —
    a run that ends in "corrupted!" is exactly when you want to see what the
    game touched.

    `read_mem`: optional list of (addr, length) pairs read from the emulated GB
    bus once the overworld is reached (never at reset — TryLoadSaveFile has to
    have run) and stored in rep["mem"][hex(addr)] = hex bytes. This is the
    docs/GEN12-PARITY-DESIGN.md §4.2 "RAM assertions" primitive: screen-scraping
    cannot see money or a fly bit, so a gate case that needs to assert on WRAM
    directly (not on what the game drew) passes addresses here instead. Empty
    (nothing read, verdict unaffected) if the run never reaches the overworld.
    """
    rep, gb = _drive(rom, sav, out, want_party=want_party,
                     max_frames=max_frames, log=log, vendor=vendor, rtc=rtc,
                     read_mem=read_mem)
    _report_sram(gb, rep, dump_save, log, dump_keep_rtc=dump_keep_rtc)
    rep["screen_evidence"] = evidence_text(rep)
    return rep, gb


def _drive(rom, sav, out, want_party=True, max_frames=4000, log=print,
           vendor=None, rtc=None, read_mem=None):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rep = Report()
    rep["rom"] = str(rom)
    rep["sav"] = str(sav) if sav else None

    gb = GbDriver(rom, sav, vendor=vendor, log=log, rtc=rtc)
    rep["game_title"] = gb.game_title
    rep["generation"] = gb.generation
    rep["rtc_pinned"] = gb.rtc_pinned.isoformat() if gb.rtc_pinned else None
    rep["save_bytes"] = len(gb.save_in) if gb.save_in is not None else 0

    log("phase 1: booting to the main menu")
    rows, seen = boot_to_main_menu(gb, rep, out, max_frames=max_frames, log=log)
    rep["frames_to_main_menu"] = gb.frames_run
    rows = gb.settle()
    rep.add_shot(gb.screenshot(out / "01-main-menu.png"))
    # Keep the distinct screens the boot walked through: when a run goes wrong
    # this is the only record of what the game actually put up, and it is what
    # tells "no save" apart from "save refused".
    rep["boot_transcript"] = [{"frame": f, "screen": clean_rows(r)}
                              for f, r in seen if clean_rows(r)][-12:]
    rep["main_menu"] = clean_rows(rows)
    log("  main menu: " + " | ".join(rep["main_menu"]))

    offers_continue = has(rows, "CONTINUE")
    rep["offers_continue"] = offers_continue
    if rep.get("refusal_text"):
        # Gen 1 runs TryLoadSaveFile before it even draws the menu
        # (pokered/engine/menus/main_menu.asm:10), so a bad checksum is
        # announced during boot. Whatever the menu says afterwards, the game
        # already told us it would not use this save.
        rep["verdict"] = "reject"
        rep["reason"] = "the game printed a refusal while loading the save"
        return rep, gb
    if not offers_continue:
        rep["verdict"] = "reject"
        rep["reason"] = ("the main menu offers no CONTINUE — the game found no "
                         "usable save")
        return rep, gb

    log("phase 2: choosing CONTINUE")
    gb.tap("A")                      # the cursor starts on CONTINUE (item 0)
    try:
        rows, _ = gb.wait_for(
            lambda r: has(r, "PLAYER") or has(r, "BADGES")
            or any(p in "\n".join(r).lower() for p in REFUSAL_PHRASES),
            timeout_frames=900, what="the continue screen")
    except RoundtripError as exc:
        rep["verdict"] = "unknown"
        rep["reason"] = str(exc)
        rep.add_shot(gb.screenshot(out / "02-stuck.png"))
        return rep, gb

    rows = gb.settle()
    low = "\n".join(rows).lower()
    if any(p in low for p in REFUSAL_PHRASES):
        rep["verdict"] = "reject"
        rep["reason"] = "the game printed a refusal after CONTINUE"
        rep["refusal_text"] = clean_lines(rows)
        rep.add_shot(gb.screenshot(out / "03-refused.png"))
        return rep, gb

    rep.add_shot(gb.screenshot(out / "02-continue.png"))
    rep["continue_screen"] = clean_rows(rows)
    rep["player_name"] = label_value(rows, "PLAYER")
    rep["badges"] = label_value(rows, "BADGES")
    rep["pokedex"] = label_value(rows, "DEX")
    rep["play_time"] = label_value(rows, "TIME")
    log(f"  PLAYER={rep['player_name']!r} BADGES={rep['badges']!r} "
        f"DEX={rep['pokedex']!r} TIME={rep['play_time']!r}")

    log("phase 3: entering the overworld")
    rows, left = gb.mash("A", lambda r: not has(r, "BADGES"),
                         tries=10, gap=40, what="the game to start")
    if not left:
        rep["verdict"] = "unknown"
        rep["reason"] = "the continue screen never went away"
        rep.add_shot(gb.screenshot(out / "04-stuck-continue.png"))
        return rep, gb
    gb.step(180)
    rep["frames_to_overworld"] = gb.frames_run
    rep["overworld_screen"] = clean_rows(gb.screen_rows())
    rep.add_shot(gb.screenshot(out / "05-overworld.png"))

    if read_mem:
        # §4.2 rule 1 (WRAM banking): Gen-2 main data lives in WRAM bank 1, and the
        # games switch SVBK (0xFF70) to banks 2-7 for scratch, so a 0xD000-0xDFFF read
        # is only meaningful when SVBK & 7 is 0 or 1 — anything else means this settled
        # frame happened to land on scratch, and the caller must not report a bank
        # number as if it were the field it asked for. Gen 1 is DMG-flat, no SVBK.
        mem = {}
        svbk = gb.read_mem(0xFF70, 1)[0] & 7 if gb.generation == 2 else None
        mem["svbk"] = svbk
        mem["svbk_ok"] = (svbk is None) or svbk in (0, 1)
        for addr, length in read_mem:
            mem[f"{addr:#06x}"] = gb.read_mem(addr, length).hex()
        rep["mem"] = mem

    if want_party:
        log("phase 4: reading the party")
        party = read_party(gb, rep, out, log=log)
        if party is not None:
            rep["party_screen"] = party
            rep["party_count"] = party_count(party)
            log(f"  party screen ({rep['party_count']} Pokemon): "
                + " | ".join(party))

    rep["verdict"] = "accept"
    rep["reason"] = "the game loaded the save and reached the overworld"
    return rep, gb


def split_rtc_tail(length):
    """(sram_bytes, rtc_footer_bytes) for a .sav of this length.

    An MBC3+RTC .sav is SRAM followed by a clock footer (mGBA and VBA-M both
    write one; Guy's Gold.sav is 32768 + 48). The footer is not part of the
    cartridge's save data and must never be diffed as if it were.
    """
    sram = 0
    for size in GB_SRAM_SIZES:
        if size <= length:
            sram = size
    if sram == 0:                    # nothing plausible — treat it all as SRAM
        return length, 0
    return sram, length - sram


def _diff_runs(a, b, lo, hi):
    """[start, end) runs where a and b differ, over [lo, hi)."""
    runs, start = [], None
    for i in range(lo, hi):
        if a[i] != b[i]:
            if start is None:
                start = i
        elif start is not None:
            runs.append((start, i)); start = None
    if start is not None:
        runs.append((start, hi))
    return runs


def _split_at(runs, bounds):
    """Cut runs at region boundaries so no run belongs to two buckets.

    A single changed run that starts in scratch and ends in save data would
    otherwise be counted entirely as whichever bucket matched first — i.e. it
    could hide save-data churn inside a scratch total, which is precisely the
    mistake this whole classification exists to stop making.
    """
    cuts = sorted(bounds)
    out = []
    for lo, hi in runs:
        edges = [lo] + [c for c in cuts if lo < c < hi] + [hi]
        out.extend(zip(edges, edges[1:]))
    return out


def _classify(runs, gen, title):
    """Split changed runs into scratch / primary / backup / other.

    Classification is by range, not by guesswork, so the caller can tell the
    one signal that matters (a Gen-2 PRIMARY that moved, meaning the game
    rejected it and booted the backup) from the three that never do.
    """
    scratch = list(SCRATCH_RANGES.get(gen, ()))
    if gen == 2 and title in ("POKEMON_GLD", "POKEMON_SLV"):
        scratch.append(GS_SCRATCH_EXTRA)
    lay = None
    if gen == 2:
        lay = GEN2_LAYOUT["gs" if title in ("POKEMON_GLD", "POKEMON_SLV")
                          else "crystal"]

    regions = [(lo, hi) for _, lo, hi in scratch]
    if lay:
        regions += list(lay["primary"]) + list(lay["backup"])
    bounds = set()
    for lo, hi in regions:
        bounds.add(lo)
        bounds.add(hi + 1)           # ranges are inclusive of hi
    runs = _split_at(runs, bounds)

    def overlap(run, lo, hi):        # run is [start, end), region [lo, hi]
        return run[0] <= hi and run[1] - 1 >= lo

    out = {"scratch": [], "primary": [], "backup": [], "other": []}
    for run in runs:
        if any(overlap(run, lo, hi) for _, lo, hi in scratch):
            out["scratch"].append(run)
        elif lay and any(overlap(run, lo, hi) for lo, hi in lay["primary"]):
            out["primary"].append(run)
        elif lay and any(overlap(run, lo, hi) for lo, hi in lay["backup"]):
            out["backup"].append(run)
        else:
            out["other"].append(run)
    return out


def _report_sram(gb, rep, dump_save, log, dump_keep_rtc=False):
    """Record what the game did to cartridge SRAM while loading.

    This is evidence, not a verdict. Three things have to be held apart or the
    number is meaningless:

      - THE RTC FOOTER is not save data. mGBA rewrites it from the host clock
        at load (12 of Gold.sav's 48 footer bytes move on an untouched run), so
        it is diffed and reported separately and never folded into the save
        figure.
      - SCRATCH is not save data. Gen 1 decompresses sprites into the start of
        bank 0 (pokered/home/pics.asm:95-97 fills sSpriteBuffer0..2); Gen 2 has
        sDecompressScratch there and G/S additionally runs its window STACK at
        $B800-$BFFF. Churn there is noise whose volume depends only on where
        the run was stopped.
      - Gen 2 rewrites the BACKUP copy from live WRAM on every successful load
        (pokegold/engine/menus/save.asm:538-552, pokecrystal:596-611), leaving
        it byte-equal to the primary. So a changed backup is expected: it
        measures how stale the INPUT's backup was, not anything the game chose
        to do. A changed PRIMARY is the real signal — on the good-primary path
        the game never writes it, so if it moved, the game rejected the primary
        and booted the backup, which looks identical on screen. A Gen-2 writer
        that updates only one copy passes the eyeball test and fails here.
    """
    after = gb.save_bytes()
    if after is None or gb.save_in is None:
        if dump_save and after is not None:
            Path(dump_save).write_bytes(after)
            rep["dumped_save"] = str(dump_save)
        return

    n_in, n_out = len(gb.save_in), len(after)
    sram_len, tail_len = split_rtc_tail(n_in)
    sram_out, tail_out = split_rtc_tail(n_out)
    rep["sram_bytes"] = sram_len
    rep["rtc_footer_bytes"] = tail_len
    rep["rtc_footer_bytes_out"] = tail_out

    if sram_out != sram_len:
        # Never silently compare the overlap: if the SRAM itself came back a
        # different size, the emulated cartridge is not the cartridge we handed
        # it and every byte offset below is suspect. Hard failure, not a note.
        # (A *footer* that appears or grows is normal and handled below —
        # mGBA gives a 32768-byte Crystal save a 48-byte RTC footer it never
        # had, which is why this compares SRAM length and not file length.)
        rep["sram_length_mismatch"] = [n_in, n_out]
        rep["harness_error"] = (
            f"emulated SRAM came back {sram_out} bytes for a {sram_len}-byte "
            "save; the dump and the diff cannot be trusted")
        log(f"  !! HARNESS ERROR: {rep['harness_error']}")
    else:
        if tail_out != tail_len:
            rep.note(f"mGBA returned a {tail_out}-byte RTC footer for a save "
                     f"that had {tail_len}; the dump is {n_out} bytes, not "
                     f"{n_in}. Footer bytes are excluded from the SRAM diff.")
        runs = _diff_runs(after, gb.save_in, 0, sram_len)
        buckets = _classify(runs, gb.generation, gb.game_title)
        rep["sram_changed_run_count"] = len(runs)
        for k in ("primary", "backup", "other", "scratch"):
            rep[f"sram_changed_{k}_bytes"] = sum(b - a for a, b in buckets[k])
            rep[f"sram_changed_{k}_ranges"] = [
                [hex(a), hex(b)] for a, b in buckets[k][:24]]
        # "sram_changed_bytes" is SAVE DATA only. Scratch and the RTC footer
        # get their own counters and are deliberately not summed in here.
        rep["sram_changed_bytes"] = sum(rep[f"sram_changed_{k}_bytes"]
                                        for k in ("primary", "backup", "other"))
        if runs:
            log(f"  cartridge SRAM: {rep['sram_changed_bytes']} save byte(s) "
                f"changed in "
                f"{sum(len(buckets[k]) for k in ('primary','backup','other'))} "
                f"run(s) (+{rep['sram_changed_scratch_bytes']} in scratch, "
                "which is meaningless)")
            if gb.generation == 2:
                log(f"    primary {rep['sram_changed_primary_bytes']} B, "
                    f"backup {rep['sram_changed_backup_bytes']} B — Gen 2 "
                    "rewrites the backup from WRAM on EVERY load; a changed "
                    "PRIMARY is the one that means the backup was booted")

        if tail_len and tail_out == tail_len:
            tail = _diff_runs(after, gb.save_in, sram_len, n_in)
            rep["rtc_footer_changed_bytes"] = sum(b - a for a, b in tail)
            if tail:
                log(f"    RTC footer: {rep['rtc_footer_changed_bytes']} of "
                    f"{tail_len} byte(s) rewritten by mGBA (host clock, not "
                    "the game) — excluded from the figures above")
        elif tail_out:
            rep["rtc_footer_changed_bytes"] = tail_out
            log(f"    RTC footer: mGBA wrote {tail_out} new byte(s) past the "
                "end of SRAM (the input had none) — excluded from the "
                "figures above")

    if dump_save:
        data = bytearray(after)
        if tail_out and sram_out == sram_len:
            if dump_keep_rtc:
                # A dump is only restorable if its clock is the input's clock.
                data[sram_len:] = gb.save_in[sram_len:]
                rep["dump_rtc_footer"] = ("restored from the input"
                                          if tail_len else "dropped (the "
                                          "input had none)")
            else:
                rep["dump_rtc_footer"] = "mGBA's, NOT the input's"
                log(f"    WARNING: the dump's last {tail_out} bytes are the "
                    "RTC footer mGBA just wrote from the host clock, not the "
                    "input's. Restoring this dump to a cartridge moves the "
                    "in-game clock. Use --dump-keep-rtc if you want a save.")
        Path(dump_save).write_bytes(bytes(data))
        rep["dumped_save"] = str(dump_save)


# --------------------------------------------------------------------------
# Assertions
# --------------------------------------------------------------------------
def evaluate(rep, a):
    """Every --expect* assertion against one report. Returns a list of failures.

    All of them are evaluated, not short-circuited: when a save loses data you
    want the whole picture in one run, not one failure per re-run. An assertion
    whose evidence is MISSING fails — "the party was never read" must never be
    mistaken for "the party was fine".
    """
    fails = []

    if a.expect != "any" and rep.get("verdict") != a.expect:
        fails.append(f"expected verdict {a.expect!r}, got "
                     f"{rep.get('verdict')!r} — {rep.get('reason')}")

    if a.expect_name is not None and rep.get("player_name") != a.expect_name:
        fails.append(f"expected player name {a.expect_name!r}, game showed "
                     f"{rep.get('player_name')!r}")

    evidence = rep.get("screen_evidence", "")
    for want in a.expect_screen or ():
        if want not in evidence:
            fails.append(f"{want!r} appears on no screen this run recorded")
    for unwanted in a.expect_no_screen or ():
        if unwanted in evidence:
            fails.append(f"{unwanted!r} was on screen and should not have been")

    wants_party = (a.expect_party or a.expect_no_party
                   or a.expect_party_count is not None)
    party = rep.get("party_screen")
    if wants_party and party is None:
        fails.append("the party screen was never read, so the --expect-party "
                     "assertions could not be checked (see notes)")
    elif wants_party:
        blob = "\n".join(party)
        for want in a.expect_party or ():
            if want not in blob:
                fails.append(f"{want!r} is not in the party the game drew: "
                             + " | ".join(party))
        for unwanted in a.expect_no_party or ():
            if unwanted in blob:
                fails.append(f"{unwanted!r} is still in the party the game "
                             "drew: " + " | ".join(party))
        if a.expect_party_count is not None:
            got = rep.get("party_count")
            if got != a.expect_party_count:
                fails.append(f"expected {a.expect_party_count} Pokemon in the "
                             f"party, the game listed {got}: "
                             + " | ".join(party))
    return fails


# --------------------------------------------------------------------------
# Self-test: the data-loss cases this gate exists to catch
#
# These are regression tests for the GATE, not for a save writer, so each one
# builds a save that a real edit could plausibly produce, boots it, and asserts
# on the tool's own exit code. Every "must FAIL" case here exits 0 without the
# content assertions — that is the bug they were written for.
#
# Offsets are stated with their authority and re-asserted at runtime against
# the corpus, so a different save fails loudly instead of silently editing the
# wrong bytes.
# --------------------------------------------------------------------------
GEN1_PARTY_COUNT_OFF = 0x2F2C        # sPartyData: count, 6 species, $FF
GEN1_SUM_SPAN = (0x2598, 0x3523)     # sGameData; pokered/ram/sram.asm ds $598
GEN1_SUM_OFF = 0x3523                # sMainDataCheckSum
CRYSTAL_SUM1_SPAN = (0x2009, 0x2B83)
CRYSTAL_SUM1_OFF = 0x2D0D
GS_SUM1_SPAN = (0x2009, 0x2D69)
GS_SUM1_OFF = 0x2D69
GS_SUM2_OFF = 0x7E6D
GS_MIRROR_MEASURED = ((0x2009, 0x222E, 0x15C7), (0x222F, 0x23D8, 0x3D96),
                      (0x23D9, 0x2855, 0x0C6B), (0x2856, 0x2889, 0x7E39),
                      (0x288A, 0x2D68, 0x10E8))
GS_MIRROR_K_GS = ((0x2009, 0x222E, 0x15C7), (0x222F, 0x23D8, 0x3D69),
                  (0x23D9, 0x2855, 0x0C6B), (0x2856, 0x2889, 0x7E39),
                  (0x288A, 0x2D68, 0x10E8))


def gen12_encode(text, width=11):
    """Name -> Gen 1/2 charmap bytes, "@"-terminated and $50-padded.

    pokered/pokecrystal constants/charmap.asm: "A" is $80, "a" is $a0, "@" (the
    string terminator) is $50.
    """
    out = bytearray()
    for c in text:
        if "A" <= c <= "Z":
            out.append(0x80 + ord(c) - 65)
        elif "a" <= c <= "z":
            out.append(0xA0 + ord(c) - 97)
        else:
            raise ValueError(f"cannot encode {c!r}")
    out.append(0x50)
    while len(out) < width:
        out.append(0x50)
    return bytes(out[:width])


def _gen1_fix_checksum(buf):
    """pokered/engine/menus/save.asm:297-310 — the COMPLEMENT of the 8-bit sum."""
    lo, hi = GEN1_SUM_SPAN
    buf[GEN1_SUM_OFF] = (~sum(buf[lo:hi])) & 0xFF


def _gen2_fix_sum(buf, span, off):
    lo, hi = span
    s = sum(buf[lo:hi]) & 0xFFFF
    buf[off] = s & 0xFF
    buf[off + 1] = s >> 8


def _build_selftest_saves(corpus, work, log):
    """Write the deliberately-damaged saves. The corpus is never modified."""
    made = {}

    # --- Crystal: a party nickname rewritten with a STALE primary checksum.
    # The game rejects the primary, boots the byte-perfect backup, and draws
    # the OLD nickname. The edit is gone and nothing on the verdict or the
    # trainer name says so.
    cr = bytearray((Path(corpus) / "Crystal.sav").read_bytes())
    old = gen12_encode("TYPHLOSION")
    lo, hi = CRYSTAL_SUM1_SPAN
    at = cr.find(old, lo, hi)          # the PRIMARY copy, inside checksum 1
    if at < 0:
        raise RoundtripError(
            "Crystal.sav does not contain a party Pokemon nicknamed "
            "TYPHLOSION inside the primary save block; the self-test is "
            "written against Guy's corpus save.")
    cr[at:at + 11] = gen12_encode("HACKED")
    (work / "crystal-nick-stale.sav").write_bytes(bytes(cr))
    fixed = bytearray(cr)
    _gen2_fix_sum(fixed, CRYSTAL_SUM1_SPAN, CRYSTAL_SUM1_OFF)
    (work / "crystal-nick-fixed.sav").write_bytes(bytes(fixed))
    made["nick_at"] = at
    log(f"  built crystal-nick-{{stale,fixed}}.sav (nickname at {at:#07x})")

    # --- Red: party count says 6, species terminator sits at index 2.
    # Four Pokemon vanish. The checksum is kept VALID so the game accepts the
    # save without a murmur — the loss is in the data, not in the envelope.
    red = bytearray((Path(corpus) / "Red.sav").read_bytes())
    if red[GEN1_PARTY_COUNT_OFF] != 6 or red[GEN1_PARTY_COUNT_OFF + 7] != 0xFF:
        raise RoundtripError(
            f"Red.sav does not have a full party at {GEN1_PARTY_COUNT_OFF:#07x} "
            "(count 6 then six species then $FF); the self-test is written "
            "against Guy's corpus save.")
    red[GEN1_PARTY_COUNT_OFF + 3] = 0xFF        # terminator at species index 2
    _gen1_fix_checksum(red)
    (work / "red-party-truncated.sav").write_bytes(bytes(red))
    log("  built red-party-truncated.sav (count 6, terminator at index 2)")

    # --- Gold: primary checksum broken, backup rebuilt with each candidate
    # mirror map. Only the map the game actually uses can rescue the save, so
    # this is the docstring's 0x3D96 claim turned into something that breaks
    # if it is ever wrong again.
    gold = bytearray((Path(corpus) / "Gold.sav").read_bytes())
    for tag, mirror in (("3d96", GS_MIRROR_MEASURED), ("3d69", GS_MIRROR_K_GS)):
        b = bytearray(gold)
        for f, t, d in mirror:
            b[d:d + (t - f + 1)] = b[f:t + 1]
        b[0x7E30:0x7E38] = b[0x2000:0x2008]      # sBackupOptions <- sOptions
        b[0x7E38] = b[0x2008]                    # sBackupCheckValue1
        b[0x7E6F] = b[0x2D6B]                    # sBackupCheckValue2
        d2 = [d for f, t, d in mirror if t - f + 1 == 426][0]
        s = (sum(b[0x0C6B:0x17ED]) + sum(b[d2:d2 + 426])
             + sum(b[0x7E39:0x7E6D])) & 0xFFFF
        b[GS_SUM2_OFF] = s & 0xFF
        b[GS_SUM2_OFF + 1] = s >> 8
        b[GS_SUM1_OFF] ^= 0xFF                   # and break the PRIMARY
        (work / f"gold-backup-{tag}.sav").write_bytes(bytes(b))
    log("  built gold-backup-{3d96,3d69}.sav (primary broken on purpose)")
    return made


def selftest(corpus, out, vendor, rtc_arg, log=print):
    corpus = Path(corpus)
    missing = [n for n in ("Red.gb", "Red.sav", "Crystal.gbc", "Crystal.sav",
                           "Gold.gbc", "Gold.sav")
               if not (corpus / n).is_file()]
    if missing:
        print(f"ERROR: --corpus {corpus} is missing {', '.join(missing)}",
              file=sys.stderr)
        return 2
    out = Path(out)
    work = out / "selftest-saves"
    work.mkdir(parents=True, exist_ok=True)
    log(f"building the damaged saves in {work}")
    try:
        _build_selftest_saves(corpus, work, log)
    except RoundtripError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    red, crystal, gold = (str(corpus / "Red.gb"), str(corpus / "Crystal.gbc"),
                          str(corpus / "Gold.gbc"))
    common = ["--out", str(out / "shots"), "--rtc", rtc_arg, "-q"]
    if vendor:
        common += ["--mgba-vendor", vendor]

    # (name, expected exit, argv, why this case exists)
    cases = [
        ("crystal stale edit, verdict+name gate only", 0,
         ["--rom", crystal, "--sav", str(work / "crystal-nick-stale.sav"),
          "--expect", "accept", "--expect-name", "MattiaP"],
         "THE BUG: the edit was discarded and this still passes"),
        ("crystal stale edit, --expect-party", 1,
         ["--rom", crystal, "--sav", str(work / "crystal-nick-stale.sav"),
          "--expect", "accept", "--expect-name", "MattiaP",
          "--expect-party", "HACKED"],
         "the fix: the game drew TYPHLOSION, not HACKED"),
        ("crystal stale edit, --expect-screen", 1,
         ["--rom", crystal, "--sav", str(work / "crystal-nick-stale.sav"),
          "--expect", "accept", "--expect-screen", "HACKED"],
         "the same blindness, caught across every screen and not just the "
         "party"),
        ("crystal stale edit, --expect-no-screen", 1,
         ["--rom", crystal, "--sav", str(work / "crystal-nick-stale.sav"),
          "--expect", "accept", "--expect-no-screen", "TYPHLOSION"],
         "the OLD nickname is still on screen, which is the proof of loss"),
        ("crystal stale edit, --expect-no-party", 1,
         ["--rom", crystal, "--sav", str(work / "crystal-nick-stale.sav"),
          "--expect", "accept", "--expect-no-party", "TYPHLOSION"],
         "the same, asserted on the party list the game drew"),
        ("crystal edit with the checksum repaired", 0,
         ["--rom", crystal, "--sav", str(work / "crystal-nick-fixed.sav"),
          "--expect", "accept", "--expect-name", "MattiaP",
          "--expect-party", "HACKED", "--expect-no-party", "TYPHLOSION",
          "--expect-screen", "HACKED", "--expect-no-screen", "TYPHLOSION",
          "--dump-save", str(work / "crystal-dump-keeprtc.sav"),
          "--dump-keep-rtc"],
         "and none of those assertions is merely always-failing"),
        ("red truncated party, verdict+name gate only", 0,
         ["--rom", red, "--sav", str(work / "red-party-truncated.sav"),
          "--expect", "accept", "--expect-name", "ASH"],
         "THE BUG: four Pokemon are gone and this still passes"),
        ("red truncated party, --expect-party-count", 1,
         ["--rom", red, "--sav", str(work / "red-party-truncated.sav"),
          "--expect", "accept", "--expect-name", "ASH",
          "--expect-party-count", "6"],
         "the fix: the game listed 2"),
        ("red untouched, a wrong --expect-name", 1,
         ["--rom", red, "--sav", str(corpus / "Red.sav"),
          "--expect-name", "NOTASH"],
         "the oldest assertion still has to be able to fail"),
        ("red untouched, --expect-party-count 6", 0,
         ["--rom", red, "--sav", str(corpus / "Red.sav"),
          "--expect-party-count", "6", "--expect-party", "MEWTWO"],
         "and the count assertion holds on a good save"),
        ("gold backup rebuilt at 0x3D96, primary broken", 0,
         ["--rom", gold, "--sav", str(work / "gold-backup-3d96.sav"),
          "--expect", "accept", "--expect-name", "MattiaP"],
         "the measured G/S mirror map rescues the save"),
        ("gold backup rebuilt at 0x3D69, primary broken", 1,
         ["--rom", gold, "--sav", str(work / "gold-backup-3d69.sav"),
          "--expect", "accept"],
         "k_gs_mirror's 0x3D69 does not — the ROM's verdict on the map"),
        ("gold backup at 0x3D69, --expect reject --expect-screen", 0,
         ["--rom", gold, "--sav", str(work / "gold-backup-3d69.sav"),
          "--expect", "reject", "--expect-screen", "corrupted"],
         "a screen assertion IS satisfiable with reject and must stay legal"),
        ("--expect reject with --expect-name is refused", 2,
         ["--rom", red, "--sav", str(corpus / "Red.sav"),
          "--expect", "reject", "--expect-name", "ASH"],
         "unsatisfiable combinations must not masquerade as failures"),
        ("--no-party with --expect-party is refused", 2,
         ["--rom", red, "--sav", str(corpus / "Red.sav"),
          "--no-party", "--expect-party", "MEW"],
         "an assertion whose evidence was skipped must not pass"),
    ]

    failures = []
    for name, want, argv, why in cases:
        try:
            got = main(argv + common)
        except SystemExit as exc:            # argparse errors exit(2)
            got = exc.code
        mark = "ok  " if got == want else "FAIL"
        print(f"  [{mark}] exit {got} (want {want})  {name}\n"
              f"          {why}")
        if got != want:
            failures.append(name)

    # The RTC tail must be split out of the save-data diff, not counted as it.
    # This is the "285-604 bytes" claim turned into a number that cannot drift:
    # save data 256, exactly, every run, all of it backup-side. Anything that
    # leaks footer churn or window-stack churn into the save figure shows up
    # here as a non-zero "other".
    rep, _ = roundtrip(gold, str(corpus / "Gold.sav"), out / "shots",
                       want_party=False, log=lambda *x: None, vendor=vendor,
                       rtc=_parse_rtc(rtc_arg),
                       dump_save=str(work / "gold-dump-plain.sav"))
    roundtrip(gold, str(corpus / "Gold.sav"), out / "shots", want_party=False,
              log=lambda *x: None, vendor=vendor, rtc=_parse_rtc(rtc_arg),
              dump_save=str(work / "gold-dump-keeprtc.sav"),
              dump_keep_rtc=True)
    # Crystal.sav's backup already equals its primary, so the SAME unconditional
    # rewrite changes nothing. That is what tells "the game rewrites the backup
    # every time" apart from "the game healed a stale backup".
    crep, _ = roundtrip(crystal, str(corpus / "Crystal.sav"), out / "shots",
                        want_party=False, log=lambda *x: None, vendor=vendor,
                        rtc=_parse_rtc(rtc_arg))
    # Gen 1 has no backup copy at all, so an untouched load must leave the save
    # data completely alone — while filling the sprite-decompression buffers at
    # the start of bank 0 with hundreds of bytes of noise.
    rrep, _ = roundtrip(red, str(corpus / "Red.sav"), out / "shots",
                        want_party=False, log=lambda *x: None, vendor=vendor,
                        rtc=_parse_rtc(rtc_arg))
    gold_in = (corpus / "Gold.sav").read_bytes()
    dump_plain = (work / "gold-dump-plain.sav").read_bytes()
    dump_keep = (work / "gold-dump-keeprtc.sav").read_bytes()
    crystal_keep = (work / "crystal-dump-keeprtc.sav").read_bytes()
    fake = argparse.Namespace(
        expect="accept", expect_name=None, expect_screen=None,
        expect_no_screen=None, expect_party=["MEW"], expect_no_party=None,
        expect_party_count=None)
    no_party_rep = Report({"verdict": "accept", "screen_evidence": ""})

    # A screen that only ever existed mid-boot must still be searchable: Gen 1
    # prints its refusal for ~100 frames and moves on by itself, so evidence
    # that skips the boot transcript cannot see the very screen this tool was
    # built to catch.
    transient = Report({"boot_transcript": [{"frame": 12,
                                             "screen": ["FLASHED BY"]}]})

    # And a length change in the SRAM itself has to be a loud harness error,
    # not a diff over the overlap. Driven with a stub because no real ROM will
    # produce it on demand — which is exactly why it would otherwise go
    # untested until the day it happens.
    stub = argparse.Namespace(save_in=bytes(0x8000), generation=1,
                              game_title="POKEMON RED",
                              save_bytes=lambda: bytes(0x400))
    stub_rep = Report()
    _report_sram(stub, stub_rep, None, lambda *x: None)

    measured_backup2 = [d for f, t, d in GS_MIRROR_MEASURED
                        if t - f + 1 == 426][0]

    # The Crystal half of GEN2_LAYOUT has no measured-mirror table to check
    # itself against, so check it against a real save instead: sBackupGameData
    # sits 9 bytes into "Backup Save" (sBackupOptions 8 + sBackupCheckValue1 1,
    # pokecrystal/ram/sram.asm:58-68) and must mirror sGameData byte for byte
    # on a file whose two stored checksums agree — which Crystal.sav's do.
    # Unlike the G/S case this cannot be fooled by a shifted window: Crystal's
    # sPlayerData opens with a name, not with 45 zeroes.
    crystal_in = (corpus / "Crystal.sav").read_bytes()
    cb0 = GEN2_LAYOUT["crystal"]["backup"][0][0] + 9
    cgame = 0x2B82 - 0x2009 + 1

    # A changed run that straddles a region boundary must be cut at it, or its
    # bytes are filed under whichever bucket matched first.
    straddle = _classify([(0x1FF0, 0x2010)], 2, "POKEMON_GLD")
    checks = [
        ("Gold.sav is seen as 32768 SRAM + 48 RTC footer",
         (rep.get("sram_bytes"), rep.get("rtc_footer_bytes")) == (0x8000, 48)),
        ("the RTC footer churn is reported separately and is non-zero",
         rep.get("rtc_footer_changed_bytes", 0) > 0),
        ("an untouched Gen-2 load writes 256 save bytes, no more",
         rep.get("sram_changed_bytes") == 256),
        ("nothing lands outside the two save copies (footer/scratch leak)",
         rep.get("sram_changed_other_bytes") == 0),
        ("an untouched Gen-2 load never writes the PRIMARY",
         rep.get("sram_changed_primary_bytes") == 0),
        ("it rewrites the backup, all 256 bytes of it on this save",
         rep.get("sram_changed_backup_bytes") == 256),
        ("the window-stack scratch is held apart from the save figure",
         rep.get("sram_changed_scratch_bytes", 0) > 0),
        ("the same load on Crystal, whose backup is current, writes 0 bytes",
         crep.get("sram_changed_bytes") == 0
         and crep.get("sram_changed_scratch_bytes", 0) > 0),
        ("a Gen-1 load writes 0 save bytes, only sprite-buffer scratch",
         rrep.get("sram_changed_bytes") == 0
         and rrep.get("sram_changed_scratch_bytes", 0) > 0),
        # Pure checks on the assertion engine, no emulator involved: a missing
        # party screen must FAIL an --expect-party, never quietly pass, or the
        # gate is blind again in exactly the way it was blind before.
        ("--expect-party with no party screen fails instead of passing",
         len(evaluate(no_party_rep, fake)) == 1),
        ("split_rtc_tail sees Gold's footer and Red's lack of one",
         (split_rtc_tail(32816), split_rtc_tail(32768), split_rtc_tail(100))
         == ((32768, 48), (32768, 0), (100, 0))),
        ("a screen seen only during boot is still searchable",
         "FLASHED BY" in evidence_text(transient)),
        ("SRAM coming back a different SIZE is a harness error",
         bool(stub_rep.get("harness_error"))
         and stub_rep.get("sram_length_mismatch") == [0x8000, 0x400]),
        ("a harness error exits 2, outranking any assertion result",
         (exit_code(stub_rep, []), exit_code(Report(), ["x"]),
          exit_code(Report(), [])) == (2, 1, 0)),
        ("the SRAM-diff layout agrees with the measured mirror map",
         any(lo == measured_backup2 and hi == measured_backup2 + 425
             for lo, hi in GEN2_LAYOUT["gs"]["backup"])),
        ("the Crystal backup region really mirrors the primary on a real save",
         crystal_in[cb0:cb0 + cgame] == crystal_in[0x2009:0x2009 + cgame]),
        ("a run straddling a boundary is split, not filed under one bucket",
         (straddle["scratch"], straddle["primary"])
         == ([(0x1FF0, 0x2000)], [(0x2000, 0x2010)])),
        # The dump footgun: a plain dump's clock is the emulator's, so it must
        # NOT be restored to a cartridge, and --dump-keep-rtc must give back a
        # dump whose clock is the input's and which is otherwise identical.
        ("a plain dump's RTC footer is mGBA's, not the input's",
         dump_plain[0x8000:] != gold_in[0x8000:]),
        ("--dump-keep-rtc restores the input's footer and nothing else",
         dump_keep[0x8000:] == gold_in[0x8000:]
         and dump_keep[:0x8000] == dump_plain[:0x8000]),
        ("--dump-keep-rtc drops a footer the input never had",
         len(crystal_keep) == 0x8000),
    ]
    for name, ok in checks:
        print(f"  [{'ok  ' if ok else 'FAIL'}] {name}")
        if not ok:
            failures.append(name)

    print()
    if failures:
        print(f"SELFTEST FAILED: {len(failures)} case(s): "
              + "; ".join(failures))
        return 1
    print(f"SELFTEST PASSED: {len(cases) + len(checks)} cases")
    return 0


def _parse_rtc(text):
    if not text or text == "host":
        return None
    from datetime import datetime
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return datetime.strptime(text, fmt)
        except ValueError:
            continue
    return None


def exit_code(rep, fails):
    """0 all assertions held, 1 one did not, 2 the run itself is untrusted.

    A harness error outranks the assertions: they are computed from the same
    suspect state, so reporting "the save is bad" off a run whose SRAM came
    back the wrong size would be a guess dressed as a measurement.
    """
    if rep.get("harness_error"):
        return 2
    return 1 if fails else 0


def _validate_args(ap, a):
    """Refuse --expect combinations that can never hold, instead of failing.

    Every reject path in _drive returns before player_name or party_screen is
    set, so "--expect reject --expect-name X" is unsatisfiable: the verdict
    matches and the name check then forces exit 1 regardless. Same for the
    party assertions, and for asking about a party with --no-party.
    (--expect-screen still works with reject: the refusal text IS a screen.)
    """
    party_flags = []
    if a.expect_party:
        party_flags.append("--expect-party")
    if a.expect_no_party:
        party_flags.append("--expect-no-party")
    if a.expect_party_count is not None:
        party_flags.append("--expect-party-count")

    if a.expect == "reject":
        bad = (["--expect-name"] if a.expect_name is not None else []) + party_flags
        if bad:
            ap.error(f"{', '.join(bad)} cannot be used with --expect reject: a "
                     "refused save never reaches the continue or party screen, "
                     "so the assertion can never hold. Use --expect any if you "
                     "want the run to be judged on its contents.")
    if a.no_party and party_flags:
        ap.error(f"{', '.join(party_flags)} cannot be used with --no-party — "
                 "--no-party is what skips reading the party.")


# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Boot a Game Boy Pokemon ROM on a save and report whether "
                    "the game accepts it.")
    ap.add_argument("--rom", help=".gb/.gbc image")
    ap.add_argument("--sav", help="battery save to attach (READ ONLY)")
    ap.add_argument("--selftest", action="store_true",
                    help="run this tool's own regression tests: build the "
                         "known data-loss saves, boot them, and check that the "
                         "gate fails on each. Needs --corpus.")
    ap.add_argument("--corpus", help="directory holding Red.gb/Red.sav, "
                                     "Crystal.gbc/Crystal.sav and "
                                     "Gold.gbc/Gold.sav for --selftest "
                                     "(READ ONLY — copies are edited, never "
                                     "these)")
    ap.add_argument("--out", default="gb_roundtrip_out",
                    help="screenshot directory")
    ap.add_argument("--expect", choices=("accept", "reject", "any"),
                    default="accept", help="verdict this run must produce")
    ap.add_argument("--expect-name", help="fail unless the game shows this "
                                          "player name")
    ap.add_argument("--expect-screen", action="append", metavar="TEXT",
                    help="fail unless TEXT appears on some screen this run "
                         "recorded (repeatable)")
    ap.add_argument("--expect-no-screen", action="append", metavar="TEXT",
                    help="fail if TEXT appears on any screen (repeatable)")
    ap.add_argument("--expect-party", action="append", metavar="TEXT",
                    help="fail unless TEXT is on the party screen the game "
                         "drew — this is what catches an edit the game "
                         "silently discarded (repeatable)")
    ap.add_argument("--expect-no-party", action="append", metavar="TEXT",
                    help="fail if TEXT is still on the party screen "
                         "(repeatable)")
    ap.add_argument("--expect-party-count", type=int, metavar="N",
                    help="fail unless the game lists exactly N Pokemon — this "
                         "is what catches Pokemon that vanished")
    ap.add_argument("--json", help="write the report as JSON to this path")
    ap.add_argument("--dump-save", help="write emulated SRAM after load here. "
                                        "EVIDENCE, not a restorable save: the "
                                        "RTC footer is mGBA's (see "
                                        "--dump-keep-rtc)")
    ap.add_argument("--dump-keep-rtc", action="store_true",
                    help="splice the input's own RTC footer back into "
                         "--dump-save so the dump can be restored to a cart "
                         "without moving the in-game clock")
    ap.add_argument("--no-party", action="store_true",
                    help="skip the start-menu party read (faster)")
    ap.add_argument("--max-frames", type=int, default=4000,
                    help="give up if the main menu is not reached by then")
    ap.add_argument("--mgba-vendor",
                    help="directory containing the `mgba` python package")
    ap.add_argument("--read-mem", action="append", metavar="ADDR:LEN",
                    help="read LEN bytes at ADDR (hex, e.g. 0xD573:3) from the emulated "
                         "GB bus once the overworld is reached; repeatable. Stored in "
                         "the JSON report under 'mem' (plus 'mem.svbk'/'svbk_ok' on "
                         "Gen 2 — see §4.2's WRAM-banking rule in this file's docstring).")
    ap.add_argument("--rtc", default="2004-09-06 10:00:00",
                    help="pin the cartridge clock (Gen 2 prints the day and "
                         "time on the main menu, so an unpinned clock makes "
                         "every run's screens differ). 'host' to use the real "
                         "wall clock.")
    ap.add_argument("-q", "--quiet", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        if not a.corpus:
            ap.error("--selftest needs --corpus DIR")
        return selftest(a.corpus, a.out, a.mgba_vendor, a.rtc,
                        log=(lambda *x: None) if a.quiet else print)
    if not a.rom:
        ap.error("--rom is required (or use --selftest --corpus DIR)")
    _validate_args(ap, a)
    log = (lambda *x: None) if a.quiet else print

    rtc = None
    if a.rtc and a.rtc != "host":
        rtc = _parse_rtc(a.rtc)
        if rtc is None:
            print(f"ERROR: --rtc {a.rtc!r} is not YYYY-MM-DD[ HH:MM[:SS]]",
                  file=sys.stderr)
            return 2

    read_mem = None
    if a.read_mem:
        read_mem = []
        for spec in a.read_mem:
            addr_s, _, len_s = spec.partition(":")
            try:
                read_mem.append((int(addr_s, 0), int(len_s, 0) if len_s else 1))
            except ValueError:
                print(f"ERROR: --read-mem {spec!r} is not ADDR:LEN", file=sys.stderr)
                return 2

    try:
        rep, gb = roundtrip(a.rom, a.sav, a.out, want_party=not a.no_party,
                            dump_save=a.dump_save, max_frames=a.max_frames,
                            log=log, vendor=a.mgba_vendor, rtc=rtc,
                            dump_keep_rtc=a.dump_keep_rtc, read_mem=read_mem)
    except RoundtripError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print()
    print(f"  game        : {rep.get('game_title')}")
    print(f"  save        : {rep.get('sav')} ({rep.get('save_bytes')} bytes)")
    print(f"  main menu   : {' | '.join(rep.get('main_menu', []))}")
    print(f"  player      : {rep.get('player_name')}")
    print(f"  badges      : {rep.get('badges')}   "
          f"dex: {rep.get('pokedex')}   time: {rep.get('play_time')}")
    if rep.get("party_screen"):
        print(f"  party screen: {' | '.join(rep['party_screen'])}")
        print(f"  party count : {rep.get('party_count')}")
    if rep.get("refusal_text"):
        print(f"  REFUSAL     : {rep['refusal_text']}")
    for n in rep.get("notes", []):
        print(f"  note        : {n}")
    print(f"  frames      : menu@{rep.get('frames_to_main_menu')} "
          f"overworld@{rep.get('frames_to_overworld')}")
    print(f"  VERDICT     : {rep['verdict'].upper()} — {rep['reason']}")
    for s in rep.get("screenshots", []):
        print(f"    shot: {s}")

    if rep.get("sram_changed_bytes") is not None:
        print(f"  sram        : {rep['sram_changed_bytes']} save byte(s) "
              f"changed (primary {rep.get('sram_changed_primary_bytes', 0)}, "
              f"backup {rep.get('sram_changed_backup_bytes', 0)}, other "
              f"{rep.get('sram_changed_other_bytes', 0)}); scratch "
              f"{rep.get('sram_changed_scratch_bytes', 0)}; rtc footer "
              f"{rep.get('rtc_footer_changed_bytes', 0)}/"
              f"{rep.get('rtc_footer_bytes', 0)}")

    if a.json:
        Path(a.json).write_text(json.dumps(rep, indent=2))
        print(f"  json        : {a.json}")

    if rep.get("harness_error"):
        print(f"  HARNESS ERROR: {rep['harness_error']}", file=sys.stderr)
    fails = evaluate(rep, a)
    for f in fails:
        print(f"  FAIL: {f}")
    return exit_code(rep, fails)


if __name__ == "__main__":
    sys.exit(main())
