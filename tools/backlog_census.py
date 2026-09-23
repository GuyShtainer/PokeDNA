#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""backlog_census.py -- stamp each BACKLOG item with its merge trail, then count what is open.

Why this exists: an item's heading is written when the item is FILED and is rarely updated when
its lane merges, so the backlog reads as far more open than it is. On 2026-09-23 a scan found 257
items of which only 23 were genuinely open -- five items picked for a lane that day turned out to
be already merged. This tool closes that gap mechanically, from the commit trail, without ever
claiming closure on its own authority.

Conservative by design: it records "referenced by merge <sha>" and tells the reader to verify. A
commit can MENTION an item without finishing it (a review defect, a follow-up, a related fix), so
the stamp is evidence, not a verdict. Only a human or a lane report closes an item.

Usage:  python3 tools/backlog_census.py [--stamp] [--json]
        --stamp   write the trail lines into docs/BACKLOG.md (default: report only)
"""
import re, subprocess, sys, json, datetime, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKLOG = os.path.join(ROOT, 'docs', 'BACKLOG.md')
DONE = re.compile(r'✅|CLOSED|MERGED|\bDONE\b|SHIPPED|FIXED ALREADY|VOID|Commit trail', re.I)
HEAD = re.compile(r'## #?(\d+[a-z]?)')


def merge_refs():
    """number -> [(sha, subject)] for every MERGE commit that names it."""
    out = subprocess.check_output(
        ['git', '-C', ROOT, 'log', '--merges', '--format=%h\x1f%s\x1f%b\x1e']).decode(errors='replace')
    refs = {}
    for rec in out.split('\x1e'):
        if not rec.strip():
            continue
        parts = rec.strip().split('\x1f')
        if len(parts) < 2:
            continue
        sha, subj = parts[0], parts[1]
        body = parts[2] if len(parts) > 2 else ''
        for n in set(re.findall(r'#(\d+[a-z]?)\b', subj + ' ' + body)):
            refs.setdefault(n, []).append((sha, subj))
            # "BACKLOG #2a" / "#2b" are SUB-ITEMS of #2 and must credit the parent too.
            # Without this, item #2 read as untouched for 16 days and a lane was briefed
            # on work that had shipped on 2026-09-07 (commits 2919383 / 07c233a).
            if n[-1].isalpha():
                refs.setdefault(n[:-1], []).append((sha, subj))
    return refs


def census(stamp=False):
    refs = merge_refs()
    lines = open(BACKLOG).read().split('\n')
    idx = [(i, l) for i, l in enumerate(lines) if l.startswith('## ')]
    today = datetime.date.today().isoformat()
    marked, open_items, inserts = [], [], []
    for k, (i, head) in enumerate(idx):
        end = idx[k + 1][0] if k + 1 < len(idx) else len(lines)
        m = HEAD.match(head)
        if not m:
            continue
        # A superseded entry kept for the record ("## 2 (original)", "## #243 (original
        # premise, disproven)") is not an open item -- it is the audit trail of one.
        if re.search(r'\((?:original|closed|disproven|superseded)', head, re.I):
            continue
        n = m.group(1)
        if DONE.search('\n'.join(lines[i:min(i + 5, end)])):
            marked.append(n)
            continue
        if n in refs:
            shas = refs[n][:2]
            trail = ', '.join('`{}` "{}"'.format(s, sj[:58].replace('"', "'")) for s, sj in shas)
            inserts.append((i + 1, '**Commit trail (auto-stamped {}):** referenced by merge {} '
                                   '— verify before treating as open.'.format(today, trail)))
            marked.append(n)
        else:
            open_items.append((n, head[:110]))
    if stamp and inserts:
        for pos, note in sorted(inserts, reverse=True):
            lines.insert(pos, note)
        open(BACKLOG, 'w').write('\n'.join(lines))
    return {'total': len(idx), 'accounted': len(marked), 'open': [h for _, h in open_items],
            'stamped_now': len(inserts) if stamp else 0}


if __name__ == '__main__':
    r = census(stamp='--stamp' in sys.argv)
    if '--json' in sys.argv:
        print(json.dumps(r, indent=2))
    else:
        print('backlog items: {}  accounted for: {}  OPEN: {}  (stamped this run: {})'.format(
            r['total'], r['accounted'], len(r['open']), r['stamped_now']))
        for h in r['open']:
            print('  ', h)
