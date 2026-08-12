#!/usr/bin/env python3
"""Generate source/statics.c — the SCRIPT-PLACED Pokemon table.

    python3 tools/gen_statics.py --from-rom          # from the repo root
    python3 tools/gen_statics.py --from-rom --check  # cross-check only, write nothing

WHY THIS TABLE EXISTS
  The wild-encounter table (source/encounters.c) describes one way a Pokemon can be
  obtained: stepping into grass, surfing, fishing, smashing a rock. The legality
  encounter hook used to reason as if it described the ONLY way, and that is wrong in a
  way that accuses real Pokemon.

  The Kecleon are the proof. In RSE a Kecleon on Route 119/120 is an INVISIBLE object
  event revealed with the Devon Scope; battling it runs a map script, and the script
  fixes the level itself:

      data/scripts/kecleon.inc:74      setwildbattle SPECIES_KECLEON, 30
      data/maps/Route120/scripts.inc:193  setwildbattle SPECIES_KECLEON, 30

  The route's wild table says L25-25 for the same species (src/data/wild_encounters.json,
  MAP_ROUTE119/120 land_mons), because that row is the rare Kecleon that walks into the
  grass. So a legitimately caught Devon-Scope Kecleon is met at L30 in a place whose wild
  table tops out at 25 — and the hook flagged it. The route's data simply does not
  describe how that Pokemon is obtained.

  The same shape is everywhere: the Regis (L40), Groudon/Kyogre (L70 in Emerald, L45 in
  R/S), Sudowoodo (L40), the FRLG legendary birds (L50), Snorlax (L30), and the Hypno
  that has Lostelle in Berry Forest (L30). And the mirror image of it is the GIFT: the
  Johto starters, Beldum, Castform, the fossils, Lapras, Eevee, the store Magikarp.

WHAT IS READ, AND HOW IT IS FOUND
  Two script-engine commands, whose byte layout is fixed by the macros in
  `daycare map/pokeemerald/asm/macros/event.inc`:

      setwildbattle  (opcode 0xB6, 6 bytes)   B6 <u16 species> <u8 level> <u16 item>
      givemon        (opcode 0x79, 15 bytes)  79 <u16 species> <u8 level> <u16 item>
                                              <4 zero bytes> <4 zero bytes> <1 zero byte>

  (Opcodes from data/script_cmd_table.inc:139 and :200. The same numbering holds in
  FRLG — see the SELF-CHECK section below for how that is demonstrated rather than
  assumed.)

  The addresses are NOT hardcoded. A raw byte scan of a 16 MiB image finds the real
  commands but also a couple of dozen coincidences in graphics and audio data, so the
  scan is confined to the EVENT-SCRIPT REGION, which is itself derived from the ROM: walk
  gMapGroups -> every map header -> its mapScripts pointer and the script pointer of
  every object / coord / background event, and take the range those pointers span. Every
  real placement lives in that span (it is where the linker puts the .inc files) and
  every coincidence measured on all five carts lives outside it.

  gMapGroups IS per-version and is cribbed verbatim from source/rom_map.c:34-46, exactly
  as tools/gen_encounters.py does — nothing in the ROM points at it.

THE GATE THAT AUTHORISES TRUSTING THE OTHER FOUR CARTS
  For Emerald this tree has the full pokeemerald checkout, so the generator greps every
  data/**/*.inc for setwildbattle/givemon and requires the ROM scan to produce EXACTLY
  the same multiset of (species, level, command) — no missing entry, no extra one. If a
  byte scan reproduces a decomp's script data exactly on the one game where that can be
  measured, the same scan is trustworthy on the four where it cannot. (Same argument,
  same shape, as crosscheck_emerald in tools/gen_encounters.py.)

  SELF-CHECK on the other carts, printed for the reader rather than asserted: the result
  is version-correct in ways a wrong opcode could not fake. Ruby yields GROUDON L45 and
  no Kyogre; Sapphire yields KYOGRE L45 and no Groudon; Emerald yields both at L70 (Terra
  Cave / Marine Cave) plus Sudowoodo; FireRed and LeafGreen yield the three birds at L50,
  Mewtwo at L70 and Snorlax at L30. Those are the games' own facts.

WHAT THIS TABLE DELIBERATELY DOES NOT CLAIM
  - THE MAP. A Pokemon's met location is a region-map SECTION, and attributing a script
    byte to a section would need a full script walker that follows goto/call across the
    shared scripts in data/scripts/ (kecleon.inc is reached from four different maps).
    A wrong map here would produce a wrong verdict, so the table stores no map at all and
    the hook uses it only to fall SILENT — never to accuse.
  - COMPLETENESS OVER EVERY ACQUISITION ROUTE. Pokemon handed out by C code rather than
    by a script command are not here: the roamers (CreateRoamerMon), and the FRLG Game
    Corner prizes (Abra/Clefairy/Dratini/Scyther-or-Pinsir/Porygon), which is why a
    Celadon-City Dratini is still the one script-shaped false positive left in the corpus.
    Eggs and in-game trades need no entry — met level 0 and met location 0xFE already
    exempt them.

Output source/statics.c is git-ignored (generate-locally policy, same as
source/encounters.c / source/learnsets2.c). The API it implements is the COMMITTED
source/statics.h.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ROMS = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"
DEFAULT_DECOMP = os.path.join(ROOT, "daycare map", "pokeemerald")
OUT = os.path.join(ROOT, "source", "statics.c")

ROM_BASE = 0x08000000
MAX_SPECIES = 411          # Chimecho; 412 is SPECIES_EGG (matches gen_encounters.py)
MAX_ITEM = 376             # ITEMS_COUNT-1 in Emerald, the widest of the five

OP_SETWILDBATTLE = 0xB6    # data/script_cmd_table.inc:200
OP_GIVEMON       = 0x79    # data/script_cmd_table.inc:139

# The two kinds, mirrored in source/statics.h.
K_BATTLE = 1
K_GIFT   = 2
KIND_NAME = {K_BATTLE: "setwildbattle", K_GIFT: "givemon"}

GAMES = [("Sapphire", 1), ("Ruby", 2), ("Emerald", 3), ("FireRed", 4), ("LeafGreen", 5)]

# (game code @0xAC, revision @0xBC) -> (gMapGroups, group count).
# Verbatim from source/rom_map.c:34-46 via tools/gen_encounters.py — keep the three in
# step; do not "improve" these without checking there.
VERSIONS = {
    ("BPEE", 0): (0x08486578, 34),
    ("AXVE", 0): (0x08308588, 34),
    ("AXVE", 1): (0x083085A0, 34),
    ("AXVE", 2): (0x083085A0, 34),
    ("AXPE", 0): (0x08308518, 34),
    ("AXPE", 1): (0x08308530, 34),
    ("AXPE", 2): (0x08308530, 34),
    ("BPRE", 0): (0x083526A8, 43),
    ("BPRE", 1): (0x08352718, 43),
    ("BPGE", 0): (0x08352688, 43),
    ("BPGE", 1): (0x083526F8, 43),
}


class RomError(Exception):
    pass


class Rom:
    """A retail Gen-3 cartridge dump, opened read-only."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        self.path = path
        if len(self.d) != 16 * 1024 * 1024:
            raise RomError("%s: %d bytes, not a 16 MiB retail image" % (path, len(self.d)))
        if self.d[0xB2] != 0x96:
            raise RomError("%s: missing the 0x96 GBA header byte" % path)
        self.code = self.d[0xAC:0xB0].decode("ascii", "replace")
        self.rev = self.d[0xBC]
        if (self.code, self.rev) not in VERSIONS:
            raise RomError("%s: unknown build %s rev %d" % (path, self.code, self.rev))
        self.map_groups, self.group_count = VERSIONS[(self.code, self.rev)]
        self._derive_groups()

    def ok(self, addr, length=1):
        if addr < ROM_BASE:
            return False
        return addr - ROM_BASE + length <= len(self.d)

    def u8(self, a):
        return self.d[a - ROM_BASE]

    def u16(self, a):
        o = a - ROM_BASE
        return self.d[o] | (self.d[o + 1] << 8)

    def u32(self, a):
        o = a - ROM_BASE
        return int.from_bytes(self.d[o:o + 4], "little")

    def _derive_groups(self):
        """Per-group map counts from consecutive pointer differences — the trick
        rom_map.c:82-109 uses, because the counts live only in the symbol sizes."""
        self.gptr, self.gsize = [], []
        for i in range(self.group_count):
            p = self.u32(self.map_groups + 4 * i)
            if not self.ok(p, 4):
                raise RomError("%s: gMapGroups[%d] = %08X out of bounds" % (self.path, i, p))
            self.gptr.append(p)
        for i in range(self.group_count):
            end = self.gptr[i + 1] if i + 1 < self.group_count else self.map_groups
            if end <= self.gptr[i] or (end - self.gptr[i]) & 3:
                raise RomError("%s: map group %d is not a whole pointer array" % (self.path, i))
            self.gsize.append((end - self.gptr[i]) >> 2)
        self.total_maps = sum(self.gsize)

    # ---- the event-script region -------------------------------------------
    def script_region(self):
        """Span of every script pointer any map header can reach.

        Map header layout is identical in RSE and FRLG for the fields used here
        (+0x04 events, +0x08 mapScripts; rom_map.c:196 relies on the same stability for
        +0x14). MapEvents is {u8 nObj, u8 nWarp, u8 nCoord, u8 nBg, ptr obj, ptr warp,
        ptr coord, ptr bg}; an object event is 24 bytes with its script at +0x10, a coord
        event 16 bytes with its script at +0x08, a bg event 12 bytes with its script (or
        an item id, for the hidden-item kind — harmless, it just widens nothing) at +0x08.

        A malformed pointer is skipped rather than fatal: this is a BOUND, and the only
        thing that matters is that it contains every real script. The count is returned so
        the caller can refuse a region derived from almost nothing."""
        ptrs = []
        for g in range(self.group_count):
            for m in range(self.gsize[g]):
                h = self.u32(self.gptr[g] + 4 * m)
                if not self.ok(h, 0x18):
                    continue
                ms = self.u32(h + 0x08)
                if ms and self.ok(ms):
                    ptrs.append(ms)
                ev = self.u32(h + 0x04)
                if not self.ok(ev, 0x14):
                    continue
                counts = (self.u8(ev), self.u8(ev + 2), self.u8(ev + 3))     # obj, coord, bg
                bases  = (self.u32(ev + 0x04), self.u32(ev + 0x0C), self.u32(ev + 0x10))
                stride = (24, 16, 12)
                inner  = (0x10, 0x08, 0x08)
                for n, base, st, off in zip(counts, bases, stride, inner):
                    if not n or not self.ok(base, st * n):
                        continue
                    for i in range(n):
                        p = self.u32(base + st * i + off)
                        if p and self.ok(p):
                            ptrs.append(p)
        if len(ptrs) < 500:
            raise RomError("%s: only %d script pointers reachable — gMapGroups is wrong"
                           % (self.path, len(ptrs)))
        return min(ptrs), max(ptrs), len(ptrs)

    # ---- the scan -----------------------------------------------------------
    def scan(self, lo, hi):
        """[(species, level, kind, address)] for every command in [lo, hi]."""
        hits = []
        for a in range(lo, hi + 1):
            b = self.u8(a)
            if b != OP_SETWILDBATTLE and b != OP_GIVEMON:
                continue
            if not self.ok(a, 15):
                continue
            sp, lvl, item = self.u16(a + 1), self.u8(a + 3), self.u16(a + 4)
            if not (1 <= sp <= MAX_SPECIES and 1 <= lvl <= 100 and item <= MAX_ITEM):
                continue
            if b == OP_GIVEMON:
                # The macro's nine trailing zero bytes (event.inc:1040-1042). They are
                # what makes the givemon scan noise-free: measured over all five carts,
                # this test alone leaves zero coincidences even before the region filter.
                if self.d[a - ROM_BASE + 6:a - ROM_BASE + 15] != b"\0" * 9:
                    continue
                hits.append((sp, lvl, K_GIFT, a))
            else:
                hits.append((sp, lvl, K_BATTLE, a))
        return hits


# ---------------------------------------------------------------------------
# The Emerald gate: the ROM scan must reproduce the decomp's script sources exactly.
# ---------------------------------------------------------------------------
def _defines(path, prefix):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*#define\s+(" + prefix + r"\w+)\s+(0[xX][0-9A-Fa-f]+|\d+)\s*$",
                     line.rstrip())
        if m:
            out[m.group(1)] = int(m.group(2), 0)
    return out


def species_names():
    p = os.path.join(ROOT, "reference", "pokeemerald_data", "include", "constants", "species.h")
    if not os.path.exists(p):
        return {}
    names = {}
    for k, v in _defines(p, "SPECIES_").items():
        names.setdefault(v, k[len("SPECIES_"):])
    return names


def decomp_truth(decomp):
    """Multiset {(species, level, kind): count} from every data/**/*.inc in the checkout."""
    spec_h = os.path.join(ROOT, "reference", "pokeemerald_data", "include",
                          "constants", "species.h")
    data = os.path.join(decomp, "data")
    if not os.path.isdir(data) or not os.path.exists(spec_h):
        return None
    spec = _defines(spec_h, "SPECIES_")
    truth = {}
    for root, _, files in os.walk(data):
        for fn in files:
            if not fn.endswith(".inc"):
                continue
            for line in open(os.path.join(root, fn), encoding="utf-8", errors="replace"):
                m = re.match(r"\s*(setwildbattle|givemon)\s+(SPECIES_\w+)\s*,\s*(\d+)", line)
                if not m:
                    continue
                sp = spec.get(m.group(2))
                if sp is None:
                    continue
                k = K_BATTLE if m.group(1) == "setwildbattle" else K_GIFT
                key = (sp, int(m.group(3)), k)
                truth[key] = truth.get(key, 0) + 1
    return truth


def crosscheck_emerald(hits, decomp, names):
    truth = decomp_truth(decomp)
    if truth is None:
        return None
    got = {}
    for sp, lvl, k, _ in hits:
        got[(sp, lvl, k)] = got.get((sp, lvl, k), 0) + 1
    problems = []
    for key in sorted(set(truth) | set(got)):
        sp, lvl, k = key
        if truth.get(key, 0) != got.get(key, 0):
            problems.append("%-12s L%-3d %-13s decomp x%d vs rom x%d"
                            % (names.get(sp, "#%d" % sp), lvl, KIND_NAME[k],
                               truth.get(key, 0), got.get(key, 0)))
    return {"truth": truth, "got": got, "problems": problems}


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------
def emit(tables, stats, out_path, names):
    nbytes = (MAX_SPECIES + 1 + 7) // 8
    with open(out_path, "w") as c:
        c.write("/* GENERATED by tools/gen_statics.py --from-rom - do not edit.\n"
                " *\n"
                " * Pokemon a MAP SCRIPT places (setwildbattle) or hands over (givemon), read\n"
                " * out of retail cartridge dumps. See source/statics.h for the contract and\n"
                " * tools/gen_statics.py for how the commands are located in the ROM.\n"
                " *\n"
                " * These are the Pokemon the wild-encounter table cannot describe, and the\n"
                " * whole point of the table is to make the legality encounter hook stay\n"
                " * SILENT about them instead of accusing them.\n")
        for name, _ in GAMES:
            s = stats.get(name)
            if not s:
                c.write(" *   %-9s ROM not available at generation time -> empty table\n" % name)
                continue
            c.write(" *\n *   %-9s scripts %08X..%08X -> %d placement(s):\n"
                    % (name, s["lo"], s["hi"], len(tables[name])))
            for sp, lvl, k in tables[name]:
                c.write(" *      %-13s L%-3d  %s\n" % (names.get(sp, "#%d" % sp), lvl,
                                                       KIND_NAME[k]))
        c.write(" */\n#define PK_STATICS_IMPL\n#include \"statics.h\"\n\n")
        c.write("#define NSPECIES %d\n#define ANYBYTES %d\n\n" % (MAX_SPECIES + 1, nbytes))

        for name, _ in GAMES:
            rows = tables.get(name, [])
            sym = name.lower()
            if not rows:
                c.write("/* %s: no ROM at generation time */\n\n" % name)
                continue
            c.write("static const PkStaticEntry s_st_%s[%d] = {\n" % (sym, len(rows)))
            for i in range(0, len(rows), 4):
                c.write("  " + " ".join("{%3d,%3d,%d}," % (sp, lvl, k)
                                        for sp, lvl, k in rows[i:i + 4]) + "\n")
            c.write("};\n")
            bits = bytearray(nbytes)
            for sp, _, _ in rows:
                bits[sp >> 3] |= 1 << (sp & 7)
            c.write("static const uint8_t s_any_%s[ANYBYTES] = {%s};\n\n"
                    % (sym, ",".join("0x%02x" % b for b in bits)))

        c.write("typedef struct { const PkStaticEntry* rows; uint16_t n; const uint8_t* any; }"
                " PkStaticTable;\n")
        c.write("static const PkStaticTable s_tab[6] = {\n  {0,0,0},   /* index 0 is unused:"
                " game ids start at 1 (Sapphire) */\n")
        for name, gid in GAMES:
            rows = tables.get(name, [])
            sym = name.lower()
            if rows:
                c.write("  {s_st_%s, %d, s_any_%s},   /* %d %s */\n"
                        % (sym, len(rows), sym, gid, name))
            else:
                c.write("  {0, 0, 0},   /* %d %s - table not generated */\n" % (gid, name))
        c.write("};\n\n")

        c.write(r"""static const PkStaticTable* tab(uint8_t g){
  return (g >= 1 && g <= 5) ? &s_tab[g] : 0;
}

bool pk_static_have_data(void){
  for (int i = 1; i <= 5; i++) if (s_tab[i].n) return true;
  return false;
}

int pk_static_count(uint8_t g){ const PkStaticTable* t = tab(g); return t ? (int)t->n : 0; }

int pk_static_any(uint8_t g, uint16_t species){
  const PkStaticTable* t = tab(g);
  if (!t || !t->n) return PK_STATIC_NO_DATA;
  if (species >= NSPECIES) return PK_STATIC_NO;
  return (t->any[species >> 3] >> (species & 7)) & 1 ? PK_STATIC_YES : PK_STATIC_NO;
}

/* The rows are sorted by (species, level), so a linear walk of one species' slice is
 * bounded by the handful of placements it has. The table is ~20 rows per game; a binary
 * search would be more code than the scan it saves. */
int pk_static_list(uint8_t g, uint16_t species, const PkStaticEntry** out){
  const PkStaticTable* t = tab(g);
  if (!t || !t->n) return 0;
  int i = 0, n = 0;
  while (i < (int)t->n && t->rows[i].species < species) i++;
  while (i + n < (int)t->n && t->rows[i + n].species == species) n++;
  if (n && out) *out = &t->rows[i];
  return n;
}

int pk_static_at_level(uint8_t g, uint16_t species, uint8_t level){
  const PkStaticTable* t = tab(g);
  if (!t || !t->n) return PK_STATIC_NO_DATA;
  const PkStaticEntry* r = 0;
  int n = pk_static_list(g, species, &r);
  for (int i = 0; i < n; i++) if (r[i].level == level) return PK_STATIC_YES;
  return PK_STATIC_NO;
}
""")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from-rom", action="store_true",
                    help="read the script commands out of the retail dumps (the only mode)")
    ap.add_argument("--roms", default=DEFAULT_ROMS, help="directory holding <Game>.gba")
    ap.add_argument("--decomp", default=DEFAULT_DECOMP,
                    help="pokeemerald checkout used to GATE the Emerald extraction")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--check", action="store_true", help="cross-check only; write nothing")
    args = ap.parse_args()

    if not args.from_rom:
        ap.error("--from-rom is required: the cartridge is the source; see the docstring")

    names = species_names()
    tables, stats, missing, all_hits = {}, {}, [], {}
    for name, _ in GAMES:
        path = os.path.join(args.roms, name + ".gba")
        if not os.path.exists(path):
            missing.append(name)
            print("  %-10s MISSING (%s) -> empty table, the hook stays silent for this game"
                  % (name, path))
            continue
        rom = Rom(path)
        lo, hi, nptr = rom.script_region()
        hits = rom.scan(lo, hi)
        all_hits[name] = hits
        rows = sorted({(sp, lvl, k) for sp, lvl, k, _ in hits})
        tables[name] = rows
        stats[name] = {"lo": lo, "hi": hi, "nptr": nptr, "hits": len(hits)}
        nb = sum(1 for r in rows if r[2] == K_BATTLE)
        print("  %-10s %s rev %d  scripts %08X..%08X (%d ptrs)  %d command(s) -> "
              "%d row(s) (%d battle / %d gift), %d species"
              % (name, rom.code, rom.rev, lo, hi, nptr, len(hits), len(rows),
                 nb, len(rows) - nb, len({r[0] for r in rows})))
        for sp, lvl, k in rows:
            print("        %-13s L%-3d  %s" % (names.get(sp, "#%d" % sp), lvl, KIND_NAME[k]))

    # THE GATE. Emerald is the one cart with a full decomp in this tree; if the byte scan
    # reproduces its script sources exactly, the same scan is trustworthy on the four
    # carts where nothing exists to compare against.
    if "Emerald" in all_hits:
        cc = crosscheck_emerald(all_hits["Emerald"], args.decomp, names)
        if cc is None:
            print("\ngate: SKIPPED (no pokeemerald checkout at %s) — NOT writing a table\n"
                  "      that nothing verified. Point --decomp at a checkout." % args.decomp)
            return 1
        print("\ngate vs pokeemerald data/**/*.inc: %d command(s) in the decomp, "
              "%d distinct (species, level, kind)"
              % (sum(cc["truth"].values()), len(cc["truth"])))
        if cc["problems"]:
            for p in cc["problems"]:
                print("    ! " + p)
            print("  %d mismatch(es) — the extraction is NOT clean, refusing to write"
                  % len(cc["problems"]))
            return 1
        print("  EXACT MATCH: every setwildbattle/givemon in the checkout, and nothing else.")
    else:
        print("\ngate: SKIPPED (no Emerald.gba) — refusing to write an ungated table")
        return 1

    if args.check:
        print("\n--check: nothing written")
        return 0

    emit(tables, stats, args.out, names)
    total = sum(len(v) for v in tables.values())
    print("\nstatics.c: %d rows over %d game(s), ~%d B of const data "
          "(%d B rows + %d B species bitmaps + table-of-tables)"
          % (total, len(tables), total * 4 + 5 * ((MAX_SPECIES + 1 + 7) // 8) + 6 * 8,
             total * 4, 5 * ((MAX_SPECIES + 1 + 7) // 8)))
    if missing:
        print("missing ROMs: %s" % ", ".join(missing))
    print("written:", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
