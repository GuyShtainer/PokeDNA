#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""vsd_diff.py -- "exactly these files changed" between two `vsd_img list` outputs.

BACKLOG #179 Phase A step A2. `vsd_img list IMG` prints one sorted "path size crc32"
line per file. A chain's assertion is not "the run didn't crash", it is: after this
run, exactly this set of paths appeared / changed / vanished, and nothing else did
(docs/briefs/s179-design.md S4.6, S7.6: the diff carries size+crc32 per file, not just
a path list, precisely so a same-length no-op write cannot slip past it).

Usage:
    vsd_diff.py BEFORE.list AFTER.list
    vsd_img list before.img > /tmp/before.list && ... && vsd_img list after.img > /tmp/after.list
    tools/vsd_diff.py /tmp/before.list /tmp/after.list

Exit code 0 always (this is a report, not a gate by itself -- a runner's own
`expect_changed=[...]` check is the gate; see tools/dgb_shots.py). Prints one line per
added/removed/changed path, plus a one-line summary, to stdout.
"""
from __future__ import annotations

import sys
from dataclasses import dataclass


@dataclass(frozen=True)
class Entry:
    """One `vsd_img list` line: a file's path, size in bytes, and CRC32."""
    path: str
    size: int
    crc: str


def parse_list(path: str) -> dict[str, Entry]:
    """Parse a `vsd_img list` output file into {path: Entry}.

    Each line is "path size crc32hex" separated by single spaces; `path` itself
    never contains a space (FatFs LFN entries the project writes are ASCII
    identifiers -- see gb_sidecar.h's key format). A malformed line is a loud
    failure, not a skipped one: a silently-dropped line would hide a real diff.
    """
    entries: dict[str, Entry] = {}
    with open(path, encoding="ascii") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.rstrip("\n")
            if not line:
                continue
            parts = line.rsplit(" ", 2)
            if len(parts) != 3:
                raise ValueError(f"{path}:{lineno}: malformed list line: {line!r}")
            file_path, size_str, crc = parts
            entries[file_path] = Entry(file_path, int(size_str), crc)
    return entries


def diff_lists(before: dict[str, Entry], after: dict[str, Entry]) -> tuple[list[str], list[str], list[str]]:
    """Return (added, removed, changed) path lists, each sorted.

    "changed" means the path exists on both sides but size or crc32 differs --
    a same-size overwrite with identical bytes correctly reports no change.
    """
    before_paths = set(before)
    after_paths = set(after)
    added = sorted(after_paths - before_paths)
    removed = sorted(before_paths - after_paths)
    changed = sorted(
        p for p in (before_paths & after_paths) if before[p] != after[p]
    )
    return added, removed, changed


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} BEFORE.list AFTER.list", file=sys.stderr)
        return 2

    before = parse_list(argv[1])
    after = parse_list(argv[2])
    added, removed, changed = diff_lists(before, after)

    for p in added:
        print(f"+ {p} {after[p].size} {after[p].crc}")
    for p in removed:
        print(f"- {p} {before[p].size} {before[p].crc}")
    for p in changed:
        print(f"~ {p} {before[p].size} {before[p].crc} -> {after[p].size} {after[p].crc}")

    print(f"# added={len(added)} removed={len(removed)} changed={len(changed)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
