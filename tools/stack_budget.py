#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""stack_budget.py - post-link: fail the build before the user stack overruns.

    python3 tools/stack_budget.py --elf PokeDNA-artless.elf --builddir build-artless

PokeDNA has ONE user stack (__sp_usr down to __iheap_start; libtonc's isr_master
switches to SYSTEM mode, so IRQ handlers also run on THIS stack, +64 B). There is no
MMU and no guard page: an overrun silently corrupts whatever newlib's heap put right
below __iheap_start, then the .bss beyond it -- the closest thing GBA homebrew has to a
segfault is "the save file went strange three screens later." The EWRAM guard above
this one in the Makefile catches the sibling failure (.sbss overrunning EWRAM); this one
catches the stack overrunning ITS ceiling, one function-call chain at a time.

WHY THIS CANNOT BE `ulimit -s` OR A LINKER CHECK
    GCC's -fstack-usage tells us each function's OWN frame, not the worst call chain --
    that requires walking the call graph from main() and summing frames along the
    heaviest branch. Four things make that walk wrong if done naively (found the hard
    way, BACKLOG #84 / the #81 review-opus report):

    1. Thumb-1's `b` has only a +-2 KB range, so GCC emits `bl <label>` for a long jump
       WITHIN a function. Treating every `bl` as a call inflates the graph with
       self-edges and can even manufacture a cycle that hides the real depth.
    2. ARMv4T has no `blx <reg>`: an indirect call compiles to `bl` targeting an
       anonymous `bx rN` thunk. objdump prints the thunk as `<ownerFunction+0xNN>`; naively
       trusting that owner name attributes the call to the WRONG function.
    3. Linker-inserted long-branch veneers (`<sym>_veneer`: `ldr ip,[pc]; bx ip`) are a
       real edge to the veneer's target, but objdump renders the final `bx ip` with no
       `<target>` at all -- the edge is invisible unless the veneer's literal word is
       read back out and matched against the symbol table.
    4. GCC's `.su` output for a cloned function (`foo.constprop.3`, `foo.isra.0`) does
       not always carry the same suffix objdump/nm use for the compiled symbol, so an
       exact-name lookup silently reads a MISSING entry as a free 0-byte frame -- the
       #81 review found this hid 40 B on `dir_read.constprop.0` and made a 160 B margin
       look like 120 B in reality.

    The walker below (lifted from the #81/#84 review-opus's own tools, ported here so
    the guard ships with the repo instead of living in /tmp) handles all four: it skips
    self-edges (1), resolves `bx rN` thunks as unresolved INDIRECT sites rather than
    calls to the thunk's owner (2), reads veneer literals back out of the disassembly
    (3), and falls back from an exact `.su` name to the same name with every
    `.isra`/`.constprop`/`.part`/`.cold`/`.lto_priv` suffix stripped, then to any `.su`
    key sharing the stripped base (4).

FUNCTIONS WITH NO .su AT ALL (newlib, statically linked)
    newlib ships no -fstack-usage output, so `_svfprintf_r`, `_dtoa_r`, `__sbprintf`, and
    friends have no `.su` entry. Rather than count them as 0 (silently wrong the same
    way #4 above was), their frame is ESTIMATED from the compiled prologue: 4 bytes per
    register in the `push {...}` list, plus a literal `sub sp, #N`, plus the
    "ldr rN, =-K ; add sp, rN" idiom GCC uses for a stack allocation too large to fit
    `sub sp`'s 7-bit immediate. Printed chains tag these frames "(estimated)" so a
    reader can tell measured bytes from a prologue guess at a glance.

THE BUDGET
    Deepest chain from main(), plus one flat +64 B for the ISR reentry onto this same
    stack, must leave at least 1024 B of headroom below (__sp_usr - __iheap_start).
    That 1024 B is not a measured number -- it is a chosen safety margin against
    anything this walker cannot see (an indirect call this build's graph didn't reach,
    a future newlib prologue shape the estimator doesn't recognize yet).
"""
import argparse
import bisect
import collections
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import time

DEVKITARM = os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM")
OBJDUMP = os.path.join(DEVKITARM, "bin", "arm-none-eabi-objdump")
NM = os.path.join(DEVKITARM, "bin", "arm-none-eabi-nm")

SAFETY_MARGIN = 1024   # B of headroom demanded below (__sp_usr - __iheap_start)
ISR_BYTES = 64         # libtonc isr_master runs handlers on __sp_usr too

# === .su parsing (exact, compiler-measured frames) ===================================

def load_su(builddir):
    """basename -> worst-case frame size, from every *.su GCC emitted alongside the .o's.

    A function can appear more than once (inlined-then-reinstantiated clones, or two
    TUs defining a static of the same name) -- take the max, as the reviewer's cg2.py
    did, since we want the WORST case, not an arbitrary one.
    """
    sizes = {}
    dup = collections.defaultdict(set)
    for f in glob.glob(os.path.join(builddir, "*.su")):
        for line in open(f):
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            name = parts[0].rsplit(":", 1)[-1]
            try:
                n = int(parts[1])
            except ValueError:
                continue
            dup[name].add((os.path.basename(f), n, parts[2]))
            sizes[name] = max(sizes.get(name, 0), n)
    dupes = {k: v for k, v in dup.items() if len({x[1] for x in v}) > 1}
    return sizes, dupes


_SUFFIX_RE = re.compile(r'\.(isra|constprop|part|cold|lto_priv)(\.\d+)*$')


def su_frame(sizes, name):
    """Trap #4: exact name, else the name with every clone suffix stripped, else any
    .su key sharing that stripped base (handles a mismatched suffix on either side)."""
    if name in sizes:
        return sizes[name]
    base = _SUFFIX_RE.sub('', name)
    if base in sizes:
        return sizes[base]
    for k in sizes:
        if k.split('.')[0] == base:
            return sizes[k]
    return None


# === disassembly: call graph + prologue frame estimator ==============================

FUNC_RE = re.compile(r'^([0-9a-f]{8}) <([^>]+)>:$')
INSN_RE = re.compile(r'^\s*([0-9a-f]+):\t[0-9a-f ]+\t(.*)$')
TGT_RE = re.compile(
    r'^(bl|blx|b|b\.n|b\.w|bx|bne|beq|bcs|bcc|bmi|bpl|bvs|bvc|bhi|bls|bge|blt|bgt|ble)'
    r'(\.[nw])?\s+([0-9a-f]+) <'
)

PUSH_RE = re.compile(r'^(push|stmfd\s+sp!|stmdb\s+sp!)\s*(\{[^}]*\})')
SUBSP_RE = re.compile(r'^sub\s+sp,(?: sp,)? #(\d+)')
LDRPC_RE = re.compile(r'^ldr\s+(r\d+|ip), \[pc, #\d+\]\s*@ \(?([0-9a-f]+)')
ADDSP_RE = re.compile(r'^add\s+sp, (r\d+|ip)$')
REG_ORDER = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'sl', 'fp',
             'ip', 'sp', 'lr', 'pc']


def _reg_count(reglist):
    n = 0
    for part in reglist.strip('{}').split(','):
        part = part.strip()
        if not part:
            continue
        if '-' in part:
            a, b = part.split('-')
            n += (REG_ORDER.index(b) - REG_ORDER.index(a) + 1
                  if a in REG_ORDER and b in REG_ORDER else 2)
        else:
            n += 1
    return n


def disassemble(elf):
    return subprocess.run([OBJDUMP, "-d", elf], capture_output=True, text=True,
                           check=True).stdout


def analyze(dump_text):
    """One pass over the disassembly: (a) build the call graph with traps #1-#3
    handled, (b) collect raw instruction lines per function for the prologue
    estimator, (c) record indirect-call sites (unresolved targets) so the caller can
    check whether one falls on the reported deepest chain."""
    starts = []
    name_at = {}
    insn = {}
    lines = []          # (func, addr, mnemonic-line) for every decoded instruction
    fn_lines = collections.defaultdict(list)   # func -> raw objdump lines (for est_frames)
    cur = None
    for raw in dump_text.splitlines():
        s = raw.strip()
        m = FUNC_RE.match(s)
        if m:
            a = int(m.group(1), 16)
            cur = m.group(2)
            starts.append(a)
            name_at[a] = cur
            fn_lines[cur].append(raw)
            continue
        m = INSN_RE.match(raw)
        if m and cur is not None:
            a = int(m.group(1), 16)
            insn[a] = m.group(2).strip()
            lines.append((cur, a, m.group(2).strip()))
            fn_lines[cur].append(raw)

    starts.sort()

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return name_at[starts[i]] if i >= 0 else None

    edges = collections.defaultdict(set)
    indirect_sites = collections.defaultdict(list)   # func -> [(addr, insn_text), ...]
    weird = []
    for fn, a, ins in lines:
        m = TGT_RE.match(ins)
        if not m:
            if re.match(r'^(bl|blx)\s', ins):
                weird.append((fn, hex(a), ins))
            continue
        op = m.group(1)
        t = int(m.group(3), 16)
        if op not in ("bl", "blx", "b", "b.n", "b.w"):
            continue                                    # conditional branch = intra-function
        tgt_ins = insn.get(t, "")
        if re.match(r'^bx\s+\w', tgt_ins) and t not in name_at:
            indirect_sites[fn].append((hex(a), ins))     # trap #2: bx-rN thunk, unresolved
            continue
        own = owner(t)
        if own == fn:
            continue                                     # trap #1: intra-function long jump
        if t in name_at:
            edges[fn].add(own)
        else:
            weird.append((fn, hex(a), ins + f"  -> mid of {own}"))

    # trap #3: linker long-branch veneers (`<sym>_veneer`: ldr ip,[pc]; bx ip; .word T).
    # objdump renders the final `bx ip` with no <target> operand, so the edge above is
    # lost unless the literal word following the veneer is read back and resolved.
    veneers = {}
    for a, nm in name_at.items():
        if not nm.endswith("_veneer"):
            continue
        for off in range(0, 0x24, 4):
            w = insn.get(a + off, "")
            wm = re.match(r'^\.word\s+0x([0-9a-f]+)$', w)
            if wm:
                t = int(wm.group(1), 16) & ~1
                if t in name_at:
                    veneers[nm] = name_at[t]
                    edges[nm].add(name_at[t])
                    break

    return {
        "funcs": set(name_at.values()),
        "edges": edges,
        "indirect_sites": dict(indirect_sites),
        "weird": weird,
        "veneers": veneers,
        "fn_lines": fn_lines,
    }


def load_extra_edges(path):
    """Parse tools/stack_edges.txt: `caller -> impl1 impl2 ...` per non-comment line.
    Multiple lines for the same caller (one BoxSource field each) ACCUMULATE -- a
    caller with several indirect-call classes (pdna_box has eleven) just gets more
    lines, all unioned into that caller's declared target set."""
    extra = collections.defaultdict(set)
    if not path or not os.path.exists(path):
        return extra
    with open(path) as f:
        for raw in f:
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            if '->' not in line:
                continue
            caller, rhs = line.split('->', 1)
            caller = caller.strip()
            impls = rhs.split()
            extra[caller].update(impls)
    return extra


def estimate_frames(fn_lines):
    """Prologue-shape estimate for functions with no .su entry (newlib, statically
    linked, compiled without -fstack-usage). Exact on the two GCC push/sub-sp idioms
    this build actually emits; anything else is left unestimated (0, tagged unknown)
    rather than guessed."""
    est = {}
    for fn, raw_lines in fn_lines.items():
        word = {}
        for l in raw_lines:
            m = re.match(r'^\s*([0-9a-f]+):\t([0-9a-f ]+)\t\.word\s+0x([0-9a-f]+)', l)
            if m:
                word[int(m.group(1), 16)] = int(m.group(3), 16)
        total = 0
        regs = {}
        for l in raw_lines:
            m = re.match(r'^\s*([0-9a-f]+):\t[0-9a-f ]+\t(.*)$', l)
            if not m:
                continue
            ins = m.group(2).split('@')[0].strip() if not m.group(2).startswith('ldr') \
                else m.group(2).strip()
            pm = PUSH_RE.match(ins)
            if pm:
                total += _reg_count(pm.group(2)) * 4
                continue
            sm = SUBSP_RE.match(ins)
            if sm:
                total += int(sm.group(1))
                continue
            lm = LDRPC_RE.match(m.group(2).strip())
            if lm:
                w = word.get(int(lm.group(2), 16))
                if w is not None:
                    regs[lm.group(1)] = w
                continue
            am = ADDSP_RE.match(ins)
            if am:
                v = regs.get(am.group(1))
                if v is not None and v & 0x80000000:
                    total += (0x100000000 - v)
                continue
        est[fn] = total
    return est


def frame_of(name, su_sizes, estimated):
    """Return (bytes, source) where source in {"su", "estimated", "unknown"}."""
    v = su_frame(su_sizes, name)
    if v is not None:
        return v, "su"
    if name in estimated:
        return estimated[name], "estimated"
    return 0, "unknown"


def deepest_from(root, edges, su_sizes, estimated, blacklist=()):
    """Heaviest root..leaf chain by DFS with memoization; returns (total, path, cycles).
    path is a list of (name, frame_bytes, source)."""
    memo = {}
    best_child = {}
    onstack = set()
    cycles = []

    def go(fn):
        if fn in memo:
            return memo[fn]
        if fn in onstack:
            cycles.append(fn)
            return 0
        onstack.add(fn)
        own, _src = frame_of(fn, su_sizes, estimated)
        bc, bn = 0, None
        for c in sorted(edges.get(fn, ())):
            if c in blacklist:
                continue
            v = go(c)
            if v > bc:
                bc, bn = v, c
        onstack.discard(fn)
        memo[fn] = own + bc
        best_child[fn] = bn
        return memo[fn]

    total = go(root)
    path = []
    cur = root
    while cur is not None:
        b, src = frame_of(cur, su_sizes, estimated)
        path.append((cur, b, src))
        cur = best_child.get(cur)
    return total, path, cycles


def top_n_chains(root, edges, su_sizes, estimated, n=5):
    """Top-N distinct chains from root, ranked by root's direct callees' subtree
    weight (each callee's own heaviest chain, prefixed with root's frame)."""
    root_frame, root_src = frame_of(root, su_sizes, estimated)
    children = sorted(edges.get(root, ()),
                       key=lambda c: deepest_from(c, edges, su_sizes, estimated)[0],
                       reverse=True)
    chains = []
    for c in children[:n]:
        tot, path, cycles = deepest_from(c, edges, su_sizes, estimated)
        chains.append((root_frame + tot, [(root, root_frame, root_src)] + path, cycles))
    if not chains:
        chains = [(root_frame, [(root, root_frame, root_src)], [])]
    return chains


# === nm: __sp_usr / __iheap_start =====================================================

def read_stack_symbols(elf):
    out = subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout
    vals = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        addr, _typ, name = parts[0], parts[1], parts[2]
        if name in ("__sp_usr", "__iheap_start"):
            vals[name] = int(addr, 16)
    return vals


# === caching (objdump + .su parse are the only slow steps) ===========================

def _cache_path(elf):
    return elf + ".stackcache.json"


def _elf_fingerprint(elf, builddir):
    st = os.stat(elf)
    su_files = sorted(glob.glob(os.path.join(builddir, "*.su")))
    su_stat = [(f, os.path.getsize(f)) for f in su_files]
    h = hashlib.sha1()
    h.update(f"{st.st_mtime_ns}:{st.st_size}".encode())
    h.update(repr(su_stat).encode())
    return h.hexdigest()


def format_num(n):
    return f"{n:,}"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--builddir", required=True)
    ap.add_argument("--root", default="main")
    ap.add_argument("--margin", type=int, default=SAFETY_MARGIN)
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--edges-file",
                     default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                           "stack_edges.txt"),
                     help="declared indirect-call targets, 'caller -> impl1 impl2 ...' "
                          "per line (default: tools/stack_edges.txt next to this script; "
                          "pass an empty string to disable)")
    args = ap.parse_args(argv)

    if not os.path.exists(args.elf):
        print(f"*** stack_budget: no such ELF: {args.elf}", file=sys.stderr)
        return 1

    fp = _elf_fingerprint(args.elf, args.builddir)
    cache_file = _cache_path(args.elf)
    cached = None
    if os.path.exists(cache_file):
        try:
            with open(cache_file) as f:
                c = json.load(f)
            if c.get("fingerprint") == fp:
                cached = c
        except (json.JSONDecodeError, OSError):
            cached = None

    if cached is not None:
        dump_text = cached["dump_text"]
    else:
        dump_text = disassemble(args.elf)

    analysis = analyze(dump_text)
    su_sizes, su_dupes = load_su(args.builddir)
    estimated = estimate_frames(analysis["fn_lines"])

    if cached is None:
        try:
            with open(cache_file, "w") as f:
                json.dump({"fingerprint": fp, "dump_text": dump_text}, f)
        except OSError:
            pass   # caching is an optimization, never a hard requirement

    syms = read_stack_symbols(args.elf)
    if "__sp_usr" not in syms or "__iheap_start" not in syms:
        print("*** stack_budget: __sp_usr or __iheap_start symbol missing from the ELF",
              file=sys.stderr)
        return 1
    stack_room = syms["__sp_usr"] - syms["__iheap_start"]

    if args.root not in analysis["funcs"]:
        print(f"*** stack_budget: root '{args.root}' not found in {args.elf}'s disassembly",
              file=sys.stderr)
        return 1

    extra_edges = load_extra_edges(args.edges_file) if args.edges_file else {}
    for caller, impls in extra_edges.items():
        analysis["edges"][caller] |= impls
    unknown_impls = sorted({impl for impls in extra_edges.values() for impl in impls}
                            - analysis["funcs"])
    if unknown_impls:
        print(f"*** stack_budget: WARNING -- {args.edges_file} names implementation(s) not "
              f"found in {args.elf}: {unknown_impls} (stale declaration? typo? inlined away?)")

    chains = top_n_chains(args.root, analysis["edges"], su_sizes, estimated, n=args.top)
    deepest_total, deepest_path, cycles = chains[0][0], chains[0][1], chains[0][2]

    # STOP-LICENCE check: any unresolved indirect-call site inside a function that
    # appears on ANY of the top-N chains means the walker could not see a possible deeper
    # continuation past that point -- surface it instead of silently trusting the number,
    # UNLESS that caller has a declared entry in --edges-file (ground truth read out of the
    # source, tools/stack_edges.txt's own header explains how). A caller with NO declared
    # edges gets no free pass just because SOME of its indirect sites might be harmless --
    # every bx-thunk site in every function on a reported chain must be accounted for, so a
    # future new BoxSource field (or a new AppCommitFn-shaped parameter) cannot silently
    # slip through uncharged.
    blind_spots = []
    on_chain = set()
    for _tot, path, _cyc in chains:
        on_chain |= {name for name, _b, _s in path}
    for fn in sorted(on_chain):
        sites = analysis["indirect_sites"].get(fn, [])
        if not sites:
            continue
        if fn in extra_edges:
            continue   # declared -- the caller's edge set now includes every known target
        for addr, ins in sites:
            blind_spots.append((fn, addr, ins))

    print(f"ELF: {args.elf}")
    print(f"root: {args.root}")
    print(f"__sp_usr=0x{syms['__sp_usr']:08x}  __iheap_start=0x{syms['__iheap_start']:08x}"
          f"  stack room={format_num(stack_room)} B")
    if su_dupes:
        print(f"NOTE: {len(su_dupes)} .su name(s) had inconsistent sizes across TUs "
              "(worst case kept):", ", ".join(sorted(su_dupes)[:6]),
              "..." if len(su_dupes) > 6 else "")
    print()
    print(f"TOP {len(chains)} CHAINS FROM {args.root}():")
    for i, (tot, path, cyc) in enumerate(chains, 1):
        print(f"\n  #{i}  total {format_num(tot)} B"
              + ("  (+64 B ISR = the guarded value below)" if i == 1 else ""))
        for name, b, src in path:
            tag = {"su": "", "estimated": " (estimated)", "unknown": " (UNKNOWN, counted 0)"}[src]
            print(f"      {b:6,d}  {name}{tag}")
        if cyc:
            print(f"      WARNING: recursion excluded at: {cyc}")

    if blind_spots:
        print("\n*** STACK_BUDGET BLIND SPOT: unresolved indirect call(s) inside a function "
              "on the deepest chain -- the true depth past this point is UNKNOWN, not "
              "bounded by this report:")
        for fn, addr, ins in blind_spots[:10]:
            print(f"***   {fn} @ {addr}: {ins}")
        print("*** Refusing to certify a number the walker cannot back. Fix: give the "
              "indirect call site a resolvable target (devirtualize / annotate), or add "
              "an explicit --extra-edge if the true target is known out-of-band.")
        return 1

    guarded_total = deepest_total + ISR_BYTES
    budget = stack_room - args.margin
    margin_left = stack_room - guarded_total

    if guarded_total > budget:
        print(f"\n*** FATAL: STACK BUDGET EXCEEDED — deepest chain {format_num(deepest_total)} B"
              f" + {ISR_BYTES} B ISR = {format_num(guarded_total)} B, but only "
              f"{format_num(budget)} B is budgeted ({format_num(stack_room)} B stack room minus "
              f"the {format_num(args.margin)} B safety margin).")
        print("***        The heaviest chain is printed above (#1) -- shrink a frame on it, "
              "or gate the call with a stack-room check before it runs.")
        return 1

    print(f"\nSTACK ok: deepest {format_num(guarded_total)} of {format_num(stack_room)} "
          f"(margin {format_num(margin_left)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
