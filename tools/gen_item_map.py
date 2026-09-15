#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gen_item_map.py -- generate the name-keyed Gen-2<->Gen-3 held-item map from the
tree's OWN two item-name tables (BACKLOG #150 S150-8-CORE, docs/BANK-CROSSGEN-DESIGN.md
S10 round-4 Q8, S11.20 item 10(a)).

This reads exactly two files: source/data_tables.c (the Gen-3 s_item[377] table) and
source/gb_item_names.c (the Gen-2 kGen2ItemName[GEN2_ITEM_MAX + 1] table). It NEVER opens
assets/upstream/ (the pret decomps are reference-only and unlicensed -- docs/kb/licensing.md);
a map derived from a decomp would make the shipped table decomp-derived, which is exactly
what the design note's "generated ... by NAME from the tree's own ... item name tables --
clean-room, factual data" avoids.

Modes:
    python3 tools/gen_item_map.py                 # write source/item_map_g2g3.{c,h}
    python3 tools/gen_item_map.py --check          # regenerate in memory, byte-compare
    python3 tools/gen_item_map.py --emit-stdout c  # print the .c text to stdout
    python3 tools/gen_item_map.py --emit-stdout h  # print the .h text to stdout

Output is byte-identical on any machine, any Python >= 3.9, any PYTHONHASHSEED: no
timestamp, no git hash, no machine path in the emitted banner -- the drift guard
(tests/host_itemmap_gen_test.py) compares bytes and any nondeterminism would turn it into
a flake that gets "fixed" by deleting the guard.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA_TABLES_C = ROOT / "source" / "data_tables.c"
GB_ITEM_NAMES_C = ROOT / "source" / "gb_item_names.c"
OUT_C = ROOT / "source" / "item_map_g2g3.c"
OUT_H = ROOT / "source" / "item_map_g2g3.h"

GEN3_DECL_RE = re.compile(r"static const char\* const s_item\[(\d+)\] = \{")
GEN2_MAX_RE = re.compile(r"#define\s+GEN2_ITEM_MAX\s+(0x[0-9A-Fa-f]+)")
GEN2_DECL_RE = re.compile(r"static const char\* const kGen2ItemName\[GEN2_ITEM_MAX \+ 1\] = \{")

# The exactly-two spelling aliases (decision 4 / D-Q4): applied to the NORMALISED key.
# Gen 2 spells "MAX ELIXER" (source/gb_item_names.c:167 in this tree), Gen 3 spells
# "MAX ELIXIR". One localisation typo fix for one real item -- growing this dict is
# how a "generated" table quietly becomes hand-written, so the generator asserts its
# length is exactly 2.
SPELLING_ALIAS = {
    "ELIXER": "ELIXIR",
    "MAXELIXER": "MAXELIXIR",
}

BANNER = """/* GENERATED FILE -- DO NOT EDIT BY HAND.
 * Produced by tools/gen_item_map.py from source/data_tables.c (Gen-3 s_item[])
 * and source/gb_item_names.c (Gen-2 kGen2ItemName[]). Re-run the generator to
 * refresh it; `python3 tools/gen_item_map.py --check` verifies it is current.
 * BACKLOG #150 S150-8-CORE -- see docs/BANK-CROSSGEN-DESIGN.md S10 Q8 / S11.20
 * item 10(a) for the design rationale. */
"""


def die(msg):
    print("gen_item_map: %s" % msg, file=sys.stderr)
    sys.exit(1)


def extract_block(text, start_idx, end_marker="};"):
    """Return the text from start_idx up to (not incl.) the first line that is
    exactly `};`, and the index just past that line."""
    lines = text[start_idx:].splitlines(keepends=True)
    out = []
    consumed = 0
    for line in lines:
        consumed += len(line)
        if line.strip() == end_marker:
            return "".join(out), start_idx + consumed
        out.append(line)
    die("unterminated block (no bare '%s' line found)" % end_marker)


def parse_gen3_items():
    """Parse source/data_tables.c's s_item[377] table. Returns a list of 377
    strings, index 0..376 (index 0 and every literal "????????" are not map
    candidates)."""
    text = DATA_TABLES_C.read_text(encoding="utf-8")
    m = GEN3_DECL_RE.search(text)
    if not m:
        die("could not find 'static const char* const s_item[N] = {' in %s" % DATA_TABLES_C)
    declared_size = int(m.group(1))
    block, _ = extract_block(text, m.end())
    strings = re.findall(r'"((?:[^"\\]|\\.)*)"', block)
    if len(strings) != declared_size:
        die("Gen-3 s_item[]: parsed %d strings, declared size is %d" % (len(strings), declared_size))
    return strings


def parse_gen2_items():
    """Parse source/gb_item_names.c's kGen2ItemName[GEN2_ITEM_MAX + 1] table,
    line by line. Returns a list of length GEN2_ITEM_MAX + 1 where each entry is
    either None (unused id) or a name string. Asserts each line's own id comment
    equals that line's index -- the cheap exact checksum decision 2 relies on."""
    text = GB_ITEM_NAMES_C.read_text(encoding="utf-8")
    mmax = GEN2_MAX_RE.search(text)
    if not mmax:
        die("could not find '#define GEN2_ITEM_MAX' in %s" % GB_ITEM_NAMES_C)
    gen2_max = int(mmax.group(1), 16)
    expected_count = gen2_max + 1

    mdecl = GEN2_DECL_RE.search(text)
    if not mdecl:
        die("could not find 'static const char* const kGen2ItemName[GEN2_ITEM_MAX + 1] = {' in %s" % GB_ITEM_NAMES_C)
    block, _ = extract_block(text, mdecl.end())

    entries = []
    line_re = re.compile(
        r'^\s*(?:NULL|"((?:[^"\\]|\\.)*)")\s*,\s*/\*\s*(0x[0-9A-Fa-f]+)\b[^*]*\*/\s*$'
    )
    idx = 0
    for raw_line in block.splitlines():
        if not raw_line.strip():
            continue
        lm = line_re.match(raw_line)
        if not lm:
            die("Gen-2 table line %d does not match the expected 'NULL, /* 0xNN ... */' or '\"NAME\", /* 0xNN */' shape: %r" % (idx, raw_line))
        name, id_comment = lm.group(1), lm.group(2)
        comment_id = int(id_comment, 16)
        if comment_id != idx:
            die("Gen-2 table line %d's own id comment says 0x%X -- self-check failed (inserted/deleted row?)" % (idx, comment_id))
        entries.append(name)
        idx += 1

    if len(entries) != expected_count:
        die("Gen-2 kGen2ItemName[]: parsed %d entries, GEN2_ITEM_MAX + 1 = %d" % (len(entries), expected_count))
    return entries, gen2_max


def normalise(name):
    """decision 3: replace UTF-8 'e' (bytes C3 A9) with 'E', upper-case, then
    delete every byte that is not A-Z or 0-9."""
    s = name.replace("é", "E").upper()
    return re.sub(r"[^A-Z0-9]", "", s)


def build_map(gen3_names, gen2_names, gen2_max):
    if len(SPELLING_ALIAS) != 2:
        die("SPELLING_ALIAS must have exactly 2 entries, has %d" % len(SPELLING_ALIAS))

    # Gen-3 side: index 0 and every "????????" placeholder are excluded.
    gen3_keys = {}  # normalised key -> gen3 id (first/only occurrence; collision asserted below)
    gen3_seen = {}
    for gid, name in enumerate(gen3_names):
        if gid == 0 or name == "????????":
            continue
        key = normalise(name)
        if key in gen3_seen:
            die("Gen-3 side: normalised key %r collides between id %d (%r) and id %d (%r)"
                % (key, gen3_seen[key], gen3_names[gen3_seen[key]], gid, name))
        gen3_seen[key] = gid
        gen3_keys[key] = gid

    gen2_seen = {}
    gen2_keys = {}
    for gid, name in enumerate(gen2_names):
        if name is None:
            continue
        key = normalise(name)
        key = SPELLING_ALIAS.get(key, key)
        if key in gen2_seen:
            die("Gen-2 side: normalised key %r collides between id 0x%02X (%r) and id 0x%02X (%r)"
                % (key, gen2_seen[key], gen2_names[gen2_seen[key]], gid, name))
        gen2_seen[key] = gid
        gen2_keys[key] = gid

    pairs = []  # (g2_id, g3_id)
    for key, g2_id in gen2_keys.items():
        g3_id = gen3_keys.get(key)
        if g3_id is not None:
            pairs.append((g2_id, g3_id))
    pairs.sort()

    # decision 5: TMs/HMs excluded both directions.
    for g2_id, g3_id in pairs:
        if 289 <= g3_id <= 346:
            die("mapped Gen-3 id %d (g2 0x%02X) falls in the TM/HM range [289,346]" % (g3_id, g2_id))
        g3_name = gen3_names[g3_id]
        if re.match(r"^(TM|HM)[0-9][0-9]$", g3_name):
            die("mapped Gen-3 name %r (id %d, g2 0x%02X) looks like a TM/HM label" % (g3_name, g3_id, g2_id))

    # Bijectivity self-check (belt and suspenders -- the dict construction already
    # forbids a collision on either side, so this can only fail if that logic breaks).
    g3_used = set()
    for g2_id, g3_id in pairs:
        if g3_id in g3_used:
            die("non-bijective: Gen-3 id %d is targeted by more than one Gen-2 id" % g3_id)
        g3_used.add(g3_id)

    return pairs


def cart_restricted_comment(g3_id):
    """decision 8: trailing comment on the three cart-restricted Gen-3 ids."""
    masks = {360: 0x06, 355: 0x06, 271: 0x03}
    if g3_id in masks:
        return "  /* cart-restricted: mask 0x%02X */" % masks[g3_id]
    return ""


def emit_c(pairs, gen2_max, gen3_count):
    kg2 = [0] * (gen2_max + 1)
    kg3 = [0] * gen3_count
    for g2_id, g3_id in pairs:
        kg2[g2_id] = g3_id
        kg3[g3_id] = g2_id

    out = [BANNER]
    out.append('#include "item_map_g2g3.h"\n\n')
    out.append("/* index = Gen-2 item id (0..ITEM_MAP_G2_MAX), value = Gen-3 item id, 0 = no counterpart */\n")
    out.append("static const uint16_t kG2ToG3[ITEM_MAP_G2_MAX + 1] = {\n")
    for i in range(0, len(kg2), 8):
        row = kg2[i:i + 8]
        out.append("  " + ", ".join(str(v) for v in row) + ",\n")
    out.append("};\n\n")

    out.append("/* index = Gen-3 item id (0..ITEM_MAP_G3_COUNT-1), value = Gen-2 item id, 0 = no counterpart */\n")
    out.append("static const uint8_t kG3ToG2[ITEM_MAP_G3_COUNT] = {\n")
    for i in range(0, len(kg3), 8):
        row = kg3[i:i + 8]
        out.append("  " + ", ".join(str(v) for v in row) + ",\n")
    out.append("};\n\n")

    out.append("uint16_t item_g2_to_g3(uint8_t g2_item) {\n")
    out.append("  return (g2_item <= ITEM_MAP_G2_MAX) ? kG2ToG3[g2_item] : 0u;\n")
    out.append("}\n\n")

    out.append("uint8_t item_g3_to_g2(uint16_t g3_item) {\n")
    out.append("  return (g3_item < ITEM_MAP_G3_COUNT) ? kG3ToG2[g3_item] : 0u;\n")
    out.append("}\n")
    return "".join(out)


def emit_h(pairs, gen2_max, gen3_count):
    out = [BANNER]
    out.append("#ifndef ITEM_MAP_G2G3_H\n#define ITEM_MAP_G2G3_H\n\n")
    out.append("#include <stdint.h>\n\n")
    out.append("#define ITEM_MAP_G2_MAX    0x%02Xu   /* mirrors GEN2_ITEM_MAX, gb_item_names.c:42 */\n" % gen2_max)
    out.append("#define ITEM_MAP_G3_COUNT  %uu    /* mirrors s_item[%u], data_tables.c:204    */\n" % (gen3_count, gen3_count))
    out.append("#define ITEM_MAP_PAIRS     %uu\n\n" % len(pairs))
    out.append(
        "/* 0 = \"no counterpart\" (and 0 in -> 0 out: id 0 is \"no item\" in both generations).\n"
        " * Gen 1 has no held items -- never pass a Gen-1 catch-rate byte here\n"
        " * (gen12_convert.h:163-165). Out-of-range ids return 0, never read past the table.\n"
        " *\n"
        " * The fallback contract for a Gen-2 item with NO Gen-3 counterpart (decision 7 /\n"
        " * D-Q6, implemented in S150-8, not here):\n"
        " *   - A NATIVE-home entry (gb_sidecar.h XR_KIND_NATIVE_HOME, gb_sidecar.h:121) keeps\n"
        " *     the item byte INSIDE the cell's own +40 ORIGINAL field (gb_sidecar.h:64) --\n"
        " *     that field IS the GBC1 cell for this entry kind. Read it back with\n"
        " *     bc_unpack() (bank_cell.h:172) followed by gb_get_held_item() (gb_edit.h:299);\n"
        " *     BC_FLAG_HOLDS_ITEM (bank_cell.h:98) is the cheap \"does it even hold one\" marker.\n"
        " *   - A Gen-3-HOME entry keeps its Gen-3 item id inside original80, restored by the\n"
        " *     per-field merge (S150-9).\n"
        " *   - No byte is added to the 128-byte sidecar entry for this; the loss screen names\n"
        " *     the item at draw time from gb2_item_name() (gb_item_names.h:33).\n"
        " *\n"
        " * Three of the mapped Gen-3 ids are cart-restricted (decision 8): BICYCLE->360,\n"
        " * CARD KEY->355 (FRLG+E only, mask 0x06), BASEMENT KEY->271 (RS+E only, mask 0x03).\n"
        " * This map is cart-agnostic; the caller consults pk_item_games() (data_tables.h:46)\n"
        " * before writing a mapped id into a specific cart's save -- that check is S150-8's,\n"
        " * not this table's. */\n"
    )
    out.append("uint16_t item_g2_to_g3(uint8_t  g2_item);\n")
    out.append("uint8_t  item_g3_to_g2(uint16_t g3_item);\n\n")
    out.append("#endif /* ITEM_MAP_G2G3_H */\n")
    return "".join(out)


def summary_lines(pairs, gen2_names, gen2_max, gen3_names):
    # The universe for this summary is ids 1..gen2_max (190 ids) -- id 0 is NO_ITEM
    # in both generations and is never a map candidate, so it is excluded here too.
    mapped_g2 = {g2 for g2, _ in pairs}
    named_g2 = sum(1 for n in gen2_names[1:] if n is not None)
    no_counterpart = named_g2 - len(pairs)
    unused = gen2_max - named_g2
    mapped_g3 = {g3 for _, g3 in pairs}
    named_g3 = sum(1 for n in gen3_names if n != "????????")  # id 0 is itself "????????"
    lines = []
    lines.append("%d pairs / %d no counterpart / %d unused ids" % (len(pairs), no_counterpart, unused))
    lines.append("Gen-3 side: %d named ids, %d mapped, %d not mapped" % (named_g3, len(mapped_g3), named_g3 - len(mapped_g3)))
    cart = []
    for g2_id, g3_id in pairs:
        c = cart_restricted_comment(g3_id)
        if c:
            cart.append("  0x%02X -> %d%s" % (g2_id, g3_id, c))
    if cart:
        lines.append("cart-restricted rows:")
        lines.extend(cart)
    return lines


def generate():
    gen3_names = parse_gen3_items()
    gen2_names, gen2_max = parse_gen2_items()
    pairs = build_map(gen3_names, gen2_names, gen2_max)
    c_text = emit_c(pairs, gen2_max, len(gen3_names))
    h_text = emit_h(pairs, gen2_max, len(gen3_names))
    return pairs, gen2_names, gen2_max, gen3_names, c_text, h_text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--emit-stdout", choices=["c", "h"])
    args = ap.parse_args()

    pairs, gen2_names, gen2_max, gen3_names, c_text, h_text = generate()

    if args.emit_stdout:
        sys.stdout.write(c_text if args.emit_stdout == "c" else h_text)
        return

    for line in summary_lines(pairs, gen2_names, gen2_max, gen3_names):
        print(line)

    if args.check:
        ok = True
        for out_path, text, label in ((OUT_C, c_text, "source/item_map_g2g3.c"),
                                       (OUT_H, h_text, "source/item_map_g2g3.h")):
            if not out_path.exists():
                print("item map: MISSING %s" % label)
                ok = False
                continue
            on_disk = out_path.read_text(encoding="utf-8")
            if on_disk != text:
                import difflib
                diff = list(difflib.unified_diff(
                    on_disk.splitlines(keepends=True),
                    text.splitlines(keepends=True),
                    fromfile="on-disk " + label,
                    tofile="regenerated " + label,
                ))[:20]
                print("item map: DRIFT in %s" % label)
                sys.stdout.writelines(diff)
                ok = False
        if ok:
            print("item map: OK (%d pairs)" % len(pairs))
            sys.exit(0)
        else:
            sys.exit(1)
        return

    OUT_C.write_text(c_text, encoding="utf-8")
    OUT_H.write_text(h_text, encoding="utf-8")
    print("wrote %s" % OUT_C)
    print("wrote %s" % OUT_H)


if __name__ == "__main__":
    main()
