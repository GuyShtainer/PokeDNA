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
        tgt_ins = insn.get(t, "")
        bxm = re.match(r'^bx\s+(\w+)', tgt_ins)
        if bxm and t not in name_at:
            # trap #2: bx-rN thunk, unresolved -- record which register it dispatches
            # through so the caller can try to resolve the site to a declared struct
            # field (D1: BACKLOG #84b review).
            indirect_sites[fn].append((hex(a), ins, bxm.group(1)))
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
        "fn_insn_seq": dict(fn_insn_seq),
    }


FIELD_LINE_RE = re.compile(r'^(\w+)\.(\w+)\s*@(\d+)$')
ARGSITE_LINE_RE = re.compile(r'^(\w+)\s+argsites=(\d+)$')
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
}


BRANCH_MNEM_RE = re.compile(
    r'^(b|bx|beq|bne|bcs|bcc|bmi|bpl|bvs|bvc|bhi|bls|bge|blt|bgt|ble)'
    r'(\.[nw])?(\s|$)')
CALL_MNEM_RE = re.compile(r'^(bl|blx)(\.[nw])?(\s|$)')
STORE_MNEM_RE = re.compile(r'^(str\w*|stm\w*|push)\b')
CMP_MNEM_RE = re.compile(r'^(cmp|cmn|tst|teq)\b')
MOV_REG_RE = re.compile(r'^movs?\s+(r\d+|ip)\s*,\s*(r\d+|ip)\s*$')
DEST_REG_RE = re.compile(r'^[a-z][a-z0-9]*\s+(r\d+|ip)\b')
LDR_FIELD_RE = re.compile(
    r'^ldr\w*\s+(r\d+|ip)\s*,\s*\[\s*(r\d+|ip|sp|pc)\s*,\s*#(-?\d+)\s*\]')
CALLER_SAVED_REGS = frozenset(('r0', 'r1', 'r2', 'r3', 'ip'))   # AAPCS scratch registers


BASE_LITERAL_WINDOW = 8   # instructions to look back for the base register's own origin


def _base_is_section_anchor(fn_insn_seq, ldr_idx, base_reg):
    """True if `base_reg` (the rY in `ldr rN, [rY, #off]`) was ITSELF just materialized
    from a PC-relative literal (`ldr rY, [pc, #imm]`) within the last BASE_LITERAL_WINDOW
    instructions, with nothing redefining it in between. That shape -- literal-load a
    fixed address, then immediately index off it -- is GCC's `-fsection-anchors` codegen
    for a STATIC/global variable (default at -O2 for this target): every small static in
    a section shares ONE base register and reaches each other by a small #offset, which
    is byte-for-byte indistinguishable from a struct-field dereference UNLESS the base's
    own origin is checked. None of BoxSource/RomGbUi/Scan is ever addressed as a global
    in this codebase (always a parameter, spilled or in a register) -- a base fed by a
    fresh literal is therefore a global/anchor access, not an instance field, and must
    NOT be trusted as 'field' (confirmed empirically: a planted global function-pointer
    dispatch compiled to exactly this shape and, without this check, silently matched a
    declared field offset by coincidence -- a real false-pass, not a hypothetical one)."""
    lo = max(0, ldr_idx - BASE_LITERAL_WINDOW)
    for _addr, ins in reversed(fn_insn_seq[lo:ldr_idx]):
        ins_clean = ins.split('@')[0].strip()
        m = LDR_FIELD_RE.match(ins_clean)
        if m and m.group(1) == base_reg:
            return m.group(2) == 'pc'
        dm = DEST_REG_RE.match(ins_clean)
        if dm and dm.group(1) == base_reg:
            return False                    # base redefined by something else first
    return False


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
    ap.add_argument("--source-dir",
                     default=os.path.join(os.path.dirname(os.path.dirname(
                         os.path.abspath(__file__))), "source"),
                     help="where struct-field @OFFSET declarations are checked against "
                          "the real header (default: source/ next to tools/)")
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

    chains = top_n_chains(args.root, analysis["edges"], su_sizes, estimated, n=args.top)
    deepest_total, deepest_path, cycles = chains[0][0], chains[0][1], chains[0][2]

    # STOP-LICENCE check: any indirect-call site inside a function on the top-N chains
    # that resolve_all_sites() could not tie to a declared struct field or argsites
    # class means the walker could not see a possible deeper continuation past that
    # point -- surface it instead of silently trusting the number. Per-SITE, not
    # per-function (D1): a function with ten declared field classes gets no free pass
    # for an eleventh, undeclared one.
    blind_spots = []
    on_chain = set()
    for _tot, path, _cyc in chains:
        on_chain |= {name for name, _b, _s in path}
    for fn in sorted(on_chain):
        for addr, ins, detail in blind.get(fn, []):
            blind_spots.append((fn, addr, ins, detail))

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
