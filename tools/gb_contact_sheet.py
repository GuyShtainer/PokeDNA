#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gb_contact_sheet.py — composite tools/gb_shots.py's output into contact sheet(s).

Combined sheet (unchanged default — this is what has always run):
    /usr/local/bin/python3 tools/gb_contact_sheet.py \
        --shots docs/shots/gb --out docs/contact-sheet-2026-09-05-gb-arc.png

Per-feature sheets for the status page (Guy: "keep updating the screenshots sorted
per feature"):
    /usr/local/bin/python3 tools/gb_contact_sheet.py --per-feature

Reads docs/shots/gb/manifest.json (written by gb_shots.py) so a caption lives in
exactly one place. Grid: 3 columns, each frame scaled 2x nearest-neighbour (GBA
screens are 240x160; this keeps them crisp, not blurred), a caption band under each.

PER-FEATURE MODE
-----------------
Every gb_shots.py caption carries a feature prefix before its first colon — "S1:",
"S2:"/"S2b:", "S3:", "S5-B:", "BACKLOG #41:" — because that script narrates each
screen against docs/FEATURE-MATRIX.md's own rows. FEATURE_TABLE below is the one
place that prefix -> feature mapping lives; entries whose caption doesn't start
with a known prefix (e.g. the "Gen-3 parity: ..." shots, which are real screens but
don't carry one of the tracked prefixes) fall into "misc" rather than being dropped.
docs/shots/bag/*.png (the before/after PNGs from 8fe3dc8) have no manifest of their
own — BAG_DIR is read directly and captioned from the filename — but are folded into
the same per-feature output as the "bag" feature so the status page has one place to
look for every feature's evidence.

Output: one PNG per feature at docs/contact-sheets/<feature-id>.png, plus
docs/contact-sheets/index.json = [{id, title, file, count, updated}, ...] in
FEATURE_TABLE's own order (not capture order, and not alphabetical) — the order
tools/gen_matrix_html.py renders the Screenshots section in.
"""
from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
GB_SHOTS_DIR = ROOT / "docs" / "shots" / "gb"
BAG_DIR = ROOT / "docs" / "shots" / "bag"
CONTACT_SHEETS_DIR = ROOT / "docs" / "contact-sheets"

SCALE = 2
COLS = 3
CAPTION_H = 46
PAD = 10
BG = (13, 17, 23)
FG = (201, 209, 217)
BORDER = (48, 54, 61)

# (feature id, human title, caption prefixes that map to it). Order here IS the
# order docs/contact-sheets/index.json lists features in and gen_matrix_html.py
# renders them in — chosen to match docs/FEATURE-MATRIX.md's own row order
# (S1 -> S2/S2a/S2b -> S3 -> bag -> #41/#40 summary -> S5-B), not capture order.
# "bag" has no manifest prefix of its own: its shots come from BAG_DIR, not from
# any caption match (see build_bag_entries()).
FEATURE_TABLE: list[tuple[str, str, tuple[str, ...]]] = [
    ("s1-open", "S1 — open a Game Boy save", ("S1",)),
    ("s2-edit", "S2 — the editor", ("S2", "S2a", "S2b")),
    ("s3-move-release", "S3 — MOVE TO / RELEASE", ("S3",)),
    ("bag", "Bag — the first-letter erase-column fix", ()),
    ("summary", "#41 — the native summary editor (E1: Gen-3 card design)",
     ("BACKLOG #41", "BACKLOG #40", "#41", "#40", "BACKLOG #41 slice E1", "E1", "Gen-3 parity")),
    ("s5b-transfer", "S5-B — Gen-3-to-GB transfer (PASTE)", ("S5-B",)),
    ("e4-settings", "E4 — the Settings > Sprites era grid (docs/SPRITE-ERA-DESIGN.md)",
     ("E4",)),
    ("e5-icons", "E5 — Gen-2 party/PC menu icons, real ROM art (docs/SPRITE-ERA-DESIGN.md)",
     ("E5",)),
    ("misc", "Misc — captions without a tracked feature prefix", ()),
]
FEATURE_ORDER = [fid for fid, _title, _prefixes in FEATURE_TABLE]
FEATURE_TITLE = {fid: title for fid, title, _prefixes in FEATURE_TABLE}
_PREFIX_TO_FEATURE = {
    prefix: fid for fid, _title, prefixes in FEATURE_TABLE for prefix in prefixes
}


def feature_for_caption(caption: str) -> str:
    """The caption's own prefix (everything before its first colon), looked up in
    _PREFIX_TO_FEATURE. No match (including no colon at all) -> "misc"."""
    prefix = caption.split(":", 1)[0].strip()
    return _PREFIX_TO_FEATURE.get(prefix, "misc")


def wrap(text: str, font: ImageFont.ImageFont, max_w: int, draw: ImageDraw.ImageDraw) -> list[str]:
    words = text.split()
    lines, cur = [], ""
    for w in words:
        trial = (cur + " " + w).strip()
        if draw.textlength(trial, font=font) <= max_w or not cur:
            cur = trial
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def load_font() -> ImageFont.ImageFont:
    try:
        return ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 13)
    except OSError:
        return ImageFont.load_default()


def render_sheet(entries: list[tuple[Path, str]], out_path: Path, *,
                  scale: int = SCALE, cols: int = COLS,
                  font: ImageFont.ImageFont | None = None) -> dict:
    """entries: [(path_to_png, caption), ...], already in the order they should
    appear on the sheet. Writes out_path and returns a small stats dict (also used
    by --per-feature to fill docs/contact-sheets/index.json)."""
    if font is None:
        font = load_font()
    frame_w, frame_h = 240 * scale, 160 * scale
    cell_w, cell_h = frame_w + PAD * 2, frame_h + CAPTION_H + PAD * 2
    rows = (len(entries) + cols - 1) // cols
    sheet = Image.new("RGB", (cell_w * cols, cell_h * rows), BG)
    draw = ImageDraw.Draw(sheet)

    missing = []
    for i, (path, caption) in enumerate(entries):
        cx, cy = (i % cols) * cell_w, (i // cols) * cell_h
        if not path.is_file():
            missing.append(str(path))
            draw.rectangle([cx + PAD, cy + PAD, cx + PAD + frame_w, cy + PAD + frame_h],
                            outline=(248, 81, 73))
            draw.text((cx + PAD + 6, cy + PAD + 6), "MISSING\n" + path.name,
                      fill=(248, 81, 73), font=font)
            continue
        img = Image.open(path).convert("RGB")
        img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
        sheet.paste(img, (cx + PAD, cy + PAD))
        draw.rectangle([cx + PAD, cy + PAD, cx + PAD + img.width - 1, cy + PAD + img.height - 1],
                        outline=BORDER)
        ty = cy + PAD + frame_h + 4
        for line in wrap(caption, font, frame_w, draw)[:3]:
            draw.text((cx + PAD, ty), line, fill=FG, font=font)
            ty += 15

    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_path)
    return {
        "width": sheet.width, "height": sheet.height,
        "bytes": out_path.stat().st_size,
        "count": len(entries), "rows": rows, "missing": missing,
    }


def load_manifest(shots_dir: Path) -> dict:
    manifest_path = shots_dir / "manifest.json"
    if not manifest_path.is_file():
        sys.exit(f"{manifest_path}: missing — run tools/gb_shots.py first")
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def manifest_entries_sorted(manifest: dict, shots_dir: Path) -> list[tuple[Path, str]]:
    # gb_shots.py's manifest is in CAPTURE order, not narrative order -- run_gold()
    # visits MOVE TO (07) and RELEASE (08) before it visits the editor (04/05/06),
    # since parking on Bulbasaur once and walking the mon menu twice is cheaper than
    # re-opening it three times in narrative order. Sort by filename (which carries
    # the narrative numbers) so the sheet reads S1 -> S2 -> S2b -> S3 -> S5-B, not the
    # order the emulator happened to visit them in.
    shots = sorted(manifest["shots"], key=lambda e: e["file"])
    return [(shots_dir / e["file"], e["caption"]) for e in shots]


def build_bag_entries(bag_dir: Path) -> list[tuple[Path, str]]:
    """docs/shots/bag has no manifest.json — caption each PNG from its own filename."""
    entries = []
    for path in sorted(bag_dir.glob("*.png")):
        caption = "bag: " + path.stem.replace("_", " ")
        entries.append((path, caption))
    return entries


def cmd_combined(a: argparse.Namespace) -> int:
    manifest = load_manifest(a.shots)
    shots = manifest_entries_sorted(manifest, a.shots)
    if not shots:
        sys.exit("manifest has zero shots — nothing to composite")

    font = load_font()
    stats = render_sheet(shots, a.out, scale=a.scale, font=font)
    print(f"{a.out}  ({stats['width']}x{stats['height']}, {stats['bytes']:,} bytes, "
          f"{stats['count']} shot(s), {stats['rows']} row(s))")
    if stats["missing"]:
        print("MISSING (drawn as red placeholders): " + ", ".join(stats["missing"]),
              file=sys.stderr)
        return 1
    if manifest.get("skipped"):
        print(f"({len(manifest['skipped'])} shot(s) skipped by gb_shots.py — see its own "
              f"[skip] lines / manifest.json's \"skipped\" list, not in this sheet)")
    return 0


def cmd_per_feature(a: argparse.Namespace) -> int:
    manifest = load_manifest(a.shots)
    shots = manifest_entries_sorted(manifest, a.shots)
    if not shots:
        sys.exit("manifest has zero shots — nothing to composite")

    groups: dict[str, list[tuple[Path, str]]] = {fid: [] for fid in FEATURE_ORDER}
    for path, caption in shots:
        groups[feature_for_caption(caption)].append((path, caption))

    if a.bag_dir.is_dir():
        groups["bag"].extend(build_bag_entries(a.bag_dir))

    font = load_font()
    a.contact_sheets_dir.mkdir(parents=True, exist_ok=True)
    today = datetime.date.today().isoformat()
    index: list[dict] = []
    missing_any = []

    for fid in FEATURE_ORDER:
        entries = groups[fid]
        if not entries:
            continue
        out_path = a.contact_sheets_dir / f"{fid}.png"
        stats = render_sheet(entries, out_path, scale=a.scale, font=font)
        rel = out_path.relative_to(ROOT).as_posix()
        index.append({
            "id": fid,
            "title": FEATURE_TITLE[fid],
            "file": rel,
            "count": stats["count"],
            "updated": today,
        })
        print(f"{rel}  ({stats['width']}x{stats['height']}, {stats['bytes']:,} bytes, "
              f"{stats['count']} shot(s), {stats['rows']} row(s))")
        missing_any.extend(stats["missing"])

    index_path = a.contact_sheets_dir / "index.json"
    index_path.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    print(f"{index_path.relative_to(ROOT)}  ({len(index)} feature(s))")

    uncategorized = len(groups.get("misc", []))
    if uncategorized:
        print(f"({uncategorized} shot(s) landed in \"misc\" — no caption prefix matched "
              f"FEATURE_TABLE)")
    if missing_any:
        print("MISSING (drawn as red placeholders): " + ", ".join(missing_any), file=sys.stderr)
        return 1
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--shots", type=Path, default=GB_SHOTS_DIR,
                     help="dir holding gb_shots.py's manifest.json + PNGs")
    ap.add_argument("--out", type=Path,
                     default=ROOT / "docs" / "contact-sheet-2026-09-05-gb-arc.png",
                     help="combined-sheet output path (used unless --per-feature is given)")
    ap.add_argument("--bag-dir", type=Path, default=BAG_DIR,
                     help="docs/shots/bag — before/after PNGs with no manifest of their own")
    ap.add_argument("--contact-sheets-dir", type=Path, default=CONTACT_SHEETS_DIR,
                     help="where --per-feature writes <feature-id>.png + index.json")
    ap.add_argument("--scale", type=float, default=SCALE,
                     help="nearest-neighbour upscale factor per frame (default 2; drop to "
                          "1.5 if a generated page gets too large to host comfortably)")
    ap.add_argument("--per-feature", action="store_true",
                     help="write one sheet per feature + index.json instead of the combined sheet")
    ap.add_argument("--all", action="store_true",
                     help="explicitly build the combined sheet (this is also the default "
                          "when neither flag is given, so nothing that already calls this "
                          "script with no flags changes behaviour)")
    a = ap.parse_args(argv)

    if a.per_feature:
        rc = cmd_per_feature(a)
        if a.all:
            rc = cmd_combined(a) or rc
        return rc
    return cmd_combined(a)


if __name__ == "__main__":
    raise SystemExit(main())
