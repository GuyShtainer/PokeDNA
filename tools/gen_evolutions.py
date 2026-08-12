#!/usr/bin/env python3
"""Generate source/evolutions.c — the FORWARD evolution index (species -> what it
evolves into, by which method, at which level) and the minimum level each evolution
stage can exist at.

    python3 tools/gen_evolutions.py --from-rom          # from the repo root
    python3 tools/gen_evolutions.py --from-rom --check  # verify only, write nothing

WHY THIS TABLE EXISTS
  PokeDNA ships only the BACKWARD map: gen3_legality_hooks.c's s_preevo[184] answers
  "what did this evolve from", which is all the move/encounter checks need (a Charizard
  keeps Charmander's moves and Charmander's met location). Nothing in the repo could
  answer the other direction, and the cost was measured: a level-5 Charizard graded
  LEGAL with zero findings, because no rule knew Charmeleon does not become Charizard
  until L36. 182 of the 361 species the create flow builds as LEGAL are evolved forms
  standing at level 5. This table is what closes that.

WHY --from-rom
  Same reasoning as tools/gen_encounters.py: the emitted numbers are measured from
  cartridges Guy owns, with no decomp expression involved, which is the IP-clean route
  (docs/kb/licensing.md) AND the only source that covers Ruby/Sapphire. The pret
  checkouts under reference/ are used here ONLY as an optional cross-check.

WHAT IS READ
  gEvolutionTable — `const struct Evolution gEvolutionTable[NUM_SPECIES][EVOS_PER_MON]`.
  In the retail images the entry is EIGHT bytes, not the six the decomp's struct
  suggests: {u16 method, u16 param, u16 targetSpecies, u16 pad}, five entries per
  species, so 40 bytes per species and 412 species = 16480 bytes. That was verified by
  reading the bytes, not assumed: species 1 (Bulbasaur) decodes as
  {method 4 (EVO_LEVEL), param 16, target 2 (Ivysaur)} at stride 40 and as garbage at
  stride 30.

  The address is NOT hardcoded. It is LOCATED BY SHAPE, exactly like gen_encounters.py
  locates gWildMonHeaders:
    * every 8-byte slot must be either all zero or a well-formed link (method 1..15,
      target species 1..411, pad 0, param small enough to be a level / item id / beauty
      threshold);
    * within one species the live slots must be PACKED at the front — this is what pins
      the row boundary, i.e. the table start modulo 40, because a table read one slot
      out of phase puts a link behind an empty slot almost immediately;
    * species 0 (SPECIES_NONE) must be an entirely empty row — this is what pins the
      start absolutely, since the 40 bytes before the real table are not zero in any of
      the five carts;
    * the run must be a full 412 species and carry at least 100 links, which is what
      stops a 16 KiB field of ROM padding from validating.
  The scan runs over the WHOLE image and the script fails loudly unless exactly one
  candidate survives. The addresses in docs/research-legal-generator.md §3 are used only
  to ASSERT the scan agreed with the earlier research; they are never read as input.

WHAT IS EMITTED
  1. s_evo[] — the forward index, one row per link, sorted by source species so a
     lookup is a binary search: {from, into, param, method}.
  2. s_min[NSPECIES] — the EVOLUTION FLOOR: the lowest level at which a Pokemon of that
     species can exist, derived by walking each chain from its base form. The subtlety
     that makes it correct:
         a LEVEL-ish method raises the floor to its param
             (Charmander 1 -> Charmeleon max(1,16)=16 -> Charizard max(16,36)=36);
         a STONE / TRADE / FRIENDSHIP / BEAUTY method does NOT
             (Golbat 22 -> Crobat 22, because friendship has no level requirement;
              Eevee 1 -> Espeon 1; Chansey 1 -> Blissey 1; Feebas 1 -> Milotic 1);
         a species that evolves from nothing has floor 1 (no evolution constraint).
     Where several routes reach one species the MINIMUM is taken — the permissive
     direction, which is the only safe one for a checker.
     DELIBERATELY NOT MODELLED: friendship and beauty evolutions physically happen on
     the level-up AFTER the condition is met, so Crobat's true floor is 23, not 22. The
     +1 is not applied. Over-accepting by one level cannot call a legitimate Pokemon
     illegal; under-accepting can.
  3. s_wild[NSPECIES] — the lowest level at which that species is found WILD in any of
     the five carts (0 = never wild). This is not evolution data and it is here for one
     reason: THE EVOLUTION FLOOR ALONE IS NOT A LEGALITY RULE. The retail wild tables
     put evolved forms below their own evolution levels — measured, 10 such rows in each
     RSE cart and 84-86 in each FRLG cart (FireRed Safari Zone Poliwhirl at L20 when
     Poliwag evolves at 25; Sootopolis Super Rod Gyarados at L5 when Magikarp evolves at
     20). A checker using the evolution floor on its own would call those legitimate
     Pokemon illegal, which is the one mistake this project forbids. The rule therefore
     uses min(evolution floor, lowest wild level anywhere), and this array is that
     second term. Read out of the SAME cartridges, via tools/gen_encounters.py's
     already-proven gWildMonHeaders locator.

Output source/evolutions.c is git-ignored (generate-locally policy, same as
source/encounters.c and source/learnsets2.c). The committed API is source/evolutions.h.
"""
import argparse
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_encounters as ge          # noqa: E402  (Rom + the wild-table extractor)

DEFAULT_ROMS = ge.DEFAULT_ROMS
OUT = os.path.join(ROOT, "source", "evolutions.c")
HOOKS_C = os.path.join(ROOT, "source", "gen3_legality_hooks.c")

ROM_BASE = ge.ROM_BASE
MAX_SPECIES = ge.MAX_SPECIES       # 411 (Chimecho); matches gen_encounters/gen_legality
NSPECIES = MAX_SPECIES + 1         # 412 rows: index 0 is SPECIES_NONE

EVOS_PER_MON = 5
SLOT = 8                           # {u16 method, u16 param, u16 target, u16 pad}
ROW = EVOS_PER_MON * SLOT          # 40
TABLE_BYTES = NSPECIES * ROW       # 16480

MIN_LINKS = 100                    # a real table has 184; ROM padding has 0
MAX_METHOD = 15
MAX_PARAM = 1000                   # levels <= 100, item ids <= 376, beauty 170

# The games' own EVO_* ids (constants/pokemon.h). Only the CLASSIFICATION matters here:
# does the method impose a level, or not?
LEVELISH = {4, 8, 9, 10, 11, 12, 13, 14}   # EVO_LEVEL + the five conditional variants
METHOD_NAME = {
    1: "FRIENDSHIP", 2: "FRIENDSHIP_DAY", 3: "FRIENDSHIP_NIGHT", 4: "LEVEL",
    5: "TRADE", 6: "TRADE_ITEM", 7: "ITEM", 8: "LEVEL_ATK_GT_DEF",
    9: "LEVEL_ATK_EQ_DEF", 10: "LEVEL_ATK_LT_DEF", 11: "LEVEL_SILCOON",
    12: "LEVEL_CASCOON", 13: "LEVEL_NINJASK", 14: "LEVEL_SHEDINJA", 15: "BEAUTY",
}

# Only ever compared against the scan's own answer; never read as input. From
# docs/research-legal-generator.md §3, which located the table independently.
RESEARCH_ADDR = {
    "Emerald": 0x0832531C, "Ruby": 0x08203B80, "Sapphire": 0x08203B10,
    "FireRed": 0x082597C4, "LeafGreen": 0x08259734,
}

# A handful of chains whose floor is the whole point of the table. Asserted here so a
# silently-wrong walk cannot ship; the reasons are in the emitted file's header.
EXPECT_FLOOR = {
    6:   ("CHARIZARD", 36),   # Charmander -> L16 Charmeleon -> L36 Charizard
    169: ("CROBAT",    22),   # Zubat -> L22 Golbat -> friendship (no level) Crobat
    196: ("ESPEON",     1),   # Eevee (base, floor 1) -> friendship_day, no level
    242: ("BLISSEY",    1),   # Chansey (base, floor 1) -> friendship, no level
    329: ("MILOTIC",    1),   # Feebas (base, floor 1) -> beauty, no level
    65:  ("ALAKAZAM",  16),   # Abra -> L16 Kadabra -> trade (no level) Alakazam
    303: ("SHEDINJA",  20),   # Nincada -> L20 Shedinja (EVO_LEVEL_SHEDINJA)
    130: ("GYARADOS",  20),   # Magikarp -> L20; the wild floor overrides it separately
     94: ("GENGAR",    25),   # Gastly -> L25 Haunter -> trade; wild NOWHERE, so 25 holds
}


class Slot:
    EMPTY, LINK, BAD = 0, 1, 2


def classify(d, off):
    """One 8-byte slot, judged on SHAPE alone."""
    if off < 0 or off + SLOT > len(d):
        return Slot.BAD
    m, p, t, pad = struct.unpack_from("<4H", d, off)
    if m == 0:
        return Slot.EMPTY if (p == 0 and t == 0 and pad == 0) else Slot.BAD
    if not 1 <= m <= MAX_METHOD:
        return Slot.BAD
    if not 1 <= t <= MAX_SPECIES:
        return Slot.BAD
    if pad != 0 or p > MAX_PARAM:
        return Slot.BAD
    return Slot.LINK


def find_evolution_table(rom):
    """Locate gEvolutionTable by shape. Returns (address, link count).

    Raises unless EXACTLY ONE candidate survives, which is the point: hardcoding five
    per-cart addresses is one more thing to get silently wrong, and a shape test either
    finds the table or fails out loud.
    """
    d = rom.d

    # 1. Every offset that LOOKS like a live link. The regex is only a cheap prefilter
    #    over 16 MiB; classify() is the real predicate.
    # Zero-width lookahead on purpose: a plain pattern CONSUMES its 8 bytes, so a
    # spurious match two bytes early would swallow — and hide — the real slot behind it.
    #   method lo 1..15 | method hi 0 | param (2, any) | target lo (any) |
    #   target hi 0..1 | pad 0 0
    pat = re.compile(rb"(?=[\x01-\x0f]\x00...[\x00-\x01]\x00\x00)", re.S)
    strong = [i for i in (mm.start() for mm in pat.finditer(d))
              if i % 2 == 0 and classify(d, i) == Slot.LINK]
    if not strong:
        raise ge.RomError("%s: no evolution-link-shaped slots at all" % rom.path)

    # 2. A real table's links are dense inside one 16 KiB span. Anything sparser is
    #    coincidence, so cluster on that span and only consider crowded clusters.
    clusters, cur = [], [strong[0]]
    for off in strong[1:]:
        if off - cur[-1] <= TABLE_BYTES:
            cur.append(off)
        else:
            clusters.append(cur)
            cur = [off]
    clusters.append(cur)

    # 3. Try every u16-aligned start from one whole table before a crowded cluster to
    #    the end of it. A cluster can be WIDER than the table (the real table's links
    #    sit close enough to unrelated look-alike bytes after it that the two merge), so
    #    the sweep must cover the cluster rather than assume it is the table. validate()
    #    rejects nearly every offset on its first slice compare, which is what keeps a
    #    ~500,000-offset sweep under a second.
    found = []
    for cl in clusters:
        if len(cl) < MIN_LINKS:
            continue
        lo = max(0, cl[0] - TABLE_BYTES)
        for a in range(lo + (lo & 1), min(cl[-1], len(d) - TABLE_BYTES) + 1, 2):
            n = validate(d, a)
            if n is not None:
                found.append((ROM_BASE + a, n))

    if not found:
        raise ge.RomError("%s: no gEvolutionTable-shaped table found" % rom.path)
    if len(found) > 1:
        raise ge.RomError("%s: %d candidate tables (%s) — the shape test is ambiguous"
                          % (rom.path, len(found),
                             ", ".join("%08X/%d" % f for f in found)))
    return found[0]


ZERO_ROW = b"\0" * ROW


def validate(d, a):
    """Full-table shape check at file offset `a`. Returns the link count, or None."""
    # Species 0 (SPECIES_NONE) is an entirely empty row. This one slice compare rejects
    # essentially every offset in the sweep, and it is also what pins the start
    # ABSOLUTELY: in all five carts the 40 bytes before the table are not zero, so the
    # otherwise-plausible "one row earlier" reading dies here.
    if a < 0 or a + TABLE_BYTES > len(d) or d[a:a + ROW] != ZERO_ROW:
        return None
    links = 0
    for sp in range(1, NSPECIES):
        base = a + sp * ROW
        seen_empty = False
        for k in range(EVOS_PER_MON):
            c = classify(d, base + k * SLOT)
            if c == Slot.BAD:
                return None
            if c == Slot.EMPTY:
                seen_empty = True
            else:
                if seen_empty:
                    return None      # a live slot behind a dead one: wrong phase
                links += 1
    return links if links >= MIN_LINKS else None


def read_links(d, a):
    """[(from, method, param, into)] in table order."""
    out = []
    for sp in range(1, NSPECIES):
        for k in range(EVOS_PER_MON):
            m, p, t, _ = struct.unpack_from("<4H", d, a - ROM_BASE + sp * ROW + k * SLOT)
            if m:
                out.append((sp, m, p, t))
    return out


# ---------------------------------------------------------------------------
# The forward index -> the floor
# ---------------------------------------------------------------------------
def min_levels(links):
    """species -> lowest level it can exist at, from evolution alone (1 = unconstrained).

    Walks the chain from each base form. The recursion is memoised and depth-guarded:
    the real graph is acyclic and at most 3 deep, but this must not hang on a table that
    a future revision (or a mis-located scan) made cyclic.
    """
    back = {}
    for src, m, p, t in links:
        back.setdefault(t, []).append((src, m, p))

    floor, busy = {}, set()

    def walk(sp, depth):
        if sp in floor:
            return floor[sp]
        if sp in busy or depth > 8:
            raise ge.RomError("evolution graph is cyclic or deeper than Gen 3 allows "
                              "at species %d — the table is mis-located" % sp)
        if sp not in back:
            floor[sp] = 1
            return 1
        busy.add(sp)
        best = None
        for src, m, p in back[sp]:
            v = walk(src, depth + 1)
            if m in LEVELISH:
                v = max(v, p)          # a level method raises the floor to its param
            # every other method (stone / trade / friendship / beauty) leaves it alone
            best = v if best is None else min(best, v)   # cheapest route wins
        busy.discard(sp)
        floor[sp] = best
        return best

    return {sp: walk(sp, 0) for sp in range(NSPECIES)}


def wild_floors(roms):
    """species -> lowest wild level over every cart we have (0 = never wild).

    See the module docstring: this is the term that stops the evolution floor from
    calling a legitimately-caught Poliwhirl illegal.
    """
    w = {}
    for name, rom in roms.items():
        for (_, sp), v in ge.extract(rom)["rows"].items():
            if sp < NSPECIES and (sp not in w or v[0] < w[sp]):
                w[sp] = v[0]
    return w


# ---------------------------------------------------------------------------
# Cross-checks
# ---------------------------------------------------------------------------
def crosscheck_preevo(links, path=HOOKS_C):
    """The 184-link backward table that already SHIPS, parsed out of the committed C.

    It came from the decomps (tools/gen_legality.py:153-172); this table came from the
    carts. Two independent derivations of the same fact — any disagreement means one of
    them is wrong and neither may ship silently.
    """
    if not os.path.exists(path):
        return None
    src = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"s_preevo\[(\d+)\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        return None
    pairs = [(int(a), int(b)) for a, b in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}",
                                                     m.group(2))]
    shipped = dict(pairs)
    mine = {}
    for src_sp, _, _, tgt in links:
        mine.setdefault(tgt, src_sp)       # first link wins, as gen_legality.py does
    problems = []
    for evo, pre in sorted(shipped.items()):
        if evo not in mine:
            problems.append("s_preevo says %d <- %d; the ROM table has no link into %d"
                            % (evo, pre, evo))
        elif mine[evo] != pre:
            problems.append("species %d: s_preevo says <- %d, ROM says <- %d"
                            % (evo, pre, mine[evo]))
    for evo, pre in sorted(mine.items()):
        if evo not in shipped:
            problems.append("ROM has %d <- %d; s_preevo has no such link" % (evo, pre))
    return {"declared": int(m.group(1)), "shipped": len(shipped), "rom": len(mine),
            "problems": problems}


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------
def emit(links, floor, wild, stats, out_path):
    rows = sorted(links)                     # by source species: binary-searchable
    with open(out_path, "w") as c:
        c.write("/* GENERATED by tools/gen_evolutions.py --from-rom - do not edit.\n"
                " *\n"
                " * gEvolutionTable read out of retail cartridge dumps, turned into the\n"
                " * FORWARD index plus a per-species minimum level. See source/evolutions.h\n"
                " * for the contract and tools/gen_evolutions.py for how the table is\n"
                " * located by shape and why the wild floor is in here too.\n")
        for name in stats["order"]:
            s = stats.get(name)
            if s:
                c.write(" *   %-9s gEvolutionTable @ %08X, %d links\n"
                        % (name, s["addr"], s["links"]))
            else:
                c.write(" *   %-9s ROM not available at generation time\n" % name)
        c.write(" *   decoded tables identical across the carts read: %s\n"
                % ("yes" if stats["identical"] else "NO — SEE THE GENERATOR OUTPUT"))
        c.write(" */\n#define PK_EVOLUTIONS_IMPL\n#include \"evolutions.h\"\n\n")
        c.write("#define NSPECIES %d\n\n" % NSPECIES)

        c.write("static const PkEvoLink s_evo[%d] = {\n" % len(rows))
        for i in range(0, len(rows), 3):
            c.write("  " + " ".join("{%3d,%3d,%3d,%2d,0}," % (f, t, p, m)
                                    for f, m, p, t in rows[i:i + 3]) + "\n")
        c.write("};\n\n")

        c.write("/* Evolution floor: the lowest level this species can exist at, from the\n"
                " * evolution chain alone. 1 = no evolution constrains it. */\n")
        c.write("static const uint8_t s_min[NSPECIES] = {\n")
        for i in range(0, NSPECIES, 20):
            c.write("  " + "".join("%3d," % floor.get(sp, 1)
                                   for sp in range(i, min(i + 20, NSPECIES))) + "\n")
        c.write("};\n\n")

        c.write("/* Lowest level this species is found WILD in any cart read (0 = never).\n"
                " * The retail tables really do place evolved forms below their own\n"
                " * evolution level, so a checker must take the min of the two. */\n")
        c.write("static const uint8_t s_wild[NSPECIES] = {\n")
        for i in range(0, NSPECIES, 20):
            c.write("  " + "".join("%3d," % min(wild.get(sp, 0), 255)
                                   for sp in range(i, min(i + 20, NSPECIES))) + "\n")
        c.write("};\n\n")

        c.write(r"""bool pk_evo_have_data(void){ return true; }

int pk_evo_count(void){ return (int)(sizeof s_evo / sizeof s_evo[0]); }

const PkEvoLink* pk_evo_row(int i){
  return (i >= 0 && i < pk_evo_count()) ? &s_evo[i] : 0;
}

/* s_evo is sorted by `from`, so one binary search gives the start of a species'
 * slice and the rows after it are the rest of it. */
int pk_evo_list(uint16_t species, const PkEvoLink** out){
  int lo = 0, hi = pk_evo_count();
  while (lo < hi) { int mid = (lo + hi) >> 1;
                    if (s_evo[mid].from < species) lo = mid + 1; else hi = mid; }
  int n = 0;
  while (lo + n < pk_evo_count() && s_evo[lo + n].from == species) n++;
  if (n && out) *out = &s_evo[lo];
  return n;
}

int pk_evo_min_level(uint16_t species){
  if (species >= NSPECIES) return PK_EVO_NO_DATA;
  return (int)s_min[species];
}

int pk_evo_wild_min(uint16_t species){
  if (species >= NSPECIES) return PK_EVO_NO_DATA;
  return (int)s_wild[species];
}

int pk_evo_floor(uint16_t species){
  if (species >= NSPECIES) return PK_EVO_NO_DATA;
  int lv = (int)s_min[species], w = (int)s_wild[species];
  /* A species catchable wild BELOW its evolution level can legitimately stand there
   * (FireRed Safari Zone Poliwhirl at L20; Sootopolis Super Rod Gyarados at L5), so the
   * wild table always wins when it is lower. w == 0 means "never wild", not "level 0". */
  if (w > 0 && w < lv) lv = w;
  return lv;
}

/* These two describe the METHOD ENUM rather than the data. They are emitted here
 * because evolutions.h's weak copies are compiled out of THIS translation unit
 * (PK_EVOLUTIONS_IMPL), and a header-only definition would leave the program relying
 * on some other file happening to include the header. */
bool pk_evo_method_is_level(uint8_t method){
  return method == PK_EVO_LEVEL ||
         (method >= PK_EVO_LEVEL_ATK_GT_DEF && method <= PK_EVO_LEVEL_SHEDINJA);
}

const char* pk_evo_method_name(uint8_t method){
  switch (method) {
    case PK_EVO_FRIENDSHIP:       return "FRIENDSHIP";
    case PK_EVO_FRIENDSHIP_DAY:   return "FRIENDSHIP DAY";
    case PK_EVO_FRIENDSHIP_NIGHT: return "FRIENDSHIP NIGHT";
    case PK_EVO_LEVEL:            return "LEVEL";
    case PK_EVO_TRADE:            return "TRADE";
    case PK_EVO_TRADE_ITEM:       return "TRADE W/ ITEM";
    case PK_EVO_ITEM:             return "ITEM";
    case PK_EVO_LEVEL_ATK_GT_DEF: return "LEVEL ATK>DEF";
    case PK_EVO_LEVEL_ATK_EQ_DEF: return "LEVEL ATK=DEF";
    case PK_EVO_LEVEL_ATK_LT_DEF: return "LEVEL ATK<DEF";
    case PK_EVO_LEVEL_SILCOON:    return "LEVEL (SILCOON PID)";
    case PK_EVO_LEVEL_CASCOON:    return "LEVEL (CASCOON PID)";
    case PK_EVO_LEVEL_NINJASK:    return "LEVEL (NINJASK)";
    case PK_EVO_LEVEL_SHEDINJA:   return "LEVEL (SHEDINJA)";
    case PK_EVO_BEAUTY:           return "BEAUTY";
    default:                      return "?";
  }
}
""")

    c_bytes = len(rows) * 8 + 2 * NSPECIES
    return c_bytes


def method_hist(links):
    h = {}
    for _, m, _, _ in links:
        h[m] = h.get(m, 0) + 1
    return h


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from-rom", action="store_true",
                    help="read gEvolutionTable out of the retail dumps (the only mode)")
    ap.add_argument("--roms", default=DEFAULT_ROMS, help="directory holding <Game>.gba")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--check", action="store_true", help="verify only; write nothing")
    args = ap.parse_args()
    if not args.from_rom:
        ap.error("--from-rom is required: it is the primary (and only) source; "
                 "see the module docstring for why")

    roms, stats, decoded, missing = {}, {"order": [n for n, _ in ge.GAMES]}, {}, []
    for name, _ in ge.GAMES:
        path = os.path.join(args.roms, name + ".gba")
        if not os.path.exists(path):
            missing.append(name)
            print("  %-10s MISSING (%s)" % (name, path))
            continue
        rom = ge.Rom(path)
        addr, n = find_evolution_table(rom)
        links = read_links(rom.d, addr)
        roms[name] = rom
        decoded[name] = links
        stats[name] = {"addr": addr, "links": n}
        agree = "" if RESEARCH_ADDR.get(name) != addr else "  (agrees with §3)"
        if RESEARCH_ADDR.get(name) not in (None, addr):
            agree = "  !! research doc says %08X" % RESEARCH_ADDR[name]
        print("  %-10s %s rev %d  gEvolutionTable@%08X  %3d links%s"
              % (name, rom.code, rom.rev, addr, n, agree))

    if not decoded:
        print("no ROMs found under %s — refusing to write an empty table" % args.roms)
        return 1

    # ---- one table, not five -------------------------------------------------
    ref_name = next(iter(decoded))
    ref = decoded[ref_name]
    identical = all(v == ref for v in decoded.values())
    print("\ndecoded tables identical across %d cart(s): %s"
          % (len(decoded), "YES" if identical else "NO"))
    if not identical:
        for name, v in decoded.items():
            if v != ref:
                diff = [x for x in v if x not in ref][:5]
                print("  ! %s differs from %s, e.g. %s" % (name, ref_name, diff))
        print("  the checker rule assumes one table; refusing to write")
        return 1

    hist = method_hist(ref)
    print("methods: " + ", ".join("%s %d" % (METHOD_NAME.get(k, "?%d" % k), v)
                                  for k, v in sorted(hist.items())))

    # ---- cross-check against the pre-evolution table that already ships -------
    cc = crosscheck_preevo(ref)
    if cc is None:
        print("cross-check vs s_preevo: SKIPPED (%s not readable)" % HOOKS_C)
    else:
        print("cross-check vs s_preevo[%d] in gen3_legality_hooks.c: "
              "%d shipped links, %d ROM links, %d disagreement(s)"
              % (cc["declared"], cc["shipped"], cc["rom"], len(cc["problems"])))
        for p in cc["problems"][:40]:
            print("    !! " + p)
        if cc["problems"]:
            print("  the two derivations DISAGREE — refusing to write")
            return 1

    # ---- the floors ----------------------------------------------------------
    floor = min_levels(ref)
    bad = []
    for sp, (nm, want) in sorted(EXPECT_FLOOR.items()):
        got = floor.get(sp)
        mark = "ok" if got == want else "!! expected %d" % want
        if got != want:
            bad.append(nm)
        print("  floor %-10s (sp %3d) = %3d   %s" % (nm, sp, got, mark))
    if bad:
        print("  pinned floors are wrong: %s — refusing to write" % ", ".join(bad))
        return 1

    wild = wild_floors(roms)
    evolved = [sp for sp in range(1, NSPECIES) if floor.get(sp, 1) > 1]
    over5 = [sp for sp in evolved if min(floor[sp],
                                         wild[sp] if wild.get(sp) else 999) > 5]
    relaxed = [sp for sp in evolved if wild.get(sp, 0) and wild[sp] < floor[sp]]
    print("\n  %d species have an evolution floor above 1; after the wild relaxation "
          "%d still cannot stand at L5" % (len(evolved), len(over5)))
    print("  %d species are catchable wild BELOW their evolution floor "
          "(that is why s_wild exists)" % len(relaxed))

    stats["identical"] = identical
    if args.check:
        print("\n--check: nothing written")
        return 0

    nbytes = emit(ref, floor, wild, stats, args.out)
    print("\nevolutions.c: %d links + %d floor bytes + %d wild bytes = ~%d B of const "
          "data, zero RAM" % (len(ref), NSPECIES, NSPECIES, nbytes))
    if missing:
        print("missing ROMs: %s" % ", ".join(missing))
    print("written:", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
