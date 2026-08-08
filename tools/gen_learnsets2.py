#!/usr/bin/env python3
"""Generate source/learnsets2.c — per-game-group move-source data WITH LEVELS.

Why a second generator instead of extending tools/gen_legality.py: that one
deliberately throws away everything V2 needs. It unions the three game groups
into one bitset, drops the learn level, and over-accepts every TM/HM and tutor
move for every species (gen_legality.py:18-24). Those choices are right for a
warn-only "could this line ever learn this?" check and wrong for the Legality-V2
catalogue, which needs to tell level-up from TM from tutor from egg, per game.
gen_legality.py stays as-is; source/learnsets.c keeps shipping alongside this.

What V2 buys with the levels (docs/research-legality-v2.md §3 C2): a move that is
ONLY a level-up move for the line, whose minimum learn level across all three
game groups exceeds the mon's current level, is impossible on retail — the Move
Reminder only reteaches moves at or below the current level and levels never
decrease. That is the one move check that survives cross-game trading, and it
needs the level, which the V1 table does not have.

Emitted per game group (PK_RS / PK_EMERALD / PK_FRLG, matching the PkGame enum in
source/gen3_trainer.h:11):
  * level-up learnsets, decomp order, packed (level << 9) | move  — same packing
    the games themselves use (LEVEL_UP_MOVE in level_up_learnsets.h:1);
  * TM/HM compatibility, one bit per machine in TM01..TM50 + HM01..HM08 order;
  * move-tutor compatibility, one bit per tutor move;
  * egg moves per species.

Plus an overlay for the four Gen-3 moves that reach a Pokemon through hand-written
code instead of any learnset table — FRLG's Cape Brink Frenzy Plant / Blast Burn /
Hydro Cannon (party_menu.c) and Volt Tackle via Light-Ball breeding (daycare.c).
Both are READ OUT OF the decomp source, not assumed, because the assumption in
gen_legality.py:183-186 ("the 9 fully-evolved starters") is wrong: the code gates
each ultimate move on exactly one Kanto starter, and Emerald has no such tutor.
Without the overlay a legitimately obtained mon would come back with NO source,
which is the one answer this module must never give.

All four are pooled and deduplicated across (game, species): the three game
groups share most of their lists verbatim, so the pool costs far less than three
independent tables. Output is const, read in place, zero EWRAM (hard convention 2).

DATA HONESTY — what is actually on disk decides what gets emitted. The script
prints a SOURCES block naming the file behind every table and marks anything it
had to approximate, and the runtime API exposes that through lg2_exact_sources().
Nothing is invented: a missing per-game table is approximated by OR-ing the game
groups that ARE present, which can only ever over-accept (never false-flags a
legit mon), and the approximation is reported rather than hidden.

Reads the git-ignored decomp checkouts; nothing decomp-derived is committed —
source/learnsets2.c is git-ignored, this generator is the committed artifact
(same policy as gen_legality.py / gen_data.py; docs/kb/licensing.md).

Run from the repo root:  python3 tools/gen_learnsets2.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "source", "learnsets2.c")

MAX_SPECIES = 411          # internal ids; 411 = Chimecho (species.h:...), 412 = SPECIES_EGG
NSP = MAX_SPECIES + 1

# PkGame order (gen3_trainer.h:11) — the emitted arrays are indexed by it directly.
GAMES = ["RS", "E", "FRLG"]
GIDX = {"RS": 0, "E": 1, "FRLG": 2}

# Search order per game group. First hit wins; the fallbacks exist because the
# decomp *excerpts* under reference/ carry the learnsets but not tmhm_learnsets.h,
# while the fuller local checkouts carry both.
ROOTS = {
    "RS":   ["reference/pokeruby_data", "assets/upstream/pokeruby"],
    "E":    ["reference/pokeemerald_data", "daycare map/pokeemerald"],
    "FRLG": ["reference/pokefirered_data", "assets/upstream/pokefirered"],
}

SOURCES = []               # (game, kind, path-or-None) — printed at the end
APPROX = {g: set() for g in GAMES}   # kinds this game group had to borrow


def find(game, rel):
    """Absolute path of `rel` in the first checkout of `game` that has it."""
    for base in ROOTS[game]:
        p = os.path.join(ROOT, base, rel)
        if os.path.exists(p):
            return p
    return None


def rd(path):
    with open(path, encoding="utf-8", errors="replace") as f:
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


# ---- id maps ---------------------------------------------------------------
# The three decomps use the same internal ids (Deoxys 410, Chimecho 411 — NOT
# National-Dex order). gen_legality.py:76 assumes this silently; here it is
# checked, because a silent id shift would corrupt every table below.
sp_h = find("E", "include/constants/species.h")
mv_h = find("E", "include/constants/moves.h")
if not sp_h or not mv_h:
    sys.exit("FATAL: no species.h/moves.h under any pokeemerald checkout — "
             "cannot build id maps")
SPEC = parse_defines(rd(sp_h), "SPECIES_")
MOVE = parse_defines(rd(mv_h), "MOVE_")
NMOVE = MOVE.get("MOVES_COUNT") or (max(MOVE.values()) + 1)

id_mismatch = 0
for g in GAMES:
    for rel, ref, pfx in (("include/constants/species.h", SPEC, "SPECIES_"),
                          ("include/constants/moves.h", MOVE, "MOVE_")):
        p = find(g, rel)
        if not p:
            continue
        other = parse_defines(rd(p), pfx)
        bad = [k for k, v in other.items() if k in ref and ref[k] != v]
        if bad:
            id_mismatch += len(bad)
            print("  !! %s %s: %d ids differ from Emerald's (e.g. %s)"
                  % (g, rel, len(bad), bad[:3]))

# ---- level-up learnsets ----------------------------------------------------
lv = {g: {s: [] for s in range(NSP)} for g in GAMES}   # species -> [(level, move)]
lv_variant_merges = []      # arrays that existed once per game VERSION
lv_dropped_moves = []       # move names the id map could not resolve


def parse_levelup(g):
    src_p = find(g, "src/data/pokemon/level_up_learnsets.h")
    ptr_p = find(g, "src/data/pokemon/level_up_learnset_pointers.h")
    SOURCES.append((g, "level-up", src_p))
    if not src_p or not ptr_p:
        return
    src = rd(src_p)
    arr = {}
    # pokeemerald/pokefirered use `static const u16 sXLearnset[]`, pokeruby
    # `const u16 gXLearnset[]`; both pack LEVEL_UP_MOVE(level, move) in ascending
    # level order, which is the order the games teach them in.
    for m in re.finditer(r"(?:static\s+)?const u16 (\w+)\[\]\s*=\s*\{(.*?)\};", src, re.S):
        name, ent = m.group(1), []
        for l, mv in re.findall(r"LEVEL_UP_MOVE\(\s*(\d+)\s*,\s*(MOVE_\w+)\s*\)", m.group(2)):
            mid, lvl = MOVE.get(mv), int(l)
            if mid is None:
                lv_dropped_moves.append((g, name, mv))
            elif lvl <= 127 and (lvl, mid) not in ent:
                # (level, move) repeats inside ONE array where a #if picks the
                # order rather than the content — pokefirered's Dugtrio lists
                # Scratch/Sand-Attack in each of two branches
                # (level_up_learnsets.h:673-679). Both branches land in this
                # regex match, so dedup instead of storing the move twice.
                ent.append((lvl, mid))
        if name in arr:
            # A whole array defined twice under #if defined(FIRERED) /
            # #elif defined(LEAFGREEN): Deoxys really does learn different moves
            # in the two versions (level_up_learnsets.h:5657-5691), because its
            # forme differs per game. PkGame has a single PK_FRLG group, so take
            # the UNION and let lg2_min_levelup_level() pick the lowest level per
            # move. Over-accepting a LeafGreen move on a FireRed cart is the safe
            # direction; picking one branch would false-flag the other version's
            # perfectly legal Deoxys. (Before this merge existed, the second
            # definition silently replaced the first and 12 entries vanished.)
            for e in ent:
                if e not in arr[name]:
                    arr[name].append(e)
            arr[name].sort(key=lambda e: e[0])
            lv_variant_merges.append((g, name))
        else:
            arr[name] = ent
    ptr = rd(ptr_p)
    if "[SPECIES_" in ptr:                       # designated table (Emerald / FRLG)
        for m in re.finditer(r"\[(SPECIES_\w+)\]\s*=\s*(\w+)", ptr):
            sid = SPEC.get(m.group(1))
            if sid is not None and 0 < sid <= MAX_SPECIES:
                lv[g][sid] = arr.get(m.group(2), [])
    else:                                        # ordered list (Ruby): index == species id
        body = re.search(r"=\s*\{(.*?)\};", ptr, re.S)
        if body:
            for sid, name in enumerate(re.findall(r"(\w+LevelUpLearnset)", body.group(1))):
                if 0 < sid <= MAX_SPECIES:
                    lv[g][sid] = arr.get(name, [])


# ---- egg moves -------------------------------------------------------------
egg = {g: {s: [] for s in range(NSP)} for g in GAMES}


def parse_egg(g):
    p = find(g, "src/data/pokemon/egg_moves.h")
    SOURCES.append((g, "egg", p))
    if not p:
        return
    # Take the gEggMoves[] body only: the `#define egg_moves(...)` line above it
    # would otherwise parse as a bogus entry.
    body = re.search(r"gEggMoves\[\]\s*=\s*\{(.*?)\};", rd(p), re.S)
    if not body:
        return
    for chunk in re.split(r"egg_moves\s*\(", body.group(1))[1:]:
        chunk = chunk[:chunk.find(")")] if ")" in chunk else chunk
        sid = SPEC.get("SPECIES_" + chunk.split(",", 1)[0].strip())
        if sid is None or not (0 < sid <= MAX_SPECIES):
            continue
        egg[g][sid] = [MOVE[n] for n in re.findall(r"MOVE_\w+", chunk) if n in MOVE]


# ---- TM/HM compatibility ---------------------------------------------------
# One canonical bit order: TM01..TM50 then HM01..HM08, straight out of the
# FOREACH_TM/FOREACH_HM macro lists. Both decomp formats are keyed by MOVE NAME
# (Emerald `.TOXIC = TRUE`, FRLG `TMHM(TM06_TOXIC)`), so the games' own bit
# orders never matter — which is what makes the "fragile bitfield parse" that
# gen_legality.py:18-24 backed away from actually safe here.
tm_names = []
tms_h = find("E", "include/constants/tms_hms.h")
if tms_h:
    tms = rd(tms_h)
    for macro in ("FOREACH_TM", "FOREACH_HM"):
        blk = re.search(r"#define\s+" + macro + r"\(F\)(.*?)(?=\n#define|\n#endif)", tms, re.S)
        if blk:
            tm_names += re.findall(r"F\((\w+)\)", blk.group(1))
NTM = len(tm_names)
tm_move = [MOVE.get("MOVE_" + n, 0) for n in tm_names]
tm_bit = {n: i for i, n in enumerate(tm_names)}

tmhm = {g: [0] * NSP for g in GAMES}
tm_unknown = []


def species_chunks(text):
    """[SPECIES_X] = <anything up to the next [SPECIES_...]> — format-agnostic."""
    hits = list(re.finditer(r"\[(SPECIES_\w+)\]\s*=", text))
    for i, m in enumerate(hits):
        end = hits[i + 1].start() if i + 1 < len(hits) else len(text)
        yield m.group(1), text[m.end():end]


def parse_tmhm(g):
    p = find(g, "src/data/pokemon/tmhm_learnsets.h")
    SOURCES.append((g, "tmhm", p))
    if not p:
        return False
    src = rd(p)
    for name, chunk in species_chunks(src):
        sid = SPEC.get(name)
        if sid is None or not (0 < sid <= MAX_SPECIES):
            continue
        mask = 0
        # Emerald struct-bitfield form: `.TOXIC = TRUE,`
        # FRLG u64-macro form:          `TMHM(TM06_TOXIC)`
        found = re.findall(r"\.(\w+)\s*=\s*TRUE", chunk) + \
                re.findall(r"TMHM\((?:TM|HM)\d+_(\w+)\)", chunk)
        for nm in found:
            b = tm_bit.get(nm)
            if b is None:
                tm_unknown.append((g, name, nm))
            else:
                mask |= 1 << b
        tmhm[g][sid] = mask
    return True


# ---- move tutors -----------------------------------------------------------
# Canonical bit space = Emerald's gTutorMoves order (30 moves), extended with any
# tutor move a later game group has that Emerald lacks (there are none today, but
# the generator must not depend on that).
tutor_names = []
tutor = {g: [0] * NSP for g in GAMES}
tut_unknown = []


def parse_tutor_list(g):
    p = find(g, "src/data/pokemon/tutor_learnsets.h")
    if not p:
        return
    for nm in re.findall(r"\[TUTOR_MOVE_\w+\]\s*=\s*(MOVE_\w+)", rd(p)):
        if nm in MOVE and nm not in tutor_names:
            tutor_names.append(nm)


def parse_tutor(g):
    p = find(g, "src/data/pokemon/tutor_learnsets.h")
    SOURCES.append((g, "tutor", p))
    if not p:
        return False
    body = re.search(r"[gs]TutorLearnsets\[[^\]]*\]\s*=\s*\{(.*)", rd(p), re.S)
    if not body:
        return False
    bit = {n: i for i, n in enumerate(tutor_names)}
    for name, chunk in species_chunks(body.group(1)):
        sid = SPEC.get(name)
        if sid is None or not (0 < sid <= MAX_SPECIES):
            continue
        mask = 0
        for nm in re.findall(r"TUTOR\((MOVE_\w+)\)", chunk):
            b = bit.get(nm)
            if b is None:
                tut_unknown.append((g, name, nm))
            else:
                mask |= 1 << b
        tutor[g][sid] = mask
    return True


# ---- moves taught by CODE, which no data file lists ------------------------
# Four Gen-3 moves reach a Pokemon through a hand-written function rather than a
# learnset table. Leaving them out would make legitimately obtained mons come back
# with NO source at all, which is the one outcome this module must never produce.
# gen_legality.py:182-192 already accepts them, but globally and with a comment
# that says "the 9 fully-evolved starters" — the decomp says otherwise, so these
# are read out of the code instead of assumed.
code_tutor_found = []
volt_tackle_games = []


def parse_code_tutors(g):
    """FRLG's Cape Brink "ultimate" tutor: eligibility is a switch, not a mask.

    pokefirered/src/party_menu.c — GetTutorMove() maps the three extra tutor slots
    to Frenzy Plant / Blast Burn / Hydro Cannon, and CanLearnTutorMove() gates each
    on ONE species (Venusaur / Charizard / Blastoise — the Kanto starters only, not
    all nine). pokeemerald's CanLearnTutorMove is a plain sTutorLearnsets lookup, so
    this correctly finds nothing there: those moves are FRLG-only in Gen 3."""
    p = find(g, "src/party_menu.c")
    if not p:
        return []
    src = rd(p)
    t2m, out = {}, []
    body = re.search(r"GetTutorMove\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
    if body:
        for t, m in re.findall(r"case\s+(TUTOR_MOVE_\w+):\s*return\s+(MOVE_\w+);", body.group(1)):
            t2m[t] = m
    body = re.search(r"CanLearnTutorMove\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
    if body:
        blk = body.group(1)
        cases = list(re.finditer(r"case\s+(TUTOR_MOVE_\w+):", blk))
        for i, cm in enumerate(cases):
            end = cases[i + 1].start() if i + 1 < len(cases) else len(blk)
            mv = t2m.get(cm.group(1))
            for sp in re.findall(r"species\s*==\s*(SPECIES_\w+)", blk[cm.end():end]):
                if mv in MOVE and sp in SPEC:
                    out.append((SPEC[sp], MOVE[mv]))
    return out


def has_volt_tackle_breeding(g):
    """GiveVoltTackleIfLightBall() in src/daycare.c — a Pikachu or Raichu parent
    holding a Light Ball passes Volt Tackle to the hatchling. Present in
    pokeemerald (daycare.c:750-760) and pokefirered, ABSENT from pokeruby's
    daycare.c, so Ruby/Sapphire genuinely cannot produce one.

    The species is not named in that function — it writes to whatever hatched —
    but the only egg a Light-Ball parent can produce is Pichu, so the move is
    recorded as a Pichu egg move (the lowest breeding stage, which is where every
    other egg move lives; callers walk the evolution chain from there)."""
    p = find(g, "src/daycare.c")
    return bool(p) and "MOVE_VOLT_TACKLE" in rd(p)


for g in GAMES:
    parse_levelup(g)
    parse_egg(g)
parse_tutor_list("E")
parse_tutor_list("FRLG")
parse_tutor_list("RS")
have_tmhm = {g: parse_tmhm(g) for g in GAMES}
have_tutor = {g: parse_tutor(g) for g in GAMES}

# Overlay the code-taught moves, AFTER the data-file parse so the extra tutor
# slots land at the end of the canonical bit space and existing indices hold.
code_tutors = {g: parse_code_tutors(g) for g in GAMES}
for g in GAMES:
    for sid, mid in code_tutors[g]:
        name = next((n for n, v in MOVE.items() if v == mid), None)
        if name not in tutor_names:
            tutor_names.append(name)
        tutor[g][sid] |= 1 << tutor_names.index(name)
        code_tutor_found.append((g, sid, mid))

VOLT_TACKLE = MOVE.get("MOVE_VOLT_TACKLE")
PICHU = SPEC.get("SPECIES_PICHU")
for g in GAMES:
    if has_volt_tackle_breeding(g) and VOLT_TACKLE and PICHU:
        if VOLT_TACKLE not in egg[g][PICHU]:
            egg[g][PICHU] = egg[g][PICHU] + [VOLT_TACKLE]
        volt_tackle_games.append(g)

if len(tutor_names) > 64:
    sys.exit("FATAL: %d tutor moves — the mask no longer fits 2x u32" % len(tutor_names))
if NTM > 64:
    sys.exit("FATAL: %d TM/HMs — the mask no longer fits 2x u32" % NTM)

# ---- degrade, loudly, for game groups whose table is not on disk -----------
# Ruby/Sapphire: the local pokeruby checkout carries level-up + egg moves but no
# src/data/pokemon/tmhm_learnsets.h and no tutor_learnsets.h. Rather than invent
# them (or silently answer "no"), RS borrows the OR of the game groups that are
# present. OR-ing can only widen the answer, so an RS mon is never false-flagged;
# lg2_exact_sources(PK_RS) reports the borrow so a caller can say "unknown"
# instead of "legal". Fix properly by completing the pokeruby checkout.
for g in GAMES:
    if not have_tmhm[g]:
        donors = [d for d in GAMES if have_tmhm[d]]
        if donors:
            tmhm[g] = [0] * NSP
            for s in range(NSP):
                for d in donors:
                    tmhm[g][s] |= tmhm[d][s]
            APPROX[g].add("tmhm")
    if not have_tutor[g]:
        donors = [d for d in GAMES if have_tutor[d]]
        if donors:
            tutor[g] = [0] * NSP
            for s in range(NSP):
                for d in donors:
                    tutor[g][s] |= tutor[d][s]
            APPROX[g].add("tutor")

# ---- cross-checks (§7.4 of the plan): print, never silently trust -----------
checks = []
checks.append(("species/move ids agree across checkouts",
               "OK" if id_mismatch == 0 else "%d MISMATCHES" % id_mismatch))
checks.append(("TM/HM machines parsed", "%d (expect 58: TM01-50 + HM01-08)" % NTM))
checks.append(("unrecognised TM/HM field names", "%d %s" % (len(tm_unknown), tm_unknown[:3])))
checks.append(("tutor moves in canonical order", "%d" % len(tutor_names)))
checks.append(("unrecognised tutor move names", "%d %s" % (len(tut_unknown), tut_unknown[:3])))
checks.append(("level-up moves dropped (unresolved id)",
               "%d %s" % (len(lv_dropped_moves), lv_dropped_moves[:3])))
checks.append(("code-taught tutors found (Cape Brink ultimates)",
               "%d %s" % (len(code_tutor_found),
                          ["%s sp%d mv%d" % t for t in code_tutor_found])))
checks.append(("Volt Tackle breeding (Light Ball) in", ", ".join(volt_tackle_games) or "NONE"))
checks.append(("per-version learnsets merged (FR vs LG etc.)",
               "%d %s" % (len(lv_variant_merges),
                          [g + ":" + n for g, n in lv_variant_merges])))

if have_tmhm["E"] and have_tmhm["FRLG"]:
    diff = [s for s in range(1, NSP) if tmhm["E"][s] != tmhm["FRLG"][s]]
    checks.append(("Emerald vs FRLG TM/HM rows differing",
                   "%d of %d species" % (len(diff), MAX_SPECIES)))
def _egg_wo_overlay(g, s):
    return [m for m in egg[g][s] if not (s == PICHU and m == VOLT_TACKLE)]


egg_same = all(_egg_wo_overlay("E", s) == _egg_wo_overlay(g, s)
               for g in GAMES for s in range(NSP))
checks.append(("egg_moves.h identical across game groups (pre-overlay)",
               "YES" if egg_same else "NO"))
if have_tutor["E"] and have_tutor["FRLG"]:
    e_moves = {n for n in tutor_names if any(
        tutor["E"][s] >> tutor_names.index(n) & 1 for s in range(NSP))}
    f_moves = {n for n in tutor_names if any(
        tutor["FRLG"][s] >> tutor_names.index(n) & 1 for s in range(NSP))}
    checks.append(("tutor moves FRLG has and Emerald does not",
                   "%s (expect only the 3 Cape Brink ultimates)"
                   % sorted(n.replace("MOVE_", "") for n in f_moves - e_moves)))
    checks.append(("tutor moves Emerald has and FRLG does not",
                   "%d (Emerald's 30-tutor list vs FRLG's 15)" % len(e_moves - f_moves)))

# ---- pool + dedup ----------------------------------------------------------
def pool_lists(per_game):
    """[game][species] -> list -> (flat pool, off[3][NSP], cnt[3][NSP]).

    Identical lists share one run in the pool. The three game groups repeat each
    other heavily (Ruby's level-up data is Emerald's for most species), so this
    is most of the difference between "27 KiB" and "three independent tables"."""
    flat, seen = [], {}
    off = [[0] * NSP for _ in GAMES]
    cnt = [[0] * NSP for _ in GAMES]
    for gi, g in enumerate(GAMES):
        for s in range(NSP):
            lst = tuple(per_game[g][s])
            if not lst:
                continue
            if lst not in seen:
                seen[lst] = len(flat)
                flat.extend(lst)
            off[gi][s] = seen[lst]
            cnt[gi][s] = len(lst)
    return flat, off, cnt


# packing is the games' own: LEVEL_UP_MOVE(level, move) == (level << 9) | move
lv_packed = {g: {s: [(l << 9) | m for l, m in lv[g][s]] for s in range(NSP)}
             for g in GAMES}
lv_flat, lv_off, lv_cnt = pool_lists(lv_packed)
egg_flat, egg_off, egg_cnt = pool_lists(egg)

for name, cnts in (("level-up", lv_cnt), ("egg", egg_cnt)):
    mx = max(max(r) for r in cnts)
    if mx > 255:
        sys.exit("FATAL: %s list of %d entries overflows the u8 count" % (name, mx))
if len(lv_flat) > 0xFFFF or len(egg_flat) > 0xFFFF:
    sys.exit("FATAL: pool larger than a u16 offset can address")


def pool_rows(per_game, width_words):
    """Dedup whole bitmask rows; per (game, species) keep a u16 row index.

    Row 0 is always the empty mask, so species with no compatibility (and the
    unused id 0) cost nothing but their index slot."""
    rows, seen = [0], {0: 0}
    ix = [[0] * NSP for _ in GAMES]
    for gi, g in enumerate(GAMES):
        for s in range(NSP):
            v = per_game[g][s]
            if v not in seen:
                seen[v] = len(rows)
                rows.append(v)
            ix[gi][s] = seen[v]
    if len(rows) > 0xFFFF:
        sys.exit("FATAL: too many distinct mask rows for a u16 index")
    return rows, ix, width_words


tm_rows, tm_ix, _ = pool_rows(tmhm, 2)
tut_rows, tut_ix, _ = pool_rows(tutor, 2)

# ---- emit ------------------------------------------------------------------
SRC_LEVELUP, SRC_TMHM, SRC_TUTOR, SRC_EGG = 1, 2, 4, 8
exact = []
for g in GAMES:
    bits = SRC_LEVELUP | SRC_EGG
    if "tmhm" not in APPROX[g]:
        bits |= SRC_TMHM
    if "tutor" not in APPROX[g]:
        bits |= SRC_TUTOR
    exact.append(bits)


def rows_c(name, ctype, data, per_line, fmt):
    out = ["static const %s %s[%d] = {" % (ctype, name, len(data))]
    for i in range(0, len(data), per_line):
        out.append("  " + "".join(fmt % v + "," for v in data[i:i + per_line]))
    out.append("};")
    return "\n".join(out) + "\n"


def table3_c(name, ctype, per_game, per_line, fmt):
    out = ["static const %s %s[3][%d] = {" % (ctype, name, NSP)]
    for gi, g in enumerate(GAMES):
        out.append("  { /* %s */" % g)
        row = per_game[gi]
        for i in range(0, NSP, per_line):
            out.append("  " + "".join(fmt % v + "," for v in row[i:i + per_line]))
        out.append("  },")
    out.append("};")
    return "\n".join(out) + "\n"


c = []
c.append("/* GENERATED by tools/gen_learnsets2.py - do not edit, do not commit.\n"
         " * Per-game-group move-source tables with learn levels (Legality V2).\n"
         " * Data provenance and the honest list of approximations: run the generator. */\n")
c.append("#define LG2_STRONG_IMPL   /* suppress learnsets2.h's weak no-data fallbacks */\n"
         '#include "learnsets2.h"\n\n')
c.append("#define NSP %d\n#define NTM %d\n#define NTUT %d\n\n" % (NSP, NTM, len(tutor_names)))

c.append("/* level-up: (level << 9) | move, ascending, pooled across (game, species) */\n")
c.append(rows_c("s_lv", "uint16_t", lv_flat, 12, "0x%04x"))
c.append(table3_c("s_lv_off", "uint16_t", lv_off, 12, "%5d"))
c.append(table3_c("s_lv_cnt", "uint8_t", lv_cnt, 20, "%3d"))
c.append("\n/* egg moves: bare move ids, pooled */\n")
c.append(rows_c("s_egg", "uint16_t", egg_flat, 12, "%4d"))
c.append(table3_c("s_egg_off", "uint16_t", egg_off, 12, "%5d"))
c.append(table3_c("s_egg_cnt", "uint8_t", egg_cnt, 20, "%3d"))

c.append("\n/* TM/HM: bit i == machine i in TM01..TM50, HM01..HM08 order; rows deduped */\n")
c.append(rows_c("s_tm_move", "uint16_t", tm_move, 12, "%4d"))
c.append("static const uint32_t s_tm_row[%d][2] = {\n" % len(tm_rows) +
         "".join("  {0x%08x,0x%08x},\n" % (v & 0xFFFFFFFF, (v >> 32) & 0xFFFFFFFF)
                 for v in tm_rows) + "};\n")
c.append(table3_c("s_tm_ix", "uint16_t", tm_ix, 12, "%5d"))

c.append("\n/* move tutors: bit i == s_tutor_move[i]; rows deduped */\n")
c.append(rows_c("s_tutor_move", "uint16_t", [MOVE[n] for n in tutor_names], 10, "%4d"))
c.append("static const uint32_t s_tut_row[%d][2] = {\n" % len(tut_rows) +
         "".join("  {0x%08x,0x%08x},\n" % (v & 0xFFFFFFFF, (v >> 32) & 0xFFFFFFFF)
                 for v in tut_rows) + "};\n")
c.append(table3_c("s_tut_ix", "uint16_t", tut_ix, 12, "%5d"))

c.append("\nstatic const uint8_t s_exact[3] = { %s };\n" % ", ".join(str(v) for v in exact))

c.append(r"""
/* ---- accessors ----------------------------------------------------------
 * Everything reads the const tables in place; no state, no buffers, pure C so
 * tests/host_learnsets2_test.c compiles the same file on the PC. */

bool lg2_have_data(void) { return true; }

uint8_t lg2_exact_sources(PkGame g) {
  return ((unsigned)g < 3u) ? s_exact[g] : 0;
}

int lg2_levelup_list(PkGame g, uint16_t species, const uint16_t** out) {
  if ((unsigned)g >= 3u || species >= NSP) { if (out) *out = 0; return 0; }
  if (out) *out = &s_lv[s_lv_off[g][species]];
  return s_lv_cnt[g][species];
}

int lg2_egg_list(PkGame g, uint16_t species, const uint16_t** out) {
  if ((unsigned)g >= 3u || species >= NSP) { if (out) *out = 0; return 0; }
  if (out) *out = &s_egg[s_egg_off[g][species]];
  return s_egg_cnt[g][species];
}

int lg2_tmhm_index(uint16_t move) {
  int i;
  if (!move) return -1;
  for (i = 0; i < NTM; i++) if (s_tm_move[i] == move) return i;
  return -1;
}

int lg2_tutor_index(uint16_t move) {
  int i;
  if (!move) return -1;
  for (i = 0; i < NTUT; i++) if (s_tutor_move[i] == move) return i;
  return -1;
}

bool lg2_tmhm(PkGame g, uint16_t species, uint16_t move) {
  int b = lg2_tmhm_index(move);
  const uint32_t* row;
  if (b < 0 || (unsigned)g >= 3u || species >= NSP) return false;
  row = s_tm_row[s_tm_ix[g][species]];
  return (row[b >> 5] >> (b & 31)) & 1u;
}

bool lg2_tutor(PkGame g, uint16_t species, uint16_t move) {
  int b = lg2_tutor_index(move);
  const uint32_t* row;
  if (b < 0 || (unsigned)g >= 3u || species >= NSP) return false;
  row = s_tut_row[s_tut_ix[g][species]];
  return (row[b >> 5] >> (b & 31)) & 1u;
}

int lg2_min_levelup_level(PkGame g, uint16_t species, uint16_t move) {
  const uint16_t* e;
  int n = lg2_levelup_list(g, species, &e), i, best = -1;
  if (!move) return -1;
  for (i = 0; i < n; i++) {
    if (LG2_LV_MOVE(e[i]) != move) continue;
    /* a species can list the same move at two levels (evolution relearns at 1) */
    if (best < 0 || LG2_LV_LEVEL(e[i]) < best) best = LG2_LV_LEVEL(e[i]);
  }
  return best;
}

int lg2_move_sources(PkGame g, uint16_t species, uint16_t move, uint8_t level,
                     Lg2Sources* out) {
  Lg2Sources r;
  const uint16_t* e;
  int n, i;
  r.srcs = 0; r.approx = 0; r.levelup_at = -1;
  if (out) *out = r;
  if ((unsigned)g >= 3u || species == 0 || species >= NSP || move == 0) return 0;

  r.levelup_at = (int16_t)lg2_min_levelup_level(g, species, move);
  /* level 0 means "ignore the level window" — the caller wants every source the
   * species has, not only the ones it could already have used. */
  if (r.levelup_at >= 0 && (level == 0 || r.levelup_at <= (int)level))
    r.srcs |= LG2_SRC_LEVELUP;

  if (lg2_tmhm(g, species, move)) r.srcs |= LG2_SRC_TMHM;
  if (lg2_tutor(g, species, move)) r.srcs |= LG2_SRC_TUTOR;

  n = lg2_egg_list(g, species, &e);
  for (i = 0; i < n; i++) if (e[i] == move) { r.srcs |= LG2_SRC_EGG; break; }

  /* Report which of the answers came from a borrowed table, so a caller can
   * downgrade a verdict to "unknown" instead of trusting an approximation. */
  r.approx = (uint8_t)((LG2_SRC_TMHM | LG2_SRC_TUTOR) & ~s_exact[g]);
  if (out) *out = r;
  return r.srcs;
}
""")

with open(OUT, "w") as f:
    f.write("".join(c))

# ---- report ----------------------------------------------------------------
size = os.path.getsize(OUT)
rom = (len(lv_flat) * 2 + 3 * NSP * 2 + 3 * NSP +          # level-up pool + off + cnt
       len(egg_flat) * 2 + 3 * NSP * 2 + 3 * NSP +         # egg pool + off + cnt
       NTM * 2 + len(tm_rows) * 8 + 3 * NSP * 2 +          # tmhm move ids + rows + index
       len(tutor_names) * 2 + len(tut_rows) * 8 + 3 * NSP * 2 + 3)

print("== sources actually used ==")
for g, kind, p in SOURCES:
    rel = os.path.relpath(p, ROOT) if p else "-- NOT ON DISK --"
    print("  %-5s %-9s %s" % (g, kind, rel))
for g in GAMES:
    if APPROX[g]:
        print("  !! %s %s approximated by OR-ing the game groups that are present "
              "(over-accepts; never false-flags)" % (g, "+".join(sorted(APPROX[g]))))

print("== cross-checks ==")
for k, v in checks:
    print("  %-44s %s" % (k, v))

print("== contents ==")
for g in GAMES:
    gi = GIDX[g]
    print("  %-5s level-up %5d entries / %3d species   egg %4d entries / %3d species   "
          "tmhm %3d species   tutor %3d species"
          % (g,
             sum(len(lv_packed[g][s]) for s in range(NSP)),
             sum(1 for s in range(NSP) if lv_packed[g][s]),
             sum(len(egg[g][s]) for s in range(NSP)),
             sum(1 for s in range(NSP) if egg[g][s]),
             sum(1 for s in range(NSP) if tmhm[g][s]),
             sum(1 for s in range(NSP) if tutor[g][s])))

print("== ROM cost ==")
print("  level-up  %6d B  (pool %d entries, deduped from %d)"
      % (len(lv_flat) * 2 + 3 * NSP * 3, len(lv_flat),
         sum(len(lv_packed[g][s]) for g in GAMES for s in range(NSP))))
print("  egg       %6d B  (pool %d entries, deduped from %d)"
      % (len(egg_flat) * 2 + 3 * NSP * 3, len(egg_flat),
         sum(len(egg[g][s]) for g in GAMES for s in range(NSP))))
print("  tmhm      %6d B  (%d distinct rows of %d bits)"
      % (NTM * 2 + len(tm_rows) * 8 + 3 * NSP * 2, len(tm_rows), NTM))
print("  tutor     %6d B  (%d distinct rows of %d bits)"
      % (len(tutor_names) * 2 + len(tut_rows) * 8 + 3 * NSP * 2, len(tut_rows), len(tutor_names)))
print("  TOTAL     %6d B  (%.1f KiB) of const ROM, 0 B EWRAM" % (rom, rom / 1024.0))
print("written:", OUT, "(%d B of C source)" % size)
