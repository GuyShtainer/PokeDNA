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
    """Trap #4: exact name wins outright. Otherwise (D3, BACKLOG #84b fifth pass) the
    frame is the MAX over every .su key sharing the stripped base -- NOT the bare
    stripped name alone. The bare-name-first version silently under-counted: a clone
    like `validate.constprop.0` strips to `validate`, and when FatFs's own tiny
    `validate` (136 B) ALSO exists as a .su key, `if base in sizes: return sizes[base]`
    returned 136 and never looked at `validate.constprop`'s real 560 B -- a live
    424 B under-count the guard could not see. Collecting every key whose stripped
    base matches (the exact base itself included) and taking the max is the same
    "worst case, not an arbitrary one" rule load_su() already applies to duplicate
    exact names; this closes the same hole for the fallback path."""
    if name in sizes:
        return sizes[name]
    base = _SUFFIX_RE.sub('', name)
    candidates = [v for k, v in sizes.items() if k == base or k.split('.')[0] == base]
    if candidates:
        return max(candidates)
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
MOVS_IMM_RE = re.compile(r'^movs?\s+(r\d+|ip)\s*,\s*#(\d+)\s*$')
LSLS_RE = re.compile(r'^lsls?\s+(r\d+|ip)\s*,\s*(r\d+|ip)\s*,\s*#(\d+)\s*$')
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
    indirect_sites = collections.defaultdict(list)   # func -> [(addr, insn_text, bx_reg), ...]
    fn_insn_seq = collections.defaultdict(list)       # func -> [(addr, insn_text), ...] in order
    weird = []
    for fn, a, ins in lines:
        fn_insn_seq[fn].append((a, ins))
        m = TGT_RE.match(ins)
        if not m:
            if re.match(r'^(bl|blx)\s', ins):
                weird.append((fn, hex(a), ins))
            continue
        op = m.group(1)
        t = int(m.group(3), 16)
        if op not in ("bl", "blx", "b", "b.n", "b.w"):
            continue                                    # conditional branch = intra-function
        # Trap #1 MUST be checked before trap #2 (D4/D6, BACKLOG #84b fourth pass): a
        # `bl`/`b.n` long jump that stays WITHIN the same function is intra-function
        # control flow no matter what instruction its target happens to be -- and a
        # function that returns via `pop {rN}; bx rN` (extremely common: GCC's own
        # shared-epilogue idiom for a function with more than one exit) puts a real
        # `bx rN` instruction right where every internal long branch to "the epilogue"
        # naturally lands. Checking the bx-thunk shape FIRST (the order this used to
        # run in) misclassified every one of those self-jumps as an unresolved
        # cross-function indirect-call site -- invisible under the old on-chain blind-
        # spot filter (it only ever looked at functions on the printed deepest chain),
        # but D4's whole-graph sweep walks every reachable function, so this ordering
        # bug alone produced 300+ false "blind spots" the moment the sweep went live
        # (memcpy16's own `bl <own pop{r3};bx r3>` byte-copy tail, pk_species_name's
        # `b.n <own bx lr>` early-return tail, and so on). own(t) is cheap to compute
        # (a bisect) so there is no reason not to check it first.
        own = owner(t)
        if own == fn:
            continue                                     # trap #1: intra-function long jump
        tgt_ins = insn.get(t, "")
        bxm = re.match(r'^bx\s+(\w+)', tgt_ins)
        if bxm and t not in name_at:
            # trap #5 (D6, BACKLOG #84b fourth pass): before treating this as an
            # unresolved indirect site, check whether the dispatch register was fed
            # by an UNINTERRUPTED PC-relative literal load right here in `fn` -- if
            # so, the "indirect" call is to a build-time CONSTANT, not a genuine
            # runtime-varying target. This is how every call from ROM code into an
            # IWRAM_CODE-resident helper compiles on ARMv4T (icopy_verified/
            # memcpy32/memset32/wp_copy_verified/hb_draw/fcio_sum_ag and friends):
            # `bl <label>` can't reach across the ROM/IWRAM boundary directly, so
            # GCC loads the helper's known address into a register and `bl`s to a
            # shared `bx rN` thunk -- byte-for-byte the same shape as a genuine
            # struct-field/vtable dispatch, but the register's value never varies.
            # D4's whole-graph sweep turned up ~100 of these (previously invisible
            # because they were never on a printed top-N chain); resolving them
            # here, generically, is far sounder than hand-declaring each IWRAM
            # helper's dozens of call sites in stack_edges.txt one by one. Only a
            # literal that lands EXACTLY on a function's entry address resolves --
            # anything else (a field load, a stack spill, a literal pointing at
            # DATA rather than a function start) still falls through to the
            # indirect_sites list below, unresolved, exactly as before.
            lit_target = _literal_call_target(fn_insn_seq[fn], insn, name_at, bxm.group(1))
            if lit_target is not None:
                edges[fn].add(lit_target)
                continue
            # trap #2: bx-rN thunk in ANOTHER function, unresolved -- record which
            # register it dispatches through so the caller can try to resolve the
            # site to a declared struct field (D1: BACKLOG #84b review).
            indirect_sites[fn].append((hex(a), ins, bxm.group(1)))
            continue
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
        "fn_insn_seq": dict(fn_insn_seq),
    }


FIELD_LINE_RE = re.compile(r'^(\w+)\.(\w+)\s*@(\d+)$')
# D6 (BACKLOG #84b fourth pass): a bare `\w+` caller name silently rejects any
# `.constprop.N`/`.isra.N`/`.part.N`/`.cold` GCC clone suffix (draw_wallpaper.
# constprop.0, fetch_pic_ex.constprop.0, ...) -- and load_extra_edges() doesn't
# error out when that happens: ARGSITE_LINE_RE simply fails to match, so the line
# falls through to the whole_func_decls catch-all with the ENTIRE "name argsites=N"
# string as a bogus caller key that can never match a real function -- a silently
# DEAD declaration, no error, no warning, the exact opposite of this file's own
# stop-licence. Found live: draw_wallpaper.constprop.0/fetch_pic_ex.constprop.0
# argsites=3 declarations (added this same pass) parsed with zero effect until this
# fix. `[\w.]+` allows the dotted clone suffix while still anchoring on whitespace
# before `argsites=`, so a real typo (stray text before the keyword) still fails to
# match and falls through exactly as before.
ARGSITE_LINE_RE = re.compile(r'^([\w.]+)\s+argsites=(\d+)$')
FRAME_LINE_RE = re.compile(r'^frame\s+(\S+)\s*=\s*(\d+)\b')


def load_extra_edges(path):
    """Parse tools/stack_edges.txt. Three declaration shapes, one non-comment line each
    (D1, BACKLOG #84b review, fixing the per-FUNCTION blind-spot exemption defect):

      Struct.field @OFFSET -> impl1 impl2 ...
        A struct-field indirect-call class (BoxSource.records, RomGbUi.read, ...).
        OFFSET is checked against the field's ACTUAL offset (computed from the
        struct's own header via struct_field_offsets()) before it is trusted --
        a stale/wrong OFFSET here is a FATAL, not a silent pass. Sites are matched
        to a class by the offset the disassembly loads, not by which function they
        sit in, so an undeclared field added to a struct with ten declared fields
        gets no free pass from its ten siblings.

      caller argsites=N -> impl1 impl2 ...
        A parameter/register-threaded dispatch class (AppCommitFn's `commit`
        argument, not a struct field -- the walker can't see a field offset for
        it). N is the number of such sites the reviewer counted BY READING THE
        SOURCE; if the disassembly's count for `caller` ever differs, that is a
        FATAL naming the caller (a new one appeared, or one vanished/inlined away).

      caller -> impl1 impl2 ...  (legacy, single-site callers only)
        Whole-function exemption. Only accepted when `caller` has AT MOST ONE
        indirect-call site in the disassembly (checked in main(), not here) --
        multi-site callers must use one of the structured forms above so a new,
        undeclared site cannot hide behind an old one's declaration.

      frame fn = BYTES  (freeform trailing text, e.g. "(measured by hand, date)")
        D2's UNKNOWN-frame override: certifies a function's frame size by hand
        when the prologue estimator can't classify it, so an honest UNKNOWN
        doesn't fail the build forever.

    Multiple lines per caller/struct ACCUMULATE (pdna_box has eleven field lines).
    Returns (field_decls, argsite_decls, whole_func_decls, frame_overrides):
      field_decls      : {(struct, field): (offset, {impls})}
      argsite_decls     : {caller: (N, {impls})}
      whole_func_decls  : {caller: {impls}}
      frame_overrides   : {fn: bytes}
    """
    field_decls = {}
    argsite_decls = {}
    whole_func_decls = collections.defaultdict(set)
    frame_overrides = {}
    if not path or not os.path.exists(path):
        return field_decls, argsite_decls, whole_func_decls, frame_overrides
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            fm = FRAME_LINE_RE.match(line)
            if fm:
                frame_overrides[fm.group(1)] = int(fm.group(2))
                continue
            if '->' not in line:
                raise ValueError(f"{path}:{lineno}: unrecognized line: {raw!r}")
            lhs, rhs = line.split('->', 1)
            lhs = lhs.strip()
            impls = set(rhs.split())
            fm2 = FIELD_LINE_RE.match(lhs)
            if fm2:
                struct, field, off = fm2.group(1), fm2.group(2), int(fm2.group(3))
                key = (struct, field)
                if key in field_decls:
                    prev_off, prev_impls = field_decls[key]
                    if prev_off != off:
                        raise ValueError(f"{path}:{lineno}: {struct}.{field} declared at "
                                          f"two different offsets ({prev_off} and {off})")
                    field_decls[key] = (off, prev_impls | impls)
                else:
                    field_decls[key] = (off, impls)
                continue
            am = ARGSITE_LINE_RE.match(lhs)
            if am:
                caller, n = am.group(1), int(am.group(2))
                if caller in argsite_decls:
                    prev_n, prev_impls = argsite_decls[caller]
                    if prev_n != n:
                        raise ValueError(f"{path}:{lineno}: {caller} argsites declared "
                                          f"twice with different counts ({prev_n} and {n})")
                    argsite_decls[caller] = (n, prev_impls | impls)
                else:
                    argsite_decls[caller] = (n, impls)
                continue
            whole_func_decls[lhs].update(impls)
    return field_decls, argsite_decls, dict(whole_func_decls), frame_overrides


# === struct-field offsets, computed from the header (D1) ==============================

_STRUCT_FIELD_RE = re.compile(r'\(\*(\w+)\)\(')
_KNOWN_PTR_TYPEDEFS = {"AppCommitFn", "GbReadFn", "Gb12ReadFn", "G2ReadFn", "Gen1ReadFn",
                        "ArtReadFn", "G2WriteFn"}
_KNOWN_VALUE_TYPES = {"bool": 1, "int": 4, "int32_t": 4, "uint32_t": 4, "int16_t": 2,
                       "uint16_t": 2, "int8_t": 1, "uint8_t": 1}


def _strip_c_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def _classify_field(stmt):
    """(name, size, align) for one struct-member declaration, or raise ValueError if
    the type isn't one this walker knows how to size -- an unrecognized type must
    FAIL the offset computation, never guess a size (stop-licence)."""
    stmt = ' '.join(stmt.split())
    m = _STRUCT_FIELD_RE.search(stmt)
    if m:
        return m.group(1), 4, 4                      # function pointer field
    parts = stmt.rsplit(None, 1)
    if len(parts) != 2:
        raise ValueError(f"can't parse struct member: {stmt!r}")
    typ, name = parts[0].strip(), parts[1].strip()
    if name.startswith('*'):
        return name.lstrip('*'), 4, 4                 # `Type* name` split by whitespace
    if typ.endswith('*'):
        return name, 4, 4                             # `Type* name` glued to the type
    if typ in _KNOWN_PTR_TYPEDEFS:
        return name, 4, 4
    if typ in _KNOWN_VALUE_TYPES:
        sz = _KNOWN_VALUE_TYPES[typ]
        return name, sz, sz
    raise ValueError(f"unknown field type {typ!r} for member {name!r} -- teach "
                      "_classify_field about it or this offset can't be trusted")


def struct_field_offsets(header_text, struct_name):
    """Field name -> byte offset within `struct_name`, computed by walking the
    header's own `typedef struct { ... } <struct_name>;` (or `struct <struct_name>
    { ... };`) with natural ARM EABI alignment (no #pragma pack anywhere in this
    codebase) -- the SAME layout arm-none-eabi-gcc gives the real struct, so a
    stack_edges.txt offset can be checked against reality instead of trusted on
    faith. Raises ValueError if the struct/a field can't be found or sized, so a
    header change this walker doesn't understand FAILS the build instead of
    silently keeping a stale offset (D1, the header-drift half of the fix)."""
    t = _strip_c_comments(header_text)
    m = (re.search(r'typedef\s+struct\s*\{(.*?)\}\s*' + re.escape(struct_name) + r'\s*;',
                    t, re.S)
         or re.search(r'struct\s+' + re.escape(struct_name) + r'\s*\{(.*?)\};', t, re.S))
    if not m:
        raise ValueError(f"struct {struct_name!r} not found")
    stmts = [s.strip() for s in m.group(1).split(';') if s.strip()]
    offsets = {}
    off = 0
    for s in stmts:
        name, size, align = _classify_field(s)
        off = (off + align - 1) // align * align
        offsets[name] = off
        off += size
    return offsets


def verify_field_declarations(field_decls, source_dir, struct_headers):
    """Cross-check every declared `Struct.field @OFF` against the offset the header
    actually gives that field today. struct_headers maps struct name -> header
    filename under source_dir. Returns a list of human-readable mismatch strings
    (empty = all declarations are honest); the caller treats ANY entry as fatal."""
    problems = []
    cache = {}
    for (struct, field), (decl_off, _impls) in sorted(field_decls.items()):
        if struct not in cache:
            hdr = struct_headers.get(struct)
            if hdr is None:
                problems.append(f"{struct}.{field}: no header registered for struct "
                                 f"{struct!r} (add it to STRUCT_HEADERS)")
                continue
            path = os.path.join(source_dir, hdr)
            try:
                with open(path) as f:
                    cache[struct] = struct_field_offsets(f.read(), struct)
            except (OSError, ValueError) as e:
                problems.append(f"{struct}.{field}: {e}")
                cache[struct] = None
                continue
        real = cache[struct]
        if real is None:
            continue
        if field not in real:
            problems.append(f"{struct}.{field}: no such field in {struct_headers[struct]} "
                             "today -- header changed, stack_edges.txt did not")
        elif real[field] != decl_off:
            problems.append(f"{struct}.{field}: declared @{decl_off}, header says "
                             f"@{real[field]} today -- header changed, stack_edges.txt did not")
    return problems


# struct name -> header file (under --source-dir) this walker knows how to size.
# Every struct named on the LHS of a `Struct.field @OFF` line in stack_edges.txt
# must be listed here, or verify_field_declarations() fails loudly instead of
# trusting an unverifiable offset.
STRUCT_HEADERS = {
    "BoxSource": "pdna_box.h",
    "RomGbUi": "rom_gbui.h",
    "Scan": "rom_gbui.c",     # file-local struct; struct_field_offsets() greps .c too
    "AppSrcOps": "pdna_app.h",
}


BRANCH_MNEM_RE = re.compile(
    r'^(b|bx|beq|bne|bcs|bcc|bmi|bpl|bvs|bvc|bhi|bls|bge|blt|bgt|ble)'
    r'(\.[nw])?(\s|$)')
CALL_MNEM_RE = re.compile(r'^(bl|blx)(\.[nw])?(\s|$)')
STORE_MNEM_RE = re.compile(r'^(str\w*|stm\w*|push)\b')
CMP_MNEM_RE = re.compile(r'^(cmp|cmn|tst|teq)\b')
# Trap #8 (D6, BACKLOG #84b fourth pass): objdump prints r10/r11 by their ARM alias
# names `sl`/`fp` (never "r10"/"r11"), the same way it already prints r12 as `ip` --
# a bare `r\d+|ip` alternation silently fails to match `mov r3, sl` / `ldr r8, [fp,
# #12]` at all, so a hi-register carrying the CHASE (the compiler routinely promotes
# a value live across multiple `bl`s -- e.g. a global's section-anchor base -- into
# sl/fp specifically because they're callee-saved) makes the mov-chase in
# resolve_indirect_site/_base_is_section_anchor silently give up and fall through to
# DEST_REG_RE's generic "redefined by something else" stop, reporting whatever the
# scan happened to be looking at as a genuine field. Confirmed live: pdna_dex_screen
# (the FULL-art build only -- its extra icon-rendering code pushes the s_dget/s_dset
# anchor into `sl` across two `bl`s the artless build's shorter code never needed)
# misclassified 15 genuine global/section-anchor dispatches as 'field' hits at
# offsets 8/20/24/28 -- offsets that happen to collide with this file's own declared
# AppSrcOps.release/.view/.editable/.create, which would have silently (and
# wrongly) exempted them. `REG_TOK` is the same register-name alternation used
# everywhere a Thumb/ARM register operand is matched in the indirect-call chase
# (NOT the separate estimate_frames prologue-idiom regexes above/below this point --
# Thumb-1 push/pop and the sub-sp/add-sp idioms they estimate never reach r8-r12,
# so those are unaffected and untouched).
REG_TOK = r'r\d+|sl|fp|ip'
MOV_REG_RE = re.compile(rf'^movs?\s+({REG_TOK})\s*,\s*({REG_TOK})\s*$')
DEST_REG_RE = re.compile(rf'^[a-z][a-z0-9]*\s+({REG_TOK})\b')
LDR_FIELD_RE = re.compile(
    rf'^ldr\w*\s+({REG_TOK})\s*,\s*\[\s*({REG_TOK}|sp|pc)\s*,\s*#(-?\d+)\s*\]')
CALLER_SAVED_REGS = frozenset(('r0', 'r1', 'r2', 'r3', 'ip'))   # AAPCS scratch registers
LDM_RE = re.compile(rf'^ldm\w*\s+({REG_TOK})(!?)\s*,\s*\{{([^}}]*)\}}')
POP_RE = re.compile(r'^pop\s*(\{[^}]*\})')


def _expand_reglist(reglist):
    """`{r3, r4-r7, lr}` -> {'r3', 'r4', 'r5', 'r6', 'r7', 'lr'}, using the same
    REG_ORDER range expansion _reg_count() already does for its byte-count tally --
    reused here (D7, BACKLOG #84b fifth pass) because a `pop {reglist}` range has
    the exact same shape and needs the same names-not-just-a-count answer."""
    regs = set()
    for part in reglist.strip('{}').split(','):
        part = part.strip()
        if not part:
            continue
        if '-' in part:
            a, b = part.split('-')
            if a in REG_ORDER and b in REG_ORDER:
                ia, ib = REG_ORDER.index(a), REG_ORDER.index(b)
                regs.update(REG_ORDER[ia:ib + 1])
            else:
                regs.add(a)
                regs.add(b)
        else:
            regs.add(part)
    return regs


def _ldm_defines(ins_clean, reg):
    """True if `ins_clean` is an `ldm` (load-multiple) OR a `pop` (D7, BACKLOG #84b
    fifth pass) that (re)defines `reg` -- either as one of the loaded destination
    registers, or (ldm only) as the base register itself when writeback (`!`) is
    present. Trap #7 (D6, BACKLOG #84b fourth pass): `ldmia rX!, {r3, r4}` is GCC's
    shape for two-or-more back-to-back struct-field loads (e.g. RomCtx's
    `read`+`ctx`, offsets 0/4), and the plain DEST_REG_RE catch-all only recognizes
    the BASE register as a destination (its `!` suffix still satisfies `\\b`, so
    `dm.group(1)` comes back "r0" for `ldmia r0!, {r3, r4}`) -- it has no idea r3/r4
    are ALSO freshly defined. Without this check the backward scan walks straight
    past the true definition to whatever STALE, unrelated instruction last wrote
    that register number earlier in the function. Confirmed live: rom_mon_icon_at's
    real dispatch (`rc->read`, RomCtx offset 0, loaded via `ldmia r0!,
    {r3, r4}`) got misattributed to `loc->ok`'s `ldrb r3, [r1, #5]` many
    instructions earlier in the SAME function, purely because both happen to
    target r3 -- reported as 'struct-field load @5', which is not even a real
    field (RomMonLoc.ok is a plain uint8_t flag, not a pointer of any kind).

    D7 (BACKLOG #84b fifth pass): `pop {reglist}` is `ldmia sp!, {reglist}` in every
    way that matters here EXCEPT that objdump/GCC's Thumb-1 assembler syntax always
    prints it as `pop`, never `ldmia sp!` -- so LDM_RE, anchored on the `ldm` mnemonic,
    never matched it at all, and a `pop {r3, r4}` sitting between a genuine field
    load and its dispatch was invisible to this whole trap: the backward scan walked
    straight through it as if it were a no-op, exactly the stale-register hazard
    trap #7 exists to close. `pop` never has explicit writeback syntax (the base is
    always the implicit `sp`, which nothing here dispatches through), so only the
    loaded-register-list half of the ldm check applies."""
    m = LDM_RE.match(ins_clean)
    if m:
        if m.group(2) == '!' and m.group(1) == reg:
            return True
        return reg in {p.strip() for p in m.group(3).split(',')}
    pm = POP_RE.match(ins_clean)
    if pm:
        return reg in _expand_reglist(pm.group(1))
    return False


def _literal_call_target(fn_insn_seq, insn_map, name_at_map, reg):
    """Trap #5 (D6, BACKLOG #84b fourth pass): scan `fn_insn_seq` (this function's own
    instructions, in order, with the `bl <bx-rN thunk>` call itself as the LAST entry)
    backward for the origin of `reg`. Returns the target function's name if `reg` was
    set by an uninterrupted `ldr reg, [pc, #imm]` whose literal word -- read back out
    of `insn_map`, Thumb bit masked off -- lands EXACTLY on a function's entry address
    in `name_at_map`; otherwise None (a struct-field/parameter dispatch, a literal
    pointing at data rather than code, or a shape this tracker can't follow -- the
    caller keeps treating it as a genuine indirect site in all of those cases).

    Same clobber-tracking discipline as resolve_indirect_site (chase `mov`
    register-to-register copies, allow push/sub-sp/cmp/store lines to pass through
    unless they redefine `reg`, treat a caller-saved `reg` as clobbered by an
    intervening `bl`/`blx`) -- this is the SAME register-origin question, just
    answered against the raw disassembly's own literal pool instead of a declared
    struct layout, so it must be exactly as conservative: erring toward "can't
    resolve" costs one more (harmless, correctly-reported) blind spot, never a
    silent wrong attribution."""
    for i in range(len(fn_insn_seq) - 2, -1, -1):
        a, ins = fn_insn_seq[i]
        ins_clean = ins.split('@')[0].strip()
        if CALL_MNEM_RE.match(ins_clean):
            if reg in CALLER_SAVED_REGS:
                return None                            # clobbered by the call
            continue                                    # callee-saved reg survives a call
        if BRANCH_MNEM_RE.match(ins_clean):
            continue
        m = LDR_FIELD_RE.match(ins_clean)
        if m:
            if m.group(1) != reg:
                continue
            if m.group(2) != 'pc':
                return None                            # field/stack/lr load, not a literal
            lit_addr = (a & ~3) + 4 + int(m.group(3))   # Thumb PC-relative: align, +4 pipeline
            word_ins = insn_map.get(lit_addr, "")
            wm = re.match(r'^\.word\s+0x([0-9a-f]+)$', word_ins)
            if not wm:
                return None
            return name_at_map.get(int(wm.group(1), 16) & ~1)   # mask the Thumb bit
        if _ldm_defines(ins_clean, reg):                # trap #7: ldm redefines it, not a literal
            return None
        if LDM_RE.match(ins_clean):
            continue                                    # ldm, but doesn't touch `reg`
        if STORE_MNEM_RE.match(ins_clean) or CMP_MNEM_RE.match(ins_clean):
            continue
        mv = MOV_REG_RE.match(ins_clean)
        if mv and mv.group(1) == reg:
            reg = mv.group(2)
            continue
        dm = DEST_REG_RE.match(ins_clean)
        if dm and dm.group(1) == reg:
            return None                                # set by something not ldr-pc/mov
    return None                                         # never (re)defined in this function


def _base_is_section_anchor(fn_insn_seq, ldr_idx, base_reg):
    """True if `base_reg` (the rY in `ldr rN, [rY, #off]`) was ITSELF materialized
    from a PC-relative literal (`ldr rY, [pc, #imm]`) with nothing redefining it in
    between (chasing `mov rX, rY` copies the same way resolve_indirect_site does, and
    treating a call as clobbering base_reg only if it's caller-saved). That shape --
    literal-load a fixed address, then index off it -- is GCC's `-fsection-anchors`
    codegen for a STATIC/global variable (default at -O2 for this target): every
    small static in a section shares ONE base register and reaches each other by a
    small #offset, which is byte-for-byte indistinguishable from a struct-field
    dereference UNLESS the base's own origin is checked. None of BoxSource/RomGbUi/
    Scan is ever addressed as a global in this codebase (always a parameter, spilled
    or in a register) -- a base fed by a fresh literal is therefore a global/anchor
    access, not an instance field, and must NOT be trusted as 'field' (confirmed
    empirically: a planted global function-pointer dispatch compiled to exactly this
    shape and, without this check, silently matched a declared field offset by
    coincidence -- a real false-pass, not a hypothetical one).

    Trap #6 (D6, BACKLOG #84b fourth pass): this used to stop after a fixed
    BASE_LITERAL_WINDOW=8 instructions and default to False (treat as a genuine
    field) the moment it ran out of window -- but a base register loaded ONCE
    before a loop and reused across every iteration (an extremely common shape:
    dex_build/pdna_dex_screen's inlined `dstate()` -> `s_dget(nat)`, a file-static
    global function pointer, loaded outside a `for` loop and called once per
    element inside it) routinely sits far more than 8 instructions before its use.
    The bounded window silently misclassified that shape as 'field' at whatever
    offset the anchor gave it -- invisible as long as nothing else declared a real
    field at that same offset, but the moment AppSrcOps.release/.editable (offsets
    8/24, genuinely declared elsewhere in this file) came along, those SAME
    numeric offsets silently and WRONGLY exempted the dex-screen sites too,
    attributing gb_release_hook/gb_editable_hook as if they were possible values
    of s_dget/s_dset -- a real, live false-pass, not a hypothetical one. There is
    no principled reason for this check to use a SMALLER search window than
    resolve_indirect_site's own unbounded backward scan for the dispatch register
    right next to it; matching that discipline (chase mov copies, unbounded) is
    the fix, not a wider constant that would just move the same failure further
    out."""
    reg = base_reg
    for i in range(ldr_idx - 1, -1, -1):
        _addr, ins = fn_insn_seq[i]
        ins_clean = ins.split('@')[0].strip()
        if CALL_MNEM_RE.match(ins_clean):
            if reg in CALLER_SAVED_REGS:
                return False                    # clobbered by the call, not an anchor base
            continue
        if BRANCH_MNEM_RE.match(ins_clean):
            continue
        m = LDR_FIELD_RE.match(ins_clean)
        if m and m.group(1) == reg:
            return m.group(2) == 'pc'
        if _ldm_defines(ins_clean, reg):        # trap #7: ldm redefines it, not a literal
            return False
        if LDM_RE.match(ins_clean):
            continue                            # ldm, but doesn't touch base_reg
        if STORE_MNEM_RE.match(ins_clean) or CMP_MNEM_RE.match(ins_clean):
            continue                            # a spill/compare READS base_reg, doesn't redefine it
        mv = MOV_REG_RE.match(ins_clean)
        if mv and mv.group(1) == reg:
            reg = mv.group(2)                   # chase the copy, keep scanning
            continue
        dm = DEST_REG_RE.match(ins_clean)
        if dm and dm.group(1) == reg:
            return False                    # base redefined by something else first
    return False                            # never (re)defined earlier in this function: a parameter


def resolve_indirect_site(fn_insn_seq, site_addr, reg):
    """Scan `fn`'s instructions backward from just before `site_addr` for the origin
    of the value dispatched through `reg`. Returns ('field', offset) if it traces to
    `ldr rN, [rY, #offset]` with rY not pc/lr/sp AND rY itself not a fresh literal
    load (a struct-field load through a genuine instance pointer, chasing plain
    `mov rX, rY` register-to-register copies on the way -- the compiler routinely
    loads a struct field into one register and copies it before the call); otherwise
    ('nonfield', None) -- set by anything else, never reassigned in this function (a
    parameter/register argument, e.g. AppCommitFn's `commit`), a section-anchor/global
    access (see _base_is_section_anchor), or CLOBBERED by an intervening `bl`/`blx`
    while held in a caller-saved register (r0-r3/ip survive a call only by accident,
    never by the ABI -- attributing a post-call value to a pre-call load would be a
    genuine lie, not a conservative guess). Erring toward 'nonfield' is the safe
    direction: an unmatched nonfield site with no argsites declaration is a blind spot
    and FAILS the build, so a misclassified field load costs a loud failure, never a
    silent pass."""
    idx = bisect.bisect_left(fn_insn_seq, (site_addr, ''))
    for i in range(idx - 1, -1, -1):
        _addr, ins = fn_insn_seq[i]
        ins_clean = ins.split('@')[0].strip()
        if CALL_MNEM_RE.match(ins_clean):
            if reg in CALLER_SAVED_REGS:
                return ('nonfield', None)              # clobbered by the call
            continue                                    # callee-saved reg survives a call
        if BRANCH_MNEM_RE.match(ins_clean):
            continue
        m = LDR_FIELD_RE.match(ins_clean)
        if m:
            if m.group(1) != reg:
                continue
            if m.group(2) in ('pc', 'lr', 'sp'):
                return ('nonfield', None)              # literal/stack-spilled param, not a field
            if _base_is_section_anchor(fn_insn_seq, i, m.group(2)):
                return ('nonfield', None)              # global/section-anchor access, not a field
            return ('field', int(m.group(3)))
        if _ldm_defines(ins_clean, reg):                # trap #7: ldm redefines it, not a spilled field
            return ('nonfield', None)
        if LDM_RE.match(ins_clean):
            continue                                    # ldm, but doesn't touch `reg`
        if STORE_MNEM_RE.match(ins_clean) or CMP_MNEM_RE.match(ins_clean):
            continue
        mv = MOV_REG_RE.match(ins_clean)
        if mv and mv.group(1) == reg:
            reg = mv.group(2)                           # chase the copy, keep scanning
            continue
        dm = DEST_REG_RE.match(ins_clean)
        if dm and dm.group(1) == reg:
            return ('nonfield', None)                  # set by something not ldr-offset/mov
    return ('nonfield', None)                           # never reassigned: a parameter


def estimate_frames(fn_lines):
    """Prologue-shape estimate for functions with no .su entry (newlib, statically
    linked, compiled without -fstack-usage). Exact on the three GCC push/sub-sp
    idioms this build actually emits; anything else marks the function UNKNOWN
    (D2, BACKLOG #84b review) rather than silently contributing 0 or a wrong number.

    D2's bug: the old walker kept a `regs[reg] = value` dict that lived for the
    WHOLE function, so an unrelated LATER `add sp, rN` (a different epilogue, a
    different basic block reusing the same register name) could consume a STALE
    value from an EARLIER, unrelated `ldr rN, [pc, #imm]` -- reproduced live:
    mgfx_zoom_optimise exploded to 2,080,375,400 B in one run because a later,
    unrelated `add sp, r3` picked up a stale r3 loaded many instructions earlier
    for something else entirely; _svfiprintf_r's reported 728 B was right only by
    the bit pattern of unrelated data, not by the walker actually tracking anything.

    Fix (stale-register class, kept): track exactly ONE pending (reg, value), set
    by `ldr rN, [pc, #imm]` and consumed ONLY by an add-sp for the SAME register
    with nothing but other push/sub-sp/ldr-pc lines in between; ANY other
    instruction invalidates it. The `movs rN, #k; lsls rN, rN, #s; add sp, rN`
    idiom gets its own tracked (reg, k<<s) pending value with the same
    single-slot, immediately-consumed-or-invalidated discipline.

    D2's SEPARATE bug (BACKLOG #84b review, third pass): `add sp, rN` has TWO
    distinct meanings depending on which idiom fed rN, and the walker was
    treating both as allocations. A `movs rN,#k; lsls rN,rN,#s` value is, by
    Thumb-1 construction, always a small non-negative literal shifted left --
    there is no way to encode a negative magnitude that way -- so
    `add sp, rN` fed by that idiom always INCREASES sp: it is a DEALLOCATION
    (an epilogue restoring a frame the prologue already allocated), never a
    second allocation on top of one. Likewise a POSITIVE `ldr rN,[pc,#imm]`
    literal fed into `add sp, rN` is also a deallocation. Only a NEGATIVE
    `ldr`-literal `add sp, rN` (sp decreases) and a direct `sub sp, #imm` are
    real allocations, and get summed into the frame total alongside the pushes.
    Counting a deallocation as if it were additional allocation is exactly how
    `_svfiprintf_r` came out at 1,420 B (728 real): its prologue allocates 692 B
    via a negative-literal `ldr+add sp`, and its epilogue restores the SAME
    692 B via the movs/lsls form -- the old code added both.

    If a movs/lsls-fed `add sp, rN` deallocation is seen but the function had NO
    allocation event at all (no `sub sp,#imm`, no negative-literal `ldr+add sp`),
    the function is UNKNOWN: something else must have done the allocation this
    walker does not recognize, and treating the deallocation's magnitude as the
    frame size would be pure guesswork. An `add sp, rN` matching NEITHER pending
    state is still an unconditional unknown, as before -- an unaccountable
    deallocation makes the running total untrustworthy."""
    est = {}
    for fn, raw_lines in fn_lines.items():
        word = {}
        for l in raw_lines:
            m = re.match(r'^\s*([0-9a-f]+):\t([0-9a-f ]+)\t\.word\s+0x([0-9a-f]+)', l)
            if m:
                word[int(m.group(1), 16)] = int(m.group(3), 16)
        total = 0              # push bytes + genuine allocation events (summed)
        saw_alloc_event = False
        saw_shift_dealloc = False
        unknown = False
        ldr_pending = None    # (reg, value) from the most recent `ldr rN, [pc, #imm]`
        shift_pending = None  # (reg, k<<s so far) from `movs rN,#k` / `lsls rN,rN,#s`
        for l in raw_lines:
            m = re.match(r'^\s*([0-9a-f]+):\t[0-9a-f ]+\t(.*)$', l)
            if not m:
                continue
            ins = m.group(2).split('@')[0].strip() if not m.group(2).startswith('ldr') \
                else m.group(2).strip()
            pm = PUSH_RE.match(ins)
            if pm:
                total += _reg_count(pm.group(2)) * 4
                continue                               # allowed between a pending ldr/movs and its use
            sm = SUBSP_RE.match(ins)
            if sm:
                total += int(sm.group(1))
                saw_alloc_event = True
                continue                               # allowed too
            lm = LDRPC_RE.match(m.group(2).strip())
            if lm:
                w = word.get(int(lm.group(2), 16))
                ldr_pending = (lm.group(1), w) if w is not None else None
                shift_pending = None
                continue
            am = ADDSP_RE.match(ins)
            if am:
                reg = am.group(1)
                if ldr_pending is not None and ldr_pending[0] == reg:
                    v = ldr_pending[1]
                    if v & 0x80000000:
                        total += (0x100000000 - v)     # negative literal: allocation
                        saw_alloc_event = True
                    # else: positive literal fed into add sp -- a deallocation,
                    # contributes nothing (ignored, not unknown)
                elif shift_pending is not None and shift_pending[0] == reg:
                    # movs/lsls can only produce a non-negative literal -- this
                    # add sp is by construction a deallocation, never a second
                    # allocation on top of the prologue's; ignore its magnitude
                    saw_shift_dealloc = True
                else:
                    unknown = True                      # unaccountable deallocation
                ldr_pending = None
                shift_pending = None
                continue
            mim = MOVS_IMM_RE.match(ins)
            if mim:
                shift_pending = (mim.group(1), int(mim.group(2)))
                ldr_pending = None
                continue
            lsm = LSLS_RE.match(ins)
            if lsm and shift_pending is not None and shift_pending[0] == lsm.group(2) \
                    and lsm.group(1) == lsm.group(2):
                shift_pending = (lsm.group(1), shift_pending[1] << int(lsm.group(3)))
                continue
            # any other instruction invalidates both pending states -- it may have
            # clobbered the register or simply means the value is no longer "just set"
            ldr_pending = None
            shift_pending = None
        if saw_shift_dealloc and not saw_alloc_event:
            unknown = True    # deallocation seen with no recognized allocation to match it
        est[fn] = {"bytes": total, "unknown": unknown}
    return est


def resolve_all_sites(analysis, field_decls, argsite_decls, whole_func_decls):
    """Classify every indirect-call site the walker found and decide which ones are
    exempted by a declaration (D1). Returns:
      edges_to_add     : {caller: {impl, ...}} to union into analysis["edges"]
      blind[fn]         : [(addr, ins, detail), ...] sites NOT exempted by anything --
                           a struct-field load at an undeclared offset, or a
                           parameter-style dispatch with no argsites/whole-function
                           declaration covering it
      count_mismatches  : [(caller, declared_n, found_n), ...] argsites declarations
                           whose count no longer matches the disassembly
      legacy_ambiguous  : [caller, ...] whole-function declarations on a caller that
                           now has more than one indirect site (needs a structured decl)
    Never trusts a caller-wide declaration for MULTIPLE sites unless every one of
    them is individually accounted for -- the whole point of D1."""
    field_offset_impls = collections.defaultdict(set)
    for (_struct, _field), (off, impls) in field_decls.items():
        field_offset_impls[off] |= impls

    fn_insn_seq = analysis["fn_insn_seq"]
    edges_to_add = collections.defaultdict(set)
    blind = collections.defaultdict(list)
    count_mismatches = []
    legacy_ambiguous = []

    for fn, sites in analysis["indirect_sites"].items():
        total = len(sites)
        if fn in whole_func_decls:
            if total > 1:
                legacy_ambiguous.append(fn)
            else:
                edges_to_add[fn] |= whole_func_decls[fn]
                continue      # every site (there's at most one) is exempted
        nonfield_sites = []
        for addr, ins, reg in sites:
            kind, off = resolve_indirect_site(fn_insn_seq.get(fn, []), int(addr, 16), reg)
            if kind == 'field':
                if off in field_offset_impls:
                    edges_to_add[fn] |= field_offset_impls[off]
                else:
                    blind[fn].append((addr, ins, f"struct-field load @{off}, no "
                                       "declared field at that offset"))
            else:
                nonfield_sites.append((addr, ins))
        if fn in argsite_decls:
            n, impls = argsite_decls[fn]
            if len(nonfield_sites) != n:
                count_mismatches.append((fn, n, len(nonfield_sites)))
            else:
                edges_to_add[fn] |= impls
        else:
            for addr, ins in nonfield_sites:
                blind[fn].append((addr, ins, "parameter/register dispatch, no "
                                   "argsites declaration for this caller"))
    return dict(edges_to_add), dict(blind), count_mismatches, legacy_ambiguous


def frame_of(name, su_sizes, estimated, overrides=None):
    """Return (bytes, source) where source in {"su", "override", "estimated", "unknown"}.
    D2: a function the prologue estimator could not classify (an `add sp, rN` with no
    provably-correct pending value) is "unknown", NOT a silently-trusted 0 -- it stays
    0 here (nothing else to report) but is tagged so the caller can refuse to certify
    a chain that passes through it, unless `overrides` (stack_edges.txt's `frame fn =
    BYTES` lines) names it."""
    v = su_frame(su_sizes, name)
    if v is not None:
        return v, "su"
    entry = estimated.get(name)
    if entry is not None and entry.get("unknown"):
        if overrides and name in overrides:
            return overrides[name], "override"
        return 0, "unknown"
    if entry is not None:
        return entry["bytes"], "estimated"
    if overrides and name in overrides:
        return overrides[name], "override"
    return 0, "unknown"


def deepest_from(root, edges, su_sizes, estimated, blacklist=(), overrides=None):
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
        own, _src = frame_of(fn, su_sizes, estimated, overrides)
        bc, bn = 0, None
        for c in sorted(edges.get(fn, ())):
            if c in blacklist:
                continue
            v = go(c)
            # >= , not > (D2 hardening): a sole child weighing 0 B -- which is
            # exactly what an UNKNOWN frame looks like before it's tagged -- must
            # still become best_child so it's VISIBLE in the printed path and can
            # be caught by the on-chain UNKNOWN check. `>` left it as `bn = None`,
            # silently dropping the child from the reported chain (the sum stayed
            # correct, since 0 contributes nothing, but "was this walked through
            # an UNKNOWN frame" became unanswerable from the output alone).
            if v >= bc:
                bc, bn = v, c
        onstack.discard(fn)
        memo[fn] = own + bc
        best_child[fn] = bn
        return memo[fn]

    total = go(root)
    path = []
    cur = root
    while cur is not None:
        b, src = frame_of(cur, su_sizes, estimated, overrides)
        path.append((cur, b, src))
        cur = best_child.get(cur)
    return total, path, cycles


def reachable_from(root, edges):
    """Every function reachable from `root` over the call graph, visited ONCE (D4,
    BACKLOG #84b fourth pass): a plain iterative DFS with a `seen` set, so a
    recursive edge back to an already-visited function just stops descending that
    branch instead of looping forever. This is deliberately separate from
    deepest_from()'s memoized search -- that one exists to find the HEAVIEST
    chain, this one exists to find EVERY function reachable at all, because a
    blind spot (an undeclared indirect-call site) can hide on a branch that never
    wins the deepest-chain comparison yet still needs to be caught."""
    seen = set()
    stack = [root]
    while stack:
        fn = stack.pop()
        if fn in seen:
            continue
        seen.add(fn)
        for c in edges.get(fn, ()):
            if c not in seen:
                stack.append(c)
    return seen


def whole_graph_blind_spots(reachable, blind):
    """Every undeclared indirect-call site inside a function reachable from root,
    independent of whether that function ever lands on a printed top-N chain
    (D4). The previous check filtered `blind` down to `on_chain` (the union of
    the printed chains' paths) -- unsound, because the deepest-chain search only
    descends the single heaviest branch at each fan-out; an undeclared site on a
    shallower sibling branch can hide an arbitrarily deep continuation that
    branch's own subtree never gets a chance to print. Confirmed live: a planted
    +6,000 B frame on banksrc_records surfaced a genuine, PRE-EXISTING blind spot
    in app_mon_menu_readonly that had been off every previously-reported deepest
    chain and so had never once failed the build."""
    out = []
    for fn in sorted(reachable):
        for addr, ins, detail in blind.get(fn, []):
            out.append((fn, addr, ins, detail))
    return out


def top_n_chains(root, edges, su_sizes, estimated, n=5, overrides=None):
    """Top-N distinct chains from root, ranked by root's direct callees' subtree
    weight (each callee's own heaviest chain, prefixed with root's frame)."""
    root_frame, root_src = frame_of(root, su_sizes, estimated, overrides)
    children = sorted(edges.get(root, ()),
                       key=lambda c: deepest_from(c, edges, su_sizes, estimated,
                                                   overrides=overrides)[0],
                       reverse=True)
    chains = []
    for c in children[:n]:
        tot, path, cycles = deepest_from(c, edges, su_sizes, estimated, overrides=overrides)
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
    ap.add_argument("--source-dir",
                     default=os.path.join(os.path.dirname(os.path.dirname(
                         os.path.abspath(__file__))), "source"),
                     help="where struct-field @OFFSET declarations are checked against "
                          "the real header (default: source/ next to tools/)")
    args = ap.parse_args(argv)

    if not os.path.exists(args.elf):
        print(f"*** stack_budget: no such ELF: {args.elf}", file=sys.stderr)
        return 1

    # D3 (BACKLOG #84b review): a wrong --builddir silently finds zero .su files, so
    # every function falls back to the prologue estimator -- no crash, no obviously
    # wrong number, just a report that LOOKS legitimate and certifies nothing. Fail
    # loud instead of guessing the directory is fine.
    if not glob.glob(os.path.join(args.builddir, "*.su")):
        print(f"*** stack_budget: 0 .su files under --builddir {args.builddir!r} -- "
              "wrong directory? (every function would silently fall back to the prologue "
              "estimator, which is not a build failure this guard could ever surface)",
              file=sys.stderr)
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

    field_decls, argsite_decls, whole_func_decls, frame_overrides = (
        load_extra_edges(args.edges_file) if args.edges_file
        else ({}, {}, {}, {}))

    # D1 header-drift check: every declared Struct.field @OFFSET is checked against
    # the offset the struct's OWN header gives that field today, before anything
    # else runs. A stale offset (header changed, this file didn't) is fatal -- it
    # would otherwise let a site silently match the WRONG field's implementation
    # list, or (worse) stop matching a real field and turn into a spurious blind spot.
    field_problems = verify_field_declarations(field_decls, args.source_dir, STRUCT_HEADERS)
    if field_problems:
        print(f"*** stack_budget: {args.edges_file} has stale struct-field declaration(s):",
              file=sys.stderr)
        for p in field_problems:
            print(f"***   {p}", file=sys.stderr)
        return 1

    edges_to_add, blind, count_mismatches, legacy_ambiguous = resolve_all_sites(
        analysis, field_decls, argsite_decls, whole_func_decls)
    for caller, impls in edges_to_add.items():
        analysis["edges"][caller] |= impls

    all_impls = ({impl for impls in field_decls.values() for impl in impls[1]}
                 | {impl for _n, impls in argsite_decls.values() for impl in impls}
                 | {impl for impls in whole_func_decls.values() for impl in impls})
    unknown_impls = sorted(all_impls - analysis["funcs"])
    if unknown_impls:
        print(f"*** stack_budget: WARNING -- {args.edges_file} names implementation(s) not "
              f"found in {args.elf}: {unknown_impls} (stale declaration? typo? inlined away?)")

    # A caller declared with `argsites=N` whose disassembly count no longer matches N
    # is a stale declaration -- FATAL immediately, independent of the top-N chains
    # (the whole point is to catch a NEW site the reviewer never counted).
    if count_mismatches:
        print(f"*** stack_budget: {args.edges_file} argsites count(s) no longer match "
              "the disassembly:", file=sys.stderr)
        for caller, n, found in count_mismatches:
            print(f"***   {caller}: declared argsites={n}, disassembly has {found} -- "
                  "a site appeared or vanished; re-read the source and update the count",
                  file=sys.stderr)
        return 1
    if legacy_ambiguous:
        print(f"*** stack_budget: {args.edges_file} declares a whole-function exemption "
              "for a caller that now has MORE THAN ONE indirect-call site -- that is "
              "exactly the per-function blind-spot defect (BACKLOG #84b D1); split it "
              "into structured 'Struct.field @OFF' / 'caller argsites=N' declarations:",
              file=sys.stderr)
        for fn in sorted(legacy_ambiguous):
            print(f"***   {fn}", file=sys.stderr)
        return 1

    chains = top_n_chains(args.root, analysis["edges"], su_sizes, estimated,
                           n=args.top, overrides=frame_overrides)
    deepest_total, deepest_path, cycles = chains[0][0], chains[0][1], chains[0][2]

    # STOP-LICENCE check (D4, BACKLOG #84b fourth pass): any indirect-call site inside
    # a function REACHABLE FROM --root that resolve_all_sites() could not tie to a
    # declared struct field or argsites class means the walker could not see a
    # possible deeper continuation past that point -- surface it instead of silently
    # trusting the number. This is now a WHOLE-GRAPH sweep, not just the printed top-N
    # chains: the deepest-chain search only descends the single heaviest branch at
    # each fan-out, so an undeclared site on a shallower sibling can hide an
    # arbitrarily deep continuation that branch's own subtree never gets printed to
    # reveal. Per-SITE, not per-function (D1): a function with ten declared field
    # classes gets no free pass for an eleventh, undeclared one.
    on_chain = set()
    unknown_on_chain = set()
    for _tot, path, _cyc in chains:
        on_chain |= {name for name, _b, _s in path}
        unknown_on_chain |= {name for name, _b, s in path if s == "unknown"}
    reachable = reachable_from(args.root, analysis["edges"])
    blind_spots = whole_graph_blind_spots(reachable, blind)

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
            tag = {"su": "", "estimated": " (estimated)", "unknown": " (UNKNOWN, counted 0)",
                   "override": " (frame override, hand-measured)"}[src]
            print(f"      {b:6,d}  {name}{tag}")
        if cyc:
            print(f"      WARNING: recursion excluded at: {cyc}")

    # D2: a frame the prologue estimator could not classify (see estimate_frames'
    # own header) is UNKNOWN, not a trustworthy 0 -- one on a top-N chain means the
    # printed total above is not actually bounded. FATAL unless stack_edges.txt
    # carries a `frame fn = BYTES` hand-measured override for it.
    if unknown_on_chain:
        print(f"\n*** STACK_BUDGET UNKNOWN FRAME: the prologue estimator could not classify "
              "the stack frame of the following function(s), which sit on a top-N chain above "
              "-- the printed chain totals are NOT bounded past this point:")
        for fn in sorted(unknown_on_chain):
            print(f"***   {fn}")
        print("*** Refusing to certify a number the walker cannot back. Fix: add a "
              "`frame fn = BYTES  (measured by hand, date)` line to "
              f"{args.edges_file}, or figure out why the estimator's known prologue "
              "idioms don't match this function's epilogue.")
        return 1

    if blind_spots:
        print("\n*** STACK_BUDGET BLIND SPOT: unresolved indirect call(s) inside a function "
              "on the deepest chain -- the true depth past this point is UNKNOWN, not "
              "bounded by this report:")
        for fn, addr, ins, detail in blind_spots[:10]:
            print(f"***   {fn} @ {addr}: {ins}  [{detail}]")
        print("*** Refusing to certify a number the walker cannot back. Fix: give the "
              "indirect call site a resolvable target (devirtualize / annotate), or "
              "declare it in stack_edges.txt ('Struct.field @OFF -> impls' or "
              "'caller argsites=N -> impls').")
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
