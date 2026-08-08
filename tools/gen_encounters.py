#!/usr/bin/env python3
"""Generate source/encounters.c — the wild-encounter tables the Legality V2 met-location
checks run on (docs/research-legality-v2.md, tables E2/E8).

    python3 tools/gen_encounters.py --from-rom          # from the repo root
    python3 tools/gen_encounters.py --from-rom --check  # cross-check only, write nothing

WHY --from-rom IS THE PRIMARY SOURCE (OVERNIGHT-DECISIONS.md item 1)
  It is the only source that exists for Ruby and Sapphire: this tree has pokeemerald's
  and pokefirered's wild_encounters.json, but pokeruby's `data/` was never fetched, so a
  decomp-driven generator would have to fake RS from Emerald. Reading Guy's own carts
  gives real R and S tables — and the origin byte distinguishes the two games (1 =
  Sapphire, 2 = Ruby), so they ship separately (decision item 5) and the version
  exclusives (Seedot line vs Lotad line) become a checkable fact.
  It is also the IP-cleanest option: the emitted table is numbers measured from
  cartridges the user owns, with no decomp expression involved. The pokeemerald JSON is
  used ONLY as a cross-check here (see crosscheck_emerald), never as the source.

WHAT IS READ
  gWildMonHeaders — `const struct WildPokemonHeader gWildMonHeaders[]`, 20 bytes per
  entry: { u8 mapGroup; u8 mapNum; u16 pad; ptr land; ptr water; ptr rock; ptr fishing }
  terminated by mapGroup == 0xFF (the game's own loop breaks on
  `wildHeader->mapGroup == MAP_GROUP(MAP_UNDEFINED)`, pokeemerald src/wild_encounter.c:312).
  Each pointer is a `struct WildPokemonInfo { u8 encounterRate; u8 pad[3]; ptr slots; }`
  and each slot array is a fixed count of `struct WildPokemon { u8 min; u8 max; u16 species; }`
  — 12 land / 5 water / 5 rock-smash / 10 fishing (include/constants/wild_encounter.h:4-7,
  identical in pokefirered and pokeruby).

  The address is NOT hardcoded. It is LOCATED by shape: find the 20-byte terminator, walk
  backwards while every entry validates (map ids in range, every pointer either NULL or
  inside the image AND resolving to a WildPokemonInfo whose whole slot array is in bounds
  with sane levels and species), and require the run to be long and unique. Hardcoding
  eleven more per-revision constants would have been one more thing to get silently wrong;
  the shape check either finds exactly one table or fails loudly.

  gMapGroups IS per-version, because map group/num -> MAPSEC needs it and nothing in the
  ROM points at it. Those addresses are cribbed verbatim from source/rom_map.c:34-46
  (pret .sym files, cross-checked against the Advance Map "map bank table" address).

WHY THE OUTPUT IS KEYED ON MAPSEC
  A record's met location is a region-map SECTION, not a map: the game does
  `value = GetCurrentRegionMapSectionId(); SetBoxMonData(..., MON_DATA_MET_LOCATION, &value)`
  (src/pokemon.c:2257-2258), and that returns the map header's regionMapSectionId
  (src/overworld.c:1391-1393). Maps sharing a MAPSEC therefore MERGE into one row per
  (mapsec, species): widest level range, OR of the methods. Merging over-accepts, which is
  the only safe direction for a checker that must never call a legit Pokemon illegal.
  Altering Cave's nine alternative tables (selected by VAR_ALTERING_CAVE_WILD_SET,
  src/wild_encounter.c:318-326) merge for the same reason.

Output source/encounters.c is git-ignored (generate-locally policy, same as
source/learnsets.c and source/data_tables.c). The API it implements is the COMMITTED
source/encounters.h.
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ROMS = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"
DEFAULT_DECOMP = os.path.join(ROOT, "daycare map", "pokeemerald")
OUT = os.path.join(ROOT, "source", "encounters.c")

ROM_BASE = 0x08000000
MAX_SPECIES = 411          # Chimecho; 412 is SPECIES_EGG (matches gen_legality.py)
NSPECIES = MAX_SPECIES + 1

HEADER_SIZE = 20
SLOT_COUNTS = (12, 5, 5, 10)          # land / water / rock smash / fishing
METHOD_BITS = (0x01, 0x02, 0x04, 0x08)
METHOD_NAMES = ("land_mons", "water_mons", "rock_smash_mons", "fishing_mons")

# Origin/metGame byte -> table, per constants/game_version.h. Order matters: it is the
# order of the emitted per-game arrays.
GAMES = [("Sapphire", 1), ("Ruby", 2), ("Emerald", 3), ("FireRed", 4), ("LeafGreen", 5)]

# (game code @0xAC, revision @0xBC) -> (gMapGroups, group count).
# Verbatim from source/rom_map.c:34-46 — do not "improve" these without checking there.
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
    """A retail Gen-3 cartridge dump, opened read-only. Mirrors rom_map.c's bounds rule:
    an address is usable only if it lands inside the image."""

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

    # ---- primitives ---------------------------------------------------------
    def ok(self, addr, length=1):
        if addr < ROM_BASE:
            return False
        off = addr - ROM_BASE
        return off + length <= len(self.d)

    def u8(self, addr):
        return self.d[addr - ROM_BASE]

    def u16(self, addr):
        o = addr - ROM_BASE
        return self.d[o] | (self.d[o + 1] << 8)

    def u32(self, addr):
        o = addr - ROM_BASE
        return int.from_bytes(self.d[o:o + 4], "little")

    # ---- gMapGroups ---------------------------------------------------------
    def _derive_groups(self):
        """Per-group map counts from consecutive pointer differences — the same trick
        rom_map.c:82-109 uses, and the same reason: the counts live only in the symbol
        sizes, not in the ROM."""
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

    def mapsec(self, group, num):
        """regionMapSectionId lives at +0x14 of the map header on BOTH RSE and FRLG
        (rom_map.c:196) — one of the few fields the FRLG divergence left alone."""
        if group >= self.group_count or num >= self.gsize[group]:
            raise RomError("%s: map %d.%d is outside gMapGroups" % (self.path, group, num))
        h = self.u32(self.gptr[group] + 4 * num)
        if not self.ok(h, 0x18):
            raise RomError("%s: map header %d.%d = %08X out of bounds" % (self.path, group, num, h))
        return self.u8(h + 0x14)

    # ---- gWildMonHeaders ----------------------------------------------------
    def _info_ok(self, addr, nslots):
        """A WildPokemonInfo whose whole slot array is in bounds and plausible.
        Strict on purpose: this predicate is what tells the table apart from any other
        run of pointers in a 16 MiB image."""
        if not self.ok(addr, 8) or addr & 3:
            return False
        if not 1 <= self.u8(addr) <= 100:                     # encounterRate
            return False
        if self.d[addr - ROM_BASE + 1:addr - ROM_BASE + 4] != b"\0\0\0":
            return False
        slots = self.u32(addr + 4)
        if not self.ok(slots, 4 * nslots) or slots & 1:
            return False
        for i in range(nslots):
            lo, hi = self.u8(slots + i * 4), self.u8(slots + i * 4 + 1)
            sp = self.u16(slots + i * 4 + 2)
            if not (1 <= lo <= 100 and 1 <= hi <= 100 and lo <= hi):
                return False
            if not 1 <= sp <= MAX_SPECIES:
                return False
        return True

    def _entry_ok(self, addr):
        if not self.ok(addr, HEADER_SIZE):
            return False
        group, num = self.u8(addr), self.u8(addr + 1)
        if self.u16(addr + 2) != 0:                            # the struct's alignment pad
            return False
        if group >= self.group_count or num >= self.gsize[group]:
            return False
        live = 0
        for i, n in enumerate(SLOT_COUNTS):
            p = self.u32(addr + 4 + 4 * i)
            if p == 0:
                continue
            if not self._info_ok(p, n):
                return False
            live += 1
        return live > 0        # a header with four NULLs is not an encounter table

    def find_wild_headers(self):
        """Locate gWildMonHeaders by shape. Returns (address, entry count)."""
        term = b"\xff\xff" + b"\x00" * (HEADER_SIZE - 2)
        found = []
        i = self.d.find(term)
        while i >= 0:
            if i % 4 == 0:
                n, o = 0, i - HEADER_SIZE
                while o >= 0 and self._entry_ok(ROM_BASE + o):
                    n += 1
                    o -= HEADER_SIZE
                if n >= 30:
                    found.append((ROM_BASE + o + HEADER_SIZE, n))
            i = self.d.find(term, i + 1)
        if not found:
            raise RomError("%s: no gWildMonHeaders-shaped table found" % self.path)
        if len(found) > 1:
            raise RomError("%s: %d candidate tables (%s) — the shape test is ambiguous"
                           % (self.path, len(found), ", ".join("%08X/%d" % f for f in found)))
        return found[0]


def extract(rom):
    """(mapsec, species) -> [min, max, methods], merged over every map and slot."""
    addr, count = rom.find_wild_headers()
    rows, mapsecs = {}, set()
    for e in range(count):
        h = addr + e * HEADER_SIZE
        ms = rom.mapsec(rom.u8(h), rom.u8(h + 1))
        mapsecs.add(ms)
        for i, nslots in enumerate(SLOT_COUNTS):
            p = rom.u32(h + 4 + 4 * i)
            if not p:
                continue
            slots = rom.u32(p + 4)
            for s in range(nslots):
                lo = rom.u8(slots + s * 4)
                hi = rom.u8(slots + s * 4 + 1)
                sp = rom.u16(slots + s * 4 + 2)
                cur = rows.get((ms, sp))
                if cur is None:
                    rows[(ms, sp)] = [lo, hi, METHOD_BITS[i]]
                else:
                    cur[0] = min(cur[0], lo)
                    cur[1] = max(cur[1], hi)
                    cur[2] |= METHOD_BITS[i]
    return {"addr": addr, "headers": count, "rows": rows, "mapsecs": mapsecs}


# ---------------------------------------------------------------------------
# Cross-check: the extracted Emerald table vs pokeemerald's wild_encounters.json.
# This is the gate that authorises trusting --from-rom for Ruby/Sapphire, where no
# decomp data exists to compare against (research-legality-v2.md §7 test 4).
# ---------------------------------------------------------------------------
def _defines(path, prefix):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*#define\s+(" + prefix + r"\w+)\s+(0[xX][0-9A-Fa-f]+|\d+)\s*$", line)
        if m:
            out[m.group(1)] = int(m.group(2), 0)
    return out


def crosscheck_emerald(rom, decomp):
    if not os.path.isdir(decomp):
        return None
    enc_path = os.path.join(decomp, "src", "data", "wild_encounters.json")
    groups_path = os.path.join(decomp, "data", "maps", "map_groups.json")
    spec_path = os.path.join(ROOT, "reference", "pokeemerald_data",
                             "include", "constants", "species.h")
    secs_path = os.path.join(ROOT, "reference", "pokeemerald_data",
                             "include", "constants", "region_map_sections.h")
    for p in (enc_path, groups_path, spec_path, secs_path):
        if not os.path.exists(p):
            return None
    spec = _defines(spec_path, "SPECIES_")
    secs = _defines(secs_path, "MAPSEC_")

    # MAP_x -> (group, num) and MAP_x -> MAPSEC, straight from the checkout's map.json
    # files. Maps this checkout ADDED (the local daycare-map work has a MAP_NEWDAYCARE)
    # are dropped, or every later map in its group would be renumbered and nothing would
    # line up; total_maps below is the assertion that the drop was correct.
    mg = json.load(open(groups_path))
    idx, sect = {}, {}
    for gi, gname in enumerate(mg["group_order"]):
        num = 0
        for name in mg[gname]:
            p = os.path.join(decomp, "data", "maps", name, "map.json")
            if not os.path.exists(p):
                continue
            j = json.load(open(p))
            if j["id"] == "MAP_NEWDAYCARE":
                continue
            idx[j["id"]] = (gi, num)
            sect[j["id"]] = j.get("region_map_section")
            num += 1

    encounters = json.load(open(enc_path))["wild_encounter_groups"][0]["encounters"]
    encounters = [e for e in encounters if e["map"] != "MAP_NEWDAYCARE"]

    addr, count = rom.find_wild_headers()
    r = {"maps": len(idx), "rom_maps": rom.total_maps, "json_headers": len(encounters),
         "rom_headers": count, "slots": 0, "slots_agree": 0,
         "mapsec_checked": 0, "mapsec_agree": 0, "problems": []}
    if len(encounters) != count:
        r["problems"].append("header count: json %d vs rom %d" % (len(encounters), count))

    for i, e in enumerate(encounters[:count]):
        h = addr + i * HEADER_SIZE
        rom_map = (rom.u8(h), rom.u8(h + 1))
        if idx.get(e["map"]) != rom_map:
            r["problems"].append("entry %d %s: rom map %s vs json %s"
                                 % (i, e["map"], rom_map, idx.get(e["map"])))
            continue
        name = sect.get(e["map"])
        if name in secs:
            r["mapsec_checked"] += 1
            if rom.mapsec(*rom_map) == secs[name]:
                r["mapsec_agree"] += 1
            else:
                r["problems"].append("entry %d %s: mapsec rom %02X vs json %s=%02X"
                                     % (i, e["map"], rom.mapsec(*rom_map), name, secs[name]))
        for k, field in enumerate(METHOD_NAMES):
            p = rom.u32(h + 4 + 4 * k)
            jf = e.get(field)
            if not p or not jf:
                if bool(p) != bool(jf):
                    r["problems"].append("entry %d %s: %s present in only one source"
                                         % (i, e["map"], field))
                continue
            if rom.u8(p) != jf["encounter_rate"]:
                r["problems"].append("entry %d %s: %s rate %d vs %d"
                                     % (i, e["map"], field, rom.u8(p), jf["encounter_rate"]))
            slots = rom.u32(p + 4)
            for s in range(SLOT_COUNTS[k]):
                got = (rom.u8(slots + s * 4), rom.u8(slots + s * 4 + 1),
                       rom.u16(slots + s * 4 + 2))
                js = jf["mons"][s]
                want = (js["min_level"], js["max_level"], spec.get(js["species"], -1))
                r["slots"] += 1
                if got == want:
                    r["slots_agree"] += 1
                elif len(r["problems"]) < 40:
                    r["problems"].append("entry %d %s: %s slot %d rom %s vs json %s"
                                         % (i, e["map"], field, s, got, want))
    return r


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------
def emit(tables, stats, out_path):
    """tables: game name -> sorted [(mapsec, species, min, max, methods)]."""
    nbytes = (NSPECIES + 7) // 8
    with open(out_path, "w") as c:
        c.write("/* GENERATED by tools/gen_encounters.py --from-rom - do not edit.\n"
                " *\n"
                " * Wild-encounter rows read out of retail cartridge dumps and merged per\n"
                " * (region-map section, species). See source/encounters.h for the contract\n"
                " * and tools/gen_encounters.py for how the table is located in the ROM.\n")
        for name, _ in GAMES:
            s = stats.get(name)
            if s:
                c.write(" *   %-9s gWildMonHeaders @ %08X, %3d headers -> %4d rows, "
                        "%3d sections\n" % (name, s["addr"], s["headers"],
                                            len(s["rows"]), len(s["mapsecs"])))
            else:
                c.write(" *   %-9s ROM not available at generation time -> empty table\n" % name)
        c.write(" */\n#define PK_ENCOUNTERS_IMPL\n#include \"encounters.h\"\n\n")
        c.write("#define NSPECIES %d\n#define ANYBYTES %d\n\n" % (NSPECIES, nbytes))

        for name, _ in GAMES:
            rows = tables.get(name, [])
            sym = name.lower()
            if rows:
                c.write("static const PkWildEntry s_wild_%s[%d] = {\n" % (sym, len(rows)))
                for i in range(0, len(rows), 4):
                    c.write("  " + " ".join(
                        "{%3d,0x%02X,%3d,%3d,0x%X}," % (sp, ms, lo, hi, mt)
                        for ms, sp, lo, hi, mt in rows[i:i + 4]) + "\n")
                c.write("};\n")
                any_bits = bytearray(nbytes)
                for _, sp, _, _, _ in rows:
                    any_bits[sp >> 3] |= 1 << (sp & 7)
                c.write("static const uint8_t s_any_%s[ANYBYTES] = {%s};\n\n"
                        % (sym, ",".join("0x%02x" % b for b in any_bits)))
            else:
                c.write("/* %s: no ROM at generation time */\n\n" % name)

        c.write("typedef struct { const PkWildEntry* rows; uint16_t n; const uint8_t* any; }"
                " PkWildTable;\n")
        c.write("static const PkWildTable s_tab[6] = {\n  {0,0,0},   /* index 0 is unused:"
                " game ids start at 1 (Sapphire) */\n")
        for name, gid in GAMES:
            rows = tables.get(name, [])
            sym = name.lower()
            if rows:
                c.write("  {s_wild_%s, %d, s_any_%s},   /* %d %s */\n"
                        % (sym, len(rows), sym, gid, name))
            else:
                c.write("  {0, 0, 0},   /* %d %s - table not generated */\n" % (gid, name))
        c.write("};\n\n")

        c.write(r"""static const PkWildTable* tab(PkEncGame g){
  return (g >= PK_ENC_SAPPHIRE && g <= PK_ENC_LEAFGREEN) ? &s_tab[g] : 0;
}

bool pk_wild_have_data(void){
  for (int i = PK_ENC_SAPPHIRE; i <= PK_ENC_LEAFGREEN; i++) if (s_tab[i].n) return true;
  return false;
}

int pk_wild_count(PkEncGame g){ const PkWildTable* t = tab(g); return t ? (int)t->n : 0; }

/* The table is sorted by (mapsec, species), so one binary search answers both the
 * point lookup and the per-section slice below. */
static int lower_bound(const PkWildTable* t, uint8_t mapsec, uint16_t species){
  int lo = 0, hi = (int)t->n;
  while (lo < hi) {
    int mid = (lo + hi) >> 1;
    const PkWildEntry* e = &t->rows[mid];
    if (e->mapsec < mapsec || (e->mapsec == mapsec && e->species < species)) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

int pk_wild_at(PkEncGame g, uint8_t mapsec, uint16_t species, PkWildEntry* out){
  const PkWildTable* t = tab(g);
  if (!t || !t->n) return PK_WILD_NO_DATA;
  int i = lower_bound(t, mapsec, species);
  if (i >= (int)t->n) return PK_WILD_NO;
  const PkWildEntry* e = &t->rows[i];
  if (e->mapsec != mapsec || e->species != species) return PK_WILD_NO;
  if (out) *out = *e;
  return PK_WILD_YES;
}

int pk_wild_anywhere(PkEncGame g, uint16_t species){
  const PkWildTable* t = tab(g);
  if (!t || !t->n) return PK_WILD_NO_DATA;
  if (species >= NSPECIES) return PK_WILD_NO;
  return (t->any[species >> 3] >> (species & 7)) & 1 ? PK_WILD_YES : PK_WILD_NO;
}

uint8_t pk_wild_games(uint16_t species){
  uint8_t m = 0;
  for (int g = PK_ENC_SAPPHIRE; g <= PK_ENC_LEAFGREEN; g++)
    if (pk_wild_anywhere((PkEncGame)g, species) == PK_WILD_YES) m |= (uint8_t)(1u << (g - 1));
  return m;
}

int pk_wild_mapsec_list(PkEncGame g, uint8_t mapsec, const PkWildEntry** out){
  const PkWildTable* t = tab(g);
  if (!t || !t->n) return 0;
  int i = lower_bound(t, mapsec, 0), n = 0;
  while (i + n < (int)t->n && t->rows[i + n].mapsec == mapsec) n++;
  if (n && out) *out = &t->rows[i];
  return n;
}
""")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from-rom", action="store_true",
                    help="read gWildMonHeaders out of the retail dumps (the only mode)")
    ap.add_argument("--roms", default=DEFAULT_ROMS, help="directory holding <Game>.gba")
    ap.add_argument("--decomp", default=DEFAULT_DECOMP,
                    help="pokeemerald checkout used only to cross-check the Emerald table")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--check", action="store_true", help="cross-check only; write nothing")
    args = ap.parse_args()

    if not args.from_rom:
        ap.error("--from-rom is required: it is the primary (and currently only) source; "
                 "see the module docstring for why")

    tables, stats, missing = {}, {}, []
    total_rows = 0
    for name, _ in GAMES:
        path = os.path.join(args.roms, name + ".gba")
        if not os.path.exists(path):
            missing.append(name)
            print("  %-10s MISSING (%s) -> empty table, checks for this game stay silent"
                  % (name, path))
            continue
        rom = Rom(path)
        st = extract(rom)
        stats[name] = st
        rows = sorted((ms, sp, v[0], v[1], v[2]) for (ms, sp), v in st["rows"].items())
        tables[name] = rows
        total_rows += len(rows)
        print("  %-10s %s rev %d  gWildMonHeaders@%08X  %3d headers  %4d rows  "
              "%3d sections  %3d species  %5d B"
              % (name, rom.code, rom.rev, st["addr"], st["headers"], len(rows),
                 len(st["mapsecs"]), len({r[1] for r in rows}), len(rows) * 6))

    # Emerald is the one game with local decomp data to check against; if its extraction
    # is byte-exact there, the same code path is trustworthy for Ruby/Sapphire.
    if "Emerald" in stats:
        cc = crosscheck_emerald(Rom(os.path.join(args.roms, "Emerald.gba")), args.decomp)
        if cc is None:
            print("\ncross-check: SKIPPED (no pokeemerald checkout at %s)" % args.decomp)
        else:
            rate = 100.0 * cc["slots_agree"] / cc["slots"] if cc["slots"] else 0.0
            print("\ncross-check vs pokeemerald src/data/wild_encounters.json:")
            print("  maps %d (rom %d)  headers json %d / rom %d"
                  % (cc["maps"], cc["rom_maps"], cc["json_headers"], cc["rom_headers"]))
            print("  encounter slots %d, agreeing %d (%.4f%%)"
                  % (cc["slots"], cc["slots_agree"], rate))
            print("  map -> MAPSEC agreements %d/%d" % (cc["mapsec_agree"], cc["mapsec_checked"]))
            for p in cc["problems"][:20]:
                print("    ! " + p)
            if cc["problems"]:
                print("  %d problem(s) — the extraction is NOT clean" % len(cc["problems"]))
                return 1

    if args.check:
        print("\n--check: nothing written")
        return 0
    if not tables:
        print("no ROMs found under %s — refusing to write an empty table" % args.roms)
        return 1

    emit(tables, stats, args.out)
    data = total_rows * 6 + 5 * ((NSPECIES + 7) // 8) + 6 * 8
    print("\nencounters.c: %d rows over %d game(s), ~%d B of const data "
          "(%d B rows + %d B species bitmaps + table-of-tables)"
          % (total_rows, len(tables), data, total_rows * 6, 5 * ((NSPECIES + 7) // 8)))
    if missing:
        print("missing ROMs: %s" % ", ".join(missing))
    print("written:", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
