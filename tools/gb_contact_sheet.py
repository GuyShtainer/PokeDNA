#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gb_contact_sheet.py — composite tools/gb_shots.py's output into one contact sheet.

    /usr/local/bin/python3 tools/gb_contact_sheet.py \
        --shots docs/shots/gb --out docs/contact-sheet-2026-09-05-gb-arc.png

Reads docs/shots/gb/manifest.json (written by gb_shots.py) so a caption lives in
exactly one place. Grid: 3 columns, each frame scaled 2x nearest-neighbour (GBA
screens are 240x160; this keeps them crisp, not blurred), a caption band under each.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent

SCALE = 2
COLS = 3
FRAME_W, FRAME_H = 240 * SCALE, 160 * SCALE
CAPTION_H = 46
PAD = 10
BG = (13, 17, 23)
FG = (201, 209, 217)
BORDER = (48, 54, 61)


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


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", type=Path, default=ROOT / "docs" / "shots" / "gb")
    ap.add_argument("--out", type=Path,
                     default=ROOT / "docs" / "contact-sheet-2026-09-05-gb-arc.png")
    a = ap.parse_args(argv)

    manifest_path = a.shots / "manifest.json"
    if not manifest_path.is_file():
        sys.exit(f"{manifest_path}: missing — run tools/gb_shots.py first")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    shots = manifest["shots"]
    if not shots:
        sys.exit("manifest has zero shots — nothing to composite")

    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 13)
    except OSError:
        font = ImageFont.load_default()

    cell_w, cell_h = FRAME_W + PAD * 2, FRAME_H + CAPTION_H + PAD * 2
    rows = (len(shots) + COLS - 1) // COLS
    sheet = Image.new("RGB", (cell_w * COLS, cell_h * rows), BG)
    draw = ImageDraw.Draw(sheet)

    missing = []
    for i, entry in enumerate(shots):
        path = a.shots / entry["file"]
        cx, cy = (i % COLS) * cell_w, (i // COLS) * cell_h
        if not path.is_file():
            missing.append(entry["file"])
            draw.rectangle([cx + PAD, cy + PAD, cx + PAD + FRAME_W, cy + PAD + FRAME_H],
                            outline=(248, 81, 73))
            draw.text((cx + PAD + 6, cy + PAD + 6), "MISSING\n" + entry["file"],
                      fill=(248, 81, 73), font=font)
            continue
        img = Image.open(path).convert("RGB")
        img = img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST)
        sheet.paste(img, (cx + PAD, cy + PAD))
        draw.rectangle([cx + PAD, cy + PAD, cx + PAD + img.width - 1, cy + PAD + img.height - 1],
                        outline=BORDER)
        ty = cy + PAD + FRAME_H + 4
        for line in wrap(entry["caption"], font, FRAME_W, draw)[:3]:
            draw.text((cx + PAD, ty), line, fill=FG, font=font)
            ty += 15

    a.out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(a.out)
    print(f"{a.out}  ({sheet.width}x{sheet.height}, {a.out.stat().st_size:,} bytes, "
          f"{len(shots)} shot(s), {rows} row(s))")
    if missing:
        print("MISSING (drawn as red placeholders): " + ", ".join(missing), file=sys.stderr)
        return 1
    if manifest.get("skipped"):
        print(f"({len(manifest['skipped'])} shot(s) skipped by gb_shots.py — see its own "
              f"[skip] lines / manifest.json's \"skipped\" list, not in this sheet)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
