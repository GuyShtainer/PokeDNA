#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""stack_budget.py - post-link: fail the build before the user stack overruns.

    python3 tools/stack_budget.py --elf PokeDNA-artless.elf --builddir build-artless

PokeDNA has ONE user stack (__sp_usr down to __iheap_start; libtonc's isr_master
switches to SYSTEM mode, so IRQ handlers also run on THIS stack, +64 B). There is no
MMU and no guard page: an overrun silently corrupts whatever sits right below
__iheap_start (D9, BACKLOG #84b fifth pass: NOT newlib's heap -- newlib's heap grows
UP from __eheap_start, which is in EWRAM, an entirely different region; what actually
sits below __iheap_start in IWRAM is __iwram_overlay_end, the tail of the .iwram.c
fast-path code -- the flashcart SD I/O routines this project keeps out of the stack's
own region on purpose, see the golden rules), then the .bss beyond THAT -- the closest
thing GBA homebrew has to a segfault is "SD reads started returning garbage three
screens later." The EWRAM guard above this one in the Makefile catches the sibling
failure (.sbss overrunning EWRAM); this one catches the stack overrunning ITS ceiling,
one function-call chain at a time.

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

COST (G7, BACKLOG #84b eighth pass)
    One normal (non -j) build variant's post-link run costs low single-digit seconds
    wall (artless/delta ~1.4-1.6 s; the 7.7 MB normal image ~3.2 s (the address-taken sweep scales with the image) -- objdump -d over
    .text plus objdump -s over every alloc/load section dominate and both scale with
    disk/CPU speed; parsing the .su files and walking the graph itself is a small
    fraction of that). Paid once per linked ELF, at the very end of the link step --
    not per .o, and not per -j worker.
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
# D9 (BACKLOG #84b fifth pass): where 64 comes from, spelled out once instead of
# asserted. libtonc's isr_master (the one IRQ vector every installed handler runs
# through) pushes {r2,r3,ip,lr} on the __sp_irq IRQ-mode stack -- a SEPARATE stack
# above __sp_usr, never this project's user-stack room -- then switches to SYSTEM
# mode (which shares __sp_usr with normal execution) and pushes {r0,lr}, 8 B, onto
# THIS stack per nesting level, before calling the installed handler. Two handlers
# are installed in this codebase (grep `irq_add`): hb_isr (pdna_main.c:1275, VBlank,
# whose own call chain measures 24 B) and pwm_isr (rumble.c:163, TIMER2, 8 B). The
# same IRQ line can't re-enter itself (isr_master masks its own line for the
# duration), so the worst nesting is one of each: 2 x 8 B system-mode entry + hb_isr's
# 24 B + pwm_isr's 8 B = 48 B <= the 64 B charged here. 64 is therefore a safety
# margin over the measured 48, not itself a measurement -- D1 below turns "the
# handlers fit under 64" into an ACTUAL guard-time check instead of a comment
# asserting it stays true.
ISR_BYTES = 64         # libtonc isr_master runs handlers on __sp_usr too (see above)

# G1 (BACKLOG #84b eighth pass, merge-blocker): an `addrtaken-ok fn (reason)` line is a
# claim nobody re-verifies -- the reviewer's own comment attached to it (the sweep just
# trusts the text) -- so bound what a WRONG one can cost instead of letting it hide an
# arbitrarily heavy chain behind a one-line exemption. 256 B is generous next to every
# legitimate exemption on file today (all <= 128 B, grep `addrtaken-ok` in
# tools/stack_edges.txt) while still catching the reviewer's planted fixture: a literal
# stored to a global then reloaded and called through a register (trap #5) whose own
# worst chain is thousands of bytes.
EXEMPT_MAX_DEEPEST = 256

# === .su parsing (exact, compiler-measured frames) ===================================

_ASM_SOURCE_EXTS = (".s", ".S")


def _object_source_is_asm(obj):
    """True when `obj`'s own .d file (written by -MMD alongside every compile, C or
    asm) says its source is a bare assembler file (.s/.S). Those compile via `$(CC) -x
    assembler-with-cpp $(ASFLAGS)` -- no -fstack-usage on that line at all (it's a
    CFLAGS/CXXFLAGS-only flag) -- so GCC never emits a .su for them, by construction,
    same as PokeDNA's own several hand-written *_data.s .incbin blobs (pure data, no
    function with a stack frame in them at all). Returns False (i.e. "expect a .su")
    when the .d is missing or unreadable -- an absent .d is itself a sign of a
    stale/broken object, not a reason to exempt it."""
    d = obj[:-2] + ".d"
    try:
        with open(d, encoding="utf-8", errors="replace") as f:
            head = f.read(4096)
    except OSError:
        return False
    # First dependency line looks like "foo.o: /path/to/foo.c [...]" (-MMD/-MF's own
    # format; the Makefile always names the target bare, matching $@ in that recipe).
    colon = head.find(":")
    if colon < 0:
        return False
    # make's line-continuation backslash ("foo.o: \\\n src.s") is its own whitespace-
    # separated token after a plain .split() -- skip it to reach the real first source
    # path instead of testing the extension of a bare "\".
    rest = [tok for tok in head[colon + 1:].split() if tok != "\\"]
    if not rest:
        return False
    src = rest[0]
    return os.path.splitext(src)[1] in _ASM_SOURCE_EXTS


def find_objects_missing_su(builddir):
    """BACKLOG #110: every *.o directly under --builddir compiled from C/C++ MUST
    carry a same-basename *.su (the Makefile always passes -fstack-usage on those
    compile lines) -- a STALE build directory can hold .o's compiled before that flag
    was on CFLAGS (or under an older CFLAGS entirely), whose frames would otherwise
    silently fall back to the prologue ESTIMATOR and print as merely "(estimated)"
    instead of the FATAL this deserves. Returns the sorted list of offending .o paths
    (empty when every C/C++ .o has its .su, including when there are no .o's at all --
    callers check that separately). Two classes are correctly exempt, both detected by
    origin rather than assumed: (1) newlib/libgcc/libtonc archive members never live
    under --builddir at all (they come from .a's under $(LIBDIRS)/lib and devkitARM's
    own lib/gcc tree); (2) a bare-assembler .o (source_is_asm(), the several
    *_data.s .incbin art/data blobs) compiles with no -fstack-usage on its own command
    line by construction, so it never gets a .su even fresh off today's build."""
    missing = []
    for obj in sorted(glob.glob(os.path.join(builddir, "*.o"))):
        if os.path.exists(obj[:-2] + ".su"):
            continue
        if _object_source_is_asm(obj):
            continue
        missing.append(obj)
    return missing


def load_su(builddir):
    """basename -> worst-case frame size, from every *.su GCC emitted alongside the .o's.

    A function can appear more than once (inlined-then-reinstantiated clones, or two
    TUs defining a static of the same name) -- take the max, as the reviewer's cg2.py
    did, since we want the WORST case, not an arbitrary one, for the bare (unqualified)
    key.

    D5b (BACKLOG #84b sixth pass): ALSO store `name@tu` for every name that shows up
    in more than one .su FILE -- tu being the .su file's own basename with the
    trailing ".su" stripped, exactly the TU key read_symbol_census() derives from the
    ELF's STT_FILE marker (both come from the same GCC-emitted "<sourcefile>.c" ->
    "<sourcefile>" transform) -- so a duplicated static's unique node name
    ("validate@region_map") resolves to ITS OWN TU's frame, not the cross-TU max the
    bare key still (harmlessly) carries for anything that never needed disambiguating.
    Restricted to only the ~dozens of genuinely multi-file names (not every one of the
    thousands of .su entries) on purpose: su_frame()'s fallback path is an O(len(sizes))
    scan over every key sharing a stripped base, run on every cache miss -- doubling
    `sizes`' size by tagging every entry, tried first, measured a real ~40% slowdown
    across the whole guard (2,700+ su_frame() calls per run) for zero benefit on the
    huge majority of names that were never ambiguous in the first place.
    """
    sizes = {}
    dup = collections.defaultdict(set)
    name_files = collections.defaultdict(set)
    tu_entries = []   # (name, tu, n) -- only replayed into `sizes` for multi-file names
    for f in glob.glob(os.path.join(builddir, "*.su")):
        tu = os.path.basename(f)
        if tu.endswith(".su"):
            tu = tu[:-3]
        base = os.path.basename(f)
        for line in open(f):
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            name = parts[0].rsplit(":", 1)[-1]
            try:
                n = int(parts[1])
            except ValueError:
                continue
            dup[name].add((base, n, parts[2]))
            sizes[name] = max(sizes.get(name, 0), n)
            name_files[name].add(base)
            tu_entries.append((name, tu, n))
    multi_file = {name for name, files in name_files.items() if len(files) > 1}
    for name, tu, n in tu_entries:
        if name in multi_file:
            sizes[f"{name}@{tu}"] = n
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


# === D5b (BACKLOG #84b sixth pass): duplicate static names are distinct nodes ========
#
# `objdump -d` labels every function block with its bare symbol NAME, and two static
# functions in two different TUs sharing a name (FatFs's tiny `validate` vs
# region_map.c's much larger `validate`, ten `s_wait`s, three `read_verified`s, ...)
# print as the SAME `<name>:` label at two different addresses. analyze() used to key
# `edges`/`name_at` purely by that string, which UNIONS the two functions' callees
# into one fictional node and can manufacture a cycle out of two unrelated call
# chains that happen to pass through same-named statics (the "f_lseek -> validate ->
# mr_lz77_size" ghost edge: FatFs's f_lseek really calls FatFs's validate, which never
# calls mr_lz77_size; region_map's validate is a different function two frames over
# on a DIFFERENT chain that also happens to call f_lseek deeper in, and the merged
# node made it look like validate calls back into its own caller's caller).
#
# The fix is a pre-pass over the ELF's OWN symbol table (`objdump -t`, not the
# disassembly): find every local FUNC symbol name with more than one distinct entry
# address, and give each address its own name, `<name>@<tu>`, tu being the source
# file that most recently preceded it (an STT_FILE / `df *ABS*` marker -- objdump -t
# lists local symbols grouped after the file that defines them, in link order).
# `<tu>` matches a `*.su` file's own basename (load_su() derives the exact same key),
# so the disambiguated node's frame is looked up in ITS OWN TU's .su output, not a
# cross-TU max. A local symbol with no preceding file marker at all (should not
# happen for a normal C build, but not asserted away) falls back to `<name>@<addr>`.


def dump_symbol_table(elf):
    return subprocess.run([OBJDUMP, "-t", elf], capture_output=True, text=True,
                           check=True).stdout


def parse_symbol_census(out):
    """Parse `objdump -t <elf>`'s own text (see dump_symbol_table()) into
    (addr_unique_name, dup_names, entries). Split out from read_symbol_census() so
    main() can cache the raw `objdump -t` text alongside the `objdump -d` disassembly
    text it already caches (both are the slow steps; this parse itself is cheap) and
    so a fixture can exercise the parsing logic without shelling out at all.

    addr_unique_name : {addr:int -> node name}, node name is the bare symbol name
                        UNLESS that name has more than one distinct entry address
                        anywhere in the table, in which case it is qualified
                        "<name>@<tu>" (tu = the preceding STT_FILE marker's
                        basename with a trailing ".c" stripped, or the address in
                        hex if no file marker precedes this symbol at all).
    dup_names        : {name, ...} bare names with more than one distinct address --
                        stack_edges.txt must never reference one of these unqualified.
    entries           : [(addr, name, tu_or_None), ...] in objdump -t's own order, for
                         diagnostics/tests.

    objdump -t's line shape (binutils, this target): a TAB separates the flags+section
    +size half from the name half, e.g.
        "08056e9c l     F .text\\t0000027c validate"
        "00000000 l    df *ABS*\\t00000000 region_map.c"
    -- split once on the tab, then whitespace-split each half; the left half's 3rd+
    token(s) carry the symbol type ("F" function, "df" file), the right half is
    "<size> <name>".
    """
    entries = []
    cur_tu = None
    for line in out.splitlines():
        if "\t" not in line:
            continue
        left, right = line.split("\t", 1)
        lparts = left.split()
        rparts = right.split(None, 1)
        if len(lparts) < 2 or len(rparts) < 2:
            continue
        try:
            addr = int(lparts[0], 16)
        except ValueError:
            continue
        rest = lparts[2:]
        name = rparts[1].strip()
        # objdump -t prefixes a non-default-visibility symbol's NAME with a literal
        # ".hidden "/".internal "/".protected " marker (only in -t's output, never in
        # -d's block labels) -- strip it so this census's names match what analyze()
        # reads out of the disassembly for the same address (".hidden __udivmoddi4"
        # in -t vs plain "__udivmoddi4" in -d); left unstripped, the census's name
        # never matches any disassembly label and scan_address_taken() reports a
        # perfectly ordinary, perfectly reachable libgcc helper as an undeclared
        # orphan on every build.
        name = re.sub(r'^\.(hidden|internal|protected)\s+', '', name)
        if "df" in rest:
            cur_tu = name
            continue
        if "F" not in rest:
            continue
        entries.append((addr, name, cur_tu))

    name_addrs = collections.defaultdict(set)
    for addr, name, _tu in entries:
        name_addrs[name].add(addr)
    dup_names = {n for n, addrs in name_addrs.items() if len(addrs) > 1}

    # Only addresses whose NAME the census found duplicated get an entry here -- the
    # opposite shape (one address, several DIFFERENT weak-alias names, e.g. libgcc's
    # __aeabi_idiv0/__aeabi_ldiv0 both naming the same 2-byte stub) is not this
    # function's problem: objdump -d already resolves that case to one consistent
    # label for every use of the address (the bl-target lookup and the disassembly
    # block header agree), so leaving those addresses OUT of this dict and letting
    # analyze() fall back to the disassembly's own m.group(2) name keeps that existing,
    # correct behaviour untouched. Populating this dict for every address (picking
    # whichever alias objdump -t happened to list first) would silently substitute a
    # DIFFERENT name than the one the call graph and address-taken scan already agree
    # on, for no reason connected to D5b's actual bug.
    addr_unique_name = {}
    for addr, name, tu in entries:
        if name not in dup_names:
            continue
        if tu is not None:
            tu_key = tu[:-2] if tu.endswith(".c") else tu
            addr_unique_name[addr] = f"{name}@{tu_key}"
        else:
            addr_unique_name[addr] = f"{name}@{addr:08x}"
    return addr_unique_name, dup_names, entries


def read_symbol_census(elf):
    """Convenience wrapper: shell out to `objdump -t` and parse it in one call (used
    by tests and any non-main() caller that doesn't need the caching main() does)."""
    return parse_symbol_census(dump_symbol_table(elf))


def analyze(dump_text, addr_unique_name=None):
    """One pass over the disassembly: (a) build the call graph with traps #1-#3
    handled, (b) collect raw instruction lines per function for the prologue
    estimator, (c) record indirect-call sites (unresolved targets) so the caller can
    check whether one falls on the reported deepest chain."""
    addr_unique_name = addr_unique_name or {}
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
            # D5b: use the census's disambiguated name for this address when the
            # bare disassembly label is one objdump -t found duplicated elsewhere in
            # the image -- everything downstream (name_at, edges, fn_lines,
            # fn_insn_seq, indirect_sites) is keyed off `cur`, so this one
            # substitution is enough to make every duplicated static a distinct node.
            cur = addr_unique_name.get(a, m.group(2))
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
        "name_at": dict(name_at),   # D1 (BACKLOG #84b fifth pass): the address-taken sweep
        "edges": edges,
        "indirect_sites": dict(indirect_sites),
        "weird": weird,
        "veneers": veneers,
        "fn_lines": fn_lines,
        "fn_insn_seq": dict(fn_insn_seq),
    }


FIELD_LINE_RE = re.compile(r'^(\w+)\.(\w+)\s*@(\d+)(?:\s+in\s+([\w.@]+(?:\s*,\s*[\w.@]+)*))?$')
# D5b's `@tu`-qualified caller names (a disambiguated duplicate static, e.g.
# `parse_header@rom_gbui`) can appear in the `in <caller>,...` list, so the caller
# character class includes `@` alongside `\w` and `.` (the .constprop/.isra/.part
# clone-suffix characters).
# D5a (BACKLOG #84b seventh pass): the optional trailing `in caller1,caller2,...`
# qualifies a field declaration to a SET OF CALLERS instead of the bare offset --
# see load_extra_edges()'s own doc for why (two unrelated structs sharing a numeric
# offset used to get UNIONED into one class, so ANY site at that offset in ANY
# caller was credited BOTH structs' implementations, real or not).
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
# D1 (BACKLOG #84b fifth pass): two more declaration shapes, for the address-taken sweep.
ISR_LINE_RE = re.compile(r'^isr\s+(\S+)$')
ADDRTAKEN_OK_RE = re.compile(r'^addrtaken-ok\s+(\S+)$')
# D4 (BACKLOG #84b sixth pass): a declared depth for a real recursive SCC.
RECURSION_LINE_RE = re.compile(r'^recursion\s+(\S+)\s+depth=(\d+)$')


def load_extra_edges(path):
    """Parse tools/stack_edges.txt. Three declaration shapes, one non-comment line each
    (D1, BACKLOG #84b review, fixing the per-FUNCTION blind-spot exemption defect):

      Struct.field @OFFSET -> impl1 impl2 ...
      Struct.field @OFFSET in caller1,caller2,... -> impl1 impl2 ...
        A struct-field indirect-call class (BoxSource.records, RomGbUi.read, ...).
        OFFSET is checked against the field's ACTUAL offset (computed from the
        struct's own header via struct_field_offsets()) before it is trusted --
        a stale/wrong OFFSET here is a FATAL, not a silent pass. Sites are matched
        to a class by the offset the disassembly loads, not by which function they
        sit in, so an undeclared field added to a struct with ten declared fields
        gets no free pass from its ten siblings.

        D5a (BACKLOG #84b seventh pass): the OLD matching was GLOBAL by bare offset
        -- two unrelated structs sharing offset 20 (BoxSource.records and
        AppSrcOps.view, say) got UNIONED into one class, so a BoxSource.records()
        site in a pdna_box.c caller was silently credited AppSrcOps.view's
        implementations too (a false PASS: a chain through the BoxSource caller
        could "reach" gb_view_hook, a function it never actually calls). The
        trailing `in caller1,caller2,...` qualifies a declaration to the exact set
        of callers it is honest for; resolution now keys on (offset, caller), not
        offset alone. The bare (unqualified) form stays legal ONLY when no OTHER
        struct.field declares that same offset anywhere in this file -- the parser
        raises "offset N is claimed by A.x and B.y -- qualify by caller" the
        moment a second struct.field shows up at an offset that already has (or
        gains) an unqualified declaration. Two DIFFERENT structs genuinely
        dereferenced at the same offset by the SAME caller (a caller that legally
        uses both) list both impl sets for that (caller, off) pair -- not a
        conflict, just two declarations sharing a key, which ACCUMULATE like any
        other repeated declaration.

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

      isr fn
        D1 (BACKLOG #84b fifth pass): names a function installed as an interrupt
        handler (via `irq_add`) -- the guard measures the ISR re-entry allowance
        from every declared handler's own worst chain instead of trusting a bare
        constant, and a declared handler is exempt from the address-taken FATAL
        (its address is taken by `irq_add` itself, not a struct field/global this
        file has any other way to name).

      addrtaken-ok fn
        D1's escape hatch: `fn`'s address is genuinely taken somewhere in the
        linked image (a real function pointer this walker's sweep will find) but
        is a confirmed false positive -- e.g. a compiler-generated table entry
        with no runtime call path this project's code ever exercises. Each use
        should carry a one-line reason in a trailing comment; the sweep still
        finds and reports these functions, they are just not fatal.

      recursion fn depth=N
        D4 (BACKLOG #84b sixth pass): `fn` sits in a real strongly-connected
        component of the call graph (found by tarjan_sccs() over --root's reachable
        set) -- the OLD walker silently truncated any such cycle to 0 B and
        memoized that 0, so the printed total secretly depended on which function
        happened to be visited FIRST (top_n_chains and deepest_from could disagree
        by hundreds of bytes on the exact same ELF). This declares how many times
        the component's OWN worst internal cycle can genuinely re-enter at runtime;
        the guard charges N times the SUM OF EVERY FRAME IN THE COMPONENT (not just
        the frames on one particular cycle through it) at the component's entry --
        conservative on purpose, since which internal path is "the" worst one can
        change as the code does. A reachable SCC with NO member declared here is a
        FATAL: an undeclared cycle is not a bound this walker can vouch for. One
        declaration per component is enough (any single member); declaring more
        than one member of the same component must agree on N or that PAIR is
        itself a FATAL (a real question -- which is right? -- not a silent pick).

    Multiple lines per caller/struct ACCUMULATE (pdna_box has eleven field lines).
    Returns (field_decls, field_offset_index, argsite_decls, whole_func_decls,
             frame_overrides, isr_decls, addrtaken_ok, recursion_decls):
      field_decls        : {(struct, field): (offset, {impls})}  -- for header
                            verification ONLY (verify_field_declarations()); it
                            unions every impl regardless of caller qualification,
                            since the offset a field lives at never depends on who
                            calls it.
      field_offset_index  : (qualified, unqualified) -- the per-caller resolution
                            structure D5a introduces:
                              qualified   : {(offset, caller): {impls}}
                              unqualified : {offset: {impls}}  -- only for offsets
                                            no OTHER struct.field also claims
      argsite_decls     : {caller: (N, {impls})}
      whole_func_decls  : {caller: {impls}}
      frame_overrides   : {fn: bytes}
      isr_decls         : {fn, ...}
      addrtaken_ok      : {fn, ...}
      recursion_decls    : {fn: depth}
    """
    field_decls = {}
    field_site_decls = []   # [(struct, field, offset, callers_frozenset_or_None, {impls})]
    argsite_decls = {}
    whole_func_decls = collections.defaultdict(set)
    frame_overrides = {}
    isr_decls = set()
    addrtaken_ok = set()
    recursion_decls = {}
    if not path or not os.path.exists(path):
        return (field_decls, ({}, {}), argsite_decls, whole_func_decls, frame_overrides,
                isr_decls, addrtaken_ok, recursion_decls)
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            fm = FRAME_LINE_RE.match(line)
            if fm:
                frame_overrides[fm.group(1)] = int(fm.group(2))
                continue
            im = ISR_LINE_RE.match(line)
            if im:
                isr_decls.add(im.group(1))
                continue
            aok = ADDRTAKEN_OK_RE.match(line)
            if aok:
                addrtaken_ok.add(aok.group(1))
                continue
            rm = RECURSION_LINE_RE.match(line)
            if rm:
                fn, depth = rm.group(1), int(rm.group(2))
                if fn in recursion_decls and recursion_decls[fn] != depth:
                    raise ValueError(f"{path}:{lineno}: recursion {fn} declared twice "
                                      f"with different depths ({recursion_decls[fn]} "
                                      f"and {depth})")
                recursion_decls[fn] = depth
                continue
            if '->' not in line:
                raise ValueError(f"{path}:{lineno}: unrecognized line: {raw!r}")
            lhs, rhs = line.split('->', 1)
            lhs = lhs.strip()
            impls = set(rhs.split())
            fm2 = FIELD_LINE_RE.match(lhs)
            if fm2:
                struct, field, off = fm2.group(1), fm2.group(2), int(fm2.group(3))
                callers_str = fm2.group(4)
                callers = (frozenset(c.strip() for c in callers_str.split(','))
                           if callers_str else None)
                key = (struct, field)
                if key in field_decls:
                    prev_off, prev_impls = field_decls[key]
                    if prev_off != off:
                        raise ValueError(f"{path}:{lineno}: {struct}.{field} declared at "
                                          f"two different offsets ({prev_off} and {off})")
                    field_decls[key] = (off, prev_impls | impls)
                else:
                    field_decls[key] = (off, impls)
                field_site_decls.append((struct, field, off, callers, impls))
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

    # D5a: build the per-caller resolution index and enforce the "qualify by caller
    # the moment a second struct.field shares your offset" rule -- one pass over
    # every parsed field-site line, now that the whole file has been read (the
    # ambiguity is a property of the FILE, not any one line).
    offset_owners = collections.defaultdict(set)
    for struct, field, off, _callers, _impls in field_site_decls:
        offset_owners[off].add((struct, field))
    qualified = collections.defaultdict(set)
    unqualified = collections.defaultdict(set)
    for struct, field, off, callers, impls in field_site_decls:
        if callers is None:
            if len(offset_owners[off]) > 1:
                owners = ', '.join(f"{s}.{f}" for s, f in sorted(offset_owners[off]))
                raise ValueError(
                    f"{path}: offset {off} is claimed by {owners} -- qualify "
                    f"'{struct}.{field} @{off}' with 'in <caller>[,<caller>...]'")
            unqualified[off] |= impls
        else:
            for c in callers:
                qualified[(off, c)] |= impls
    field_offset_index = (dict(qualified), dict(unqualified))

    return (field_decls, field_offset_index, argsite_decls, dict(whole_func_decls),
            frame_overrides, isr_decls, addrtaken_ok, recursion_decls)


# === struct-field offsets, computed from the header (D1) ==============================

_STRUCT_FIELD_RE = re.compile(r'\(\*(\w+)\)\(')
_KNOWN_PTR_TYPEDEFS = {"AppCommitFn", "GbReadFn", "Gb12ReadFn", "G2ReadFn", "Gen1ReadFn",
                        "ArtReadFn", "G2WriteFn", "RomReadFn", "ArtIconsProgressFn"}
_KNOWN_VALUE_TYPES = {"bool": 1, "int": 4, "int32_t": 4, "uint32_t": 4, "int16_t": 2,
                       "uint16_t": 2, "int8_t": 1, "uint8_t": 1, "char": 1}


def _strip_c_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return text


# === G4 (BACKLOG #106): parse a struct by BRACE DEPTH, not a lazy forward regex =========
#
# The old struct_field_offsets() searched for `typedef struct { ... } NAME;` with a
# non-greedy `.*?` body -- which matches from the FIRST `typedef struct {` anywhere
# earlier in the file to the FIRST `} NAME;` after it, spanning every unrelated
# anonymous struct in between whenever more than one precedes the target (confirmed
# live: RomGbSprite/Gb12Mount/G2Writer/RomCtx/ArtIconsGen all misparsed this way,
# hence the six `STRUCT_HEADERS[...] = None` hand-verified escapes this fix removes).
# The replacement locates the struct/enum by BRACE-DEPTH matching instead: a tagged
# form (`struct NAME {`) is found directly; an anonymous typedef form is found by its
# ENDING (`} NAME;`, unique to that one struct) and walked BACKWARD to its true
# opening brace -- immune to however many other structs sit in between either way.

def _matching_close_brace(text, open_pos):
    """Index of the `}` that closes the `{` at `text[open_pos]`, or None if the
    braces from there on are unbalanced."""
    depth = 0
    for i in range(open_pos, len(text)):
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i
    return None


def _struct_or_enum_body(text, name):
    """(kind, body_text) for `name`'s struct/enum definition in `text` -- 'struct'
    or 'enum', and the text strictly between its braces. Tries a TAGGED opening
    (`struct NAME {` / `enum NAME {`) first; falls back to an ANONYMOUS typedef's
    ENDING (`} NAME;`) walked backward via brace-depth counting to its real
    opening brace, checking that a bare `typedef struct`/`typedef enum` (nothing
    else) immediately precedes that brace so an unrelated `}` earlier in the file
    can never be mistaken for this struct's own open. Returns None if `name` is
    defined nowhere in `text` this way (an external type, or a shape this walker
    doesn't recognize -- e.g. a plain untagged, non-typedef'd struct)."""
    esc = re.escape(name)
    m = re.search(r'\b(struct|enum)\s+' + esc + r'\s*\{', text)
    if m:
        open_pos = m.end() - 1
        close_pos = _matching_close_brace(text, open_pos)
        if close_pos is not None:
            return m.group(1), text[open_pos + 1:close_pos]
    for end_m in re.finditer(r'\}\s*' + esc + r'\s*;', text):
        close_pos = end_m.start()
        depth = 1
        open_pos = None
        for i in range(close_pos - 1, -1, -1):
            c = text[i]
            if c == '}':
                depth += 1
            elif c == '{':
                depth -= 1
                if depth == 0:
                    open_pos = i
                    break
        if open_pos is None:
            continue
        prefix_m = re.search(r'typedef\s+(struct|enum)\s*\Z', text[:open_pos].rstrip())
        if prefix_m:
            return prefix_m.group(1), text[open_pos + 1:close_pos]
    return None


_DEFINE_INT_RE = re.compile(r'^\s*#\s*define\s+(\w+)\s+(0[xX][0-9a-fA-F]+|\d+)\s*[uUlL]*\s*$',
                             re.M)


def _parse_int_macros(text):
    """{macro_name: int} for every simple `#define NAME 123` / `#define NAME
    0x20u` in `text` -- resolves an array member's element count ONLY when it is
    a plain integer macro defined in the SAME header (ROM_MAX_GROUPS,
    GB12_REPORT_MAX, ...). A macro defined elsewhere, or an arithmetic expression
    (`G2_NUM_BOXES * 9`), is intentionally out of scope -- see _classify_member's
    external-type fallback, which the same 4-byte documented approximation covers."""
    out = {}
    for m in _DEFINE_INT_RE.finditer(text):
        try:
            out[m.group(1)] = int(m.group(2), 0)
        except ValueError:
            continue
    return out


def _split_top_level_commas(s):
    """Split `s` on commas that are not nested inside (), [], or {} -- so a
    multi-declarator member (`uint32_t a, b, c;`) splits into per-name pieces
    without also splitting an (unused-in-practice-here, but not assumed absent)
    array size expression that itself contains a comma."""
    depth = 0
    parts = []
    cur = []
    for ch in s:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append(''.join(cur))
    return parts


_ARRAY_SUFFIX_RE = re.compile(r'^(\w+)\s*\[\s*([^\]]*?)\s*\]$')

# D1 (BACKLOG #106 G4 fix pass): a struct-member declarator, with or without an array
# suffix -- anything a member name can legally be. _classify_member() raises ValueError
# for a declarator that doesn't match this instead of silently mis-splitting it (the
# live bug: `uint8_t g2names[G2_NUM_BOXES * 9];` rsplit(None, 1) on the LAST space in
# the whole normalized statement landed INSIDE the array-size expression and produced a
# member literally named "9]" -- see the array-suffix carve-out in _classify_member).
_DECLARATOR_NAME_RE = re.compile(r'^[A-Za-z_][A-Za-z_0-9]*(\[[^\]]*\])?$')

# D1: the leading "type ... name[expr]" split, done BEFORE any whitespace-based
# type/name split runs -- so an array-size expression containing internal spaces
# (`[G2_NUM_BOXES * 9]`) never lands inside the split. Non-greedy on the prefix so a
# trailing array suffix (if any) is isolated first; declarators with no array suffix
# don't match and fall through to the plain rsplit(None, 1) unaffected.
_FIRST_DECL_ARRAY_RE = re.compile(r'^(.*?)\s*\[\s*([^\]]*)\s*\]\s*$')


def _resolve_int_literal_or_macro(expr, macros):
    """int for `expr` if it is a plain literal or a macro this header defines,
    else None (an arithmetic expression, or a macro from elsewhere -- both
    intentionally out of scope, see _parse_int_macros())."""
    expr = expr.strip()
    if not expr:
        return None
    try:
        return int(expr, 0)
    except ValueError:
        return macros.get(expr)


def _sized_local_type(text, type_name, macros, size_cache):
    """(size, align, exact) for `type_name` if it is ALSO a struct/enum this SAME
    header defines (recurses into _layout_members() for a real, computed size --
    not a guess), memoized in `size_cache` for the duration of one top-level
    struct_field_offsets() call (a member type can recur, e.g. an array of
    records). `exact` is False when the size is only a best-effort guess (an
    enum whose enumerator values this walker couldn't resolve, or a nested
    struct that itself bottomed out on an inexact member) -- see
    _classify_member()'s break_here handling of the exact flag. Returns None if
    `type_name` is not locally defined at all (an external type from another
    header/TU) -- the caller's documented 4-byte fallback applies then, marked
    NOT exact."""
    if type_name in size_cache:
        return size_cache[type_name]
    found = _struct_or_enum_body(text, type_name)
    if found is None:
        return None
    kind, body = found
    if kind == 'enum':
        size_cache[type_name] = _enum_layout(body, macros)
    else:
        _offsets, total, align, all_exact = _layout_members(text, body, macros, size_cache)
        size_cache[type_name] = (total, align, all_exact)
    return size_cache[type_name]


_ENUM_ITEM_RE = re.compile(r'^\s*([A-Za-z_]\w*)\s*(?:=\s*(.+))?$')


def _enum_layout(body, macros):
    """(size, align, exact) for a LOCAL enum's body text, sized the way this
    project's actual toolchain sizes it -- NOT "int-sized unless -fshort-enums"
    (that claim was backwards and cost BACKLOG #106 G4 four silent false-PASSes:
    RomCtx.version, RomGbSprite.id_hash, G2Writer.ready, Gb12Mount.nboxes were all
    declared at the wrong offset because every enum member before them was sized
    4 B). Verified with an offsetof() probe against arm-none-eabi-gcc (this
    project's actual CFLAGS, Makefile:260-296, pass neither -fshort-enums nor
    -fno-short-enums): `-fshort-enums` IS the arm-eabi target's default --
    __ARM_SIZEOF_MINIMAL_ENUM is predefined, and a plain `typedef enum {A,B,C} E;`
    measured sizeof(E)==1. The flag whose ABSENCE would matter here is
    -fno-short-enums, not -fshort-enums. Per AAPCS short-enum packing, GCC picks
    the smallest of {1,2,4} bytes that represents every enumerator value.
    `exact` is False when an enumerator's value can't be resolved from this
    file alone (an expression, or a macro this header doesn't define) -- the
    4-byte fallback then matches the OLD (wrong-reasoning, but at least
    conservative) behaviour, and _layout_members()'s break_here stops trusting
    anything after it."""
    items = _split_top_level_commas(body)
    values = []
    cur = -1
    resolvable = True
    saw_any = False
    for item in items:
        item = item.strip()
        if not item:
            continue
        saw_any = True
        m = _ENUM_ITEM_RE.match(item)
        if not m:
            resolvable = False
            continue
        expr = m.group(2)
        if expr is None:
            cur += 1
        else:
            v = _resolve_int_literal_or_macro(expr.strip(), macros)
            if v is None:
                resolvable = False
                continue
            cur = v
        values.append(cur)
    if not saw_any or not resolvable or not values:
        return (4, 4, False)          # can't resolve every enumerator -- documented fallback
    lo, hi = min(values), max(values)
    if lo >= 0:
        size = 1 if hi <= 0xFF else 2 if hi <= 0xFFFF else 4
    else:
        size = 1 if (lo >= -128 and hi <= 127) else 2 if (lo >= -32768 and hi <= 32767) else 4
    return (size, size, True)


def _classify_member(stmt, macros, text, size_cache):
    """[(name, size, align, exact), ...] for one struct-member declaration -- a
    list because a multi-declarator member (`uint32_t a, b, c;`) names several
    fields at once. `exact` is False for a size this walker had to GUESS at
    (an external type's or an unresolvable array count's 4-byte fallback, or an
    enum whose enumerator values it couldn't resolve) -- see _layout_members()'s
    break_here, which stops trusting any offset after the first inexact member
    (D1, BACKLOG #106 G4's false-PASS fix: the old code sized every guess as if
    it were certain, which is how RomCtx.version/RomGbSprite.id_hash/
    G2Writer.ready/Gb12Mount.nboxes all got declared at the wrong offset and
    still verified clean). Raises ValueError for a shape this walker refuses to
    guess at (a bitfield -- none appear in the six structs this fix registers,
    and silently sizing one wrong is worse than failing loudly), or for a
    declarator that doesn't match _DECLARATOR_NAME_RE (the "9]" trap: an array
    member whose size is an EXPRESSION with internal spaces, e.g.
    `uint8_t g2names[G2_NUM_BOXES * 9];`, used to be split on the last space in
    the whole normalized statement -- landing inside the expression and naming
    the member "9]" -- rather than between the type and the name)."""
    stmt = ' '.join(stmt.split())
    if re.search(r':\s*\d+$', stmt) and '(' not in stmt:
        raise ValueError(f"bitfield member not supported: {stmt!r}")
    m = _STRUCT_FIELD_RE.search(stmt)
    if m:
        return [(m.group(1), 4, 4, True)]             # function pointer field

    parts = _split_top_level_commas(stmt)
    # D1: carve any trailing array suffix off the FIRST declarator BEFORE the
    # type/name whitespace split runs, so an array-size expression with internal
    # spaces never lands inside the split (the "9]" trap).
    first_raw = parts[0]
    first_arr_m = _FIRST_DECL_ARRAY_RE.match(first_raw)
    if first_arr_m:
        first_pre, first_arr_expr = first_arr_m.group(1), first_arr_m.group(2)
    else:
        first_pre, first_arr_expr = first_raw, None
    first = first_pre.rsplit(None, 1)
    if len(first) != 2:
        raise ValueError(f"can't parse struct member: {stmt!r}")
    base_type = first[0].strip()
    first_name = first[1].strip()
    first_declarator = f"{first_name}[{first_arr_expr}]" if first_arr_expr is not None else first_name
    declarators = [first_declarator] + [p.strip() for p in parts[1:]]

    out = []
    for decl in declarators:
        decl = decl.strip()
        is_ptr = False
        while decl.startswith('*'):
            is_ptr = True
            decl = decl[1:].strip()
        if not _DECLARATOR_NAME_RE.match(decl):
            raise ValueError(f"can't parse struct member declarator: {decl!r} in {stmt!r}")
        name = decl
        has_array = False
        arr_count = None
        am = _ARRAY_SUFFIX_RE.match(decl)
        if am:
            has_array = True
            name = am.group(1)
            arr_count = _resolve_int_literal_or_macro(am.group(2), macros)

        if is_ptr or base_type.endswith('*') or base_type in _KNOWN_PTR_TYPEDEFS:
            out.append((name, 4, 4, True))
            continue
        if base_type in _KNOWN_VALUE_TYPES:
            elem_size = elem_align = _KNOWN_VALUE_TYPES[base_type]
            elem_exact = True
        else:
            sized = _sized_local_type(text, base_type, macros, size_cache)
            if sized is None:
                out.append((name, 4, 4, False))       # external type -- documented fallback, NOT exact
                continue
            elem_size, elem_align, elem_exact = sized
        if has_array:
            if arr_count is None:
                out.append((name, 4, 4, False))       # unresolvable count -- documented fallback, NOT exact
            else:
                out.append((name, elem_size * arr_count, elem_align, elem_exact))
        else:
            out.append((name, elem_size, elem_align, elem_exact))
    return out


def _layout_members(text, body, macros, size_cache):
    """(offsets: {name: off}, total_size, max_align, all_exact) for a struct's
    own `body` text (between its braces), with natural ARM EABI alignment (no
    #pragma pack anywhere in this codebase) -- the same layout arm-none-eabi-gcc
    gives the real struct, UP TO the first member this walker can't size for
    certain. D1 (BACKLOG #106 G4's false-PASS fix): once a member comes back
    inexact (an external type, an unresolvable array count, or an unresolvable
    enum -- see _classify_member()), its OWN offset is still recorded (every
    member before it was exact, so the running `off` up to and including this
    one is trustworthy) but nothing AFTER it is -- the walk stops there
    (`all_exact=False`). struct_field_offsets() surfaces this as a normal
    'no such field' FATAL for any declaration below the break, instead of the
    old behaviour of silently keeping every field 'found' at a guessed offset."""
    stmts = [s.strip() for s in body.split(';') if s.strip()]
    offsets = {}
    off = 0
    max_align = 1
    all_exact = True
    stop = False
    for s in stmts:
        if stop:
            break
        for name, size, align, exact in _classify_member(s, macros, text, size_cache):
            off = (off + align - 1) // align * align
            offsets[name] = off
            off += size
            max_align = max(max_align, align)
            if not exact:
                all_exact = False
                stop = True
                break              # break_here -- nothing declared below an
                                    # imprecise member's own (trustworthy) offset
                                    # can be trusted; see the docstring above.
    total = (off + max_align - 1) // max_align * max_align
    return offsets, total, max_align, all_exact


def struct_field_offsets(header_text, struct_name):
    """Field name -> byte offset within `struct_name`, computed by walking the
    header's own struct definition (see _struct_or_enum_body() for how it's
    located) with natural ARM EABI alignment. Raises ValueError if the struct
    can't be found or a field can't be parsed at all, so a header change this
    walker doesn't understand FAILS the build instead of silently keeping a
    stale offset (D1, the header-drift half of the fix; G4 extends it to
    multi-typedef headers, arrays, multi-declarators, and locally-nested
    struct/enum members). A field below the first inexact member (see
    _layout_members()) is simply ABSENT from the returned dict -- the caller
    (verify_field_declarations()) already FATALs on a declared field that
    isn't in this dict, so an imprecise header region fails loudly instead of
    verifying a guessed offset."""
    t = _strip_c_comments(header_text)
    found = _struct_or_enum_body(t, struct_name)
    if found is None or found[0] != 'struct':
        raise ValueError(f"struct {struct_name!r} not found")
    macros = _parse_int_macros(t)
    offsets, _total, _align, _all_exact = _layout_members(t, found[1], macros, {})
    return offsets


_HAND_VERIFIED = object()   # sentinel: STRUCT_HEADERS[struct] is None on purpose --
                             # no local header exists to check against (e.g. TTC, a
                             # libtonc struct this devkitPro install ships no .c/.h
                             # body for), the offset is verified by hand against the
                             # disassembly instead -- not the same as "unregistered".


def verify_field_declarations(field_decls, source_dir, struct_headers):
    """Cross-check every declared `Struct.field @OFF` against the offset the header
    actually gives that field today. struct_headers maps struct name -> header
    filename under source_dir, OR to None for a struct this walker cannot verify
    from a header at all (hand-verified against the disassembly instead -- see
    _HAND_VERIFIED). Returns a list of human-readable mismatch strings (empty = all
    declarations are honest); the caller treats ANY entry as fatal."""
    problems = []
    cache = {}
    for (struct, field), (decl_off, _impls) in sorted(field_decls.items()):
        if struct not in cache:
            if struct not in struct_headers:
                problems.append(f"{struct}.{field}: no header registered for struct "
                                 f"{struct!r} (add it to STRUCT_HEADERS)")
                continue
            hdr = struct_headers[struct]
            if hdr is None:
                cache[struct] = _HAND_VERIFIED
            else:
                path = os.path.join(source_dir, hdr)
                try:
                    with open(path) as f:
                        cache[struct] = struct_field_offsets(f.read(), struct)
                except (OSError, ValueError) as e:
                    problems.append(f"{struct}.{field}: {e}")
                    cache[struct] = None
                    continue
        real = cache[struct]
        if real is None or real is _HAND_VERIFIED:
            continue
        if field not in real:
            problems.append(f"{struct}.{field}: no such field in {struct_headers[struct]} "
                             "today -- header changed, stack_edges.txt did not")
        elif real[field] != decl_off:
            problems.append(f"{struct}.{field}: declared @{decl_off}, header says "
                             f"@{real[field]} today -- header changed, stack_edges.txt did not")
    return problems


# struct name -> header file (under --source-dir) this walker knows how to size, OR
# None for a struct this walker cannot verify from a header at all (see
# _HAND_VERIFIED) -- every struct named on the LHS of a `Struct.field @OFF` line in
# stack_edges.txt must be listed here, or verify_field_declarations() fails loudly
# instead of trusting an unverifiable offset.
STRUCT_HEADERS = {
    "BoxSource": "pdna_box.h",
    "RomGbUi": "rom_gbui.h",
    "Scan": "rom_gbui.c",     # file-local struct; struct_field_offsets() greps .c too
    "AppSrcOps": "pdna_app.h",
    "RomGbIcon": "rom_gbicon.h",
    "RomGbLearn": "rom_gblearn.h",
    "Br": "gb_sprite_codec.c",
    # BACKLOG #106 G4: these six used to be HAND_VERIFIED (None) escapes -- the OLD
    # lazy forward regex anchored on the FIRST `typedef struct {` in the file and
    # matched to the FIRST `} NAME;` after it, so a header with SEVERAL anonymous
    # typedef structs before the target one (all five headers below have this shape)
    # silently parsed the WRONG struct's body. struct_field_offsets() now locates a
    # struct by brace-depth matching on its OWN ending (`} NAME;`, unique to it) or
    # tagged opening, immune to how many unrelated structs precede it -- all six
    # parse cleanly now and get the same real header verification as the seven
    # above (see tests/host_stack_budget_test.py's fixture + mutation for the fix).
    "RomGbSprite": "rom_gbsprite.h",
    "Gb12Mount": "pdna_gen12.h",
    "G2Writer": "gen2_write.h",
    "RomCtx": "rom_map.h",
    "ArtIconsGen": "art_icons_extract.h",
    "TTC": None,     # libtonc's tte_write dispatch table -- no .c/.h source shipped
                      # in this devkitPro install to grep (see the `recursion
                      # tte_write depth=2` declaration's own comment). Still
                      # genuinely HAND_VERIFIED: no header, not a parser limitation.
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


def _field_origin_step(fn_insn_seq, i, reg):
    """The effect of ONE instruction, fn_insn_seq[i], on `reg`'s traced origin --
    the single-instruction unit both _chase_field_origin() (a whole straight-line
    run at once) and resolve_indirect_site_all_predecessors() (G5, BACKLOG #106:
    one instruction at a time, so a join found PARTWAY through a run is never
    silently skipped) are built from. Returns one of:
      ('resolved', ('field', offset))   -- `ldr rN, [rY, #offset]` with rY not
                                            pc/lr/sp and not itself a fresh
                                            literal load (see _base_is_section_
                                            anchor) -- a genuine struct-field load
      ('resolved', ('nonfield', None))  -- CLOBBERED by a `bl`/`blx` while
                                            caller-saved, a literal/stack-spilled
                                            load, an `ldm` redefinition (trap #7),
                                            a section-anchor/global access, or
                                            set by anything else not ldr-field/mov
      ('rename', new_reg)               -- `mov rX, rY` copies `reg` (was rY,
                                            now the caller should keep chasing rX)
      ('pass', None)                    -- doesn't touch `reg` at all; keep
                                            walking backward past this instruction
    Erring toward 'nonfield' is the safe direction: an unmatched nonfield site
    with no argsites declaration is a blind spot and FAILS the build, so a
    misclassified field load costs a loud failure, never a silent pass."""
    _addr, ins = fn_insn_seq[i]
    ins_clean = ins.split('@')[0].strip()
    if CALL_MNEM_RE.match(ins_clean):
        if reg in CALLER_SAVED_REGS:
            return ('resolved', ('nonfield', None))    # clobbered by the call
        return ('pass', None)                          # callee-saved reg survives a call
    if BRANCH_MNEM_RE.match(ins_clean):
        return ('pass', None)
    m = LDR_FIELD_RE.match(ins_clean)
    if m:
        if m.group(1) != reg:
            return ('pass', None)
        if m.group(2) in ('pc', 'lr', 'sp'):
            return ('resolved', ('nonfield', None))    # literal/stack-spilled param, not a field
        if _base_is_section_anchor(fn_insn_seq, i, m.group(2)):
            return ('resolved', ('nonfield', None))    # global/section-anchor access, not a field
        return ('resolved', ('field', int(m.group(3))))
    if _ldm_defines(ins_clean, reg):                    # trap #7: ldm redefines it, not a spilled field
        return ('resolved', ('nonfield', None))
    if LDM_RE.match(ins_clean):
        return ('pass', None)                           # ldm, but doesn't touch `reg`
    if STORE_MNEM_RE.match(ins_clean) or CMP_MNEM_RE.match(ins_clean):
        return ('pass', None)
    mv = MOV_REG_RE.match(ins_clean)
    if mv and mv.group(1) == reg:
        return ('rename', mv.group(2))                  # chase the copy, keep scanning
    dm = DEST_REG_RE.match(ins_clean)
    if dm and dm.group(1) == reg:
        return ('resolved', ('nonfield', None))         # set by something not ldr-offset/mov
    return ('pass', None)


def _chase_field_origin(fn_insn_seq, start_i, reg, stop_i=-1):
    """Walk `fn_insn_seq` backward from index `start_i` (inclusive) down to (but
    NOT including) index `stop_i`, applying _field_origin_step() at each
    position, for the origin of `reg`. Returns ('field', offset) or ('nonfield',
    None) the moment some instruction resolves it; or -- only reachable when the
    caller passes a non-default `stop_i` -- ('boundary', reg) if the walk reaches
    `stop_i` with `reg` (possibly renamed by a `mov` along the way) still
    unresolved. `resolve_indirect_site()` (single linear predecessor, no join-
    awareness) and `resolve_indirect_site_all_predecessors()` (G5, BACKLOG #106:
    checks for a real join at EVERY instruction, not just once per straight-line
    run) are built from the same per-instruction step; only how far each is
    willing to walk in one uninterrupted run -- and what it does when it can't
    resolve -- differs."""
    for i in range(start_i, stop_i, -1):
        kind, val = _field_origin_step(fn_insn_seq, i, reg)
        if kind == 'resolved':
            return val
        if kind == 'rename':
            reg = val
    if stop_i >= 0:
        return ('boundary', reg)                        # hit the join point, still unresolved
    return ('nonfield', None)                           # never reassigned: a parameter


def resolve_indirect_site(fn_insn_seq, site_addr, reg):
    """Scan `fn`'s instructions backward from just before `site_addr`, LINEARLY
    through the disassembly's own address order (transparently passing through
    any branch instruction it meets), for the origin of the value dispatched
    through `reg`. See _chase_field_origin() for the full field/nonfield
    discipline this shares with resolve_indirect_site_all_predecessors(). Correct
    whenever the code immediately above the site really is its only way in;
    resolve_indirect_site_all_predecessors() is the G5 fix for when it isn't."""
    idx = bisect.bisect_left(fn_insn_seq, (site_addr, ''))
    kind, val = _chase_field_origin(fn_insn_seq, idx - 1, reg)
    return (kind, val)


_UNCONDITIONAL_EXIT_RE = re.compile(
    r'^(b|b\.n|b\.w)\s|^bx\b|^pop\s+\{[^}]*pc[^}]*\}'
    # D4 (BACKLOG #106): the two ARM-mode (not just Thumb `pop {..,pc}`) return
    # shapes this regex used to miss entirely -- `ldm...{...,pc}` (a register-list
    # restore ending in pc, ARM's general-purpose "epilogue" form, of which
    # `pop {..,pc}` is only the Thumb ldmfd-sp! special case) and the plain
    # `mov pc, lr` leaf-function return. Either one really does unconditionally
    # exit the function, so the instruction just above it does NOT fall through
    # into whatever comes next -- treating it as if it did (the pre-fix
    # behaviour) would resume _real_predecessors()'s backward walk past a real
    # function boundary.
    r'|^ldm\w*\s+\w+!?\s*,\s*\{[^}]*\bpc\b[^}]*\}'
    r'|^mov\s+pc\s*,\s*lr\b')


def _intra_function_branch_targets(fn_insn_seq):
    """{target_addr: [source_index, ...]} for every `b`/`bXX` (never `bl`/`blx`,
    which return to their caller rather than jump, and never `bx`, whose operand
    is a register objdump prints as a name, not a resolvable hex target -- TGT_RE
    simply never matches either shape) branch found anywhere in `fn_insn_seq`
    whose target lands on one of this SAME function's own instructions -- the
    raw material G5's predecessor search needs."""
    addr_index = {addr: i for i, (addr, _ins) in enumerate(fn_insn_seq)}
    targets = collections.defaultdict(list)
    for i, (_addr, ins) in enumerate(fn_insn_seq):
        ins_clean = ins.split('@')[0].strip()
        if not BRANCH_MNEM_RE.match(ins_clean):
            continue
        m = TGT_RE.match(ins_clean)
        if not m:
            continue
        t = int(m.group(3), 16)
        if t in addr_index:
            targets[t].append(i)
    return dict(targets)


def _real_predecessors(fn_insn_seq, i, branch_targets):
    """Every REAL inbound edge reaching fn_insn_seq[i]'s own address: each
    `b`/`bXX` elsewhere targeting it (the branch instruction's own index --
    taking the branch doesn't itself touch any register, so the register's
    value flowing in along that edge is exactly whatever it was just before the
    branch instruction ran), PLUS the fall-through edge from i-1 when i-1 exists
    and doesn't itself unconditionally exit (a `b`/`bx`/`pop {..,pc}` there
    means nothing actually falls through into i). Returns a plain list of
    indices to resume scanning FROM (not i-1 pre-subtracted for the branch
    case -- see resolve_indirect_site_all_predecessors() for why each is
    already "the last instruction that ran on this edge")."""
    addr = fn_insn_seq[i][0]
    preds = list(branch_targets.get(addr, []))
    if i > 0:
        above_clean = fn_insn_seq[i - 1][1].split('@')[0].strip()
        if _UNCONDITIONAL_EXIT_RE.match(above_clean) is None:
            preds.append(i - 1)
    return preds


def resolve_indirect_site_all_predecessors(fn_insn_seq, site_addr, reg):
    """G5 (BACKLOG #106): resolve_indirect_site() walks backward LINEARLY through
    the disassembly's own address order, transparently passing through any branch
    instruction it meets -- correct when the block above the site really is its
    ONLY predecessor, silently wrong (only ever caught by a human before this fix)
    when the compiler tail-merged SEVERAL differently-sourced blocks into one
    shared dispatch, each feeding a DIFFERENT struct-field offset into the same
    register before falling into it (confirmed live: app_mon_menu_readonly's -O2
    four-way merge at offset 8/4/16/32, previously correct only for three of the
    four offsets because a human listed them by hand after reading the
    disassembly -- the fourth, offset 4, had gone unnoticed).

    Walks backward ONE INSTRUCTION AT A TIME (via _field_origin_step()), and
    -- critically, unlike a plain linear scan or a "find the nearest join, walk
    the shared span, THEN fan out once" version (an earlier, insufficient draft
    of this same fix) -- checks whether EACH instruction it is about to move
    into is a REAL join (more than one inbound edge, per _real_predecessors())
    BEFORE moving there. The instant it finds one, it stops the straight-line
    walk and pushes every inbound edge onto a worklist as its own independent
    continuation, each carrying whatever register name the walk had traced
    `reg` to by that point (a `mov` along the way may have renamed it).

    Checking at EVERY instruction, not just once at the site's own nearest
    join, matters because a predecessor block can ITSELF be reached only via
    some OTHER, unrelated forward branch (an out-of-line/cold tail -- a clamp,
    an error path) whose own physical predecessor in the listing is dead code,
    typically the function's own epilogue laid out right after the hot path
    that cold tail jumps back into. Confirmed live: rom_gbui.c's all_blank()'s
    out-of-line `if (chunk > 64) chunk = 64;` clamp is reached only by a forward
    `bhi.n`; an earlier draft of this fix that only re-checked joins once per
    predecessor FRONTIER (not once per instruction within a frontier's own scan)
    still walked straight through that clamp block's single instruction into
    the function's `pop {r4-r7}` epilogue beyond it, which redefines r7 and
    manufactured a FALSE 'nonfield' verdict no real predecessor ever produces.
    Checking before every single step removes that whole class of mistake.

    Returns ('field', frozenset_of_offsets): a one-member frozenset in the
    ordinary case (a single predecessor, or several that all resolve the exact
    same offset), more than one member when different predecessors genuinely
    feed different offsets -- the caller MUST require a declaration to cover
    EVERY member, never just one. Returns ('nonfield', None) if there is no real
    predecessor to trace (function entry) or if ANY predecessor traces to
    something other than a field load -- same conservative discipline as
    resolve_indirect_site() itself: one unresolved predecessor makes the WHOLE
    site unresolved, never a silent partial pass."""
    idx = bisect.bisect_left(fn_insn_seq, (site_addr, ''))
    if idx == 0:
        return ('nonfield', None)
    branch_targets = _intra_function_branch_targets(fn_insn_seq)

    # Each worklist entry is (i, cur_reg): "resume scanning AT index i" (i's own
    # instruction has not been examined yet). The visited-set guards against a
    # genuine loop (a back-edge reaching its own frontier again) never
    # terminating.
    #
    # The site's OWN address can itself be a branch target (a case/switch's
    # OTHER arms `bne`-ing directly to the shared `bl`/`blx` instruction, no
    # intervening shared tail at all) -- seeding the worklist with a bare
    # `idx - 1` would silently miss that predecessor edge exactly the way the
    # site's own call instruction is never examined for `reg`'s definition
    # (see below): the first real join check has to happen for `idx` itself,
    # through the SAME _real_predecessors() every later frontier uses, not a
    # hard-coded "the physically preceding instruction is the only way in".
    # `idx == len(fn_insn_seq)` (the site's own address isn't present in the
    # sequence at all -- every real caller's disassembly always includes its
    # own call instruction, but a hand-built fixture may not) has nothing to
    # introspect there, so it falls back to the plain `idx - 1` start.
    if idx < len(fn_insn_seq):
        site_preds = _real_predecessors(fn_insn_seq, idx, branch_targets)
    else:
        site_preds = [idx - 1]
    if len(site_preds) == 1 and site_preds[0] == idx - 1:
        worklist = [(idx - 1, reg)]
    elif not site_preds:
        return ('nonfield', None)
    else:
        worklist = [(p, reg) for p in site_preds]
    visited = set()
    offsets = set()
    saw_any_predecessor = False

    while worklist:
        i, cur_reg = worklist.pop()
        if (i, cur_reg) in visited:
            continue
        visited.add((i, cur_reg))

        while True:
            kind, val = _field_origin_step(fn_insn_seq, i, cur_reg)
            if kind == 'resolved':
                field_kind, field_val = val
                if field_kind == 'field':
                    offsets.add(field_val)
                    saw_any_predecessor = True
                else:
                    return ('nonfield', None)      # clobbered/parameter on this path --
                                                    # whole site unresolved (one bad path
                                                    # is enough)
                break
            if kind == 'rename':
                cur_reg = val

            if i == 0:
                return ('nonfield', None)          # ran off the function's own entry
                                                    # with `reg` still unresolved
            preds = _real_predecessors(fn_insn_seq, i, branch_targets)
            if len(preds) == 1 and preds[0] == i - 1:
                i -= 1                             # the ordinary, single-predecessor
                continue                           # case: keep walking in this same frontier
            if not preds:
                return ('nonfield', None)          # an unreachable/veneer-only join this
                                                    # scan can't vouch for
            for p in preds:
                worklist.append((p, cur_reg))
            break

    if not saw_any_predecessor:
        return ('nonfield', None)
    return ('field', frozenset(offsets))


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


def resolve_all_sites(analysis, field_offset_index, argsite_decls, whole_func_decls):
    """Classify every indirect-call site the walker found and decide which ones are
    exempted by a declaration (D1). `field_offset_index` is the (qualified,
    unqualified) pair load_extra_edges() builds (D5a): a 'field' site first checks
    (offset, caller) in `qualified`, then falls back to `offset` in `unqualified`
    (legal only when the offset has exactly one struct.field owner, enforced at
    parse time) -- two structs sharing a numeric offset no longer merge their
    implementation sets for a caller that only genuinely dispatches through one of
    them. Returns:
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
    qualified_offset_impls, unqualified_offset_impls = field_offset_index

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
            # G5 (BACKLOG #106): a `bl`/`blx` whose basic block has more than one
            # inbound predecessor (an -O2 tail merge of several differently-sourced
            # blocks, each feeding a DIFFERENT struct-field offset into the same
            # register before falling into the shared dispatch) must have EVERY
            # resulting offset declared, not just whichever one a linear backward
            # scan happens to land on by code-layout luck.
            kind, offs = resolve_indirect_site_all_predecessors(
                fn_insn_seq.get(fn, []), int(addr, 16), reg)
            if kind == 'field':
                declared_offsets, undeclared_offsets, site_impls = set(), set(), set()
                for off in sorted(offs):
                    impls = qualified_offset_impls.get((off, fn))
                    if impls is None:
                        impls = unqualified_offset_impls.get(off)
                    if impls:
                        declared_offsets.add(off)
                        site_impls |= impls
                    else:
                        undeclared_offsets.add(off)
                if undeclared_offsets:
                    if len(offs) > 1:
                        blind[fn].append((addr, ins,
                            f"multi-predecessor struct-field load: offsets "
                            f"{sorted(offs)} via {len(offs)} predecessors; declared "
                            f"{sorted(declared_offsets)} -- missing a field "
                            f"declaration covering {sorted(undeclared_offsets)} for "
                            f"caller {fn!r}"))
                    else:
                        off = next(iter(offs))
                        blind[fn].append((addr, ins, f"struct-field load @{off}, no "
                                           f"declared field at that offset for caller {fn!r} "
                                           "(and no unqualified owner of that offset)"))
                else:
                    edges_to_add[fn] |= site_impls
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


def dump_sites(analysis, field_offset_index, argsite_decls, whole_func_decls):
    """D5a diagnostic (--dump-sites): print every indirect-call site this walker's
    disassembly walk found, one line each, with how it resolves TODAY -- ground
    truth for converting stack_edges.txt's declarations to the honest per-caller
    form, instead of guessing from source reading alone which callers a bare-offset
    declaration was actually covering."""
    qualified_offset_impls, unqualified_offset_impls = field_offset_index
    fn_insn_seq = analysis["fn_insn_seq"]
    for fn in sorted(analysis["indirect_sites"]):
        sites = analysis["indirect_sites"][fn]
        total = len(sites)
        if fn in whole_func_decls and total <= 1:
            for addr, ins, reg in sites:
                print(f"{fn}  {addr}  whole-function -> "
                      f"{' '.join(sorted(whole_func_decls[fn]))}")
            continue
        nonfield_i = 0
        for addr, ins, reg in sites:
            kind, offs = resolve_indirect_site_all_predecessors(
                fn_insn_seq.get(fn, []), int(addr, 16), reg)
            if kind == 'field':
                for off in sorted(offs):
                    impls = qualified_offset_impls.get((off, fn))
                    src = "qualified"
                    if impls is None:
                        impls = unqualified_offset_impls.get(off)
                        src = "unqualified"
                    via = f" via {len(offs)} predecessors" if len(offs) > 1 else ""
                    if impls:
                        print(f"{fn}  {addr}  field @{off} ({src}){via} -> "
                              f"{' '.join(sorted(impls))}")
                    else:
                        print(f"{fn}  {addr}  field @{off}{via} -> BLIND")
            else:
                nonfield_i += 1
                if fn in argsite_decls:
                    n, impls = argsite_decls[fn]
                    print(f"{fn}  {addr}  argsite {nonfield_i}/{total} "
                          f"(declared N={n}) -> {' '.join(sorted(impls))}")
                else:
                    print(f"{fn}  {addr}  argsite {nonfield_i}/{total} -> BLIND "
                          "(no argsites declaration)")


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


def deepest_from(root, edges, su_sizes, estimated, blacklist=(), overrides=None,
                  scc_of=None):
    """Heaviest root..leaf chain by DFS with memoization; returns (total, path, cycles).
    path is a list of (name, frame_bytes, source).

    D4 (BACKLOG #84b sixth pass): `scc_of`, when given, is {node -> (charge_bytes,
    members_frozenset)} for every node inside a REAL, DECLARED recursive SCC (built by
    main() from tarjan_sccs() + stack_edges.txt's `recursion fn depth=N` lines --
    charge_bytes = N * sum of every frame in the component). A node with a `scc_of`
    entry is treated as an ATOMIC unit: go() charges the whole component's declared
    bytes exactly once, walks ONLY the edges leaving the component (an edge to a
    fellow member is absorbed into the charge, not walked -- walking it would just
    re-enter the same component), and every member memoizes to the SAME total/exit
    child, since which member happens to be entered first cannot change the
    component's worst case. This replaces the old truncate-to-0-and-memoize behaviour,
    which made the printed total secretly depend on DFS visitation order (the very
    defect D4 exists to fix) -- with `scc_of=None` (every existing caller that never
    passes it, and every test fixture without a real cycle) this function's behaviour
    is untouched, onstack/cycles included, for backward compatibility."""
    memo = {}
    best_child = {}
    onstack = set()
    cycles = []
    scc_of = scc_of or {}

    def go(fn):
        if fn in memo:
            return memo[fn]
        comp = scc_of.get(fn)
        if comp is not None:
            charge, members = comp
            bc, bn = 0, None
            for m in members:
                for c in sorted(edges.get(m, ())):
                    if c in members or c in blacklist:
                        continue          # internal edge: absorbed into `charge` already
                    v = go(c)
                    if v >= bc:
                        bc, bn = v, c
            total_here = charge + bc
            for m in members:
                memo[m] = total_here
                best_child[m] = bn
            return total_here
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
    printed_scc = set()
    while cur is not None:
        comp = scc_of.get(cur)
        if comp is not None:
            charge, members = comp
            key = frozenset(members)
            if key not in printed_scc:
                printed_scc.add(key)
                name = "{" + ",".join(sorted(members)) + "}"
                path.append((name, charge, "recursion"))
            cur = best_child.get(cur)
            continue
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


def check_ambiguous_declarations(dup_names, addr_unique_name, field_decls, argsite_decls,
                                  whole_func_decls, frame_overrides, isr_decls,
                                  addrtaken_ok, recursion_decls=()):
    """D5b: any stack_edges.txt reference to a NAME the census found duplicated
    (`dup_names`, from read_symbol_census()) without its `@tu` qualifier is ambiguous
    -- it could silently resolve to whichever instance's node happens to exist under
    that bare string (today: none, since analyze() never emits an unqualified name for
    a duplicated symbol any more, so an unqualified reference here is dead on arrival
    and would misleadingly warn "not found in the ELF" instead of naming the real
    problem). Returns a list of human-readable error strings; empty = clean."""
    candidates_by_name = collections.defaultdict(set)
    for uniq in addr_unique_name.values():
        if "@" in uniq:
            base = uniq.rsplit("@", 1)[0]
            candidates_by_name[base].add(uniq)

    problems = []

    def check(name, where):
        if name in dup_names:
            cands = ", ".join(sorted(candidates_by_name.get(name, set()))) or "(none linked)"
            problems.append(f"{where}: {name!r} is a duplicated static name -- qualify "
                             f"it as one of: {cands}")

    for (struct, field), (off, impls) in sorted(field_decls.items()):
        for impl in sorted(impls):
            check(impl, f"{struct}.{field} @{off} ->")
    for caller, (n, impls) in sorted(argsite_decls.items()):
        check(caller, "argsites caller")
        for impl in sorted(impls):
            check(impl, f"{caller} argsites={n} ->")
    for caller, impls in sorted(whole_func_decls.items()):
        check(caller, "whole-function caller")
        for impl in sorted(impls):
            check(impl, f"{caller} ->")
    for fn in sorted(frame_overrides):
        check(fn, "frame override")
    for fn in sorted(isr_decls):
        check(fn, "isr")
    for fn in sorted(addrtaken_ok):
        check(fn, "addrtaken-ok")
    for fn in sorted(recursion_decls):
        check(fn, "recursion")
    return problems


def tarjan_sccs(root, edges):
    """D5b (`--sccs`): strongly-connected components (Tarjan) over the subgraph
    reachable from `root`. A component is reported only when it names REAL recursion
    -- more than one function, or a single function with a genuine self-edge (after
    trap #1 already strips same-function long jumps, a surviving self-edge is a
    function that calls itself). Before the duplicate-name fix (D5b, this same
    commit), a merged same-named node could manufacture a component that was really
    two unrelated chains; after it, this list should contain only real recursion --
    the diagnostic this flag exists to prove.

    F5 (BACKLOG #84b seventh pass): ITERATIVE, not recursive -- the previous version
    used Python's own call stack (one strongconnect() frame per node on the deepest
    DFS path) and raised sys.setrecursionlimit() to compensate, which only pushes the
    ceiling higher rather than removing it; a sufficiently long real call chain (this
    codebase's own reachable graph from main() already has chains 30+ functions deep,
    and a future one could easily exceed whatever limit was chosen) would still blow
    the interpreter's C stack, not just Python's tracked recursion counter, crashing
    the guard instead of reporting a real cycle. This version keeps its own explicit
    `work_stack` of (node, remaining-children-iterator) frames -- one dict lookup and
    one iterator per stack frame instead of one C stack frame -- so it scales with the
    heap, not a hard-coded/derived limit. Produces IDENTICAL output to the recursive
    version for the same graph (same child visit order via `sorted(edges.get(v, ()))`,
    same lowlink propagation, same component/self-edge rule) -- verified by the D5a/F5
    fixture (a fabricated 3,000-node straight chain, host_stack_budget_test.py)."""
    reachable = reachable_from(root, edges)
    index_counter = [0]
    index = {}
    lowlink = {}
    on_stack = {}
    stack = []
    result = []

    for start in sorted(reachable):
        if start in index:
            continue
        index[start] = index_counter[0]
        lowlink[start] = index_counter[0]
        index_counter[0] += 1
        stack.append(start)
        on_stack[start] = True
        # work_stack[i] = (node, iterator over that node's still-unprocessed children)
        work_stack = [(start, iter(sorted(edges.get(start, ()))))]

        while work_stack:
            v, child_iter = work_stack[-1]
            descended = False
            for w in child_iter:
                if w not in reachable:
                    continue
                if w not in index:
                    index[w] = index_counter[0]
                    lowlink[w] = index_counter[0]
                    index_counter[0] += 1
                    stack.append(w)
                    on_stack[w] = True
                    work_stack.append((w, iter(sorted(edges.get(w, ())))))
                    descended = True
                    break
                elif on_stack.get(w):
                    lowlink[v] = min(lowlink[v], index[w])
            if descended:
                continue      # new frame pushed; resume from its own children next
            # this node's children are exhausted -- pop it and fold its lowlink
            # into its parent (the recursive version's post-recursion-call line)
            work_stack.pop()
            if work_stack:
                parent = work_stack[-1][0]
                lowlink[parent] = min(lowlink[parent], lowlink[v])
            if lowlink[v] == index[v]:
                comp = []
                while True:
                    w = stack.pop()
                    on_stack[w] = False
                    comp.append(w)
                    if w == v:
                        break
                if len(comp) > 1 or v in edges.get(v, ()):
                    result.append(comp)
    return result


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


def top_n_chains(root, edges, su_sizes, estimated, n=5, overrides=None, scc_of=None):
    """Top-N distinct chains from root, ranked by root's direct callees' subtree
    weight (each callee's own heaviest chain, prefixed with root's frame)."""
    root_frame, root_src = frame_of(root, su_sizes, estimated, overrides)
    children = sorted(edges.get(root, ()),
                       key=lambda c: deepest_from(c, edges, su_sizes, estimated,
                                                   overrides=overrides, scc_of=scc_of)[0],
                       reverse=True)
    chains = []
    for c in children[:n]:
        tot, path, cycles = deepest_from(c, edges, su_sizes, estimated,
                                          overrides=overrides, scc_of=scc_of)
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


# === D1 (BACKLOG #84b fifth pass): the address-taken sweep ===========================
#
# Every declaration class above (Struct.field/argsites/whole-function) exempts a call
# SITE the disassembly walk found -- but none of them ever asks the opposite question:
# is there a function in this linked image whose address is stored somewhere (a struct
# instance, a dispatch table, a literal pool) that NO declared class names as a possible
# implementation, and that the call graph never reaches any other way? A brand-new
# implementation slotted into an ALREADY-declared class (the reviewer's own plant: a
# `static uint8_t* rv_deep_records(int box)` with a giant stack frame, installed as
# `s.records = (g_pc_last_box == 12345) ? rv_deep_records : pcsrc_records`) is exactly
# that: its address is taken (assigned into BoxSource.records) but nothing in
# stack_edges.txt names it, and the disassembly walk that builds `edges` only ever
# reasons about DECLARED implementations, never about "what else could this dispatch
# reach." This sweep closes that hole by finding every function address stored ANYWHERE
# in the linked image, independent of the declaration grammar, and refusing to build
# unless each one is accounted for by a declaration, by being reachable through the
# ordinary call graph, or by a declared ISR handler / `addrtaken-ok` escape line.

_ALLOC_LOAD_HDR_RE = re.compile(r'^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s')


def alloc_load_sections(elf):
    """Section names with real on-disk CONTENTS that are ALLOC+LOAD -- i.e. every
    section whose bytes actually land in the shipped image (.text/.rodata/.data/
    .iwram/.init_array/...). .bss/.sbss are ALLOC but NOT LOAD (zero-initialized,
    no CONTENTS line) -- they cannot hold a literal function-pointer byte pattern
    on disk, so scanning them would either error out or read stale objdump noise;
    skipped on purpose, not an oversight."""
    out = subprocess.run([OBJDUMP, "-h", elf], capture_output=True, text=True,
                          check=True).stdout
    lines = out.splitlines()
    sections = []
    for i, line in enumerate(lines):
        m = _ALLOC_LOAD_HDR_RE.match(line)
        if not m or int(m.group(2), 16) == 0:
            continue
        flags = lines[i + 1] if i + 1 < len(lines) else ""
        if "CONTENTS" in flags and "ALLOC" in flags and "LOAD" in flags:
            sections.append(m.group(1))
    return sections


_OBJDUMP_S_LINE_RE = re.compile(r'^\s([0-9a-f]+) ((?:[0-9a-f]{2,8} ?){1,4})')


def _words_from_objdump_s_text(text):
    """Pure parsing half of the address-taken sweep (split out from scan_address_taken()
    so a fixture can exercise the byte-order/masking logic without a real objdump or
    ELF -- see the fixture in host_stack_budget_test.py). Yields every 4-byte-aligned
    little-endian word found in one `objdump -s` section's text. `objdump -s` prints
    raw bytes in ADDRESS order regardless of target endianness -- interpreting a
    4-byte group as this (little-endian ARM) target's word requires reading the hex
    pairs in the order printed (byte 0 is the low byte), i.e. int.from_bytes(...,
    'little') on the parsed bytes, NOT a naive int(hexstring, 16) (which would read
    them as if BIG-endian and silently check the wrong address for every single word)."""
    for line in text.splitlines():
        m = _OBJDUMP_S_LINE_RE.match(line)
        if not m:
            continue
        for g in m.group(2).split():
            if len(g) != 8:
                continue            # a short trailing group at section end: not a full word
            yield int.from_bytes(bytes.fromhex(g), byteorder="little")


_OBJDUMP_S_SECTION_HDR_RE = re.compile(r'^Contents of section ([^:]+):\s*$')


def dump_alloc_load_sections(elf, sections):
    """F7 (BACKLOG #84b seventh pass): ONE `objdump -s` invocation covering every
    named section (repeated `-j sec` flags -- binutils accepts as many as given),
    instead of one subprocess per section. scan_address_taken()/read_build_dir_stamp()
    used to each shell out to `objdump -s -j <sec>` once PER SECTION (this build's own
    ELF has 9 ALLOC+LOAD sections), and main() calls scan_address_taken() once --
    9 extra process spawns just for that one sweep, on top of everything else objdump
    already does. `objdump -s` prints one `Contents of section NAME:` header per
    section in the combined dump; splitting on that header re-derives exactly the
    same per-section text each caller used to get from its own separate invocation,
    at 1/9th the process-spawn cost. Returns {section_name: text}."""
    args = [OBJDUMP, "-s"]
    for sec in sections:
        args += ["-j", sec]
    args.append(elf)
    try:
        out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    except subprocess.CalledProcessError:
        return {}
    result = {}
    cur = None
    lines = []
    for line in out.splitlines():
        m = _OBJDUMP_S_SECTION_HDR_RE.match(line)
        if m:
            if cur is not None:
                result[cur] = "\n".join(lines)
            cur = m.group(1)
            lines = []
            continue
        if cur is not None:
            lines.append(line)
    if cur is not None:
        result[cur] = "\n".join(lines)
    return result


# === G2 (BACKLOG #106): the address-taken sweep, restricted to PROVEN pointer holders ==
#
# The original scan_address_taken() (kept in git history) treated every 4-byte-aligned
# word in every ALLOC+LOAD section -- including .text, where it double-counts as two
# adjacent 16-bit Thumb opcodes -- as a candidate function-pointer store, purely
# because its VALUE happened to equal some function's entry address. A data table
# entry, a hash constant, or a ROM-span constant is exactly as likely to collide with
# a small, densely-populated 0x08xxxxxx address space as a genuine pointer is -- seven
# `addrtaken-ok` lines were added across 2026-09-10 alone to paper over exactly that
# (tte_cmd_skip, __aeabi_d2iz, encode_checked, em_get_ribbon_flag, the _EZFO pair, ...),
# each one an unverified escape hatch a reviewer has to trust on faith.
#
# The fix asks a different, checkable question per section class:
#   - DATA sections (.rodata/.data/.iwram/...): does the COMPILER'S OWN OBJECT FILE
#     say, via a relocation record, "this word is that function's address"? A
#     relocation is the compiler's own proof, not a coincidence -- scan_relocated_
#     addresses() below reads it straight from `objdump -r`/`-t` on the *.o files
#     (pre-link; ARM ELF relocations are REL, not RELA, so a LOCAL/static target's
#     addend has to be read back out of the referencing bytes themselves, exactly
#     like read_build_dir_stamp() already does for one known symbol).
#   - .text literal pools: objdump's OWN disassembler already tells us, unambiguously,
#     which trailing bytes of a function are a `.word` literal rather than a decoded
#     instruction (the veneer/trap-#5 resolvers above already trust this same
#     distinction) -- scan_text_literal_pool() below reuses it, so two adjacent
#     Thumb opcodes that happen to spell a function's address are never mistaken for
#     a pointer store the way a raw byte-value scan of the whole section would.

_OBJDUMP_RELOC_HDR_RE = re.compile(r'^RELOCATION RECORDS FOR \[([^\]]+)\]:$')
_OBJDUMP_RELOC_LINE_RE = re.compile(r'^([0-9a-f]+)\s+(\S+)\s+(\S+)')
_WORD_RELOC_TYPES = {"R_ARM_ABS32", "R_ARM_TARGET1"}
# Relocation-bearing sections this sweep has no business reading as "data that might
# hold a function pointer" -- debug info/unwind tables/string tables carry symbol
# references for entirely different reasons and would only add noise (or, for
# .debug_*, potentially a huge amount of it on a -g build).
_RELOC_SECTION_SKIP_PREFIXES = (".debug", ".ARM.exidx", ".ARM.extab", ".comment",
                                 ".note", ".symtab", ".strtab", ".shstrtab", ".group")


def dump_object_relocations(obj):
    """`objdump -r <obj>`'s raw text for one pre-link object file. Returns "" (not an
    exception) when the tool fails on a malformed/missing object -- the caller then
    simply finds no relocations there, the same fail-open-to-"no evidence" shape
    dump_alloc_load_sections() already uses for a missing section."""
    try:
        return subprocess.run([OBJDUMP, "-r", obj], capture_output=True, text=True,
                               check=True).stdout
    except subprocess.CalledProcessError:
        return ""


def parse_object_relocations(text):
    """{section_name: [(offset:int, reloc_type:str, value:str), ...]} from one
    `objdump -r` dump's 'RELOCATION RECORDS FOR [section]:' blocks (see
    dump_object_relocations()). `value` is either a real global symbol's name
    (globals keep their name in a relocatable object's relocation record) or the
    bare name of the SECTION the word targets (a local/static symbol collapses to
    its containing section -- see scan_relocated_addresses() for how the actual
    target address is then recovered)."""
    out = {}
    cur = None
    for line in text.splitlines():
        m = _OBJDUMP_RELOC_HDR_RE.match(line)
        if m:
            cur = m.group(1)
            out.setdefault(cur, [])
            continue
        if cur is None:
            continue
        m = _OBJDUMP_RELOC_LINE_RE.match(line)
        if not m or m.group(2) == "TYPE":
            continue
        try:
            off = int(m.group(1), 16)
        except ValueError:
            continue
        out[cur].append((off, m.group(2), m.group(3)))
    return out


def dump_object_symbols(obj):
    """`objdump -t <obj>`'s raw text for one pre-link object file (see
    dump_object_relocations() for the fail-open rationale)."""
    try:
        return subprocess.run([OBJDUMP, "-t", obj], capture_output=True, text=True,
                               check=True).stdout
    except subprocess.CalledProcessError:
        return ""


def parse_object_symbol_table(text):
    """{section_name: [(addr, size, name, kind), ...]} sorted by addr, kind in {'F'
    function, 'O' data object} -- from ONE object file's own `objdump -t` (pre-link,
    section-relative addresses, not final-image addresses). The 'd' rows objdump
    emits for a section's OWN definition (name identical to the section, e.g. a row
    literally named '.rodata') are skipped -- those exist so a relocation can target
    "the section itself" (see parse_object_relocations()), not as something a real
    pointer word could be "inside". Same tolerant flag-column parsing as
    parse_symbol_census() (the flag field's width varies; only the SET of flag
    tokens between the address and the section name is ever tested)."""
    by_section = collections.defaultdict(list)
    for line in text.splitlines():
        if "\t" not in line:
            continue
        left, right = line.split("\t", 1)
        lparts = left.split()
        rparts = right.split(None, 1)
        if len(lparts) < 4 or len(rparts) < 2:
            continue
        try:
            addr = int(lparts[0], 16)
        except ValueError:
            continue
        try:
            size = int(rparts[0], 16)
        except ValueError:
            continue
        section = lparts[-1]
        name = re.sub(r'^\.(hidden|internal|protected)\s+', '', rparts[1].strip())
        if name == section:
            continue                                # the section's own definition row
        flags = lparts[2:-1]
        kind = 'F' if 'F' in flags else ('O' if 'O' in flags else None)
        if kind is None:
            continue
        by_section[section].append((addr, size, name, kind))
    for sec in by_section:
        by_section[sec].sort()
    return dict(by_section)


def _containing_symbol(symbols_in_section, offset):
    """(name, delta) for the last (addr,size,name,kind) entry whose addr <= offset --
    same bisect-on-sorted-starts discipline as analyze()'s owner() -- or (None, 0)
    when the list is empty or offset precedes every entry."""
    if not symbols_in_section:
        return None, 0
    addrs = [s[0] for s in symbols_in_section]
    i = bisect.bisect_right(addrs, offset) - 1
    if i < 0:
        return None, 0
    addr, _size, name, _kind = symbols_in_section[i]
    return name, offset - addr


def scan_relocated_addresses(builddir, name_at):
    """G2: every function in `name_at` whose address is taken via a genuine
    R_ARM_ABS32/R_ARM_TARGET1 relocation in some *.o's DATA section (anything that
    isn't .text and isn't one of the debug/metadata sections _RELOC_SECTION_SKIP_
    PREFIXES excludes). Returns (taken: set, detail: {fn: [location strings]}) --
    detail is the human-readable "<obj>:<section>+<off> inside <symbol>+<delta>"
    trail a FATAL orphan report names, per BACKLOG #106 G2 item 2.

    A LOCAL/static relocation target collapses to its containing SECTION name
    (e.g. value == ".text"): ARM's REL (not RELA) relocations carry no addend field
    of their own, so the actual target offset has to be read back out of the
    referencing word's own bytes (`objdump -s`) -- identical in spirit to
    read_build_dir_stamp()'s single-symbol byte read, generalized to any (obj,
    section, offset). A GLOBAL target keeps its own name in the relocation record
    directly and needs no byte read. Either way, the resolved name only counts as
    "taken" once it is confirmed to be one of THIS OBJECT's own .text FUNCTIONS
    (Thumb bit masked off) that also appears in `name_at` -- a data-to-data
    relocation (an array pointing at another array) never becomes a false hit."""
    func_names = set(name_at.values())
    taken = set()
    detail = collections.defaultdict(list)
    word_cache = {}   # (obj, section) -> {byte_addr: value}, avoid re-dumping per reloc

    def word_at(obj, section, offset):
        key = (obj, section)
        if key not in word_cache:
            data = {}
            out = subprocess.run([OBJDUMP, "-s", "-j", section, obj],
                                  capture_output=True, text=True).stdout
            for line in out.splitlines():
                m = _OBJDUMP_S_LINE_RE.match(line)
                if not m:
                    continue
                base = int(m.group(1), 16)
                raw = bytes.fromhex(m.group(2).replace(" ", ""))
                for i, b in enumerate(raw):
                    data[base + i] = b
            word_cache[key] = data
        data = word_cache[key]
        if any((offset + i) not in data for i in range(4)):
            return None
        return int.from_bytes(bytes(data[offset + i] for i in range(4)), "little")

    for obj in sorted(glob.glob(os.path.join(builddir, "*.o"))):
        relocs = parse_object_relocations(dump_object_relocations(obj))
        if not relocs:
            continue
        symbols = parse_object_symbol_table(dump_object_symbols(obj))
        # EXACT starts only, not "which function's range contains this address" --
        # a switch-statement jump table (GCC's `ldr pc, [pc, rN, lsl #2]`/computed-
        # goto codegen on this ARMv4T target, no Thumb-2 tbb/tbh available) is ALSO
        # a section+addend relocation into .text, with every entry an address
        # *inside* some function's body (a case label) but essentially never
        # exactly at its first instruction -- confirmed live: gen1_write.o's char-
        # encode table put 80+ such entries inside gen1_encode_char's own range,
        # none matching its start, a real false-positive class a containment
        # lookup would have (mis)confirmed as "gen1_encode_char's address is
        # taken". Masking bit 0 before the lookup normalizes both conventions this
        # target's relocations use for a genuine function-pointer's target value
        # (with the Thumb interworking bit, e.g. readelf's raw 0x2e5 for a symbol
        # objdump -t itself always PRINTS stripped, e.g. 0x2e4 -- so the map below
        # is already bit0-stripped) to the SAME key a case-label addend (always
        # even, mid-function, never a function start either way) still correctly
        # misses.
        text_starts = {addr: name for addr, _size, name, _kind in symbols.get(".text", [])}
        for sec, entries in relocs.items():
            if sec == ".text" or sec.startswith(_RELOC_SECTION_SKIP_PREFIXES):
                continue
            for off, typ, value in entries:
                if typ not in _WORD_RELOC_TYPES:
                    continue
                fn_name = None
                if value == ".text":
                    addend = word_at(obj, sec, off)
                    if addend is None:
                        continue
                    fn_name = text_starts.get(addend & ~1)
                elif not value.startswith("."):
                    fn_name = value               # a global symbol names itself
                if fn_name is None or fn_name not in func_names:
                    continue
                taken.add(fn_name)
                sym_name, delta = _containing_symbol(symbols.get(sec, []), off)
                loc = f"{os.path.basename(obj)}:{sec}+{off:#x}"
                if sym_name is not None:
                    loc += f" inside {sym_name}+{delta:#x}"
                detail[fn_name].append(loc)
    return taken, dict(detail)


_TEXT_WORD_RE = re.compile(r'^\.word\s+0x([0-9a-f]+)$')


def scan_text_literal_pool(dump_text, name_at):
    """G2: every function in `name_at` whose address is taken via a `.word` literal-
    pool entry somewhere in .text -- found through objdump's OWN disassembly
    annotation (an INSN_RE-shaped line whose mnemonic half is exactly
    `.word 0xNNNNNNNN`), the same trusted distinction the veneer resolver (:531) and
    the trap-#5 literal-call resolver (:1076) already rely on. Deliberately NOT a
    raw byte scan of .text: two adjacent 16-bit Thumb opcodes read as one 32-bit
    word can coincidentally equal a function's address far more easily than a real
    literal pool entry can (.text is far denser than .rodata), and unlike a literal
    pool a decoded instruction was never a pointer store to begin with."""
    taken = set()
    for raw in dump_text.splitlines():
        m = INSN_RE.match(raw.rstrip("\n"))
        if not m:
            continue
        wm = _TEXT_WORD_RE.match(m.group(2).strip())
        if not wm:
            continue
        fn = name_at.get(int(wm.group(1), 16) & ~1)
        if fn:
            taken.add(fn)
    return taken


def own_function_names(builddir):
    """Every FUNCTION symbol name defined in one of THIS PROJECT'S OWN *.o files
    under `builddir` -- i.e. everything scan_relocated_addresses() can actually
    read a relocation for. crt0/libgcc/newlib/libtonc code is linked in from
    prebuilt *.a archives whose member objects never land in `builddir` at all, so
    a name in `name_at` but NOT in this set is third-party: see scan_address_taken()
    for why that distinction matters."""
    names = set()
    for obj in sorted(glob.glob(os.path.join(builddir, "*.o"))):
        for entries in parse_object_symbol_table(dump_object_symbols(obj)).values():
            for _addr, _size, name, kind in entries:
                if kind == 'F':
                    names.add(name)
    return names


def scan_third_party_words_raw(sections, section_dumps, name_at, restrict_to):
    """The OLD (pre-G2) whole-section raw-word scan, DELIBERATELY restricted to
    `restrict_to` (a name set) rather than every function in the image. See
    scan_address_taken()'s docstring for why this fallback exists and why
    restricting it closes the coincidence risk it would otherwise reopen."""
    restrict_addrs = {addr: name for addr, name in name_at.items() if name in restrict_to}
    if not restrict_addrs:
        return set()
    taken = set()
    for sec in sections:
        out = section_dumps.get(sec)
        if out is None:
            continue
        for w in _words_from_objdump_s_text(out):
            fn = restrict_addrs.get(w & ~1)
            if fn:
                taken.add(fn)
    return taken


def scan_address_taken(builddir, dump_text, name_at, sections, section_dumps):
    """The whole G2 address-taken sweep: scan_relocated_addresses() (DATA sections,
    relocation-proven, covers everything THIS PROJECT compiles) union
    scan_text_literal_pool() (.text literal pools, disassembly-proven) union a
    THIRD, NARROW fallback for the one real class those two provably cannot see:
    crt0/libgcc/newlib/libtonc functions, whose *.o member objects are extracted
    from prebuilt archives at link time and never appear under `--builddir` for
    scan_relocated_addresses() to read a relocation from at all.

    Diffing this sweep's output against the pre-G2 whole-image word scan (run over
    both shipped ELFs, per the BACKLOG #106 G2 stop-licence: "compare the
    implementation sets before/after") found exactly that gap: __do_global_dtors_
    aux/frame_dummy/_fini (.init_array/.fini_array, crtstuff), __utf8_mbtowc/
    __utf8_wctomb (a newlib locale table), and the 9 sbmp16_* tonc surface-drawg
    entries -- all genuinely address-taken, all invisible to a relocation read
    because their .o's are inside libgcc.a/libc.a/libtonc.a, not `builddir`. (The
    diff's OTHER two "misses", art_icons_read_rows_fp and g2w_insert, are this
    project's OWN functions, called directly by name everywhere they appear in
    source/ -- the old scan's coincidental match on them is exactly the false-
    positive class G2 exists to remove, not a real miss.)

    own_function_names(builddir) draws the line precisely: a name this project's
    OWN *.o's define gets ONLY the relocation-proven treatment (no raw-scan
    fallback -- that's where the coincidence risk this fix closes actually lived);
    a name that is NOT one of this project's own functions (crt/libgcc/newlib/
    tonc) falls back to the raw scan, restricted to just that small, already-
    enumerated, human-reviewable set (every one of them already has, or needs, its
    own `addrtaken-ok` line) -- never re-widened to the whole image. `sections`/
    `section_dumps` are the alloc_load_sections()/dump_alloc_load_sections()
    result -- REQUIRED, not computed here, so main() can pass the ones it already
    fetched for read_build_dir_stamp() (no extra objdump invocations) and a direct
    caller/test is never surprised by a hidden shell-out.

    Returns (taken: set, detail: {fn: [locations]}, provenance: {fn: 'reloc'|'lit'|
    'raw'}) -- `detail` only ever names DATA-section relocation hits; the
    literal-pool and third-party-fallback classes have no single "containing
    symbol" worth naming (a literal pool is already "somewhere in this function's
    own pool"; a crt/tonc table's location is a coincidence-scan hit by
    construction, not a proven relocation site). `provenance` is D2's (BACKLOG
    #106) per-name answer to "which of the three classes found this" -- a name
    can only ever be reached by 'raw' if own_function_names() ALREADY placed it
    outside this project's own object files (scan_third_party_words_raw() is
    restricted to exactly that set), so 'reloc' and 'lit' take priority whenever
    a name happens to be provable more than one way."""
    reloc_taken, detail = scan_relocated_addresses(builddir, name_at)
    lit_taken = scan_text_literal_pool(dump_text, name_at)

    own = own_function_names(builddir)
    third_party = {name for name in name_at.values() if name not in own}
    raw_taken = set()
    if third_party:
        raw_taken = scan_third_party_words_raw(sections, section_dumps, name_at, third_party)

    taken = reloc_taken | lit_taken | raw_taken
    provenance = {}
    for fn in raw_taken:
        provenance[fn] = 'raw'
    for fn in lit_taken:
        provenance[fn] = 'lit'
    for fn in reloc_taken:
        provenance[fn] = 'reloc'          # highest priority: a real relocation record
    return taken, detail, provenance


# === D2 (BACKLOG #84b fifth pass): the ELF names the build dir it was linked from =====
#
# `--builddir` is a command-line argument with no link to the ELF at all -- nothing
# stopped `--elf PokeDNA-artless.elf --builddir build-delta` from silently reading the
# WRONG variant's .su files and printing a plausible-looking "STACK ok" for a number
# that has nothing to do with the ELF actually being certified (confirmed live: this
# exact mismatched invocation printed "STACK ok 10,120" against artless's true 13,752).
# perf.c's `pdna_build_dir[]` (a plain .rodata NUL-terminated string, stamped by the
# Makefile from the same $(BUILD) variable that names the --builddir the guard is
# handed) lets the guard read the ELF's OWN opinion of which build dir it came from
# and refuse when it disagrees with the one on the command line.

def read_build_dir_stamp(elf, sections, section_dumps=None):
    """The NUL-terminated string content of the `pdna_build_dir` symbol, read straight
    out of the linked ELF's own bytes (nm for the address, objdump -s for the bytes --
    the same two tools every other check in this file already shells out to). Returns
    None if the symbol is absent (an ELF built before this stamp existed, or a
    hand-rolled compile outside the Makefile) -- absence alone is never fatal, only
    a MISMATCH once both sides have an opinion (see main()). `section_dumps` (F7): see
    scan_address_taken()'s own note -- the same shared dump, avoiding a second
    per-section objdump sweep when main() already fetched one."""
    if section_dumps is None:
        section_dumps = dump_alloc_load_sections(elf, sections)
    out = subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout
    addr = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == "pdna_build_dir":
            addr = int(parts[0], 16)
            break
    if addr is None:
        return None
    for sec in sections:
        dump = section_dumps.get(sec)
        if dump is None:
            continue
        data = {}
        for line in dump.splitlines():
            m = _OBJDUMP_S_LINE_RE.match(line)
            if not m:
                continue
            line_addr = int(m.group(1), 16)
            raw = bytes.fromhex(m.group(2).replace(" ", ""))
            for i, b in enumerate(raw):
                data[line_addr + i] = b
        if addr not in data:
            continue
        out_bytes = bytearray()
        a = addr
        while data.get(a, 0) != 0:
            out_bytes.append(data[a])
            a += 1
        return out_bytes.decode("ascii", errors="replace")
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--builddir", required=True)
    ap.add_argument("--root", default="main")
    ap.add_argument("--margin", type=int, default=SAFETY_MARGIN)
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--sccs", action="store_true",
                     help="D5b: print the Tarjan SCCs (real recursion only) over the "
                          "graph reachable from --root, instead of running the guard")
    ap.add_argument("--dump-sites", action="store_true",
                     help="D5a: print every indirect-call site this walker found -- "
                          "caller, address, offset (for a 'field' site), and how it "
                          "resolved (qualified/unqualified field, argsites, whole-"
                          "function, or BLIND) -- instead of running the guard. Used to "
                          "convert stack_edges.txt's declarations to the honest "
                          "per-caller form.")
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

    # D3 (BACKLOG #84b review): a wrong --builddir has no .o files under it at all, so
    # every function would silently fall back to the prologue estimator -- no crash, no
    # obviously wrong number, just a report that LOOKS legitimate and certifies nothing.
    # Fail loud instead of guessing the directory is fine.
    builddir_objs = sorted(glob.glob(os.path.join(args.builddir, "*.o")))
    if not builddir_objs:
        print(f"*** stack_budget: 0 .o files under --builddir {args.builddir!r} -- "
              "wrong directory? (every function would silently fall back to the prologue "
              "estimator, which is not a build failure this guard could ever surface)",
              file=sys.stderr)
        return 1

    # BACKLOG #110: a STALE build directory can hold .o files compiled before
    # -fstack-usage was on CFLAGS (or under a different CFLAGS set entirely) -- their
    # frames then silently fall back to the same prologue estimator, printed as merely
    # "(estimated)" instead of the FATAL this deserves. It bit the p84b merge: main's
    # build/ had 22 .su files for 170 objects, so eight functions came out UNKNOWN and
    # the normal build FATALed only because those particular frames didn't match any
    # estimator idiom -- a different eight could just as easily have matched one and
    # printed a plausible, under-counted "STACK ok". See find_objects_missing_su()'s own
    # docstring for why an object under --builddir must always have one and archive
    # members are correctly exempt.
    missing_su = find_objects_missing_su(args.builddir)
    if missing_su:
        print(f"*** stack_budget: {len(missing_su)} object(s) under --builddir "
              f"{args.builddir!r} have no matching .su (stale build directory -- "
              "compiled before -fstack-usage was added, or under different CFLAGS):",
              file=sys.stderr)
        for obj in missing_su:
            print(f"***   {obj}", file=sys.stderr)
        print("*** Fix: `make rebuild` so every object in this build dir is compiled "
              "fresh, with -fstack-usage, under today's CFLAGS.", file=sys.stderr)
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

    need_write = cached is None
    if cached is not None:
        dump_text = cached["dump_text"]
        sym_text = cached.get("sym_text")   # absent in a cache written before D5b
    else:
        dump_text = disassemble(args.elf)
        sym_text = None
    if sym_text is None:
        sym_text = dump_symbol_table(args.elf)
        need_write = True                   # upgrade an old cache to carry sym_text too

    # D5b: the symbol-table census runs BEFORE analyze() so the disassembly walk can
    # substitute a disambiguated "<name>@<tu>" node the moment it sees a duplicated
    # static's block header, instead of ever building the merged/ambiguous graph and
    # trying to fix it up after the fact. `objdump -t` is cached alongside `objdump -d`
    # for the same reason the latter already was: both are the slow steps, re-run only
    # when the ELF/.su fingerprint changes.
    addr_unique_name, dup_names, _sym_entries = parse_symbol_census(sym_text)
    analysis = analyze(dump_text, addr_unique_name)
    su_sizes, su_dupes = load_su(args.builddir)
    estimated = estimate_frames(analysis["fn_lines"])

    if need_write:
        try:
            with open(cache_file, "w") as f:
                json.dump({"fingerprint": fp, "dump_text": dump_text,
                           "sym_text": sym_text}, f)
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

    (field_decls, field_offset_index, argsite_decls, whole_func_decls, frame_overrides,
     isr_decls, addrtaken_ok, recursion_decls) = (
        load_extra_edges(args.edges_file) if args.edges_file
        else ({}, ({}, {}), {}, {}, {}, set(), set(), {}))

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

    # D5b: every name stack_edges.txt references (as a caller or an implementation)
    # must be unambiguous -- a bare name the census found duplicated across TUs is an
    # ERROR listing the qualified candidates, never a silent pick of "whichever one".
    ambiguous = check_ambiguous_declarations(
        dup_names, addr_unique_name, field_decls, argsite_decls, whole_func_decls,
        frame_overrides, isr_decls, addrtaken_ok, recursion_decls)
    if ambiguous:
        print(f"*** stack_budget: {args.edges_file} references a duplicated static "
              "name without a '@tu' qualifier:", file=sys.stderr)
        for p in ambiguous:
            print(f"***   {p}", file=sys.stderr)
        return 1

    edges_to_add, blind, count_mismatches, legacy_ambiguous = resolve_all_sites(
        analysis, field_offset_index, argsite_decls, whole_func_decls)

    # F1(b) (BACKLOG #84b seventh pass): an `addrtaken-ok` exemption is CHECKED, not
    # trusted -- the reviewer's second review found several (era_resolver_cb, g3cross_
    # pic_cb, the 17 ScanCb entries, ...) that were genuinely dispatched despite being
    # declared "never called", a false-pass class just as real as an undeclared field.
    # Any name this run's resolve_all_sites() actually resolved a real site TO (i.e. it
    # is now one of the exempted implementations backing a genuine caller's edge) can no
    # longer also claim "never dispatched" -- FATAL, naming the caller that dispatches
    # it, so the fix is to move the entry into a real declaration (as this pass's own
    # stack_edges.txt rewrite did for every one the reviewer found) instead of silently
    # re-trusting a stale exemption. This does not (yet) chase the harder half of F1(b)
    # -- "or whose address is loaded as a literal that reaches a call/blx in reachable
    # code" -- which needs tracing literal loads across the whole reachable set, not
    # just the sites resolve_all_sites() already classified; left for a future pass,
    # noted honestly rather than claimed done.
    expired_exemptions = []
    for caller, impls in sorted(edges_to_add.items()):
        hit = sorted(impls & addrtaken_ok)
        if hit:
            expired_exemptions.append((caller, hit))
    if expired_exemptions:
        print(f"\n*** STACK_BUDGET EXEMPTION EXPIRED: an `addrtaken-ok` function in "
              f"{args.edges_file} is genuinely dispatched -- it can no longer claim "
              "never to be called:", file=sys.stderr)
        for caller, hit in expired_exemptions:
            print(f"***   {caller} dispatches: {', '.join(hit)}", file=sys.stderr)
        print("*** Fix: move each name into a real 'Struct.field @OFF in <caller> -> "
              "...' or 'caller argsites=N -> ...' declaration naming the caller above, "
              "and delete its addrtaken-ok line.", file=sys.stderr)
        return 1

    for caller, impls in edges_to_add.items():
        analysis["edges"][caller] |= impls

    if args.dump_sites:
        dump_sites(analysis, field_offset_index, argsite_decls, whole_func_decls)
        return 0

    if args.sccs:
        # Diagnostic-only: run over the fully-declared graph (every indirect site
        # resolve_all_sites() could tie to a declaration is unioned in above) so a
        # recursive edge hidden behind a struct-field/argsites declaration still
        # shows up here, even though --sccs doesn't itself enforce the blind-spot/
        # count-mismatch checks below (those are the guard's job, not this flag's).
        sccs = tarjan_sccs(args.root, analysis["edges"])
        print(f"SCCs reachable from {args.root}() (real recursion only, "
              f"{len(sccs)} found):")
        for comp in sccs:
            print(f"  {{{', '.join(sorted(comp))}}}")
        return 0

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

    # D4 (BACKLOG #84b sixth pass): every REAL strongly-connected component reachable
    # from --root must be declared -- the old walker truncated a cycle to 0 B and
    # memoized that, which made the printed total secretly depend on which function a
    # DFS happened to reach first (top_n_chains's root-children-first order could
    # disagree with a plain deepest_from(root, ...) call by hundreds of bytes on the
    # exact same ELF). An undeclared component is a FATAL naming it; a component whose
    # declared members disagree on the depth is also a FATAL (a real question -- which
    # one is right? -- not a silent pick of either).
    real_sccs = tarjan_sccs(args.root, analysis["edges"])
    scc_of = {}
    recursion_problems = []
    for comp in real_sccs:
        declared = sorted({(m, recursion_decls[m]) for m in comp if m in recursion_decls})
        depths = {d for _m, d in declared}
        if not declared:
            recursion_problems.append(
                f"{{{', '.join(sorted(comp))}}}: no member has a `recursion fn "
                "depth=N` declaration in " + args.edges_file)
            continue
        if len(depths) > 1:
            recursion_problems.append(
                f"{{{', '.join(sorted(comp))}}}: declared members disagree on depth: "
                + ", ".join(f"{m} depth={d}" for m, d in declared))
            continue
        depth = depths.pop()
        charge = depth * sum(frame_of(m, su_sizes, estimated, frame_overrides)[0]
                              for m in comp)
        fs = frozenset(comp)
        for m in comp:
            scc_of[m] = (charge, fs)
    if recursion_problems:
        print("\n*** STACK_BUDGET UNDECLARED RECURSION: a static walk cannot bound an "
              "undeclared recursive chain -- every real cycle reachable from "
              f"{args.root}() needs a `recursion fn depth=N` line in {args.edges_file}:",
              file=sys.stderr)
        for p in recursion_problems:
            print(f"***   {p}", file=sys.stderr)
        return 1

    chains = top_n_chains(args.root, analysis["edges"], su_sizes, estimated,
                           n=args.top, overrides=frame_overrides, scc_of=scc_of)
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
    for _tot, path, _cyc in chains:
        on_chain |= {name for name, _b, _s in path}
    reachable = reachable_from(args.root, analysis["edges"])
    blind_spots = whole_graph_blind_spots(reachable, blind)

    # D8 (BACKLOG #84b fifth pass): UNKNOWN frames are checked over the WHOLE
    # REACHABLE set, not just the printed top-N chains. An UNKNOWN frame contributes
    # 0 B to deepest_from()'s search (frame_of() returns 0 for it) -- so a function
    # whose TRUE frame is unknown, and therefore unbounded, can never win the "heaviest
    # child" comparison against a sibling with any measured weight at all. It loses
    # the chain race precisely because the walker doesn't know how heavy it really is,
    # which is exactly backwards: an unknown frame must never be allowed to hide by
    # being invisible to the ranking that decides what gets printed. Sweeping every
    # function reachable from --root (the same set whole_graph_blind_spots() already
    # uses for the same reason) catches an UNKNOWN sitting on a branch that never
    # prints, not just one unlucky enough to land on the printed #1..#N chains.
    # A declared implementation NAME that isn't actually LINKED into this build variant
    # (stack_edges.txt's own "WHY A NAMED IMPLEMENTATION MISSING FROM THE ELF IS A
    # WARNING, NOT A FATAL" note already covers this -- e.g. fused_gb_slice_read on the
    # artless build) still gets unioned into `analysis["edges"]` as a bare STRING by
    # `edges_to_add`, with no existence check -- so it becomes a phantom node that
    # `reachable_from()` happily walks to. Such a name has no .su entry and no
    # `estimated` entry (both are keyed off REAL disassembled functions), so frame_of()
    # falls through to its "unknown" default -- a false positive this whole-reachable
    # sweep must not report, since a name that isn't linked at all can never actually
    # be called and contributes exactly 0 B, not an unbounded unknown. Confirmed live:
    # the delta build's rom_open declaration names iconrom_fatfs_read/g3x_fatfs_read/
    # gb_art_read/gbscr_sd_read, several of which this variant never links -- before
    # this filter, D8's own sweep (this same commit's sibling fix) FATALed the delta
    # build on phantom names instead of real, unclassifiable frames.
    # F4 (BACKLOG #84b seventh pass): the BUILDDIR MISMATCH check moved HERE -- before
    # the unknown-frame check below (and every other content-dependent FATAL) -- since
    # a wrong --builddir means every number downstream (including which frames are
    # "unknown") is being computed against the WRONG variant's .su files in the first
    # place; printing "UNKNOWN FRAME" or "BLIND SPOT" first would send someone chasing
    # a phantom problem instead of the actual one-line fix (pass the right --builddir).
    sections = alloc_load_sections(args.elf)
    # F7 (BACKLOG #84b seventh pass): ONE objdump -s sweep, shared by both the
    # BUILDDIR-stamp read (right below) and the address-taken scan (further down) --
    # see dump_alloc_load_sections()'s own note.
    section_dumps = dump_alloc_load_sections(args.elf, sections)
    stamp = read_build_dir_stamp(args.elf, sections, section_dumps)
    builddir_basename = os.path.basename(os.path.normpath(args.builddir))
    if stamp is None:
        print(f"WARNING: {args.elf} has no pdna_build_dir stamp (pre-D2 ELF, or a "
              "hand-rolled compile) -- the --builddir/ELF pairing cannot be cross-checked.")
    elif stamp != builddir_basename:
        print(f"\n*** STACK_BUDGET BUILDDIR MISMATCH: {args.elf} was linked from build "
              f"dir {stamp!r}, but --builddir names {builddir_basename!r} -- the .su "
              "files being read almost certainly belong to a DIFFERENT variant than "
              "this ELF, so any number this run reports is meaningless.", file=sys.stderr)
        print("*** Pass the matching --builddir, or rebuild the ELF you actually meant "
              "to certify.", file=sys.stderr)
        return 1

    unknown_reachable = sorted(
        fn for fn in reachable
        if fn in analysis["funcs"]
        and frame_of(fn, su_sizes, estimated, frame_overrides)[1] == "unknown")

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
                   "override": " (frame override, hand-measured)",
                   "recursion": " (declared recursion, charged once)"}[src]
            print(f"      {b:6,d}  {name}{tag}")
        if cyc:
            print(f"      WARNING: recursion excluded at: {cyc}")

    # D2: a frame the prologue estimator could not classify (see estimate_frames'
    # own header) is UNKNOWN, not a trustworthy 0 -- one on a top-N chain means the
    # printed total above is not actually bounded. FATAL unless stack_edges.txt
    # carries a `frame fn = BYTES` hand-measured override for it.
    if unknown_reachable:
        print(f"\n*** STACK_BUDGET UNKNOWN FRAME: the prologue estimator could not classify "
              "the stack frame of the following function(s), reachable from "
              f"{args.root}() -- an unknown frame contributes 0 B to the chain search and "
              "can therefore never win the ranking that decides what gets printed above, "
              "so the printed chain totals are NOT bounded even when none of these names "
              "appear on them:")
        for fn in unknown_reachable:
            print(f"***   {fn}")
        print("*** Refusing to certify a number the walker cannot back. Fix: add a "
              "`frame fn = BYTES  (measured by hand, date)` line to "
              f"{args.edges_file}, or figure out why the estimator's known prologue "
              "idioms don't match this function's epilogue.")
        return 1

    if blind_spots:
        print("\n*** STACK_BUDGET BLIND SPOT: unresolved indirect call(s) inside a function "
              "reachable from the root (D9: this is a whole-graph sweep, not just the "
              "printed chains) -- the true depth past this point is UNKNOWN, not "
              "bounded by this report:")
        for fn, addr, ins, detail in blind_spots[:10]:
            print(f"***   {fn} @ {addr}: {ins}  [{detail}]")
        print("*** Refusing to certify a number the walker cannot back. Fix: give the "
              "indirect call site a resolvable target (devirtualize / annotate), or "
              "declare it in stack_edges.txt ('Struct.field @OFF -> impls' or "
              "'caller argsites=N -> impls').")
        return 1

    # D1 (BACKLOG #84b fifth pass): the ISR allowance is now a MEASUREMENT, not a bare
    # trusted constant. Every `isr fn` declaration in stack_edges.txt names a real
    # installed handler (irq_add); its own worst chain (deepest_from(fn, ...), which
    # already includes the handler's own frame) plus the 8 B libtonc's isr_master pushes
    # onto the user stack per nesting level to reach it is summed across every declared
    # handler -- see ISR_BYTES's own comment for why summing (not maxing) is the right
    # worst case (two DIFFERENT lines nesting, not one line re-entering itself). If that
    # measured sum ever exceeds the 64 B this guard actually charges every chain above,
    # the constant is stale and must be raised by hand -- refusing loudly here is exactly
    # what keeps ISR_BYTES honest instead of a number nobody re-derives.
    isr_allowance = 0
    isr_chain_details = []
    for handler in sorted(isr_decls):
        if handler not in analysis["funcs"]:
            print(f"*** stack_budget: {args.edges_file} declares isr {handler!r} but no "
                  f"such function exists in {args.elf}'s disassembly", file=sys.stderr)
            return 1
        h_total, _h_path, _h_cyc = deepest_from(handler, analysis["edges"], su_sizes,
                                                 estimated, overrides=frame_overrides,
                                                 scc_of=scc_of)
        isr_chain_details.append((handler, h_total))
        isr_allowance += 8 + h_total
    if isr_allowance > ISR_BYTES:
        print(f"\n*** STACK_BUDGET ISR ALLOWANCE EXCEEDED: the measured worst-case ISR "
              f"re-entry cost is {format_num(isr_allowance)} B, more than the "
              f"{ISR_BYTES} B every chain above is charged:")
        for handler, h_total in isr_chain_details:
            print(f"***   {handler}: 8 B entry + {format_num(h_total)} B own chain")
        print("*** Raise ISR_BYTES to match (and re-derive its own header comment), or "
              "shrink the named handler's chain.")
        return 1

    # D1 (BACKLOG #84b fifth pass): the address-taken sweep. A function whose address is
    # taken ANYWHERE in the linked image but is not (a) named by some declared class
    # above, (b) reachable through the ordinary call graph, (c) a declared ISR handler,
    # or (d) an explicit `addrtaken-ok` escape is a function this guard's whole reasoning
    # never accounted for at all -- a brand-new implementation slotted into an
    # ALREADY-declared class (the false-pass class D1 exists to close) looks EXACTLY
    # like this: its address is taken (assigned into the struct field/table) but no
    # declaration names it and the graph never reaches it any other way. (D2's own
    # BUILDDIR MISMATCH check that used to live here moved up before the unknown-frame
    # check, F4 BACKLOG #84b seventh pass -- `sections`/`stamp` are already computed.)
    #
    # G3 (BACKLOG #84b eighth pass): (b)'s `reachable` set is reachable_from(args.root,
    # ...) -- for the real entry point (--root main) that is every function the whole
    # linked image can reach, so an orphan really is unaccounted for. The two documented
    # re-measurement commands (source/pdna_box.c, rom_gbui.h) pass a SUB-root
    # (gbscr_open_inner, pcp_open_party_strip_inner) to re-derive one chain's number in
    # isolation -- `reachable` from a sub-root is a small subset of main's, so a
    # function that is address-taken and genuinely reachable only from OTHER parts of
    # main's graph looks like a spurious orphan here even though the real (--root main)
    # sweep already accounts for it. The sweep is a property of the whole image, not of
    # whichever root a re-measurement happens to pass, so it only runs for --root main.
    if args.root == "main":
        taken, taken_detail, taken_provenance = scan_address_taken(
            args.builddir, dump_text, analysis["name_at"], sections, section_dumps)
        declared_or_reachable = all_impls | reachable | isr_decls | addrtaken_ok
        candidates = sorted(taken - declared_or_reachable)

        # D2 (BACKLOG #106): scan_third_party_words_raw() is still a coincidence
        # scanner (a data word happening to equal a function's address, restricted
        # to the small third-party name set -- see scan_address_taken()'s
        # docstring) and it cannot be deleted (it is the only path that finds
        # isr_master, the m4/m5_surface tables, .init_array entries, sbmp16_* and
        # the rest of the crt/libgcc/tonc set a relocation read can never see). A
        # name found ONLY that way is bounded the SAME way an addrtaken-ok
        # exemption is bounded below (G1): if its own worst chain, AS IF IT WERE
        # --root, is <= EXEMPT_MAX_DEEPEST, a coincidental hit on it cannot hide
        # an arbitrarily heavy chain, so it is a NOTE, not a build-breaking FATAL.
        # A reloc/lit hit is never downgraded -- those are proof, not coincidence.
        orphans = []
        bounded_notes = []
        for fn in candidates:
            if taken_provenance.get(fn) == 'raw' and fn in analysis["funcs"]:
                own_total, _own_path, _own_cycles = deepest_from(
                    fn, analysis["edges"], su_sizes, estimated,
                    overrides=frame_overrides, scc_of=scc_of)
                if own_total <= EXEMPT_MAX_DEEPEST:
                    bounded_notes.append((fn, own_total))
                    continue
            orphans.append(fn)

        if bounded_notes:
            print("\n*** STACK_BUDGET NOTE: third-party raw hit, bounded (own worst chain "
                  f"<= {EXEMPT_MAX_DEEPEST} B, no addrtaken-ok line needed):")
            for fn, own_total in bounded_notes:
                print(f"***   {fn}: own worst chain {format_num(own_total)} B")

        if orphans:
            print("\n*** STACK_BUDGET ADDRESS-TAKEN, UNREACHED, UNDECLARED:")
            for fn in orphans:
                b, _src = frame_of(fn, su_sizes, estimated, frame_overrides)
                print(f"***   {fn} frame {b}")
                for loc in taken_detail.get(fn, []):
                    print(f"***     {loc}")
            print("*** This function's address is stored somewhere in the linked image (a "
                  "struct field, a dispatch table, a literal pool) but it is named by no "
                  "declaration in stack_edges.txt, not reachable through the ordinary call "
                  "graph, and not a declared ISR handler. It could be a brand-new "
                  "implementation silently slotted into an existing dispatch class -- the "
                  "exact false pass this sweep exists to catch. Add it to the right "
                  "declaration's implementation list, or (if it is a genuine false positive) "
                  "an `addrtaken-ok fn  (reason)` line.")
            return 1

        # D3 (BACKLOG #106): nothing ever retired a stale `addrtaken-ok` line -- one
        # that used to be genuinely address-taken (in SOME earlier build, or on the
        # OTHER image variant) but isn't in THIS one. A WARNING, not a FATAL: a line
        # only one image variant needs is legitimately unneeded on the other, so this
        # is "delete it if it truly serves nothing on either build", never a gate.
        stale = sorted(addrtaken_ok - taken)
        if stale:
            print(f"\n*** STACK_BUDGET WARNING: {len(stale)} addrtaken-ok line(s) are no "
                  "longer address-taken in this image (delete if unneeded on BOTH images): "
                  + ", ".join(stale))

    # G1 (BACKLOG #84b eighth pass, merge-blocker): an `addrtaken-ok` claim is never
    # re-verified above -- it just removes `fn` from the orphans check. That is fine
    # for a genuinely tiny dispatch shim, but nothing stops a heavy function from being
    # exempted by a wrong or stale comment. Bound the damage: every addrtaken-ok
    # function actually present in this ELF must have its OWN worst chain (as if it
    # were --root) no heavier than EXEMPT_MAX_DEEPEST, independent of whether anything
    # calls it in this build.
    heavy_exemptions = []
    for fn in sorted(addrtaken_ok):
        if fn not in analysis["funcs"]:
            continue
        own_total, _own_path, _own_cycles = deepest_from(
            fn, analysis["edges"], su_sizes, estimated,
            overrides=frame_overrides, scc_of=scc_of)
        if own_total > EXEMPT_MAX_DEEPEST:
            heavy_exemptions.append((fn, own_total))
    if heavy_exemptions:
        print(f"\n*** STACK_BUDGET EXEMPTION TOO HEAVY: an `addrtaken-ok` exemption in "
              f"{args.edges_file} may only cover functions whose own worst chain is <= "
              f"{EXEMPT_MAX_DEEPEST} B (an unverified `addrtaken-ok` claim must not be "
              "able to hide an arbitrarily heavy chain):", file=sys.stderr)
        for fn, own_total in heavy_exemptions:
            print(f"***   {fn}: own worst chain {format_num(own_total)} B", file=sys.stderr)
        print("*** Fix: give it a real Struct.field @OFF in <caller> -> ... declaration.",
              file=sys.stderr)
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
