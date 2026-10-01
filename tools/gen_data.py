#!/usr/bin/env python3
"""Generate source/data_tables.c (Gen-3 lookup tables BY NUMBER) from the
pokeemerald decomp data files under reference/pokeemerald_data/ (git-ignored,
fetched separately). Matches the API in source/data_tables.h.

Output source/data_tables.c is git-ignored (generate-locally policy). Run from
the repo root:  python3 tools/gen_data.py

TRAP (found in review, 2026-09-05): sp_national (internal species id -> national
dex number) is read from assets/sprites/Gen 3 Sprite Pack V1/PBS/pokemon_metrics.txt
(PBS_PATH below). If that file is absent, the `except FileNotFoundError: pass`
there does NOT fail the run -- it silently leaves pbs_nat empty, and every species
with an internal id > 251 (the "displaced legendaries" the code comment a few lines
down warns about, e.g. Kyogre at internal 404) falls back to national=0 instead of
its real dex number. This does not crash or warn; data_tables.c generates fine and
looks plausible. Sanity-check after any regeneration:
    grep -c '0,0,0,0,0,252,253,254' source/data_tables.c
should print exactly 1 (one legitimate run of zeros in s_national, not a mass
zeroing from a missing PBS file) -- if it's higher, or the file is missing entirely,
the sprite pack didn't get picked up and sp_national is wrong for every displaced
legendary.
"""
import os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RD = os.path.join(ROOT, "reference", "pokeemerald_data")
OUT = os.path.join(ROOT, "source", "data_tables.c")
# BACKLOG #19 / docs/AUDIT-2026-09-05-backlog-3-19.md section B: the 741 verbatim
# item/move/ability description strings live in their OWN generated, git-ignored
# file so PDNA_ARTLESS=1 can drop them the way it drops the ripped art modules --
# see source/desc_gate.h and source/data_desc_shim.c.
OUT_DESC = os.path.join(ROOT, "source", "data_desc.c")

MAX_SPECIES = 411


def rd(path):
    with open(os.path.join(RD, path), encoding="utf-8", errors="replace") as f:
        return f.read()


def eval_int(expr, env):
    expr = re.sub(r"//.*$", "", expr).strip().rstrip(",").strip()
    try:
        return int(expr, 0)
    except ValueError:
        pass
    # substitute known identifiers, then evaluate a simple arithmetic expression
    def sub(m):
        k = m.group(0)
        return str(env[k]) if k in env else k
    e = re.sub(r"[A-Za-z_]\w*", sub, expr)
    if re.fullmatch(r"[0-9xXa-fA-F+\-*/() ]+", e or ""):
        try:
            return int(eval(e))
        except Exception:
            return None
    return None


def parse_defines(text, prefix):
    out = {}
    for line in text.splitlines():
        m = re.match(r"\s*#define\s+(" + prefix + r"\w+)\s+(.+)$", line)
        if not m:
            continue
        v = eval_int(m.group(2), out)
        if v is not None:
            out[m.group(1)] = v
    return out


def parse_named_array(text, const_map):
    """[CONST] = _("NAME") -> {id: name}."""
    out = {}
    for m in re.finditer(r'\[(\w+)\]\s*=\s*_\("((?:[^"\\]|\\.)*)"\)', text):
        cid = const_map.get(m.group(1))
        if cid is not None:
            out[cid] = m.group(2)
    return out


def iter_blocks(text):
    """Yield (CONST, body) for `[CONST] = { ... }` initializers."""
    for m in re.finditer(r"\[(\w+)\]\s*=\s*\{", text):
        i = m.end()
        depth = 1
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        yield m.group(1), text[m.end():i - 1]


def field(body, name):
    m = re.search(r"\." + name + r"\s*=\s*([^,\n}]+)", body)
    return m.group(1).strip() if m else None


def cstr(s):
    s = s.replace("{POKEBLOCK}", "POK\u00e9BLOCK")   # the ligature macro; never print the raw token (#334)
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def prettify(const, *strip):
    s = const
    for p in strip:
        if s.startswith(p):
            s = s[len(p):]
    return s.replace("_", " ")


# ---- constant maps ----
SPEC = parse_defines(rd("include/constants/species.h"), "SPECIES_")
MOVE = parse_defines(rd("include/constants/moves.h"), "MOVE_")
ITEM = parse_defines(rd("include/constants/items.h"), "ITEM_")
ABIL = parse_defines(rd("include/constants/abilities.h"), "ABILITY_")
PKMN = rd("include/constants/pokemon.h")
TYPE = {k: v for k, v in parse_defines(PKMN, "TYPE_").items() if v < 64}  # drop sentinels (TYPE_NONE=255)
GROWTH = parse_defines(PKMN, "GROWTH_")
GENDER = parse_defines(PKMN, "MON_")          # MON_MALE/FEMALE/GENDERLESS
# Contest categories aren't a simple #define list in the decomp; hardcode the
# standard Gen-3 order (COOL, BEAUTY, CUTE, SMART, TOUGH).
CONTEST = {"CONTEST_CATEGORY_COOL": 0, "CONTEST_CATEGORY_BEAUTY": 1,
           "CONTEST_CATEGORY_CUTE": 2, "CONTEST_CATEGORY_SMART": 3,
           "CONTEST_CATEGORY_TOUGH": 4}
MAPSEC = parse_defines(rd("include/constants/region_map_sections.h"), "MAPSEC_")

inv_type = {v: k for k, v in TYPE.items()}
inv_contest = {v: k for k, v in CONTEST.items()}


def gender_val(tok):
    tok = tok.strip()
    if tok in GENDER:
        return GENDER[tok]
    m = re.match(r"PERCENT_FEMALE\(([\d.]+)\)", tok)
    if m:
        return int(float(m.group(1)) * 255 / 100)
    try:
        return int(tok, 0)
    except ValueError:
        return 0


# ---- species data (indexed by internal id) ----
sp_name = parse_named_array(rd("src/data/text/species_names.h"), SPEC)
sp_base = [[0] * 6 for _ in range(MAX_SPECIES + 1)]
sp_t1 = [0] * (MAX_SPECIES + 1)
sp_t2 = [0] * (MAX_SPECIES + 1)
sp_a0 = [0] * (MAX_SPECIES + 1)
sp_a1 = [0] * (MAX_SPECIES + 1)
sp_gr = [0xFF] * (MAX_SPECIES + 1)
sp_growth = [0] * (MAX_SPECIES + 1)
for const, body in iter_blocks(rd("src/data/pokemon/base_stats.h")):
    sid = SPEC.get(const)
    if sid is None or sid > MAX_SPECIES:
        continue
    for i, f in enumerate(["baseHP", "baseAttack", "baseDefense", "baseSpeed", "baseSpAttack", "baseSpDefense"]):
        v = field(body, f)
        sp_base[sid][i] = int(v, 0) if v else 0
    sp_t1[sid] = TYPE.get((field(body, "type1") or "TYPE_NORMAL").strip(), 0)
    sp_t2[sid] = TYPE.get((field(body, "type2") or "TYPE_NORMAL").strip(), 0)
    g = field(body, "genderRatio")
    if g:
        sp_gr[sid] = gender_val(g)
    gr = field(body, "growthRate")
    if gr:
        sp_growth[sid] = GROWTH.get(gr.strip(), 0)
    ab = re.search(r"\.abilities\s*=\s*\{([^}]*)\}", body)
    if ab:
        parts = [p.strip() for p in ab.group(1).split(",")]
        sp_a0[sid] = ABIL.get(parts[0], 0) if len(parts) > 0 else 0
        sp_a1[sid] = ABIL.get(parts[1], 0) if len(parts) > 1 else 0

# ---- internal -> national dex number (exact; the +25 shortcut mismaps the
#      displaced legendaries, e.g. Kyogre 404->382 not 379) ----
# See the TRAP note in this file's module docstring: a missing PBS_PATH does NOT
# fail this script, it silently zeroes sp_national for every id > 251 below.
PBS_PATH = os.path.join(ROOT, "assets", "sprites", "Gen 3 Sprite Pack V1", "PBS", "pokemon_metrics.txt")
pbs_nat = {}
try:
    cur = None
    with open(PBS_PATH, encoding="utf-8-sig") as pf:
        for line in pf:
            s = line.strip()
            mm = re.match(r"^\[(.+)\]$", s)
            if mm:
                cur = re.sub(r"[^A-Z0-9]", "", mm.group(1).upper())
                continue
            mm = re.match(r"^#(\d+)", s)
            if mm and cur:
                pbs_nat[cur] = int(mm.group(1))
                cur = None
except FileNotFoundError:
    pass
pbs_nat.setdefault("NIDORANF", pbs_nat.get("NIDORANFE", 29))
pbs_nat.setdefault("NIDORANM", pbs_nat.get("NIDORANMA", 32))
sp_national = [0] * (MAX_SPECIES + 1)
for cname, iid in SPEC.items():
    if iid <= MAX_SPECIES:
        key = re.sub(r"[^A-Z0-9]", "", cname[len("SPECIES_"):].upper())
        sp_national[iid] = pbs_nat.get(key, (iid if iid <= 251 else 0))

# ---- moves ----
mv_name = parse_named_array(rd("src/data/text/move_names.h"), MOVE)
NMOVE = (max(MOVE.values()) + 1) if MOVE else 355
mv_type = [0] * NMOVE
mv_pp = [0] * NMOVE
mv_power = [0] * NMOVE
mv_acc = [0] * NMOVE
for const, body in iter_blocks(rd("src/data/battle_moves.h")):
    mid = MOVE.get(const)
    if mid is None or mid >= NMOVE:
        continue
    mv_type[mid] = TYPE.get((field(body, "type") or "TYPE_NORMAL").strip(), 0)
    pp = field(body, "pp")
    mv_pp[mid] = int(pp, 0) if pp else 0
    pw = field(body, "power")
    mv_power[mid] = int(pw, 0) if pw else 0
    ac = field(body, "accuracy")
    mv_acc[mid] = int(ac, 0) if ac else 0
mv_contest = [0] * NMOVE
for const, body in iter_blocks(rd("src/data/contest_moves.h")):
    mid = MOVE.get(const)
    if mid is None or mid >= NMOVE:
        continue
    cc = field(body, "contestCategory")
    if cc:
        mv_contest[mid] = CONTEST.get(cc.strip(), 0)

# ---- move descriptions (text -> var -> move id via the pointer table) ----
mv_desc = {}
_dsrc = rd("src/data/text/move_descriptions.h")
_dtext = {}
for _m in re.finditer(r"static const u8 (\w+)\[\]\s*=\s*_\((.*?)\);", _dsrc, re.S):
    _parts = re.findall(r'"([^"]*)"', _m.group(2))
    _dtext[_m.group(1)] = "".join(_parts).replace("\\n", " ").replace("\\p", " ").replace("\\l", " ").strip()
for _m in re.finditer(r"\[MOVE_(\w+)\s*-\s*1\]\s*=\s*(\w+)", _dsrc):
    _mid = MOVE.get("MOVE_" + _m.group(1))
    if _mid is not None and _mid < NMOVE:
        mv_desc[_mid] = _dtext.get(_m.group(2), "")

# ---- items (names + descriptions) ----
it_name = {}
it_desc = {}
_idsrc = rd("src/data/text/item_descriptions.h")
_idtext = {}
for _m in re.finditer(r"static const u8 (\w+)\[\]\s*=\s*_\((.*?)\);", _idsrc, re.S):
    _parts = re.findall(r'"([^"]*)"', _m.group(2))
    _idtext[_m.group(1)] = "".join(_parts).replace("\\n", " ").replace("\\p", " ").replace("\\l", " ").strip()
for const, body in iter_blocks(rd("src/data/items.h")):
    iid = ITEM.get(const)
    if iid is None:
        continue
    m = re.search(r'\.name\s*=\s*_\("((?:[^"\\]|\\.)*)"\)', body)
    if m:
        it_name[iid] = m.group(1)
    dm = re.search(r"\.description\s*=\s*(\w+)", body)
    if dm and dm.group(1) in _idtext:
        it_desc[iid] = _idtext[dm.group(1)]
NITEM = (max(ITEM.values()) + 1) if ITEM else 400

# ---- per-item game-availability mask (bit0 RS, bit1 Emerald, bit2 FRLG) ----
# Sources: each game's OWN include/constants/items.h — Emerald from the
# reference data set, Ruby + FireRed from the vendored decomps under
# assets/upstream/. The Gen-3 item-id space is SHARED across all five carts,
# so ids never need remapping: RS's table simply ends at 348 (ITEMS_COUNT 349,
# pokeruby include/constants/items.h — no FRLG key items/teas), FRLG's at 374
# (ITEMS_COUNT 375 — no Emerald-only Magma Emblem 375 / Old Sea Map 376).
# An id counts as PRESENT when the game gives it a real constant name; the
# decomps name every empty table slot with a hex placeholder (ITEM_034 /
# ITEM_10B / ITEM_15B style), which we drop.
# CAVEAT: pokefirered's constants still NAME the Hoenn-era items FRLG's table
# inherited from RS (pokefirered include/constants/items.h:50 ITEM_SHOAL_SALT
# .. :311 ITEM_DEVON_SCOPE all keep real names), but those items do not exist
# in FRLG — curated exclusion list below (Shoal salt/shell, contest scarves,
# Pokeblock Case + the Hoenn key items, HM08 Dive).
UPSTREAM = os.path.join(ROOT, "assets", "upstream")


def rd_abs(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


ITEM_R = parse_defines(rd_abs(os.path.join(UPSTREAM, "pokeruby", "include", "constants", "items.h")), "ITEM_")
ITEM_F = parse_defines(rd_abs(os.path.join(UPSTREAM, "pokefirered", "include", "constants", "items.h")), "ITEM_")

_ITEM_PLACEHOLDER = re.compile(r"ITEM_[0-9A-F]{1,3}$")   # empty-slot names: ITEM_034 / ITEM_10B / ITEM_15B


def real_item_ids(defs, maxid):
    return set(v for k, v in defs.items()
               if isinstance(v, int) and 0 < v <= maxid
               and not _ITEM_PLACEHOLDER.match(k)
               and k not in ("ITEMS_COUNT", "ITEM_FIELD_ARROW"))


# Hoenn-only items whose ids pokefirered's shared table still names but which
# don't exist in FRLG (each verified present in pokeruby's constants at the
# same id — pokeruby include/constants/items.h:54-55 Shoal, :264-268 scarves,
# :271-311 the RS key-item block 259..288, :349 ITEM_HM08 346 = Dive, FRLG
# has HM01-07 only).
FRLG_HOENN_ONLY = [
    "ITEM_SHOAL_SALT", "ITEM_SHOAL_SHELL",
    "ITEM_RED_SCARF", "ITEM_BLUE_SCARF", "ITEM_PINK_SCARF",
    "ITEM_GREEN_SCARF", "ITEM_YELLOW_SCARF",
    "ITEM_MACH_BIKE", "ITEM_CONTEST_PASS", "ITEM_WAILMER_PAIL",
    "ITEM_DEVON_GOODS", "ITEM_SOOT_SACK", "ITEM_BASEMENT_KEY",
    "ITEM_ACRO_BIKE", "ITEM_POKEBLOCK_CASE", "ITEM_LETTER",
    "ITEM_EON_TICKET", "ITEM_RED_ORB", "ITEM_BLUE_ORB", "ITEM_SCANNER",
    "ITEM_GO_GOGGLES", "ITEM_METEORITE", "ITEM_ROOM_1_KEY",
    "ITEM_ROOM_2_KEY", "ITEM_ROOM_4_KEY", "ITEM_ROOM_6_KEY",
    "ITEM_STORAGE_KEY", "ITEM_ROOT_FOSSIL", "ITEM_CLAW_FOSSIL",
    "ITEM_DEVON_SCOPE", "ITEM_HM08",
]

rs_ids = real_item_ids(ITEM_R, 348)         # pokeruby ITEMS_COUNT 349
e_ids = real_item_ids(ITEM, 376)            # pokeemerald ITEMS_COUNT 377
f_ids = real_item_ids(ITEM_F, 374)          # pokefirered ITEMS_COUNT 375
for _sym in FRLG_HOENN_ONLY:
    f_ids.discard(ITEM_F.get(_sym))

it_games = [(1 if i in rs_ids else 0) | (2 if i in e_ids else 0) | (4 if i in f_ids else 0)
            for i in range(NITEM)]

# sanity: the era differences BACKLOG #13 calls out must hold
assert it_games[ITEM["ITEM_POTION"]] == 7, "Potion should be in all games"
assert it_games[ITEM["ITEM_MASTER_BALL"]] == 7
assert it_games[ITEM["ITEM_TEA"]] == 6, "Tea = FRLG (+Emerald table), not RS"
assert it_games[ITEM["ITEM_OAKS_PARCEL"]] == 6, "FRLG key item, not RS"
assert it_games[ITEM["ITEM_VS_SEEKER"]] == 6
assert it_games[ITEM["ITEM_POKEBLOCK_CASE"]] == 3, "contest/pokeblock = RS+E, not FRLG"
assert it_games[ITEM["ITEM_RED_SCARF"]] == 3
assert it_games[ITEM["ITEM_SHOAL_SALT"]] == 3
assert it_games[ITEM["ITEM_ACRO_BIKE"]] == 3
assert it_games[ITEM["ITEM_MAGMA_EMBLEM"]] == 2, "Emerald-only"
assert it_games[ITEM["ITEM_OLD_SEA_MAP"]] == 2, "Emerald-only"
assert it_games[ITEM["ITEM_HM08"]] == 3, "Dive: RS+E, FRLG has HM01-07 only"
assert it_games[ITEM["ITEM_HM07"]] == 7
assert it_games[ITEM["ITEM_CHERI_BERRY"]] == 7
assert it_games[ITEM["ITEM_034"]] == 0, "placeholder slot in every game"
assert it_games[0] == 0, "ITEM_NONE"

# ---- TM/HM -> move ---------------------------------------------------------
# Items 289..346 are named TM01..TM50 / HM01..HM08 and nothing else: that IS the
# item's own name in every Gen-3 ROM. The games append the move at DISPLAY time —
# retail Emerald's bag prints "No01 FOCUS PUNCH", and pokefirered's TM case does
# it explicitly (src/tm_case.c:696, StringAppend of gMoveNames[ItemIdToBattleMoveId]).
# A bare "TM26" tells the player nothing, so we do the same.
#
# TWO INDEPENDENT SOURCES, and they must agree or this build fails:
#   (a) the item constants themselves — every TM has an ALIAS whose name carries the
#       move, e.g. `#define ITEM_TM01_FOCUS_PUNCH ITEM_TM01`;
#   (b) pokefirered's own sTMHMMoves[] array (src/data/party_menu.h), in TM order.
# The mapping is identical across all five Gen-3 carts (same 50 TMs + 8 HMs).
# It is generated and cross-checked precisely because an earlier hand-written
# attempt was wrong — a mislabelled TM is worse than an unlabelled one.
TMHM_FIRST, TMHM_COUNT = ITEM["ITEM_TM01"], 58
assert ITEM["ITEM_HM01"] == TMHM_FIRST + 50 and ITEM["ITEM_HM08"] == TMHM_FIRST + 57

_alias = {}
for _m in re.finditer(r"^#define ITEM_((?:TM|HM)\d{2})_(\w+)\s+ITEM_(?:TM|HM)\d{2}\s*$",
                      rd("include/constants/items.h"), re.M):
    _slot, _mv = _m.group(1), _m.group(2)
    _base = ITEM.get("ITEM_" + _slot)
    _mid = MOVE.get("MOVE_" + _mv)
    assert _base is not None and _mid, "unmapped TM alias %s_%s" % (_slot, _mv)
    _alias[_base] = _mid
assert len(_alias) == TMHM_COUNT, "expected %d TM/HM aliases, found %d" % (TMHM_COUNT, len(_alias))

# (b) the array, in order, from the vendored FireRed decomp
_pm = rd_abs(os.path.join(UPSTREAM, "pokefirered", "src", "data", "party_menu.h"))
_arr = re.search(r"static const u16 sTMHMMoves\[\]\s*=\s*\{(.*?)\};", _pm, re.S)
assert _arr, "sTMHMMoves not found in the vendored pokefirered decomp"
_order = [MOVE[s] for s in re.findall(r"MOVE_\w+", _arr.group(1))]
assert len(_order) == TMHM_COUNT, "sTMHMMoves has %d entries, expected %d" % (len(_order), TMHM_COUNT)

tmhm = [0] * TMHM_COUNT
for _i in range(TMHM_COUNT):
    _from_alias = _alias[TMHM_FIRST + _i]
    assert _from_alias == _order[_i], (
        "TM/HM %d disagrees: item alias says move %d, pokefirered sTMHMMoves says %d"
        % (_i + 1, _from_alias, _order[_i]))
    tmhm[_i] = _from_alias

# spot-checks against what the running game prints (retail Emerald bag, TMs pocket)
assert mv_name[tmhm[0]] == "FOCUS PUNCH", mv_name[tmhm[0]]
assert mv_name[tmhm[1]] == "DRAGON CLAW"
assert mv_name[tmhm[2]] == "WATER PULSE"
assert mv_name[tmhm[5]] == "TOXIC"
assert mv_name[tmhm[49]] == "OVERHEAT", "TM50"
assert mv_name[tmhm[50]] == "CUT", "HM01"
assert mv_name[tmhm[57]] == "DIVE", "HM08"

# ---- abilities (names + descriptions) ----
abtext = rd("src/data/text/abilities.h")
ab_name = parse_named_array(abtext, ABIL)
desc_vars = {m.group(1): m.group(2) for m in
             re.finditer(r'static const u8 (\w+)\[\]\s*=\s*_\("((?:[^"\\]|\\.)*)"\)', abtext)}
ab_desc = {}
for m in re.finditer(r"\[(ABILITY_\w+)\]\s*=\s*(\w+)", abtext):
    aid = ABIL.get(m.group(1))
    if aid is not None and m.group(2) in desc_vars:
        ab_desc[aid] = desc_vars[m.group(2)]
NABIL = (max(ABIL.values()) + 1) if ABIL else 78

# ---- game stats (counter names) ----
GS = parse_defines(rd("include/constants/game_stat.h"), "GAME_STAT_")
gs_name = {}
for k, idx in GS.items():
    if not isinstance(idx, int) or idx < 0 or idx >= 64:
        continue
    nm = k[len("GAME_STAT_"):].replace("_", " ").lower()
    gs_name[idx] = nm[:1].upper() + nm[1:]
NGS = 64

# ---- named event flags (per-game curated allowlist) ----
# Flags differ per game (number AND meaning), so we resolve a small curated set of
# symbolic FLAG_* names against each decomp's constants/flags.h. Values are exprs
# like (SYSTEM_FLAGS + 0x7); the base constants chain into trainer-count constants
# defined in OTHER files, but their resolved values are canonical and well-known,
# so we pre-seed them and self-check the badge result. ASCII display names only.
NFLAG_BASE = {  # game -> {base const name: value}  (Emerald 0x860, FRLG/RS 0x800)
    "pokeemerald_data": {"SYSTEM_FLAGS": 0x860},
    "pokefirered_data": {"SYS_FLAGS": 0x800, "SYSTEM_FLAGS": 0x800},
    "pokeruby_data":    {"SYSTEM_FLAGS": 0x800},
}
NFLAG_BADGE1_EXPECT = {"pokeemerald_data": 0x867, "pokefirered_data": 0x820, "pokeruby_data": 0x807}
BADGES_HOENN = ["Stone Badge", "Knuckle Badge", "Dynamo Badge", "Heat Badge",
                "Balance Badge", "Feather Badge", "Mind Badge", "Rain Badge"]
BADGES_KANTO = ["Boulder Badge", "Cascade Badge", "Thunder Badge", "Rainbow Badge",
                "Soul Badge", "Marsh Badge", "Volcano Badge", "Earth Badge"]
# (symbol, display) lists per category — each resolved per game, silently skipped
# where the flag is absent (so the Hoenn-only / Kanto-only names just drop out).
NFLAG_SYSTEM = [
    ("FLAG_SYS_POKEDEX_GET",  "Pokedex obtained"),
    ("FLAG_SYS_NATIONAL_DEX", "National Dex"),
    ("FLAG_SYS_POKENAV_GET",  "PokeNav (RSE)"),
    ("FLAG_SYS_GAME_CLEAR",   "Game cleared (HoF)"),
    ("FLAG_SYS_B_DASH",       "Running Shoes"),
    ("FLAG_SYS_POKEMON_GET",  "First Pokemon"),
    ("FLAG_SYS_RIBBON_GET",   "First Ribbon"),
]
# Fly destinations. ONE bit per town unlocks it on the region map's Fly cursor —
# there is no heal-location record or derived state involved. Hoenn (RSE) uses
# FLAG_VISITED_*, laid out as a contiguous run parallel to the MAPSEC ids; Kanto
# (FRLG) uses a COMPLETELY DIFFERENT family, FLAG_WORLD_MAP_*, so this is not
# "Emerald minus 0x60". add_category drops symbols a game doesn't have, so the
# combined Hoenn+Kanto list resolves correctly per game with no branching.
# Traps encoded here:
#  - Emerald's 17th destination (Battle Frontier) is a LANDMARK flag, far from the
#    contiguous visited run — a "16 towns" list silently misses it;
#  - FRLG lists SEVEN ISLAND *before* SIX ISLAND, in both the flags and the MAPSECs;
#  - FRLG's FLAG_WORLD_MAP_* run STOPS at ROUTE10 (0xA3). 0xA4+ are dungeon
#    map-preview flags, not Fly destinations — listing them yields ~30 dead rows;
#  - the Sevii world-map flags are inert unless the two SEVII_MAP prereqs are set.
NFLAG_FLY = [
    # --- Hoenn (Emerald + Ruby/Sapphire), in MAPSEC order 0x00..0x0F ---
    ("FLAG_VISITED_LITTLEROOT_TOWN", "Littleroot Town"),
    ("FLAG_VISITED_OLDALE_TOWN",     "Oldale Town"),
    ("FLAG_VISITED_DEWFORD_TOWN",    "Dewford Town"),
    ("FLAG_VISITED_LAVARIDGE_TOWN",  "Lavaridge Town"),
    ("FLAG_VISITED_FALLARBOR_TOWN",  "Fallarbor Town"),
    ("FLAG_VISITED_VERDANTURF_TOWN", "Verdanturf Town"),
    ("FLAG_VISITED_PACIFIDLOG_TOWN", "Pacifidlog Town"),
    ("FLAG_VISITED_PETALBURG_CITY",  "Petalburg City"),
    ("FLAG_VISITED_SLATEPORT_CITY",  "Slateport City"),
    ("FLAG_VISITED_MAUVILLE_CITY",   "Mauville City"),
    ("FLAG_VISITED_RUSTBORO_CITY",   "Rustboro City"),
    ("FLAG_VISITED_FORTREE_CITY",    "Fortree City"),
    ("FLAG_VISITED_LILYCOVE_CITY",   "Lilycove City"),
    ("FLAG_VISITED_MOSSDEEP_CITY",   "Mossdeep City"),
    ("FLAG_VISITED_SOOTOPOLIS_CITY", "Sootopolis City"),
    ("FLAG_VISITED_EVER_GRANDE_CITY", "Ever Grande City"),
    ("FLAG_LANDMARK_BATTLE_FRONTIER", "Battle Frontier"),      # Emerald: 17th fly dest
    ("FLAG_LANDMARK_BATTLE_TOWER",    "Battle Tower"),         # R/S equivalent
    ("FLAG_LANDMARK_POKEMON_LEAGUE",  "Pokemon League"),       # Emerald: cursor only
    ("FLAG_SYS_POKEMON_LEAGUE_FLY",   "Pokemon League"),       # R/S: cursor only
    # --- Kanto + Sevii (FireRed/LeafGreen), in flag order 0x90..0xA3 ---
    ("FLAG_WORLD_MAP_PALLET_TOWN",              "Pallet Town"),
    ("FLAG_WORLD_MAP_VIRIDIAN_CITY",            "Viridian City"),
    ("FLAG_WORLD_MAP_PEWTER_CITY",              "Pewter City"),
    ("FLAG_WORLD_MAP_CERULEAN_CITY",            "Cerulean City"),
    ("FLAG_WORLD_MAP_LAVENDER_TOWN",            "Lavender Town"),
    ("FLAG_WORLD_MAP_VERMILION_CITY",           "Vermilion City"),
    ("FLAG_WORLD_MAP_CELADON_CITY",             "Celadon City"),
    ("FLAG_WORLD_MAP_FUCHSIA_CITY",             "Fuchsia City"),
    ("FLAG_WORLD_MAP_CINNABAR_ISLAND",          "Cinnabar Island"),
    ("FLAG_WORLD_MAP_INDIGO_PLATEAU_EXTERIOR",  "Indigo Plateau"),
    ("FLAG_WORLD_MAP_SAFFRON_CITY",             "Saffron City"),
    ("FLAG_WORLD_MAP_ONE_ISLAND",               "One Island"),
    ("FLAG_WORLD_MAP_TWO_ISLAND",               "Two Island"),
    ("FLAG_WORLD_MAP_THREE_ISLAND",             "Three Island"),
    ("FLAG_WORLD_MAP_FOUR_ISLAND",              "Four Island"),
    ("FLAG_WORLD_MAP_FIVE_ISLAND",              "Five Island"),
    ("FLAG_WORLD_MAP_SEVEN_ISLAND",             "Seven Island"),   # NOTE: before Six
    ("FLAG_WORLD_MAP_SIX_ISLAND",               "Six Island"),
    ("FLAG_WORLD_MAP_ROUTE4_POKEMON_CENTER_1F", "Route 4 Center"),
    ("FLAG_WORLD_MAP_ROUTE10_POKEMON_CENTER_1F", "Route 10 Center"),
    ("FLAG_SYS_SEVII_MAP_123",  "Sevii map 1-2-3 *"),   # * = prereq for the islands
    ("FLAG_SYS_SEVII_MAP_4567", "Sevii map 4-5-6-7 *"),
]
NFLAG_GYMS = [
    # Hoenn (R/S/E) gyms are keyed by location...
    ("FLAG_DEFEATED_RUSTBORO_GYM",   "Rustboro Gym"),
    ("FLAG_DEFEATED_DEWFORD_GYM",    "Dewford Gym"),
    ("FLAG_DEFEATED_MAUVILLE_GYM",   "Mauville Gym"),
    ("FLAG_DEFEATED_LAVARIDGE_GYM",  "Lavaridge Gym"),
    ("FLAG_DEFEATED_PETALBURG_GYM",  "Petalburg Gym"),
    ("FLAG_DEFEATED_FORTREE_GYM",    "Fortree Gym"),
    ("FLAG_DEFEATED_MOSSDEEP_GYM",   "Mossdeep Gym"),
    ("FLAG_DEFEATED_SOOTOPOLIS_GYM", "Sootopolis Gym"),
    # ...Kanto (FR/LG) gyms are keyed by leader.
    ("FLAG_DEFEATED_BROCK",          "Brock"),
    ("FLAG_DEFEATED_MISTY",          "Misty"),
    ("FLAG_DEFEATED_LT_SURGE",       "Lt. Surge"),
    ("FLAG_DEFEATED_ERIKA",          "Erika"),
    ("FLAG_DEFEATED_KOGA",           "Koga"),
    ("FLAG_DEFEATED_SABRINA",        "Sabrina"),
    ("FLAG_DEFEATED_BLAINE",         "Blaine"),
    ("FLAG_DEFEATED_LEADER_GIOVANNI","Giovanni"),
]
NFLAG_ELITE = [
    # The per-member E4 flags are GAME-TRUTH "off" after beating the league: the Hall
    # of Fame script clears all four so the E4 can be rechallenged (pokeemerald
    # hall_of_fame.inc ResetEliteFour; pokeruby identical). The row that reliably says
    # "beat the league" is FLAG_SYS_GAME_CLEAR — surfaced here next to them.
    ("FLAG_SYS_GAME_CLEAR",          "League beaten (HoF)"),
    ("FLAG_DEFEATED_ELITE_4_SIDNEY", "E4 Sidney"),
    ("FLAG_DEFEATED_ELITE_4_SYDNEY", "E4 Sidney"),     # Ruby spelling
    ("FLAG_DEFEATED_ELITE_4_PHOEBE", "E4 Phoebe"),
    ("FLAG_DEFEATED_ELITE_4_GLACIA", "E4 Glacia"),
    ("FLAG_DEFEATED_ELITE_4_DRAKE",  "E4 Drake"),
    ("FLAG_DEFEATED_LORELEI",        "E4 Lorelei"),
    ("FLAG_DEFEATED_BRUNO",          "E4 Bruno"),
    ("FLAG_DEFEATED_AGATHA",         "E4 Agatha"),
    ("FLAG_DEFEATED_LANCE",          "E4 Lance"),
    ("FLAG_DEFEATED_CHAMP",          "Champion"),
]
NFLAG_FRONTIER = [   # Emerald Battle Frontier silver/gold symbols (absent in RS/FRLG)
    ("FLAG_SYS_TOWER_SILVER",   "Tower Silver"),   ("FLAG_SYS_TOWER_GOLD",   "Tower Gold"),
    ("FLAG_SYS_DOME_SILVER",    "Dome Silver"),     ("FLAG_SYS_DOME_GOLD",    "Dome Gold"),
    ("FLAG_SYS_PALACE_SILVER",  "Palace Silver"),   ("FLAG_SYS_PALACE_GOLD",  "Palace Gold"),
    ("FLAG_SYS_ARENA_SILVER",   "Arena Silver"),    ("FLAG_SYS_ARENA_GOLD",   "Arena Gold"),
    ("FLAG_SYS_FACTORY_SILVER", "Factory Silver"),  ("FLAG_SYS_FACTORY_GOLD", "Factory Gold"),
    ("FLAG_SYS_PIKE_SILVER",    "Pike Silver"),      ("FLAG_SYS_PIKE_GOLD",    "Pike Gold"),
    ("FLAG_SYS_PYRAMID_SILVER", "Pyramid Silver"),  ("FLAG_SYS_PYRAMID_GOLD", "Pyramid Gold"),
]
NFLAG_LEGENDS = [
    ("FLAG_DEFEATED_GROUDON",         "Groudon"),
    ("FLAG_DEFEATED_KYOGRE",          "Kyogre"),
    ("FLAG_DEFEATED_RAYQUAZA",        "Rayquaza"),
    ("FLAG_DEFEATED_REGIROCK",        "Regirock"),
    ("FLAG_DEFEATED_REGICE",          "Regice"),
    ("FLAG_DEFEATED_REGISTEEL",       "Registeel"),
    ("FLAG_DEFEATED_LATIAS_OR_LATIOS","Latias/Latios"),
    ("FLAG_DEFEATED_SUDOWOODO",       "Sudowoodo"),
    ("FLAG_DEFEATED_MEW",             "Mew"),
    ("FLAG_DEFEATED_DEOXYS",          "Deoxys"),
    ("FLAG_DEFEATED_HO_OH",           "Ho-Oh"),
    ("FLAG_DEFEATED_LUGIA",           "Lugia"),
]


def parse_all_defines(text, seed):
    """Every `#define NAME expr` resolved incrementally, with `seed` pre-injected
    (for cross-file base constants like SYSTEM_FLAGS)."""
    env = dict(seed)
    for line in text.splitlines():
        m = re.match(r"\s*#define\s+(\w+)\s+(.+)$", line)
        if not m:
            continue
        v = eval_int(m.group(2), env)
        if v is not None:
            env[m.group(1)] = v
    return env


def build_named_flags(game_dir):
    """-> ordered list of (num, display) for one game; num 0xFFFF == category header."""
    path = os.path.join(ROOT, "reference", game_dir, "include", "constants", "flags.h")
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as f:
        env = parse_all_defines(f.read(), NFLAG_BASE.get(game_dir, {}))
    b1 = env.get("FLAG_BADGE01_GET")
    assert b1 == NFLAG_BADGE1_EXPECT[game_dir], \
        "%s: FLAG_BADGE01_GET resolved to %r, expected 0x%X" % (game_dir, b1, NFLAG_BADGE1_EXPECT[game_dir])
    badges = BADGES_KANTO if game_dir == "pokefirered_data" else BADGES_HOENN
    out = [(0xFFFF, "Badges")]
    for i in range(8):
        n = env.get("FLAG_BADGE0%d_GET" % (i + 1))
        if n is not None and n < 0xFFFF:
            out.append((n, badges[i]))

    def add_category(title, rows):
        seen, resolved = set(), []
        for sym, disp in rows:
            v = env.get(sym)
            if v is not None and 0 <= v < 0xFFFF and v not in seen:
                seen.add(v)
                resolved.append((v, disp))
        if resolved:
            out.append((0xFFFF, title))
            out.extend(resolved)

    add_category("Fly destinations", NFLAG_FLY)
    add_category("System", NFLAG_SYSTEM)
    add_category("Gyms", NFLAG_GYMS)
    add_category("Elite Four (reset @HoF)", NFLAG_ELITE)
    add_category("Battle Frontier", NFLAG_FRONTIER)
    add_category("Legends", NFLAG_LEGENDS)

    # ---- comprehensive auto-scan: every meaningful flag, named from its symbol.
    # Story/progress + collectible-pickup flags are the useful ones for an editor;
    # script-local (TEMP), NPC-visibility (HIDE) and unused/sentinel flags are noise.
    flag_count = 6496 if game_dir == "pokeemerald_data" else 6400
    used = set(v for (v, _) in out if v != 0xFFFF)

    def prettify(sym, prefix):
        s = sym[len(prefix):] if sym.startswith(prefix) else sym
        s = s.strip("_").replace("_", " ").title()
        return s[:22] if s else sym

    # pret/pokeruby names most hidden-item flags with hex placeholders
    # (FLAG_HIDDEN_ITEM_1 .. _61), which prettify into bare numbers. The RS
    # hidden-item range (base 0x258) is a verified 1:1 offset match with Emerald's
    # (base 0x1F4; same items in the same order — E only APPENDS E-only ones past
    # RS's end), so borrow Emerald's real names by offset for the placeholders.
    ruby_hidden = {}
    if game_dir == "pokeruby_data":
        epath = os.path.join(ROOT, "reference", "pokeemerald_data", "include", "constants", "flags.h")
        with open(epath, encoding="utf-8", errors="replace") as f:
            eenv = parse_all_defines(f.read(), NFLAG_BASE.get("pokeemerald_data", {}))
        ebase = eenv.get("FLAG_HIDDEN_ITEMS_START", 0x1F4)
        for esym, ev in eenv.items():
            if (esym.startswith("FLAG_HIDDEN_ITEM_") and isinstance(ev, int)
                    and not esym.endswith(("_START", "_END"))):
                ruby_hidden[ev - ebase] = prettify(esym, "FLAG_HIDDEN_ITEM_")

    def add_auto(title, prefix, want=None):
        rows = []
        for sym, val in env.items():
            if not sym.startswith(prefix):                continue
            if not isinstance(val, int):                  continue
            if val < 0 or val >= flag_count or val in used: continue
            if "UNUSED" in sym or "UNKNOWN" in sym or "NEVER" in sym: continue
            if sym.startswith("FLAG_TEMP") or sym.startswith("FLAG_HIDE"): continue
            if sym.endswith("_START") or sym.endswith("_END"): continue
            if want and not want(sym):                    continue
            disp = prettify(sym, prefix)
            if ruby_hidden and re.fullmatch(r"FLAG_HIDDEN_ITEM_[0-9A-F]{1,2}", sym):
                rbase = env.get("FLAG_HIDDEN_ITEMS_START", 0x258)
                disp = ruby_hidden.get(val - rbase, disp)
            rows.append((val, disp))
        seen, uniq = set(), []
        for v, d in sorted(rows):
            if v in seen:
                continue
            seen.add(v); used.add(v); uniq.append((v, d))
        if uniq:
            out.append((0xFFFF, title))
            out.extend(uniq)

    add_auto("Hidden Items", "FLAG_HIDDEN_ITEM")
    add_auto("Item Balls",   "FLAG_ITEM_")
    add_auto("Got Items",    "FLAG_GOT_")
    add_auto("Received",     "FLAG_RECEIVED_")
    add_auto("Trainers",     "FLAG_DEFEATED_")
    add_auto("System (more)", "FLAG_SYS_")
    return out


named_flags = {g: build_named_flags(g) for g in
               ("pokeemerald_data", "pokefirered_data", "pokeruby_data")}

# ---- natures ----
nat_names = re.findall(r'_\("((?:[^"\\]|\\.)*)"\)', rd("src/data/text/nature_names.h"))
nat_boost = [-1] * 25
nat_hinder = [-1] * 25
nb = re.search(r"gNatureStatTable.*?\{(.*?)\n\};", rd("src/pokemon.c"), re.S)
if nb:
    rows = re.findall(r"\[NATURE_\w+\]\s*=\s*\{([^}]*)\}", nb.group(1))
    for i, row in enumerate(rows[:25]):
        vals = [int(x) for x in re.findall(r"[+-]?\d+", row)]   # Atk,Def,Spd,SpAtk,SpDef
        for col, v in enumerate(vals[:5]):
            if v > 0:
                nat_boost[i] = col + 1     # -> PK_ATK..PK_SPD (1..5)
            elif v < 0:
                nat_hinder[i] = col + 1

# ---- locations ----
loc_name = {}
for const, v in MAPSEC.items():
    loc_name[v] = prettify(const, "MAPSEC_", "KANTO_") if 0 <= v < 0xF0 else prettify(const, "MAPSEC_")
loc_name[0xFD] = "EGG"
loc_name[0xFE] = "TRADE"
loc_name[0xFF] = "FATEFUL"
NLOC = 256

# ---- experience tables (computed from the Gen-3 formulas) ----
def exp_at(growth, n):
    if n == 0:
        return 0
    if growth == GROWTH["GROWTH_MEDIUM_FAST"]:
        return n**3
    if growth == GROWTH["GROWTH_ERRATIC"]:
        if n <= 50:  return (100 - n) * n**3 // 50
        if n <= 68:  return (150 - n) * n**3 // 100
        if n <= 98:  return ((1911 - 10 * n) // 3) * n**3 // 500
        return (160 - n) * n**3 // 100
    if growth == GROWTH["GROWTH_FLUCTUATING"]:
        if n <= 15:  return ((n + 1) // 3 + 24) * n**3 // 50
        if n <= 36:  return (n + 14) * n**3 // 50
        return ((n // 2) + 32) * n**3 // 50
    if growth == GROWTH["GROWTH_MEDIUM_SLOW"]:
        # raw formula is NEGATIVE at level 1 (-54); clamp so it doesn't wrap in uint32
        return max(0, 6 * n**3 // 5 - 15 * n**2 + 100 * n - 140)
    if growth == GROWTH["GROWTH_FAST"]:
        return 4 * n**3 // 5
    if growth == GROWTH["GROWTH_SLOW"]:
        return 5 * n**3 // 4
    return n**3

NGROWTH = max(GROWTH.values()) + 1
exp_tbl = [[exp_at(g, L) for L in range(101)] for g in range(NGROWTH)]


# ---- emit ----
def emit_strtab(c, name, d, n, default="?"):
    c.write("static const char* const %s[%d] = {\n" % (name, n))
    for i in range(0, n, 4):
        c.write("  " + ",".join(cstr(d.get(j, default)) for j in range(i, min(i + 4, n))) + ",\n")
    c.write("};\n\n")


def emit_u8(c, name, arr):
    c.write("static const uint8_t %s[%d] = {\n" % (name, len(arr)))
    for i in range(0, len(arr), 16):
        c.write("  " + ",".join(str(x) for x in arr[i:i + 16]) + ",\n")
    c.write("};\n\n")


def emit_u16(c, name, arr):
    c.write("static const uint16_t %s[%d] = {\n" % (name, len(arr)))
    for i in range(0, len(arr), 12):
        c.write("  " + ",".join(str(x) for x in arr[i:i + 12]) + ",\n")
    c.write("};\n\n")


with open(OUT, "w") as c:
    c.write("/* GENERATED by tools/gen_data.py - do not edit. */\n")
    c.write('#include "data_tables.h"\n#include "gen3_flags.h"\n#include <stddef.h>\n\n')

    SN = MAX_SPECIES + 1
    emit_strtab(c, "s_species", sp_name, SN)
    emit_strtab(c, "s_move", mv_name, NMOVE, "-")
    emit_strtab(c, "s_item", it_name, NITEM, "????????")
    emit_u8(c, "s_itemgames", it_games)
    emit_u16(c, "s_tmhm", tmhm)
    emit_strtab(c, "s_ability", ab_name, NABIL, "-")
    emit_strtab(c, "s_location", loc_name, NLOC, "FARAWAY PLACE")

    c.write("static const char* const s_nature[25] = {\n  ")
    c.write(",".join(cstr(nat_names[i]) if i < len(nat_names) else '"?"' for i in range(25)))
    c.write("\n};\n\n")
    TYPES = max(TYPE.values()) + 1
    c.write("static const char* const s_type[%d] = {\n  " % TYPES)
    c.write(",".join(cstr(prettify(inv_type.get(i, "TYPE_?"), "TYPE_")) for i in range(TYPES)))
    c.write("\n};\n\n")
    CC = max(CONTEST.values()) + 1
    c.write("static const char* const s_contest[%d] = {\n  " % CC)
    c.write(",".join(cstr(prettify(inv_contest.get(i, "CONTEST_CATEGORY_?"), "CONTEST_CATEGORY_")) for i in range(CC)))
    c.write("\n};\n\n")

    # species numeric data
    c.write("static const uint8_t s_base[%d][6] = {\n" % SN)
    for sid in range(SN):
        c.write("  {" + ",".join(str(x) for x in sp_base[sid]) + "},\n")
    c.write("};\n\n")
    emit_u8(c, "s_t1", sp_t1)
    emit_u8(c, "s_t2", sp_t2)
    emit_u8(c, "s_gr", sp_gr)
    emit_u8(c, "s_growth", sp_growth)
    c.write("static const uint16_t s_a0[%d] = {\n" % SN)
    for i in range(0, SN, 16):
        c.write("  " + ",".join(str(x) for x in sp_a0[i:i + 16]) + ",\n")
    c.write("};\n\n")
    c.write("static const uint16_t s_a1[%d] = {\n" % SN)
    for i in range(0, SN, 16):
        c.write("  " + ",".join(str(x) for x in sp_a1[i:i + 16]) + ",\n")
    c.write("};\n\n")
    c.write("static const uint16_t s_national[%d] = {\n" % SN)
    for i in range(0, SN, 16):
        c.write("  " + ",".join(str(x) for x in sp_national[i:i + 16]) + ",\n")
    c.write("};\n\n")

    # move numeric data
    emit_u8(c, "s_mvtype", mv_type)
    emit_u8(c, "s_mvpp", mv_pp)
    emit_u8(c, "s_mvcontest", mv_contest)
    emit_u8(c, "s_mvpower", mv_power)
    emit_u8(c, "s_mvacc", mv_acc)
    emit_strtab(c, "s_gamestat", gs_name, NGS, "")

    # nature mods
    c.write("static const signed char s_natboost[25] = {%s};\n" % ",".join(str(x) for x in nat_boost))
    c.write("static const signed char s_nathinder[25] = {%s};\n\n" % ",".join(str(x) for x in nat_hinder))

    # exp tables
    c.write("static const uint32_t s_exp[%d][101] = {\n" % NGROWTH)
    for g in range(NGROWTH):
        c.write("  {" + ",".join(str(x) for x in exp_tbl[g]) + "},\n")
    c.write("};\n\n")

    # getters
    c.write(f"""
const char* pk_species_name(uint16_t i){{ return i<{SN}?s_species[i]:"?"; }}
uint16_t pk_national_no(uint16_t i){{ return i<{SN}?s_national[i]:0; }}
void pk_base_stats(uint16_t i,uint8_t o[6]){{ for(int k=0;k<6;k++)o[k]=(i<{SN})?s_base[i][k]:0; }}
uint8_t pk_species_type1(uint16_t i){{ return i<{SN}?s_t1[i]:0; }}
uint8_t pk_species_type2(uint16_t i){{ return i<{SN}?s_t2[i]:0; }}
uint16_t pk_species_ability(uint16_t i,uint8_t n){{ if(i>={SN})return 0; return n?s_a1[i]:s_a0[i]; }}
uint8_t pk_species_gender_ratio(uint16_t i){{ return i<{SN}?s_gr[i]:0xFF; }}
uint8_t pk_species_growth(uint16_t i){{ return i<{SN}?s_growth[i]:0; }}
const char* pk_move_name(uint16_t i){{ return i<{NMOVE}?s_move[i]:"-"; }}
uint8_t pk_move_type(uint16_t i){{ return i<{NMOVE}?s_mvtype[i]:0; }}
uint8_t pk_move_pp(uint16_t i){{ return i<{NMOVE}?s_mvpp[i]:0; }}
uint8_t pk_move_contest(uint16_t i){{ return i<{NMOVE}?s_mvcontest[i]:0; }}
uint8_t pk_move_power(uint16_t i){{ return i<{NMOVE}?s_mvpower[i]:0; }}
uint8_t pk_move_accuracy(uint16_t i){{ return i<{NMOVE}?s_mvacc[i]:0; }}
const char* pk_game_stat_name(int i){{ return (i>=0&&i<{NGS}&&s_gamestat[i][0])?s_gamestat[i]:"(stat)"; }}
const char* pk_item_name(uint16_t i){{ return i<{NITEM}?s_item[i]:"????????"; }}
uint8_t pk_item_games(uint16_t i){{ return i<{NITEM}?s_itemgames[i]:0; }}
uint16_t pk_tmhm_move(uint16_t i){{ return (i>={TMHM_FIRST}&&i<{TMHM_FIRST}+{TMHM_COUNT})?s_tmhm[i-{TMHM_FIRST}]:0; }}
int pk_tmhm_number(uint16_t i){{
  if(i<{TMHM_FIRST}||i>={TMHM_FIRST}+{TMHM_COUNT}) return 0;
  int k=i-{TMHM_FIRST};
  return (k<50)?k+1:k-49;
}}
int pk_item_is_hm(uint16_t i){{ return i>={TMHM_FIRST}+50&&i<{TMHM_FIRST}+{TMHM_COUNT}; }}
/* Retail's own bag label for a TM/HM row: "No01 FOCUS PUNCH" (Emerald prints the
 * number in its small font and the move in the normal one; pokefirered/src/tm_case.c
 * builds the same string). Plain items fall through to their own name. */
void pk_item_label(uint16_t i, char* out, int cap){{
  const char* nm = pk_item_name(i);
  uint16_t mv = pk_tmhm_move(i);
  int o = 0;
  if(mv){{
    int n = pk_tmhm_number(i);
    const char* pfx = pk_item_is_hm(i) ? "HM" : "No";
    while(*pfx && o<cap-1) out[o++]=*pfx++;
    if(o<cap-1) out[o++]=(char)('0'+n/10);
    if(o<cap-1) out[o++]=(char)('0'+n%10);
    if(o<cap-1) out[o++]=' ';
    nm = pk_move_name(mv);
  }}
  while(*nm && o<cap-1) out[o++]=*nm++;
  out[o]=0;
}}
const char* pk_ability_name(uint16_t i){{ return i<{NABIL}?s_ability[i]:"-"; }}
const char* pk_nature_name(uint8_t i){{ return i<25?s_nature[i]:"?"; }}
int pk_nature_boost(uint8_t i){{ return i<25?s_natboost[i]:-1; }}
int pk_nature_hinder(uint8_t i){{ return i<25?s_nathinder[i]:-1; }}
const char* pk_type_name(uint8_t i){{ return i<{TYPES}?s_type[i]:"?"; }}
const char* pk_contest_name(uint8_t i){{ return i<{CC}?s_contest[i]:"?"; }}
const char* pk_location_name(uint16_t i){{ return i<{NLOC}?s_location[i]:"FARAWAY PLACE"; }}
uint32_t pk_exp_for_level(uint8_t g,uint8_t l){{ if(g>={NGROWTH}||l>100)return 0; if(l<=1)return 0; return s_exp[g][l]; }}
uint8_t pk_level_from_exp(uint8_t g,uint32_t e){{
  if(g>={NGROWTH})return 1; int l=1; while(l<100&&e>=s_exp[g][l+1])l++; return (uint8_t)l;
}}
""")

    # ---- named event flags (per-game curated) ----
    gmap = [("pokeemerald_data", "E", "PK_EMERALD"),
            ("pokefirered_data", "F", "PK_FRLG"),
            ("pokeruby_data", "R", "PK_RS")]
    for gd, suf, _ in gmap:
        rows = named_flags.get(gd, []) or [(0xFFFF, "")]
        c.write("static const NamedFlag s_nflag_%s[%d] = {\n" % (suf, len(rows)))
        for num, disp in rows:
            c.write("  {0x%04X, %s},\n" % (num, cstr(disp)))
        c.write("};\n\n")
    c.write("int pk_named_flags(PkGame g, const NamedFlag** out){\n  switch(g){\n")
    for gd, suf, enum in gmap:
        c.write("    case %s: *out=s_nflag_%s; return %d;\n" % (enum, suf, len(named_flags.get(gd, []))))
    c.write("    default: *out=s_nflag_E; return %d;\n  }\n}\n"
            % len(named_flags.get("pokeemerald_data", [])))

# ---- source/data_desc.c: the 741 verbatim description strings, on their own ----
# (BACKLOG #19 / docs/AUDIT-2026-09-05-backlog-3-19.md section B). Split out of
# data_tables.c so PDNA_ARTLESS=1 can exclude just this file (see desc_gate.h) --
# everything else a build needs (names, base stats, type/ability tables) stays in
# data_tables.c above, ungated, the same as it always was.
with open(OUT_DESC, "w") as d:
    d.write("/* GENERATED by tools/gen_data.py - do not edit.\n"
             " *\n"
             " * The 741 verbatim item/move/ability description strings (BACKLOG #19 /\n"
             " * docs/AUDIT-2026-09-05-backlog-3-19.md section B) -- gated by desc_gate.h /\n"
             " * PDNA_DESC_TEXT_COMPILED exactly like hand_gate.h/mon_icons_gate.h/\n"
             " * rom_chrome_gate.h gate the art modules; see data_desc_shim.c for the\n"
             " * placeholder this file's absence falls back to. */\n")
    d.write('#include "data_tables.h"\n#include <stddef.h>\n\n')
    emit_strtab(d, "s_itemdesc", it_desc, NITEM, "")
    emit_strtab(d, "s_abilitydesc", ab_desc, NABIL, "")
    emit_strtab(d, "s_mvdesc", mv_desc, NMOVE, "")
    d.write(f"""
const char* pk_item_desc(uint16_t i){{ return i<{NITEM}?s_itemdesc[i]:""; }}
const char* pk_ability_desc(uint16_t i){{ return i<{NABIL}?s_abilitydesc[i]:""; }}
const char* pk_move_desc(uint16_t i){{ return i<{NMOVE}?s_mvdesc[i]:""; }}
""")

print("data_tables.c: species=%d names, moves=%d, items=%d, abilities=%d, locations~%d, growth=%d"
      % (len(sp_name), len(mv_name), len(it_name), len(ab_name), len(loc_name), NGROWTH))
print("item game masks: RS=%d E=%d FRLG=%d ids (bit0 RS, bit1 E, bit2 FRLG)"
      % (len(rs_ids), len(e_ids), len(f_ids)))
print("named flags: E=%d F=%d R=%d (incl. category headers)"
      % (len(named_flags["pokeemerald_data"]), len(named_flags["pokefirered_data"]), len(named_flags["pokeruby_data"])))
print("written:", OUT)
print("written:", OUT_DESC)
