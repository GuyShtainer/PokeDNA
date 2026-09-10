#!/usr/bin/env python3
"""Generate source/gb_fields.c and source/gb_flags.c — the per-game Gen-1/2 field-offset
and named-event-flag tables (BACKLOG #49 P0, docs/GEN12-PARITY-DESIGN.md §4.0).

WHY A GENERATED TABLE, not hand-written offsets in source/: headline #3 of the design doc
says it plainly — "Gold/Silver and Crystal share no field offset past 0x2050 — a single
'Gen 2' constant is a bug". Four games, one struct shape, means every offset has to be
looked up per (game, field) or someone eventually copies a Gold constant onto a Crystal
save. This script is that lookup, computed from source instead of typed by hand twice.

THREE INDEPENDENT DERIVATIONS/GUARDS, cross-checked against each other:

  1. FIELD OFFSETS. Every field's file offset is `region_file_base + (wram_addr -
     region_wram_start)` — the exact formula docs/GEN12-PARITY-DESIGN.md §1.0 states
     ("file = region_base + (wram_symbol - region_start)") — or, for the handful of
     fields living directly in SRAM outside any WRAM-copied region (Crystal's gender,
     the GS Ball flag, Mystery Gift), `bank*0x2000 + (addr-0xA000)` via sram_file_off().
     Every address is resolved from the pinned symbols/*.sym files, not typed in by
     hand — a renamed symbol in a future decomp update fails the build loudly
     (KeyError). D()/S() below cross-check the result against the design doc's own
     cited, corpus-VERIFIED offset (`check_off`) and, wherever the doc states a byte
     count, its cited length too (`check_len`) — a mismatch is this script's bug, not
     something to paper over.

  2. THE OVERRUN SCAN (P0 review D5). A wrong SIZE does not show up as a wrong offset —
     GBF_OPTIONS's offset was always right, its claimed 8-byte width on Red/Yellow was
     not. overrun_scan() resolves every field's own WRAM/SRAM address and asserts no
     OTHER top-level symbol in that game's .sym file sits strictly inside
     (addr, addr+size) — the second, size-carrying symbol a wrong width would swallow.
     _selftest_overrun_scan() proves the scanner would have caught the three review
     found this way (GBF_OPTIONS on Red/Yellow, GBF_UNLOCKED_UNOWN, GBF_DAYCARE_REC)
     using their real addresses, before main() trusts it on the live table.

  3. NAMED EVENT-FLAG INDICES (source/gb_flags.c). §1.3's own warning — "flag NUMBERING
     is per-game, not per-generation... EVENT_GOT_RAINBOW_WING is #120 in Gold and #822
     in Crystal" — means these can never be typed in by hand either. Each game's
     `const_def`/`const`/`const_skip`/`const_next` event-constants file is a literal,
     auto-incrementing enum (pret's own RGBDS macro convention); parse_const_file() below
     replays that counter exactly the way the assembler does. Cross-checked against every
     index docs/GEN12-PARITY-DESIGN.md §1.3 cites by name (57 checks, see
     _selftest_flag_indices) before this script trusts its own parser on the rest.

LICENSING (docs/kb/licensing.md): offsets, WRAM addresses and flag bit-indices are FACTS,
not copyrightable expression — this script reads them out of the pinned, reference-only
decomp checkouts (assets/upstream/*, unlicensed) and the .sym files, and writes them back
out as our own C tables and (for flags) our OWN label text. No decomp comment, code
structure or prose is copied into source/.

Outputs (git-ignored, like source/learnsets2.c -- same policy, no Makefile step needed:
the auto-glob picks them up when present, source/gb_fields_fallback.c's weak symbols
cover a clone that never ran this script):
  source/gb_fields.c   { GbGame x GbField -> file offset, size, GbFieldKind }
  source/gb_flags.c    { GbGame x flag index -> our own label text }, four tables

Run from the repo root:  python3 tools/gen_gbfields.py

P0 REVIEW (SHIP WITH CHANGES), findings D1-D5 and D10 addressed in this pass:
  D1-D4: four fields' sizes were a single value copied across all four games instead
    of per-game like the offset (GBF_OPTIONS 8B on every game when Red/Yellow's is 1B;
    GBF_DAYCARE_REC 33B on every game when Gen 2's is 32B; GBF_UNLOCKED_UNOWN 4B when
    it is 1B; GBF_DEX_OWNED/SEEN 19B on every game when Gen 2's is 32B). Fixed by
    giving D()/S() an optional per-game `size` override, matching how `check_off`
    already overrides per game.
  D5: the overrun scan above, new.
  D10: all 114 previously hand-transcribed offsets that live inside a tracked region
    are now D(...) calls (resolved from a real symbol, not typed in); the 6 that live
    directly in SRAM outside any region are now S(...) calls through the previously
    unused sram_file_off(). The generated OFFSETS are unchanged by this (every
    resolved address maps back to the exact same file offset that was already there);
    only the four D1-D4 SIZES actually change.
NOTE for the P1 owner (not this slice's job): g2w_finish() never HEALS a stale Gen-2
mirror region — it re-describes whatever bytes already sit at the backup destinations.
The retail-faithful behaviour is TryLoadSaveFile's (pokegold/engine/menus/save.asm:
538-552): copy all five primary regions to their backups, THEN recompute the checksum.
A slice that wants PokeDNA to actually re-sync a stale region (not just its checksum)
needs that copy step and its own retail-gate case — tracked as P1 scope, not P0's.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIELDS_OUT = os.path.join(ROOT, "source", "gb_fields.c")
FLAGS_OUT = os.path.join(ROOT, "source", "gb_flags.c")

GAMES = ["RED", "YELLOW", "GS", "CRYSTAL"]  # RED stands in for Red/Blue (byte-identical,
                                             # design doc headline #2); GS for Gold/Silver.

SYM_PATHS = {
    "RED":     "assets/upstream/pokered/symbols/pokered.sym",
    "YELLOW":  "assets/upstream/pokeyellow/symbols/pokeyellow.sym",
    "GS":      "assets/upstream/pokegold/symbols/pokegold.sym",
    "CRYSTAL": "assets/upstream/pokecrystal/symbols/pokecrystal.sym",
}
EVENT_CONST_PATHS = {
    "RED":     "assets/upstream/pokered/constants/event_constants.asm",
    "YELLOW":  "assets/upstream/pokeyellow/constants/event_constants.asm",
    "GS":      "assets/upstream/pokegold/constants/event_flags.asm",
    "CRYSTAL": "assets/upstream/pokecrystal/constants/event_flags.asm",
}


# ============================================================== .sym parsing =====
def load_sym(path):
    """{symbol_name: (bank, addr)} from a pinned rgbds .sym file, last-definition-wins
    (a handful of WRAM labels are legitimately reused for different purposes; the LAST
    line in the file's own bank/address order is the one live at the point this script
    cares about, matching how the linker itself would resolve a duplicate)."""
    out = {}
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        for line in f:
            line = line.split(";")[0].strip()
            m = re.match(r'^([0-9a-fA-F]{2}):([0-9a-fA-F]{4})\s+(\S+)$', line)
            if not m:
                continue
            bank = int(m.group(1), 16)
            addr = int(m.group(2), 16)
            out[m.group(3)] = (bank, addr)
    return out


def sram_file_off(bank, addr):
    """A symbol whose address is IN the SRAM window (0xA000-0xBFFF) maps to a save-file
    offset directly: file = bank*0x2000 + (addr-0xA000). Used for fields whose own
    SRAM-side label exists (sOptions, sCrystalData, sGSBallFlag, sMysteryGiftItem/
    Unlocked) rather than only a WRAM working copy this script would have to chase
    through a region."""
    if not (0xA000 <= addr < 0xC000):
        raise ValueError(f"{addr:#06x} is not in the SRAM window 0xA000-0xBFFF")
    return bank * 0x2000 + (addr - 0xA000)


class Region:
    """One WRAM->SRAM save-copy region (docs/GEN12-PARITY-DESIGN.md §1.0's table): a
    symbol name marking the region's WRAM start, and the file offset the save routine
    copies that WRAM byte to. A field inside the region derives its own file offset by
    difference: file_base + (wram_addr(field_symbol) - wram_addr(start_symbol))."""
    def __init__(self, start_symbol, file_base, length):
        self.start_symbol = start_symbol
        self.file_base = file_base
        self.length = length


# §1.0's own region tables, keyed by game. Every base below is cross-checked against a
# LIVE symbol lookup in main() (region.wram_start is filled in from the .sym file, then
# asserted the region arithmetic reproduces every field this script also derives).
REGIONS = {
    "RED": {
        "player_name": Region("wPlayerName",      0x2598, 11),
        "main_data":   Region("wMainDataStart",    0x25A3, 1929),
        "sprite_data": Region("wSpriteDataStart",  0x2D2C, 512),
        "party_data":  Region("wPartyDataStart",   0x2F2C, 404),
        "box_data":    Region("wBoxDataStart",     0x30C0, 1122),
    },
    "GS": {
        "options":       Region("sOptions",        0x2000, 8),   # SRAM-side symbol
        "player_data_1": Region("wPlayerData1",     0x2009, 550),
        "player_data_2": Region("wPlayerData2",     0x222F, 426),
        "player_data_3": Region("wPlayerData3",     0x23D9, 1149),
        "cur_map_data":  Region("wCurMapData",      0x2856, 52),
        "pokemon_data":  Region("wPokemonData",     0x288A, 1247),
    },
    "CRYSTAL": {
        "options":       Region("sOptions",         0x2000, 8),  # SRAM-side symbol
        "player_data":   Region("wPlayerData",       0x2009, 2090),
        "cur_map_data":  Region("wCurMapData",       0x2833, 50),
        "pokemon_data":  Region("wPokemonData",      0x2865, 798),
    },
}
REGIONS["YELLOW"] = REGIONS["RED"]   # byte-identical save layout (§1.10, headline #2);
                                     # symbols differ by a byte or two but the SAME
                                     # region-symbol NAMES exist in pokeyellow's own .sym.


def region_off(symtab, region, wram_symbol):
    """file offset of `wram_symbol`, via its containing `region`."""
    start_bank, start_addr = symtab[region.start_symbol]
    bank, addr = symtab[wram_symbol]
    if bank != start_bank:
        raise ValueError(f"{wram_symbol} bank {bank} != region start bank {start_bank}")
    off = region.file_base + (addr - start_addr)
    if not (region.file_base <= off < region.file_base + region.length):
        raise ValueError(f"{wram_symbol} -> file {off:#06x} falls outside its own "
                         f"region {region.start_symbol} [{region.file_base:#06x}, "
                         f"{region.file_base + region.length:#06x})")
    return off


# ============================================================== fields ===========
# GbFieldKind tags, matching source/gb_fields.h's enum.
U8, U16BE, U24BE, BCD24BE, TEXT, BYTES, BITFIELD = (
    "GBFK_U8", "GBFK_U16BE", "GBFK_U24BE", "GBFK_BCD24BE", "GBFK_TEXT", "GBFK_BYTES",
    "GBFK_BITFIELD")

ABSENT = None


def D(region, sym, check_off, size=None, check_len=None):
    """A field this script DERIVES from the .sym files (region + wram symbol), with the
    design doc's own cited offset as a build-time cross-check. `size`, if given,
    OVERRIDES the field row's default size for this one game (P0 review D1-D4: a
    field's byte width can differ per game just like its offset can). `check_len`, if
    given, is an INDEPENDENT cross-check against a byte count the design doc's own
    prose states (separate from whatever `size`/the row default says) -- a second
    witness, not a restatement of the first."""
    return ("derive", region, sym, check_off, size, check_len)


def S(sym, check_off, size=None, check_len=None):
    """A field resolved directly from an SRAM-side symbol (sram_file_off), for the
    handful that live outside every tracked WRAM-copy region entirely (Crystal's
    gender byte, the GS Ball flag, Mystery Gift) -- P0 review D10."""
    return ("sram", sym, check_off, size, check_len)


def _assert_off(game, sym, where, off, check_off):
    if off != check_off:
        raise AssertionError(
            f"{game} {sym} ({where}) derived {off:#06x} but "
            f"docs/GEN12-PARITY-DESIGN.md cites {check_off:#06x} -- the .sym file, this "
            f"script's region table, or the design doc's own citation disagree; this "
            f"must be resolved by hand before the table can be trusted")


def _assert_len(game, sym, size, check_len):
    if check_len is not None and size != check_len:
        raise AssertionError(
            f"{game} {sym}: this row's size is {size} B but docs/GEN12-PARITY-DESIGN.md's "
            f"own prose cites {check_len} B -- independently disagreeing witnesses, "
            f"must be resolved by hand")


def resolve_field(symtabs, game, spec, default_size):
    """spec is a D(...)/S(...) tuple, a plain int (should not remain after P0 review
    D10 -- kept only as an escape hatch), or None/ABSENT.
    Returns (off, size, addr) -- addr is (bank, wram_or_sram_addr) for the overrun
    scan, or None when there is no single symbol behind this cell (a bare literal)."""
    if spec is ABSENT:
        return None, None, None
    if isinstance(spec, int):
        return spec, default_size, None   # no symbol backing -- exempt from the overrun scan

    kind = spec[0]
    if kind == "derive":
        _, region_key, sym, check_off, size_override, check_len = spec
        region = REGIONS[game][region_key]
        off = region_off(symtabs[game], region, sym)
        size = default_size if size_override is None else size_override
        _assert_off(game, sym, f"region {region_key}", off, check_off)
        _assert_len(game, sym, size, check_len)
        bank, addr = symtabs[game][sym]
        return off, size, (bank, addr)

    if kind == "sram":
        _, sym, check_off, size_override, check_len = spec
        bank, addr = symtabs[game][sym]
        off = sram_file_off(bank, addr)
        size = default_size if size_override is None else size_override
        _assert_off(game, sym, "SRAM", off, check_off)
        _assert_len(game, sym, size, check_len)
        return off, size, (bank, addr)

    raise ValueError(f"unknown spec kind {kind!r}")


# (field_name, kind, default_size, {game: D(...)|S(...)|int|ABSENT})
#
# default_size applies to every game whose spec does not override it with size=N.
# Every present cell below is a D()/S() call (P0 review D10) except the handful the
# comments call out as a bare literal because no symbol resolves there.
FIELDS = [
  # ---- 1.1 trainer card -----------------------------------------------------------
  ("PLAYER_NAME", TEXT, 11, {
      "RED": D("player_name", "wPlayerName", 0x2598), "YELLOW": D("player_name", "wPlayerName", 0x2598),
      "GS": D("player_data_1", "wPlayerName", 0x200B), "CRYSTAL": D("player_data", "wPlayerName", 0x200B)}),
  ("TRAINER_ID", U16BE, 2, {
      "RED": D("main_data", "wPlayerID", 0x2605), "YELLOW": D("main_data", "wPlayerID", 0x2605),
      "GS": D("player_data_1", "wPlayerID", 0x2009), "CRYSTAL": D("player_data", "wPlayerID", 0x2009)}),
  ("MONEY", BCD24BE, 3, {
      "RED": D("main_data", "wPlayerMoney", 0x25F3), "YELLOW": D("main_data", "wPlayerMoney", 0x25F3),
      "GS": ABSENT, "CRYSTAL": ABSENT}),   # Gen-2 money is BINARY, not BCD -- see MONEY_BIN
  ("MONEY_BIN", U24BE, 3, {
      "RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wMoney", 0x23DB), "CRYSTAL": D("player_data", "wMoney", 0x23DC)}),
  ("COINS", BCD24BE, 2, {   # Gen 1: 2-byte BCD (§1.1: "Gen 1 2 B BCD")
      "RED": D("main_data", "wPlayerCoins", 0x2850), "YELLOW": D("main_data", "wPlayerCoins", 0x2850),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("COINS_BIN", U16BE, 2, {
      "RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wCoins", 0x23E2), "CRYSTAL": D("player_data", "wCoins", 0x23E3)}),
  ("MOMS_MONEY", U24BE, 3, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wMomsMoney", 0x23DE), "CRYSTAL": D("player_data", "wMomsMoney", 0x23DF)}),
  ("MOM_SAVING_FLAG", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wMomSavingMoney", 0x23E1),
      "CRYSTAL": D("player_data", "wMomSavingMoney", 0x23E2)}),
  ("BADGES", BITFIELD, 1, {   # Gen 1: one region, both leagues in one byte
      "RED": D("main_data", "wObtainedBadges", 0x2602), "YELLOW": D("main_data", "wObtainedBadges", 0x2602),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("BADGES_JOHTO", BITFIELD, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wJohtoBadges", 0x23E4), "CRYSTAL": D("player_data", "wJohtoBadges", 0x23E5)}),
  ("BADGES_KANTO", BITFIELD, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wKantoBadges", 0x23E5), "CRYSTAL": D("player_data", "wKantoBadges", 0x23E6)}),
  ("RIVAL_NAME", TEXT, 11, {
      "RED": D("main_data", "wRivalName", 0x25F6), "YELLOW": D("main_data", "wRivalName", 0x25F6),
      "GS": D("player_data_1", "wRivalName", 0x2021), "CRYSTAL": D("player_data", "wRivalName", 0x2021)}),
  ("MOTHERS_NAME", TEXT, 11, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wMomsName", 0x2016), "CRYSTAL": D("player_data", "wMomsName", 0x2016)}),
  ("PLAYTIME_HOURS", U8, 1, {
      "RED": D("main_data", "wPlayTimeHours", 0x2CED), "YELLOW": D("main_data", "wPlayTimeHours", 0x2CED),
      "GS": ABSENT, "CRYSTAL": ABSENT}),   # Gen-2 play time lives in the clock block, §1.8
  ("PLAYTIME_MAXED", U8, 1, {
      "RED": D("main_data", "wPlayTimeMaxed", 0x2CEE), "YELLOW": D("main_data", "wPlayTimeMaxed", 0x2CEE),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("PLAYTIME_MINUTES", U8, 1, {
      "RED": D("main_data", "wPlayTimeMinutes", 0x2CEF), "YELLOW": D("main_data", "wPlayTimeMinutes", 0x2CEF),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("PLAYTIME_SECONDS", U8, 1, {
      "RED": D("main_data", "wPlayTimeSeconds", 0x2CF0), "YELLOW": D("main_data", "wPlayTimeSeconds", 0x2CF0),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("PLAYTIME_FRAMES", U8, 1, {
      "RED": D("main_data", "wPlayTimeFrames", 0x2CF1), "YELLOW": D("main_data", "wPlayTimeFrames", 0x2CF1),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  # GENDER: outside every checksummed span (§1.9); Crystal's own sCrystalData SRAM
  # symbol, direct (P0 review D10 -- was a bare literal).
  ("GENDER", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT, "GS": ABSENT,
      "CRYSTAL": S("sCrystalData", 0x3E3D)}),
  # OPTIONS: P0 review D1 -- Red/Yellow's wOptions is ONE byte (wObtainedBadges sits
  # immediately after it, measured via the .sym files); only G/S's and Crystal's
  # SRAM-side sOptions block is genuinely 8 bytes (§1.0: "sOptions ... 8 B").
  ("OPTIONS", BYTES, 8, {
      "RED": D("main_data", "wOptions", 0x2601, size=1), "YELLOW": D("main_data", "wOptions", 0x2601, size=1),
      "GS": D("options", "sOptions", 0x2000), "CRYSTAL": D("options", "sOptions", 0x2000)}),

  # ---- 1.2 bag / PC ------------------------------------------------------------------
  ("BAG_COUNT", U8, 1, {
      "RED": D("main_data", "wNumBagItems", 0x25C9), "YELLOW": D("main_data", "wNumBagItems", 0x25C9),
      "GS": D("player_data_3", "wNumItems", 0x241F), "CRYSTAL": D("player_data", "wNumItems", 0x2420)}),
  ("BAG_BODY", BYTES, 41, {
      "RED": D("main_data", "wBagItems", 0x25CA), "YELLOW": D("main_data", "wBagItems", 0x25CA),
      "GS": D("player_data_3", "wItems", 0x2420), "CRYSTAL": D("player_data", "wItems", 0x2421)}),
  ("KEY_ITEMS_COUNT", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wNumKeyItems", 0x2449), "CRYSTAL": D("player_data", "wNumKeyItems", 0x244A)}),
  ("KEY_ITEMS_BODY", BYTES, 26, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wKeyItems", 0x244A), "CRYSTAL": D("player_data", "wKeyItems", 0x244B)}),
  ("BALLS_COUNT", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wNumBalls", 0x2464), "CRYSTAL": D("player_data", "wNumBalls", 0x2465)}),
  ("BALLS_BODY", BYTES, 25, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wBalls", 0x2465), "CRYSTAL": D("player_data", "wBalls", 0x2466)}),
  ("TMHM_COUNTS", BYTES, 57, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wTMsHMs", 0x23E6), "CRYSTAL": D("player_data", "wTMsHMs", 0x23E7)}),
  ("PC_COUNT", U8, 1, {
      "RED": D("main_data", "wNumBoxItems", 0x27E6), "YELLOW": D("main_data", "wNumBoxItems", 0x27E6),
      "GS": D("player_data_3", "wNumPCItems", 0x247E), "CRYSTAL": D("player_data", "wNumPCItems", 0x247F)}),
  ("PC_BODY", BYTES, 101, {
      "RED": D("main_data", "wBoxItems", 0x27E7), "YELLOW": D("main_data", "wBoxItems", 0x27E7),
      "GS": D("player_data_3", "wPCItems", 0x247F), "CRYSTAL": D("player_data", "wPCItems", 0x2480)}),

  # ---- 1.3 flags/counters base offsets ---------------------------------------------
  ("EVENT_FLAGS_BASE", BYTES, 320, {   # size is per-game; Gen 1 320 B, Gen 2 256 B --
      "RED": D("main_data", "wEventFlags", 0x29F3), "YELLOW": D("main_data", "wEventFlags", 0x29F3),
      "GS": ABSENT, "CRYSTAL": ABSENT}),   # (Red/Blue+Yellow row only; see EVENT_FLAGS_BASE_G2)
  ("EVENT_FLAGS_BASE_G2", BYTES, 256, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wEventFlags", 0x261F),
      "CRYSTAL": D("player_data", "wEventFlags", 0x2600)}),
  ("HIDDEN_ITEM_FLAGS", BYTES, 14, {
      "RED": D("main_data", "wObtainedHiddenItemsFlags", 0x299C),
      "YELLOW": D("main_data", "wObtainedHiddenItemsFlags", 0x299C),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("HIDDEN_COIN_FLAGS", BYTES, 2, {
      "RED": D("main_data", "wObtainedHiddenCoinsFlags", 0x29AA),
      "YELLOW": D("main_data", "wObtainedHiddenCoinsFlags", 0x29AA),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("TOGGLE_OBJ_FLAGS", BYTES, 32, {
      "RED": D("main_data", "wToggleableObjectFlags", 0x2852),
      "YELLOW": D("main_data", "wToggleableObjectFlags", 0x2852),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("BIKE_FLAGS", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wBikeFlags", 0x27A7), "CRYSTAL": D("player_data", "wBikeFlags", 0x2783)}),
  # UNLOCKED_UNOWN: P0 review D3 -- 1 byte, not 4 (wFirstUnownSeen sits immediately
  # after it in both Gold's and Crystal's own .sym; measured, not guessed at a
  # 26-letter bitfield's naive minimum byte count).
  ("UNLOCKED_UNOWN", BITFIELD, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("pokemon_data", "wUnlockedUnowns", 0x2AA6),
      "CRYSTAL": D("pokemon_data", "wUnlockedUnowns", 0x2A81)}),
  ("GS_BALL_FLAG", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT, "GS": ABSENT,
      "CRYSTAL": S("sGSBallFlag", 0x3E3C)}),   # outside every checksummed span, §1.3
  ("MYSTERY_GIFT_ITEM", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": S("sMysteryGiftItem", 0x0BE2), "CRYSTAL": S("sMysteryGiftItem", 0x0BE2)}),
  ("MYSTERY_GIFT_UNLOCKED", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": S("sMysteryGiftUnlocked", 0x0BE3), "CRYSTAL": S("sMysteryGiftUnlocked", 0x0BE3)}),

  # ---- 1.4 fly destinations ---------------------------------------------------------
  ("FLY_FLAGS", BITFIELD, 2, {
      "RED": D("main_data", "wTownVisitedFlag", 0x29B7), "YELLOW": D("main_data", "wTownVisitedFlag", 0x29B7),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("FLY_FLAGS_G2", BITFIELD, 4, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wVisitedSpawns", 0x2856), "CRYSTAL": D("cur_map_data", "wVisitedSpawns", 0x2833)}),

  # ---- 1.5 map position / warp -------------------------------------------------------
  ("MAP_ID", U8, 1, {
      "RED": D("main_data", "wCurMap", 0x260A), "YELLOW": D("main_data", "wCurMap", 0x260A),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("MAP_GROUP", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wMapGroup", 0x2868), "CRYSTAL": D("cur_map_data", "wMapGroup", 0x2843)}),
  ("MAP_NUMBER", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wMapNumber", 0x2869), "CRYSTAL": D("cur_map_data", "wMapNumber", 0x2844)}),
  ("POS_X", U8, 1, {
      "RED": D("main_data", "wXCoord", 0x260E), "YELLOW": D("main_data", "wXCoord", 0x260E),
      "GS": D("cur_map_data", "wXCoord", 0x286B), "CRYSTAL": D("cur_map_data", "wXCoord", 0x2846)}),
  ("POS_Y", U8, 1, {
      "RED": D("main_data", "wYCoord", 0x260D), "YELLOW": D("main_data", "wYCoord", 0x260D),
      "GS": D("cur_map_data", "wYCoord", 0x286A), "CRYSTAL": D("cur_map_data", "wYCoord", 0x2845)}),
  ("POS_XBLOCK", U8, 1, {
      "RED": D("main_data", "wXBlockCoord", 0x2610), "YELLOW": D("main_data", "wXBlockCoord", 0x2610),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("POS_YBLOCK", U8, 1, {
      "RED": D("main_data", "wYBlockCoord", 0x260F), "YELLOW": D("main_data", "wYBlockCoord", 0x260F),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("WARP_NUMBER", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wWarpNumber", 0x2867), "CRYSTAL": D("cur_map_data", "wWarpNumber", 0x2842)}),
  ("LAST_MAP", U8, 1, {
      "RED": D("main_data", "wLastMap", 0x2611), "YELLOW": D("main_data", "wLastMap", 0x2611),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("ESCAPE_WARP", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wDigWarpNumber", 0x285A), "CRYSTAL": D("cur_map_data", "wDigWarpNumber", 0x2837)}),
  ("ESCAPE_GROUP", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wDigMapGroup", 0x285B), "CRYSTAL": D("cur_map_data", "wDigMapGroup", 0x2838)}),
  ("ESCAPE_NUMBER", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wDigMapNumber", 0x285C), "CRYSTAL": D("cur_map_data", "wDigMapNumber", 0x2839)}),
  ("BACKUP_WARP", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wBackupWarpNumber", 0x285D),
      "CRYSTAL": D("cur_map_data", "wBackupWarpNumber", 0x283A)}),
  ("BACKUP_GROUP", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wBackupMapGroup", 0x285E), "CRYSTAL": D("cur_map_data", "wBackupMapGroup", 0x283B)}),
  ("BACKUP_NUMBER", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wBackupMapNumber", 0x285F),
      "CRYSTAL": D("cur_map_data", "wBackupMapNumber", 0x283C)}),
  ("LAST_SPAWN_MAP", U8, 1, {   # Gen 1: one flat id, like MAP_ID
      "RED": D("main_data", "wLastBlackoutMap", 0x29C5), "YELLOW": D("main_data", "wLastBlackoutMap", 0x29C5),
      "GS": ABSENT, "CRYSTAL": ABSENT}),
  ("LAST_SPAWN_GROUP", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wLastSpawnMapGroup", 0x2863),
      "CRYSTAL": D("cur_map_data", "wLastSpawnMapGroup", 0x2840)}),
  ("LAST_SPAWN_NUMBER", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("cur_map_data", "wLastSpawnMapNumber", 0x2864),
      "CRYSTAL": D("cur_map_data", "wLastSpawnMapNumber", 0x2841)}),

  # ---- 1.6 pokedex --------------------------------------------------------------------
  # DEX_OWNED/DEX_SEEN: P0 review D4 -- Gen 2's bitfield is 32 B (251 bits), not 19
  # (Gen 1's own 151-bit width); wPokedexCaught->wPokedexSeen is exactly 0x20 apart in
  # both Gold's and Crystal's .sym, and §1.6's own prose already said "32 B" for both.
  ("DEX_OWNED", BITFIELD, 19, {
      "RED": D("main_data", "wPokedexOwned", 0x25A3, check_len=19),
      "YELLOW": D("main_data", "wPokedexOwned", 0x25A3, check_len=19),
      "GS": D("pokemon_data", "wPokedexCaught", 0x2A4C, size=32, check_len=32),
      "CRYSTAL": D("pokemon_data", "wPokedexCaught", 0x2A27, size=32, check_len=32)}),
  ("DEX_SEEN", BITFIELD, 19, {
      "RED": D("main_data", "wPokedexSeen", 0x25B6, check_len=19),
      "YELLOW": D("main_data", "wPokedexSeen", 0x25B6, check_len=19),
      "GS": D("pokemon_data", "wPokedexSeen", 0x2A6C, size=32, check_len=32),
      "CRYSTAL": D("pokemon_data", "wPokedexSeen", 0x2A47, size=32, check_len=32)}),

  # ---- 1.7 day-care -------------------------------------------------------------------
  ("DAYCARE_FLAG", U8, 1, {
      "RED": D("main_data", "wDayCareInUse", 0x2CF4), "YELLOW": D("main_data", "wDayCareInUse", 0x2CF4),
      "GS": D("pokemon_data", "wDayCareMan", 0x2AA8), "CRYSTAL": D("pokemon_data", "wDayCareMan", 0x2A83)}),
  ("DAYCARE_NICK", TEXT, 11, {
      "RED": D("main_data", "wDayCareMonName", 0x2CF5), "YELLOW": D("main_data", "wDayCareMonName", 0x2CF5),
      "GS": D("pokemon_data", "wBreedMon1Nickname", 0x2AA9),
      "CRYSTAL": D("pokemon_data", "wBreedMon1Nickname", 0x2A84)}),
  ("DAYCARE_OT", TEXT, 11, {
      "RED": D("main_data", "wDayCareMonOT", 0x2D00), "YELLOW": D("main_data", "wDayCareMonOT", 0x2D00),
      "GS": D("pokemon_data", "wBreedMon1OT", 0x2AB4), "CRYSTAL": D("pokemon_data", "wBreedMon1OT", 0x2A8F)}),
  # DAYCARE_REC: P0 review D2 -- Gen 2's box record here is 32 B, not Gen 1's 33
  # (wBreedMon1->wBreedMon1BoxEnd is exactly 0x20 apart in both Gold's and Crystal's
  # .sym, and wDayCareLady sits at that same +32 address -- §1.7's own prose already
  # said "32 B" for G/S and Crystal). Gen 1's 33 is genuinely different
  # (GEN1_BOX_REC_BYTES) and unaffected.
  ("DAYCARE_REC", BYTES, 32, {
      "RED": D("main_data", "wDayCareMon", 0x2D0B, size=33, check_len=33),
      "YELLOW": D("main_data", "wDayCareMon", 0x2D0B, size=33, check_len=33),
      "GS": D("pokemon_data", "wBreedMon1", 0x2ABF, check_len=32),
      "CRYSTAL": D("pokemon_data", "wBreedMon1", 0x2A9A, check_len=32)}),
  ("DAYCARE_LADY_FLAG", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("pokemon_data", "wDayCareLady", 0x2ADF), "CRYSTAL": D("pokemon_data", "wDayCareLady", 0x2ABA)}),
  ("DAYCARE_STEPS", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("pokemon_data", "wStepsToEgg", 0x2AE0), "CRYSTAL": D("pokemon_data", "wStepsToEgg", 0x2ABB)}),
  ("DAYCARE2_NICK", TEXT, 11, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("pokemon_data", "wBreedMon2Nickname", 0x2AE2),
      "CRYSTAL": D("pokemon_data", "wBreedMon2Nickname", 0x2ABD)}),
  ("DAYCARE2_OT", TEXT, 11, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("pokemon_data", "wBreedMon2OT", 0x2AED), "CRYSTAL": D("pokemon_data", "wBreedMon2OT", 0x2AC8)}),
  ("DAYCARE2_REC", BYTES, 32, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("pokemon_data", "wBreedMon2", 0x2AF8), "CRYSTAL": D("pokemon_data", "wBreedMon2", 0x2AD3)}),
  ("DAYCARE_EGG_NICK", TEXT, 11, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("pokemon_data", "wEggMonNickname", 0x2B18),
      "CRYSTAL": D("pokemon_data", "wEggMonNickname", 0x2AF3)}),
  ("DAYCARE_EGG_REC", BYTES, 32, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("pokemon_data", "wEggMon", 0x2B2E), "CRYSTAL": D("pokemon_data", "wEggMon", 0x2B09)}),

  # ---- 1.8 Gen-2 clock (Gen 1 has none) -------------------------------------------------
  ("RTC_START_DAY", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wStartDay", 0x2044), "CRYSTAL": D("player_data", "wStartDay", 0x2044)}),
  ("RTC_START_HOUR", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wStartHour", 0x2045), "CRYSTAL": D("player_data", "wStartHour", 0x2045)}),
  ("RTC_START_MINUTE", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wStartMinute", 0x2046), "CRYSTAL": D("player_data", "wStartMinute", 0x2046)}),
  ("RTC_START_SECOND", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wStartSecond", 0x2047), "CRYSTAL": D("player_data", "wStartSecond", 0x2047)}),
  ("RTC_SNAPSHOT", BYTES, 4, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wRTC", 0x2048), "CRYSTAL": D("player_data", "wRTC", 0x2048)}),
  ("RTC_DST", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wDST", 0x2050), "CRYSTAL": D("player_data", "wDST", 0x2050)}),
  # P1a review D6: Gen 2 is NOT hour-uncapped the way gb_trainer.h used to claim --
  # wGameTimeCap sits one byte before wGameTimeHours in BOTH games (pokegold.sym
  # 01:d1ea / pokecrystal.sym 01:d4c3, one byte before wGameTimeHours' own d1eb/d4c4),
  # bit GAME_TIME_CAPPED = 0 (both ram_constants.asm), set once wGameTimeHours would
  # overflow (home/game_time.asm). Gen 1's PLAYTIME_MAXED above is the exact analogue.
  ("GAMETIME_CAP", BITFIELD, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wGameTimeCap", 0x2052),
      "CRYSTAL": D("player_data", "wGameTimeCap", 0x2051)}),
  ("GAMETIME_HOURS", U16BE, 2, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wGameTimeHours", 0x2053),
      "CRYSTAL": D("player_data", "wGameTimeHours", 0x2052)}),
  ("GAMETIME_MINUTES", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wGameTimeMinutes", 0x2055),
      "CRYSTAL": D("player_data", "wGameTimeMinutes", 0x2054)}),
  ("GAMETIME_SECONDS", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wGameTimeSeconds", 0x2056),
      "CRYSTAL": D("player_data", "wGameTimeSeconds", 0x2055)}),
  ("GAMETIME_FRAMES", U8, 1, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wGameTimeFrames", 0x2057),
      "CRYSTAL": D("player_data", "wGameTimeFrames", 0x2056)}),
  ("CUR_DAY", U8, 1, {
      "RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_1", "wCurDay", 0x205A), "CRYSTAL": D("player_data", "wCurDay", 0x2059)}),

  # ---- BACKLOG #94: Gen-2 box names ----------------------------------------------------
  # sBoxNames: 14 x BOX_NAME_LENGTH(9) GB-encoded, 0x50-terminated names (pret's
  # constants/text_constants.asm: BOX_NAME_LENGTH EQU 9; engine/menus/intro_menu.asm
  # SetDefaultBoxNames writes "BOX1".."BOX14" at wBoxNames on a new game -- the exact
  # bytes this generator's own check_off below cross-checks against Gold.sav/Crystal.sav,
  # both still holding the untouched defaults). wBoxNames is bank-1 0xD8BF in pokegold.sym
  # (falls inside GS's own player_data_3 region, wPlayerData3=0xD571) and bank-1 0xDB75 in
  # pokecrystal.sym (inside Crystal's single player_data region, wPlayerData=0xD47B). Gen 1
  # has no such table (boxes stay "BOX n" -- gb_fields.h's own comment on GBF_BOXNAMES).
  ("BOXNAMES", BYTES, 126, {"RED": ABSENT, "YELLOW": ABSENT,
      "GS": D("player_data_3", "wBoxNames", 0x2727, check_len=126),
      "CRYSTAL": D("player_data", "wBoxNames", 0x2703, check_len=126)}),
]


# ============================================================== overrun scan (D5) ==
def build_reverse_index(symtab):
    """(bank,addr) -> sorted list of names at that exact address, for the overrun
    scan below."""
    rev = {}
    for name, (bank, addr) in symtab.items():
        rev.setdefault((bank, addr), []).append(name)
    for key in rev:
        rev[key].sort()
    return rev


def overrun_scan(symtabs, cells):
    """`cells`: list of (field_name, game, (bank, addr), size). For every cell whose
    size > 1, assert no OTHER, UNRELATED top-level symbol in that game's .sym file sits
    strictly inside (addr, addr+size) -- the second, size-carrying symbol a claimed
    width that is too wide would swallow. Deliberately open at both ends: a symbol AT
    addr itself is the field's own start (possibly several aliases, e.g. wBreedMon1 /
    wBreedMon1Species); a symbol AT addr+size is the NEXT field's own start, adjacent,
    not an overrun.

    "UNRELATED" excludes any symbol whose name starts with one of the field's OWN
    names at `addr` as a string prefix -- Pokemon decomps name every field of a struct
    with the struct's own label as a prefix (wBreedMon1Item, wBreedMon1Moves, ...,
    all inside wBreedMon1's own 32 bytes; wPartyMon1Species, wPartyMon1HP, ... inside
    wPartyMon1's own record), so a composite record's LEGITIMATE internal sub-fields
    would otherwise swamp this scan with false positives on every multi-field struct
    (measured: DAYCARE_REC alone has 16 such sub-labels). A genuine overrun's
    intruder does NOT share the field's prefix (wObtainedBadges inside a too-wide
    wOptions; wDayCareLady inside a too-wide wBreedMon1) -- confirmed by
    _selftest_overrun_scan() staying sensitive to all three real P0 review D5 bugs
    even with this exclusion in place. Returns a list of problem strings (empty =
    clean)."""
    revs = {}
    problems = []
    for name, game, addr_info, size in cells:
        if addr_info is None or size is None or size <= 1:
            continue
        bank, addr = addr_info
        if game not in revs:
            revs[game] = build_reverse_index(symtabs[game])
        own_names = revs[game].get((bank, addr), [])
        for other_addr in range(addr + 1, addr + size):
            hit = revs[game].get((bank, other_addr))
            if not hit:
                continue
            foreign = [h for h in hit if not any(h.startswith(o) for o in own_names)]
            if foreign:
                problems.append(
                    f"{game} {name} ({bank:02x}:{addr:04x}, size {size}) overruns into "
                    f"{'/'.join(foreign)} at {bank:02x}:{other_addr:04x} "
                    f"(+{other_addr - addr})")
    return problems


def _selftest_overrun_scan(symtabs):
    """Proves the scanner catches exactly the three real bugs P0 review D5 found
    (using their REAL addresses, resolved fresh from the .sym files -- not a copy of
    the numbers above) at their OLD, wrong sizes, one at a time so each is verified
    independently, and stays clean at the FIXED sizes. Run BEFORE the live table is
    scanned, so a change to overrun_scan() itself gets caught here first."""
    cases = [
        ("OPTIONS", "RED", symtabs["RED"]["wOptions"], 8, 1),          # was 8, really 1
        ("UNLOCKED_UNOWN", "GS", symtabs["GS"]["wUnlockedUnowns"], 4, 1),  # was 4, really 1
        ("DAYCARE_REC", "GS", symtabs["GS"]["wBreedMon1"], 33, 32),    # was 33, really 32
    ]
    n_flagged = 0
    for name, game, addr_info, wrong_size, right_size in cases:
        broken = overrun_scan(symtabs, [(name, game, addr_info, wrong_size)])
        if not broken:
            raise AssertionError(
                f"overrun-scan self-test: {game} {name} at its OLD, wrong size "
                f"{wrong_size} B must be flagged and was not")
        n_flagged += len(broken)
        fixed = overrun_scan(symtabs, [(name, game, addr_info, right_size)])
        if fixed:
            raise AssertionError(
                f"overrun-scan self-test: {game} {name} at its FIXED size {right_size} B "
                f"must NOT be flagged:\n  " + "\n  ".join(fixed))
    print(f"  overrun-scan self-test: OLD sizes for OPTIONS(RED)/UNLOCKED_UNOWN(GS)/"
         f"DAYCARE_REC(GS) each flagged ({n_flagged} finding(s) total); FIXED sizes "
         f"each clean")


# ============================================================== event flags ======
def to_int(tok):
    tok = tok.strip()
    if tok.startswith("$"):
        return int(tok[1:], 16)
    if tok.startswith("%"):
        return int(tok[1:], 2)
    return int(tok, 0)


def eval_expr(expr):
    """Minimal RGBDS constant-expression evaluator: a left-to-right sum/difference of
    int literals ($hex, %bin, decimal) -- every const_next/const_skip argument in these
    four files (checked by hand, every non-trivial line, before trusting this). A bare
    const_skip (no argument) is RGBDS's own macro default: skip exactly 1."""
    expr = expr.strip()
    if not expr:
        return 1
    tokens = re.findall(r'[+-]|\S+', expr)
    total, sign = 0, 1
    for t in tokens:
        if t == '+':
            sign = 1
        elif t == '-':
            sign = -1
        else:
            total += sign * to_int(t)
            sign = 1
    return total


def parse_const_file(path):
    """{EVENT_NAME: index} by replaying the file's const_def/const/const_skip/
    const_next counter exactly as RGBDS's own macros would (pret/pokered macros/
    coords.asm-style `const.asm` convention; every pinned checkout here uses it
    identically). Facts (names, positions), not expression -- docs/kb/licensing.md."""
    idx = {}
    counter = 0
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        for raw in f:
            line = raw.split(";")[0].strip()
            if not line:
                continue
            m = re.match(r'const_def\s*(.*)', line)
            if m:
                counter = eval_expr(m.group(1)) if m.group(1).strip() else 0
                continue
            m = re.match(r'const_skip\s*(.*)', line)
            if m:
                counter += eval_expr(m.group(1))
                continue
            m = re.match(r'const_next\s+(.*)', line)
            if m:
                counter = eval_expr(m.group(1))
                continue
            m = re.match(r'const\s+(\w+)', line)
            if m:
                idx[m.group(1)] = counter
                counter += 1
                continue
    return idx


# §1.3's own ~40-flag shortlist, by symbol name. OUR OWN label text lives in LABELS
# below (a separate dict, deliberately: the symbol name is a decomp FACT used to look
# up the per-game bit index; the label a player sees is this app's own writing).
SHORTLIST_GEN1 = [
    "EVENT_GOT_STARTER", "EVENT_GOT_POKEDEX", "EVENT_GOT_TOWN_MAP", "EVENT_GOT_BICYCLE",
    "EVENT_GOT_SS_TICKET", "EVENT_GOT_HM01", "EVENT_GOT_HM02", "EVENT_GOT_HM03",
    "EVENT_GOT_HM04", "EVENT_GOT_HM05", "EVENT_GOT_OLD_AMBER", "EVENT_GOT_DOME_FOSSIL",
    "EVENT_GOT_HELIX_FOSSIL", "EVENT_GOT_MASTER_BALL", "EVENT_GOT_POKE_FLUTE",
    "EVENT_GOT_HITMONLEE", "EVENT_GOT_HITMONCHAN", "EVENT_MET_BILL",
    "EVENT_RESCUED_MR_FUJI", "EVENT_GAVE_GOLD_TEETH", "EVENT_FOUND_ROCKET_HIDEOUT",
    "EVENT_BEAT_BROCK", "EVENT_BEAT_MISTY", "EVENT_BEAT_LT_SURGE", "EVENT_BEAT_ERIKA",
    "EVENT_BEAT_KOGA", "EVENT_BEAT_SABRINA", "EVENT_BEAT_BLAINE",
    "EVENT_BEAT_VIRIDIAN_GYM_GIOVANNI", "EVENT_BEAT_ROCKET_HIDEOUT_GIOVANNI",
    "EVENT_BEAT_SILPH_CO_GIOVANNI", "EVENT_BEAT_ARTICUNO", "EVENT_BEAT_ZAPDOS",
    "EVENT_BEAT_MOLTRES", "EVENT_BEAT_MEWTWO",
    "EVENT_BEAT_LORELEIS_ROOM_TRAINER_0", "EVENT_BEAT_BRUNOS_ROOM_TRAINER_0",
    "EVENT_BEAT_AGATHAS_ROOM_TRAINER_0", "EVENT_BEAT_LANCES_ROOM_TRAINER_0",
    "EVENT_BEAT_LANCE", "EVENT_BEAT_CHAMPION_RIVAL", "EVENT_HALL_OF_FAME_DEX_RATING",
]
SHORTLIST_GEN2 = [
    "EVENT_GOT_HM01_CUT", "EVENT_GOT_HM02_FLY", "EVENT_GOT_HM03_SURF",
    "EVENT_GOT_HM04_STRENGTH", "EVENT_GOT_HM05_FLASH", "EVENT_GOT_HM06_WHIRLPOOL",
    "EVENT_GOT_HM07_WATERFALL", "EVENT_GOT_A_POKEMON_FROM_ELM", "EVENT_GOT_BICYCLE",
    "EVENT_GOT_OLD_ROD", "EVENT_GOT_GOOD_ROD", "EVENT_GOT_SUPER_ROD",
    "EVENT_GOT_MASTER_BALL_FROM_ELM", "EVENT_GOT_SQUIRTBOTTLE", "EVENT_GOT_RAINBOW_WING",
    "EVENT_GOT_SILVER_WING", "EVENT_GOT_MYSTERY_EGG_FROM_MR_POKEMON",
    "EVENT_BEAT_FALKNER", "EVENT_BEAT_BUGSY", "EVENT_BEAT_WHITNEY", "EVENT_BEAT_MORTY",
    "EVENT_BEAT_CHUCK", "EVENT_BEAT_JASMINE", "EVENT_BEAT_PRYCE", "EVENT_BEAT_CLAIR",
    "EVENT_BEAT_BROCK", "EVENT_BEAT_MISTY", "EVENT_BEAT_LTSURGE", "EVENT_BEAT_ERIKA",
    "EVENT_BEAT_JANINE", "EVENT_BEAT_SABRINA", "EVENT_BEAT_BLAINE", "EVENT_BEAT_BLUE",
    "EVENT_BEAT_ELITE_4_WILL", "EVENT_BEAT_ELITE_4_KOGA", "EVENT_BEAT_ELITE_4_BRUNO",
    "EVENT_BEAT_ELITE_4_KAREN", "EVENT_BEAT_CHAMPION_LANCE",
]

# Our own label text (never the decomp's identifier/comment) -- shown in the UI.
LABELS = {
    "EVENT_GOT_STARTER": "Chose starter Pokemon", "EVENT_GOT_POKEDEX": "Received the Pokedex",
    "EVENT_GOT_TOWN_MAP": "Got the Town Map", "EVENT_GOT_BICYCLE": "Got the Bicycle",
    "EVENT_GOT_SS_TICKET": "Got the S.S. Ticket", "EVENT_GOT_HM01": "Got HM01 Cut",
    "EVENT_GOT_HM02": "Got HM02 Fly", "EVENT_GOT_HM03": "Got HM03 Surf",
    "EVENT_GOT_HM04": "Got HM04 Strength", "EVENT_GOT_HM05": "Got HM05 Flash",
    "EVENT_GOT_OLD_AMBER": "Got the Old Amber", "EVENT_GOT_DOME_FOSSIL": "Got the Dome Fossil",
    "EVENT_GOT_HELIX_FOSSIL": "Got the Helix Fossil", "EVENT_GOT_MASTER_BALL": "Got a Master Ball",
    "EVENT_GOT_POKE_FLUTE": "Got the Poke Flute", "EVENT_GOT_HITMONLEE": "Chose Hitmonlee",
    "EVENT_GOT_HITMONCHAN": "Chose Hitmonchan", "EVENT_MET_BILL": "Met Bill",
    "EVENT_RESCUED_MR_FUJI": "Rescued Mr. Fuji", "EVENT_GAVE_GOLD_TEETH": "Gave the gold teeth",
    "EVENT_FOUND_ROCKET_HIDEOUT": "Found the Rocket Hideout",
    "EVENT_BEAT_BROCK": "Defeated Brock", "EVENT_BEAT_MISTY": "Defeated Misty",
    "EVENT_BEAT_LT_SURGE": "Defeated Lt. Surge", "EVENT_BEAT_LTSURGE": "Defeated Lt. Surge",
    "EVENT_BEAT_ERIKA": "Defeated Erika", "EVENT_BEAT_KOGA": "Defeated Koga",
    "EVENT_BEAT_SABRINA": "Defeated Sabrina", "EVENT_BEAT_BLAINE": "Defeated Blaine",
    "EVENT_BEAT_VIRIDIAN_GYM_GIOVANNI": "Defeated Giovanni (Viridian Gym)",
    "EVENT_BEAT_ROCKET_HIDEOUT_GIOVANNI": "Defeated Giovanni (Rocket Hideout)",
    "EVENT_BEAT_SILPH_CO_GIOVANNI": "Defeated Giovanni (Silph Co.)",
    "EVENT_BEAT_ARTICUNO": "Defeated Articuno", "EVENT_BEAT_ZAPDOS": "Defeated Zapdos",
    "EVENT_BEAT_MOLTRES": "Defeated Moltres", "EVENT_BEAT_MEWTWO": "Defeated Mewtwo",
    "EVENT_BEAT_LORELEIS_ROOM_TRAINER_0": "Beat Lorelei's room",
    "EVENT_BEAT_BRUNOS_ROOM_TRAINER_0": "Beat Bruno's room",
    "EVENT_BEAT_AGATHAS_ROOM_TRAINER_0": "Beat Agatha's room",
    "EVENT_BEAT_LANCES_ROOM_TRAINER_0": "Beat Lance's room",
    "EVENT_BEAT_LANCE": "Defeated Lance", "EVENT_BEAT_CHAMPION_RIVAL": "Became Champion",
    "EVENT_HALL_OF_FAME_DEX_RATING": "Hall of Fame Pokedex rating shown",
    "EVENT_GOT_HM01_CUT": "Got HM01 Cut", "EVENT_GOT_HM02_FLY": "Got HM02 Fly",
    "EVENT_GOT_HM03_SURF": "Got HM03 Surf", "EVENT_GOT_HM04_STRENGTH": "Got HM04 Strength",
    "EVENT_GOT_HM05_FLASH": "Got HM05 Flash", "EVENT_GOT_HM06_WHIRLPOOL": "Got HM06 Whirlpool",
    "EVENT_GOT_HM07_WATERFALL": "Got HM07 Waterfall",
    "EVENT_GOT_A_POKEMON_FROM_ELM": "Received starter from Elm",
    "EVENT_GOT_OLD_ROD": "Got the Old Rod", "EVENT_GOT_GOOD_ROD": "Got the Good Rod",
    "EVENT_GOT_SUPER_ROD": "Got the Super Rod",
    "EVENT_GOT_MASTER_BALL_FROM_ELM": "Got a Master Ball (Elm)",
    "EVENT_GOT_SQUIRTBOTTLE": "Got the Squirtbottle", "EVENT_GOT_RAINBOW_WING": "Got the Rainbow Wing",
    "EVENT_GOT_SILVER_WING": "Got the Silver Wing",
    "EVENT_GOT_MYSTERY_EGG_FROM_MR_POKEMON": "Got the Mystery Egg",
    "EVENT_BEAT_FALKNER": "Defeated Falkner", "EVENT_BEAT_BUGSY": "Defeated Bugsy",
    "EVENT_BEAT_WHITNEY": "Defeated Whitney", "EVENT_BEAT_MORTY": "Defeated Morty",
    "EVENT_BEAT_CHUCK": "Defeated Chuck", "EVENT_BEAT_JASMINE": "Defeated Jasmine",
    "EVENT_BEAT_PRYCE": "Defeated Pryce", "EVENT_BEAT_CLAIR": "Defeated Clair",
    "EVENT_BEAT_JANINE": "Defeated Janine", "EVENT_BEAT_BLUE": "Defeated Blue",
    "EVENT_BEAT_ELITE_4_WILL": "Defeated Elite Four Will",
    "EVENT_BEAT_ELITE_4_KOGA": "Defeated Elite Four Koga",
    "EVENT_BEAT_ELITE_4_BRUNO": "Defeated Elite Four Bruno",
    "EVENT_BEAT_ELITE_4_KAREN": "Defeated Elite Four Karen",
    "EVENT_BEAT_CHAMPION_LANCE": "Defeated Champion Lance",
}


def _selftest_flag_indices(tables):
    """Every index docs/GEN12-PARITY-DESIGN.md §1.3 cites by name, replayed against
    this script's own const-file parser. Run BEFORE anything is emitted."""
    checks = [
        ("RED", "EVENT_GOT_TOWN_MAP", 24), ("RED", "EVENT_GOT_DOME_FOSSIL", 1406),
        ("RED", "EVENT_HALL_OF_FAME_DEX_RATING", 3), ("RED", "EVENT_GOT_STARTER", 34),
        ("RED", "EVENT_GOT_POKEDEX", 37), ("RED", "EVENT_GOT_BICYCLE", 192),
        ("RED", "EVENT_GOT_SS_TICKET", 1372), ("RED", "EVENT_GOT_HM01", 1504),
        ("RED", "EVENT_GOT_HM02", 1230), ("RED", "EVENT_GOT_HM03", 2176),
        ("RED", "EVENT_GOT_HM04", 568), ("RED", "EVENT_GOT_HM05", 984),
        ("RED", "EVENT_GOT_OLD_AMBER", 105), ("RED", "EVENT_GOT_HELIX_FOSSIL", 1407),
        ("RED", "EVENT_GOT_MASTER_BALL", 1933), ("RED", "EVENT_GOT_POKE_FLUTE", 296),
        ("RED", "EVENT_GOT_HITMONLEE", 854), ("RED", "EVENT_GOT_HITMONCHAN", 855),
        ("RED", "EVENT_MET_BILL", 1360), ("RED", "EVENT_RESCUED_MR_FUJI", 1231),
        ("RED", "EVENT_GAVE_GOLD_TEETH", 569), ("RED", "EVENT_FOUND_ROCKET_HIDEOUT", 441),
        ("RED", "EVENT_BEAT_BROCK", 119), ("RED", "EVENT_BEAT_MISTY", 191),
        ("RED", "EVENT_BEAT_LT_SURGE", 359), ("RED", "EVENT_BEAT_ERIKA", 425),
        ("RED", "EVENT_BEAT_KOGA", 601), ("RED", "EVENT_BEAT_SABRINA", 865),
        ("RED", "EVENT_BEAT_BLAINE", 665),
        ("RED", "EVENT_BEAT_VIRIDIAN_GYM_GIOVANNI", 81),
        ("RED", "EVENT_BEAT_ROCKET_HIDEOUT_GIOVANNI", 1703),
        ("RED", "EVENT_BEAT_SILPH_CO_GIOVANNI", 1935),
        ("RED", "EVENT_BEAT_ARTICUNO", 2522), ("RED", "EVENT_BEAT_ZAPDOS", 1129),
        ("RED", "EVENT_BEAT_MOLTRES", 1342), ("RED", "EVENT_BEAT_MEWTWO", 2241),
        ("RED", "EVENT_BEAT_LORELEIS_ROOM_TRAINER_0", 2273),
        ("RED", "EVENT_BEAT_BRUNOS_ROOM_TRAINER_0", 2281),
        ("RED", "EVENT_BEAT_AGATHAS_ROOM_TRAINER_0", 2289),
        ("RED", "EVENT_BEAT_LANCES_ROOM_TRAINER_0", 2297),
        ("RED", "EVENT_BEAT_LANCE", 2302), ("RED", "EVENT_BEAT_CHAMPION_RIVAL", 2305),
        ("YELLOW", "EVENT_GOT_DOME_FOSSIL", 1400),
        ("GS", "EVENT_GOT_HM01_CUT", 16), ("GS", "EVENT_GOT_HM06_WHIRLPOOL", 21),
        ("GS", "EVENT_GOT_HM07_WATERFALL", 1672),
        ("GS", "EVENT_GOT_A_POKEMON_FROM_ELM", 26), ("GS", "EVENT_GOT_BICYCLE", 91),
        ("GS", "EVENT_GOT_OLD_ROD", 23), ("GS", "EVENT_GOT_GOOD_ROD", 24),
        ("GS", "EVENT_GOT_SUPER_ROD", 25), ("GS", "EVENT_GOT_MASTER_BALL_FROM_ELM", 124),
        ("GS", "EVENT_GOT_SQUIRTBOTTLE", 92), ("GS", "EVENT_GOT_RAINBOW_WING", 120),
        ("GS", "EVENT_GOT_SILVER_WING", 121),
        ("GS", "EVENT_GOT_MYSTERY_EGG_FROM_MR_POKEMON", 30),
        ("CRYSTAL", "EVENT_GOT_RAINBOW_WING", 822),
    ]
    bad = []
    for game, name, want in checks:
        got = tables[game].get(name)
        if got != want:
            bad.append(f"{game} {name}: parser says {got}, "
                      f"docs/GEN12-PARITY-DESIGN.md §1.3 says {want}")
    if bad:
        raise AssertionError("event-flag index self-test failed:\n  " + "\n  ".join(bad))
    counts = {"RED": 507, "YELLOW": 522, "GS": 1235, "CRYSTAL": 1332}
    for game, want in counts.items():
        got = len(tables[game])
        if got != want:
            raise AssertionError(
                f"{game}: parsed {got} named flags, §1.3 cites {want} named of "
                f"{{'RED':2560,'YELLOW':2560,'GS':2048,'CRYSTAL':2048}}[game] total -- "
                f"a decomp update or a parser bug changed the count")
    print(f"  flag-index self-test: {len(checks)} cited indices + 4 total-counts OK")


# ============================================================== emit =============
HEADER = """/* GENERATED by tools/gen_gbfields.py -- DO NOT EDIT BY HAND.
 * Regenerate: python3 tools/gen_gbfields.py
 * Git-ignored (like source/learnsets2.c); source/gb_fields_fallback.c's weak symbols
 * cover a clone that never ran the generator. docs/kb/licensing.md: offsets and flag
 * indices are facts read out of the pinned, reference-only decomp checkouts; nothing
 * here is decomp code or prose. BACKLOG #49 P0 (P0 review D1-D5, D10 applied).
 */
"""


def emit_fields_c(symtabs, cells_out):
    """`cells_out` is filled with (field_name, game, addr_info, size) for every present
    cell, so main() can feed it straight to overrun_scan() without a second pass."""
    lines = [HEADER, '#include "gb_fields.h"', ""]
    lines.append("static const struct { uint32_t off; uint16_t len; GbFieldKind kind; } "
                 "k_fields[GBF_FIELD_COUNT][GBF_G_COUNT] = {")
    for name, kind, default_size, per_game in FIELDS:
        row = []
        for game in GAMES:
            off, size, addr_info = resolve_field(symtabs, game, per_game[game], default_size)
            if off is None:
                row.append("{0, 0, GBFK_U8}")
            else:
                row.append(f"{{{off:#06x}, {size}, {kind}}}")
                cells_out.append((name, game, addr_info, size))
        lines.append(f"  [GBF_{name}] = {{ {', '.join(row)} }},")
    lines.append("};")
    lines.append("")
    lines.append('static const char* const k_field_names[GBF_FIELD_COUNT] = {')
    for name, _, _, _ in FIELDS:
        lines.append(f'  [GBF_{name}] = "{name}",')
    lines.append("};")
    lines.append("")
    lines.append("static int game_idx(GbGame g) { return (g >= 0 && g < GBF_G_COUNT) ? (int)g : -1; }")
    lines.append("static int field_idx(GbField f) { return (f >= 0 && f < GBF_FIELD_COUNT) ? (int)f : -1; }")
    lines.append("")
    lines.append("uint32_t gbf_off(GbGame g, GbField f) {")
    lines.append("  int gi = game_idx(g), fi = field_idx(f);")
    lines.append("  return (gi < 0 || fi < 0) ? 0 : k_fields[fi][gi].off;")
    lines.append("}")
    lines.append("uint16_t gbf_len(GbGame g, GbField f) {")
    lines.append("  int gi = game_idx(g), fi = field_idx(f);")
    lines.append("  return (gi < 0 || fi < 0) ? 0 : k_fields[fi][gi].len;")
    lines.append("}")
    lines.append("GbFieldKind gbf_kind(GbGame g, GbField f) {")
    lines.append("  int gi = game_idx(g), fi = field_idx(f);")
    lines.append("  return (gi < 0 || fi < 0) ? GBFK_U8 : k_fields[fi][gi].kind;")
    lines.append("}")
    lines.append("const char* gbf_field_name(GbField f) {")
    lines.append("  int fi = field_idx(f);")
    lines.append('  return fi < 0 ? "?" : k_field_names[fi];')
    lines.append("}")
    lines.append("")
    with open(FIELDS_OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def emit_flags_c(tables):
    lines = [HEADER, '#include "gb_flags.h"', ""]
    shortlists = {"RED": SHORTLIST_GEN1, "YELLOW": SHORTLIST_GEN1,
                  "GS": SHORTLIST_GEN2, "CRYSTAL": SHORTLIST_GEN2}
    for game in GAMES:
        rows = []
        for sym in shortlists[game]:
            idx = tables[game].get(sym)
            if idx is None:
                continue   # a Gen-1-only or Yellow-only name absent from this game
            label = LABELS.get(sym, sym)
            rows.append((idx, label))
        rows.sort()
        lines.append(f"static const GbFlagEntry k_flags_{game.lower()}[] = {{")
        for idx, label in rows:
            esc = label.replace('"', '\\"')
            lines.append(f'  {{ {idx}, "{esc}" }},')
        lines.append("};")
        lines.append(f"#define GBFL_{game}_COUNT "
                     f"((int)(sizeof k_flags_{game.lower()} / sizeof k_flags_{game.lower()}[0]))")
        lines.append("")
    lines.append("static const GbFlagEntry* k_tables[GBF_G_COUNT] = {")
    for game in GAMES:
        lines.append(f"  [GBF_G_{game}] = k_flags_{game.lower()},")
    lines.append("};")
    lines.append("static const int k_counts[GBF_G_COUNT] = {")
    for game in GAMES:
        lines.append(f"  [GBF_G_{game}] = GBFL_{game}_COUNT,")
    lines.append("};")
    lines.append("")
    lines.append("int gbfl_count(GbGame g) {")
    lines.append("  return (g >= 0 && g < GBF_G_COUNT) ? k_counts[g] : 0;")
    lines.append("}")
    lines.append("const char* gbfl_name(GbGame g, uint16_t flag) {")
    lines.append("  if (g < 0 || g >= GBF_G_COUNT) return 0;")
    lines.append("  for (int i = 0; i < k_counts[g]; i++)")
    lines.append("    if (k_tables[g][i].index == flag) return k_tables[g][i].label;")
    lines.append("  return 0;")
    lines.append("}")
    lines.append("bool gbfl_at(GbGame g, int i, uint16_t* flag_out, const char** label_out) {")
    lines.append("  if (g < 0 || g >= GBF_G_COUNT || i < 0 || i >= k_counts[g]) return false;")
    lines.append("  if (flag_out) *flag_out = k_tables[g][i].index;")
    lines.append("  if (label_out) *label_out = k_tables[g][i].label;")
    lines.append("  return true;")
    lines.append("}")
    lines.append("")
    with open(FLAGS_OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    symtabs = {g: load_sym(SYM_PATHS[g]) for g in GAMES}
    for g in GAMES:
        for region in REGIONS[g].values():
            if region.start_symbol not in symtabs[g]:
                print(f"ERROR: {g}: region symbol {region.start_symbol!r} not found "
                     f"in {SYM_PATHS[g]}", file=sys.stderr)
                sys.exit(1)

    _selftest_overrun_scan(symtabs)

    cells = []
    emit_fields_c(symtabs, cells)
    problems = overrun_scan(symtabs, cells)
    if problems:
        print("ERROR: overrun scan found field(s) whose claimed size reaches another "
             "symbol:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        sys.exit(1)

    n_derived = sum(1 for _, _, _, pg in FIELDS for game in GAMES
                    if isinstance(pg[game], tuple))
    n_total = sum(1 for _, _, _, pg in FIELDS for game in GAMES if pg[game] is not ABSENT)
    print(f"wrote {FIELDS_OUT}: {len(FIELDS)} fields x {len(GAMES)} games, "
         f"{n_total} present, {n_derived} derived+cross-checked against a live .sym "
         f"lookup, {n_total - n_derived} bare literal(s), overrun scan clean "
         f"({len(cells)} symbol-backed cells checked)")

    const_tables = {g: parse_const_file(EVENT_CONST_PATHS[g]) for g in GAMES}
    _selftest_flag_indices(const_tables)
    emit_flags_c(const_tables)
    print(f"wrote {FLAGS_OUT}: shortlist of {len(SHORTLIST_GEN1)} Gen-1 / "
         f"{len(SHORTLIST_GEN2)} Gen-2 flag names, per-game indices from a live "
         f"const_def/const_skip/const_next replay of each game's own event-constants file")


if __name__ == "__main__":
    main()
