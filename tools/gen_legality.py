#!/usr/bin/env python3
"""Generate source/learnsets.c — a per-species "can this species line ever learn
this move?" bitset, used by the legality checker (gen3_legality.c).

Design (deliberately CONSERVATIVE — zero false positives on legit mons):
  learnable(S) = U over {S and all its pre-evolutions} of
                   ( level-up moves  U  egg moves )
                 U  ALL TM/HM moves        (accepted for every species)
                 U  ALL move-tutor moves   (accepted for every species)
                 U  the 3 "ultimate" starter tutor moves (special FRLG tutor)

CROSS-GAME: level-up sets, egg moves AND move tutors all differ between Ruby/
Sapphire, Emerald and FireRed/LeafGreen. A save edited on this cart can hold a mon
from ANY of the five games, so we UNION the learnsets across all three decomps
(pokeemerald + pokefirered + pokeruby). Validating an FRLG mon against Emerald
data alone false-flags legit moves (Mr. Mime/Magical Leaf, etc.).

TM/HM and tutor compatibility is per-species in the games, but the modern
pokeemerald data encodes it as struct bitfields that are fragile to parse, and
getting it wrong would FALSE-flag legit mons. So we over-accept those globally:
a move is only flagged when no Gen-3 method (level-up/egg for the line, or any
TM/HM/tutor) could teach it — i.e. a signature move on the wrong species, or a
later-generation move. Those are real hack tells; a wrong-TM combo is missed on
purpose (acceptable, since it ships as a WARNING, never a hard "illegal").

Reads the git-ignored decomp excerpts under reference/{pokeemerald,pokefirered,
pokeruby}_data/. Output source/learnsets.c is git-ignored (generate-locally
policy). Run from the repo root:  python3 tools/gen_legality.py
"""
import os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "reference")
EMER = os.path.join(REF, "pokeemerald_data")
GAMES = ["pokeemerald_data", "pokefirered_data", "pokeruby_data"]
OUT = os.path.join(ROOT, "source", "learnsets.c")
MAX_SPECIES = 411


def rd(base, path):
    with open(os.path.join(base, path), encoding="utf-8", errors="replace") as f:
        return f.read()


def eval_int(expr, env):
    expr = re.sub(r"//.*$", "", expr).strip().rstrip(",").strip()
    try:
        return int(expr, 0)
    except ValueError:
        pass

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


# ---- id maps (identical internal ids across the three Gen-3 decomps) ----
SPEC = parse_defines(rd(EMER, "include/constants/species.h"), "SPECIES_")
MOVE = parse_defines(rd(EMER, "include/constants/moves.h"), "MOVE_")
NMOVE = (max(MOVE.values()) + 1) if MOVE else 355
NBYTES = (NMOVE + 7) // 8
SN = MAX_SPECIES + 1


def move_ids(text):
    out = set()
    for name in re.findall(r"MOVE_\w+", text):
        mid = MOVE.get(name)
        if mid is not None and 0 < mid < NMOVE:
            out.add(mid)
    return out


# per-species accumulators, unioned across all three games
sp_levelup = {s: set() for s in range(SN)}
sp_egg = {s: set() for s in range(SN)}
global_tutor = set()


def parse_levelup(base):
    """species id -> {move ids}; handles designated and ordered pointer tables,
    and both `static const u16 sXLearnset[]` and `const u16 gXLearnset[]`."""
    src = rd(base, "src/data/pokemon/level_up_learnsets.h")
    arr = {}
    for m in re.finditer(r"(?:static\s+)?const u16 (\w+)\[\]\s*=\s*\{(.*?)\};", src, re.S):
        arr[m.group(1)] = move_ids(m.group(2))
    ptr = rd(base, "src/data/pokemon/level_up_learnset_pointers.h")
    if "[SPECIES_" in ptr:                                   # designated (E / FRLG)
        for m in re.finditer(r"\[(SPECIES_\w+)\]\s*=\s*(\w+)", ptr):
            sid = SPEC.get(m.group(1))
            if sid is not None and sid <= MAX_SPECIES:
                sp_levelup[sid] |= arr.get(m.group(2), set())
    else:                                                    # ordered list (R/S) -> index == species id
        body = re.search(r"=\s*\{(.*?)\};", ptr, re.S)
        if body:
            names = re.findall(r"(\w+LevelUpLearnset)", body.group(1))
            for sid, name in enumerate(names):
                if sid <= MAX_SPECIES:
                    sp_levelup[sid] |= arr.get(name, set())


def parse_egg(base):
    src = rd(base, "src/data/pokemon/egg_moves.h")
    body = re.search(r"gEggMoves\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not body:
        return
    for chunk in re.split(r"egg_moves\s*\(", body.group(1))[1:]:
        chunk = chunk[:chunk.find(")")] if ")" in chunk else chunk
        first = chunk.split(",", 1)[0].strip()
        sid = SPEC.get("SPECIES_" + first)
        if sid is not None and sid <= MAX_SPECIES:
            sp_egg[sid] |= move_ids(chunk)


def parse_tutor(base):
    """gTutorMoves[] (Emerald) or sTutorMoves[] (FRLG). RS has no data file."""
    p = "src/data/pokemon/tutor_learnsets.h"
    if not os.path.exists(os.path.join(base, p)):
        return
    src = rd(base, p)
    body = re.search(r"[gs]TutorMoves\[[^\]]*\]\s*=\s*\{(.*?)\};", src, re.S)
    if body:
        global_tutor.update(move_ids(body.group(1)))


for g in GAMES:
    base = os.path.join(REF, g)
    if not os.path.isdir(base):
        continue
    parse_levelup(base)
    parse_egg(base)
    parse_tutor(base)

# ---- evolution: immediate pre-evo map (internal ids identical across games) ----
preevo = {}
for m in re.finditer(r"\[(SPECIES_\w+)\]\s*=\s*\{(.*?)\}\s*,?\s*(?=\[SPECIES_|\};)",
                     rd(EMER, "src/data/pokemon/evolution.h"), re.S):
    base_sid = SPEC.get(m.group(1))
    if base_sid is None:
        continue
    for tgt in re.findall(r",\s*(SPECIES_\w+)\s*\}", m.group(2)):
        ev = SPEC.get(tgt)
        if ev is not None and ev <= MAX_SPECIES and ev not in preevo:
            preevo[ev] = base_sid


def chain(sid):
    out, seen, cur = [], set(), sid
    while cur is not None and cur not in seen:
        out.append(cur)
        seen.add(cur)
        cur = preevo.get(cur)
    return out


# ---- global TM/HM set (identical across Gen 3) + ultimate starter tutor moves ----
tm_names = re.findall(r"F\(([A-Z_0-9]+)\)", rd(EMER, "include/constants/tms_hms.h"))
global_moves = set(global_tutor)
for nm in tm_names:
    mid = MOVE.get("MOVE_" + nm)
    if mid is not None and 0 < mid < NMOVE:
        global_moves.add(mid)
# A few legit Gen-3 moves are taught by CODE, not any data file the generator
# reads, so they must be accepted explicitly (else they false-flag legit mons):
#   - Blast Burn / Frenzy Plant / Hydro Cannon: FRLG Cape-Brink "ultimate" tutor
#     for the 9 fully-evolved starters.
#   - Volt Tackle: a Pichu hatches knowing it when a parent holds a Light Ball
#     (the daycare/breeding special case) — affects the Pichu/Pikachu/Raichu line.
# Accepted globally, consistent with the conservative over-acceptance posture.
for nm in ("MOVE_BLAST_BURN", "MOVE_FRENZY_PLANT", "MOVE_HYDRO_CANNON", "MOVE_VOLT_TACKLE"):
    mid = MOVE.get(nm)
    if mid is not None and 0 < mid < NMOVE:
        global_moves.add(mid)

# ---- build per-species bitset ----
bitset = [bytearray(NBYTES) for _ in range(SN)]
for sid in range(SN):
    mset = set(global_moves)
    for anc in chain(sid):
        mset |= sp_levelup.get(anc, set())
        mset |= sp_egg.get(anc, set())
    for mid in mset:
        bitset[sid][mid >> 3] |= 1 << (mid & 7)

# ---- emit ----
with open(OUT, "w") as c:
    c.write("/* GENERATED by tools/gen_legality.py - do not edit. */\n")
    c.write('#include "learnsets.h"\n\n')
    c.write("#define NMOVE %d\n#define NBYTES %d\n\n" % (NMOVE, NBYTES))
    c.write("static const uint8_t s_learn[%d][NBYTES] = {\n" % SN)
    for sid in range(SN):
        c.write("  {" + ",".join("0x%02x" % b for b in bitset[sid]) + "},\n")
    c.write("};\n\n")
    c.write("""bool pk_can_learn(uint16_t species, uint16_t move){
  if (species >= %d || move >= NMOVE) return true; /* unknown -> don't flag */
  return (s_learn[species][move >> 3] >> (move & 7)) & 1;
}
""" % SN)

nlevel = sum(1 for s in range(SN) if sp_levelup[s])
negg = sum(1 for s in range(SN) if sp_egg[s])
print("learnsets.c: species=%d, moves=%d (%d bytes/mon), global TM/HM+tutor+ult=%d, "
      "level-up species=%d, egg species=%d, pre-evo links=%d"
      % (SN, NMOVE, NBYTES, len(global_moves), nlevel, negg, len(preevo)))
print("written:", OUT)
