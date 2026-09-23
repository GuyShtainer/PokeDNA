#!/usr/bin/env python3
"""Render docs/FEATURE-MATRIX.md as a single self-contained HTML page.

Quick viewer, not a build step: run it again after editing the markdown.
    python3 tools/gen_matrix_html.py && open docs/feature-matrix.html

Three source documents feed the page, in this order:
  1. docs/HW-QUEUE.md   -- the live hardware queue (PASS/FAIL/SKIP per row, "Copy report").
                            Rendered FIRST, above the feature matrix.
  2. docs/FEATURE-MATRIX.md -- the traceability matrix (unchanged behaviour/format).
  3. docs/HW-TEST-2026-09-05-GB-ARC.md -- the step-by-step cart script, rendered AFTER the
     matrix as a collapsible "Detailed hardware steps" section, one <details> per lettered
     ("## X. ...") section, with per-step checkboxes.
Screenshots stay last, exactly as before.
"""
import base64
import shutil
import datetime
import datetime
import html
import json
import pathlib
import re
import sys

import markdown

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "FEATURE-MATRIX.md"
HWQ = ROOT / "docs" / "HW-QUEUE.md"
HWTEST = ROOT / "docs" / "HW-TEST-2026-09-05-GB-ARC.md"
OUT = ROOT / "docs" / "feature-matrix.html"
# A second copy in a folder that holds ONLY the page: docs/ itself carries fused ROM
# builds and analysis dumps, so the tailnet server (tools/hosting/) serves docs/site alone.
SITE = ROOT / "docs" / "site" / "index.html"
# tools/gb_contact_sheet.py --per-feature writes one sheet per feature (docs/FEATURE-
# MATRIX.md's own S1/S2/S3/bag/#41/S5-B rows) plus this index, in the fixed order Guy
# asked the status page to stay sorted in ("keep updating the screenshots sorted per
# feature") rather than capture order. Older sheets below are still listed as-is.
CONTACT_SHEETS_INDEX = ROOT / "docs" / "contact-sheets" / "index.json"
# Screenshots embedded as data URIs so the page stays one self-contained file. These
# render AFTER the per-feature sheets above (when the index exists) as the pre-
# per-feature history.
SHOTS = [
    (ROOT / "docs" / "contact-sheet-2026-09-05-gb-arc.png",
     "Gen-1/2 arc (S1..S5-B) in mGBA via a fused GB save, 2026-09-05 — SD writes "
     "refuse in the emulator, so the \"Saving\" outcomes are hardware-only "
     "(combined sheet — see the per-feature sheets above for the same shots sorted "
     "by feature)"),
    (ROOT / "docs" / "contact-sheet-2026-08-30.png",
     "Contact sheet, 2026-08-30 arc (repaint pass D1-D10, DMA audit B3) — emulator build"),
    (ROOT / "docs" / "shots" / "sheet-artless.png", "Artless build, 2026-08-08"),
    (ROOT / "docs" / "shots" / "sheet-p1-rom-gated-icons.png",
     "ROM-gated icons, 2026-08-08"),
]

# status token -> (css class, colour)
TOKENS = {
    "HW-PASS": ("pass", "#3fb950"),
    "HW-BUG": ("bug", "#f85149"),
    "HW-PEND": ("pend", "#e3b341"),
    "EMU": ("emu", "#58a6ff"),
    "HOST": ("host", "#bc8cff"),
    "DARK": ("dark", "#8b949e"),
    "IDEA": ("idea", "#39c5cf"),
    "NOT-IN-BUILD": ("notinbuild", "#7d8fa9"),
}

MD_EXTENSIONS = ["tables", "sane_lists", "attr_list"]

TEMPLATE = """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PokeDNA - feature x verification matrix</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; background:#0d1117; color:#c9d1d9;
         font:15px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif; }
  .wrap { max-width:1180px; margin:0 auto; padding:0 24px 96px; }
  h1 { font-size:30px; margin:32px 0 4px; letter-spacing:-.3px; }
  h2 { font-size:22px; margin:44px 0 12px; padding-top:14px; border-top:1px solid #21262d; }
  h3 { font-size:17px; margin:28px 0 8px; color:#e6edf3; }
  p, li { color:#c9d1d9; }
  a { color:#58a6ff; }
  blockquote { margin:16px 0; padding:12px 18px; border-left:3px solid #30363d;
               background:#11161d; color:#9aa5b1; border-radius:0 6px 6px 0; }
  blockquote p { margin:8px 0; }
  hr { border:0; border-top:1px solid #21262d; margin:40px 0; }
  code { background:#161b22; border:1px solid #21262d; border-radius:5px;
         padding:1px 5px; font:12.5px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace; }
  table { border-collapse:collapse; width:100%; margin:14px 0 22px; font-size:14px; }
  th, td { text-align:left; vertical-align:top; padding:8px 12px; border:1px solid #21262d; }
  th { background:#161b22; color:#e6edf3; font-weight:600; position:sticky; top:0; z-index:2; }
  tbody tr:nth-child(even) { background:#0f141a; }
  tbody tr:hover { background:#161d26; }
  td:first-child { white-space:nowrap; }
  .toolbar button, .hwq-bar button, .hwbtn {
    touch-action: manipulation;      /* no 300ms double-tap-zoom delay on a phone */
    min-height: 40px; min-width: 40px;   /* a thumb-sized target, not a mouse-sized one */
  }
  .fhidden { display: none; }
  .badge { display:inline-block; padding:1px 8px; border-radius:11px; font-weight:600;
           font-size:11.5px; letter-spacing:.4px; border:1px solid; background:transparent; }
  BADGECSS
  .toolbar { position:sticky; top:0; z-index:5; background:#0d1117ee; backdrop-filter:blur(6px);
             border-bottom:1px solid #21262d; padding:10px 24px; display:flex; gap:8px;
             flex-wrap:wrap; align-items:center; }
  .toolbar button { cursor:pointer; font:inherit; font-size:12.5px; padding:3px 11px;
                    border-radius:11px; border:1px solid #30363d; background:#161b22;
                    color:#c9d1d9; }
  .toolbar button.on { outline:2px solid #58a6ff; outline-offset:1px; }
  .toolbar input { flex:1; min-width:160px; font:inherit; font-size:13px; padding:4px 10px;
                   border-radius:6px; border:1px solid #30363d; background:#0d1117; color:#c9d1d9; }
  .toolbar .n { opacity:.6; font-variant-numeric:tabular-nums; }
  .hidden { display:none; }
  .foot { margin-top:56px; color:#6e7681; font-size:12.5px; }

  /* --- Hardware queue (docs/HW-QUEUE.md) --- */
  .hwq-bar { position:sticky; top:42px; z-index:4; background:#0d1117ee; backdrop-filter:blur(6px);
             border-bottom:1px solid #21262d; padding:8px 24px; margin:10px -24px 14px;
             display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
  .hwq-bar button { cursor:pointer; font:inherit; font-size:12.5px; padding:3px 11px;
                    border-radius:11px; border:1px solid #30363d; background:#161b22; color:#c9d1d9; }
  .hwq-bar button.on { outline:2px solid #58a6ff; outline-offset:1px; }
  .hwq-count { opacity:.65; font-size:12.5px; font-variant-numeric:tabular-nums; }
  .hwq-table-wrap { overflow-x:auto; -webkit-overflow-scrolling:touch; margin:8px 0 6px;
                    border:1px solid #21262d; border-radius:8px; }
  .hwq-table { min-width:960px; font-size:13px; margin:0; }
  .hwq-table th, .hwq-table td { padding:6px 9px; }
  .hwbtns { display:flex; gap:4px; flex-wrap:nowrap; }
  .hwbtn { cursor:pointer; font:inherit; font-size:11px; padding:3px 9px; border-radius:9px;
           border:1px solid #30363d; background:#161b22; color:#8b949e; white-space:nowrap; }
  .hwbtn-pass.on { color:#3fb950; border-color:#3fb95088; background:#3fb95022; }
  .hwbtn-fail.on { color:#f85149; border-color:#f8514988; background:#f8514922; }
  .hwbtn-skip.on { color:#e3b341; border-color:#e3b34188; background:#e3b34122; }
  .hwnote { width:100%; min-width:120px; font:inherit; font-size:12px; padding:3px 6px;
            border-radius:5px; border:1px solid #30363d; background:#0d1117; color:#c9d1d9; }
  tr.hwrow-pass { background:#3fb9500f !important; }
  tr.hwrow-fail { background:#f851490f !important; }
  tr.hwrow-skip { background:#e3b3410f !important; }
  .hwreport { width:100%; min-height:130px; margin-top:10px;
              font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;
              background:#0d1117; color:#c9d1d9; border:1px solid #30363d; border-radius:6px;
              padding:10px; }
  .hwq-hint { color:#6e7681; font-size:12px; margin:4px 0 0; }

  /* --- Detailed hardware steps (docs/HW-TEST-....md) --- */
  .hwtest details { margin:10px 0; border:1px solid #21262d; border-radius:8px;
                     padding:2px 16px 12px; background:#11161d; }
  .hwtest summary { cursor:pointer; font-weight:600; color:#e6edf3; padding:10px 0; }
  .hwtest-intro p { color:#8b949e; }
  .hwsteps { list-style:none; margin:6px 0 4px; padding:0; }
  .hwsteps li { padding:7px 2px; border-top:1px solid #21262d; }
  .hwsteps li:first-child { border-top:none; }
  .hwsteps label { display:flex; gap:9px; align-items:flex-start; cursor:pointer; }
  .hwsteps input[type=checkbox] { margin-top:4px; flex:none; }
  .hwstep-done { opacity:.55; }
  .hwstep-done label { text-decoration:line-through; }
  :target { scroll-margin-top:96px; }
  details:target, li:target { outline:2px solid #58a6ff; outline-offset:3px; border-radius:6px; }
</style>
</head><body>
<div class="toolbar" id="bar">
  <button data-tok="" class="on">All</button>
  BUTTONS
  <input id="q" type="search" placeholder="filter rows by text...">
  <span id="filtercount" style="color:#58a6ff;font-size:13px;cursor:pointer;padding:6px 2px"></span>
</div>
<div class="wrap">
HWQUEUE_HTML
BODY
HWTEST_HTML
SHOTS_HTML
<p class="foot">Generated from <code>docs/FEATURE-MATRIX.md</code> (+ <code>docs/HW-QUEUE.md</code>,
<code>docs/HW-TEST-2026-09-05-GB-ARC.md</code>) by
<code>tools/gen_matrix_html.py</code>. Edit the markdown, re-run the script.</p>
</div>
<script>
var bar = document.getElementById('bar'), q = document.getElementById('q'), tok = '';
function apply() {
  var text = q.value.trim().toLowerCase();
  var shown = 0, total = 0;
  document.querySelectorAll('table.matrix tbody tr').forEach(function (tr) {
    var t = tr.textContent;
    var okTok = !tok || (tr.querySelector('.badge-' + tok) !== null);
    var okTxt = !text || t.toLowerCase().indexOf(text) !== -1;
    tr.classList.toggle('hidden', !(okTok && okTxt));
    total++; if (okTok && okTxt) shown++;
  });
  document.querySelectorAll('table.matrix').forEach(function (tb) {
    var any = tb.querySelectorAll('tbody tr:not(.hidden)').length;
    tb.classList.toggle('hidden', any === 0);
  });
  /* The matrix starts ~330 KB below this toolbar, past the whole hardware queue, so a
     filter used to change only things you could not see -- on a phone that reads as a
     dead button. Filter the queue too (it is the table directly below), and say how many
     rows matched. Uses its own class so the queue's "Show unchecked only" cannot fight it. */
  var hwq = document.getElementById('hwq-table');
  if (hwq) {
    var hshown = 0, htotal = 0;
    hwq.querySelectorAll('tbody tr').forEach(function (tr) {
      var okTok = !tok || (tr.querySelector('.badge-' + tok) !== null);
      var okTxt = !text || tr.textContent.toLowerCase().indexOf(text) !== -1;
      tr.classList.toggle('fhidden', !(okTok && okTxt));
      htotal++; if (okTok && okTxt) hshown++;
    });
    var hc = document.getElementById('hwq-filtercount');
    if (hc) hc.textContent = (tok || text) ? (hshown + ' of ' + htotal + ' rows match') : '';
  }
  var fc = document.getElementById('filtercount');
  if (fc) fc.textContent = (tok || text)
    ? (shown + ' feature row' + (shown === 1 ? '' : 's') + ' below \u2014 tap to jump')
    : '';
}
bar.addEventListener('click', function (e) {
  /* Each filter button wraps its label in <span>s, so on a touch screen e.target is the
     SPAN, not the BUTTON -- the old tagName test bailed out and the toolbar did nothing
     on a phone while working with a mouse that happened to hit the button's padding.
     Walk up to the button instead. */
  var btn = e.target.closest ? e.target.closest('button') : null;
  if (!btn || !bar.contains(btn)) return;
  tok = btn.dataset.tok;
  bar.querySelectorAll('button').forEach(function (b) { b.classList.toggle('on', b === btn); });
  apply();
});
q.addEventListener('input', apply);
var fcEl = document.getElementById('filtercount');
if (fcEl) fcEl.addEventListener('click', function () {
  var first = document.querySelector('table.matrix:not(.hidden)');
  if (first && first.scrollIntoView) first.scrollIntoView({ block: 'start' });
});

// --- Hardware queue: PASS/FAIL/SKIP per row, notes, "Show unchecked only", "Copy report" ---
(function () {
  var HW_BUILD = HW_BUILD_JS, HW_BUILD_DATE = HW_BUILD_DATE_JS;
  var table = document.getElementById('hwq-table');
  if (!table) return;
  var rows = Array.prototype.slice.call(table.querySelectorAll('tbody tr'));

  function keyFor(id) { return 'pokedna-hw:' + id; }
  function load(id) {
    try { return JSON.parse(localStorage.getItem(keyFor(id)) || 'null') || {}; }
    catch (e) { return {}; }
  }
  function save(id, obj) {
    try { localStorage.setItem(keyFor(id), JSON.stringify(obj)); } catch (e) { /* storage full/blocked */ }
  }
  function paintRow(tr, state) {
    tr.classList.remove('hwrow-pass', 'hwrow-fail', 'hwrow-skip');
    if (state) tr.classList.add('hwrow-' + state.toLowerCase());
  }
  function updateCount() {
    var done = rows.filter(function (tr) { return !!load(tr.dataset.id).v; }).length;
    var el = document.getElementById('hwq-count');
    if (el) el.textContent = done + ' / ' + rows.length + ' checked';
  }

  rows.forEach(function (tr) {
    var id = tr.dataset.id;
    var data = load(id);
    var btns = Array.prototype.slice.call(tr.querySelectorAll('.hwbtn'));
    var note = tr.querySelector('.hwnote');
    if (data.v) {
      btns.forEach(function (b) { b.classList.toggle('on', b.dataset.v === data.v); });
      paintRow(tr, data.v);
    }
    if (note && data.n) note.value = data.n;
    btns.forEach(function (b) {
      b.addEventListener('click', function () {
        var cur = load(id);
        var next = (cur.v === b.dataset.v) ? null : b.dataset.v;
        cur.v = next;
        save(id, cur);
        btns.forEach(function (bb) { bb.classList.toggle('on', !!next && bb.dataset.v === next); });
        paintRow(tr, next);
        updateCount();
        applyUncheckedFilter();
      });
    });
    if (note) {
      note.addEventListener('input', function () {
        var cur = load(id);
        cur.n = note.value;
        save(id, cur);
      });
    }
  });
  updateCount();

  var uncheckedOnly = false;
  /* Touch targets: give the toolbar buttons room for a thumb and stop the browser
     double-tap-zooming on them (a 300 ms tap delay reads as "the button did nothing"). */
  var btnUnchecked = document.getElementById('hwq-unchecked');
  var btnCopy = document.getElementById('hwq-copy');
  var btnReset = document.getElementById('hwq-reset');
  var reportBox = document.getElementById('hwq-reportbox');

  function applyUncheckedFilter() {
    rows.forEach(function (tr) {
      var hide = uncheckedOnly && !!load(tr.dataset.id).v;
      tr.classList.toggle('hidden', hide);
    });
  }
  if (btnUnchecked) {
    btnUnchecked.addEventListener('click', function () {
      uncheckedOnly = !uncheckedOnly;
      btnUnchecked.classList.toggle('on', uncheckedOnly);
      applyUncheckedFilter();
    });
  }
  if (btnReset) {
    btnReset.addEventListener('click', function () {
      if (!window.confirm('Clear all HW-queue results and step checkboxes on this device?')) return;
      var toRemove = [];
      for (var i = 0; i < localStorage.length; i++) {
        var k = localStorage.key(i);
        if (k && (k.indexOf('pokedna-hw:') === 0 || k.indexOf('pokedna-hw-step:') === 0)) toRemove.push(k);
      }
      toRemove.forEach(function (k) { localStorage.removeItem(k); });
      location.reload();
    });
  }
  if (btnCopy) {
    btnCopy.addEventListener('click', function () {
      var lines = ['PokeDNA HW report — build ' + HW_BUILD + ' — ' + HW_BUILD_DATE];
      rows.forEach(function (tr) {
        var id = tr.dataset.id;
        var data = load(id);
        if (!data.v) return;
        var line = id + ' ' + data.v;
        if (data.v === 'FAIL' && data.n) line += ': ' + data.n;
        lines.push(line);
        if (data.v === 'FAIL') {
          var sec = tr.dataset.stepsSection;
          if (sec) {
            var det = document.getElementById('hw-' + sec);
            if (det) {
              var un = Array.prototype.slice.call(det.querySelectorAll('.hwstep-cb'))
                .filter(function (cb) { return !cb.checked; })
                .map(function (cb) { return cb.dataset.step; });
              if (un.length) lines.push('  unchecked in §' + sec + ': ' + un.join(', '));
            }
          }
        }
      });
      var text = lines.length > 1 ? lines.join('\\n') : lines[0] + '\\n(no rows marked yet)';
      if (reportBox) {
        reportBox.value = text;
        reportBox.classList.remove('hidden');
        reportBox.focus();
        reportBox.select();
      }
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).catch(function () { /* fall back to the textarea above */ });
      }
    });
  }
  // The mini toolbar is sticky just under the global filter bar -- measure it (it can
  // wrap to 2-3 lines on a phone) instead of guessing a fixed offset.
  function stickHwqBar() {
    var hwqBar = document.getElementById('hwq-bar');
    if (bar && hwqBar) hwqBar.style.top = bar.offsetHeight + 'px';
  }
  window.addEventListener('resize', stickHwqBar);
  stickHwqBar();
})();

// --- Detailed hardware steps: per-step checkboxes + open the section named in the URL hash ---
(function () {
  document.querySelectorAll('.hwstep-cb').forEach(function (cb) {
    var key = 'pokedna-hw-step:' + cb.dataset.step;
    var v;
    try { v = localStorage.getItem(key); } catch (e) { v = null; }
    var li = cb.closest('li');
    if (v === '1') {
      cb.checked = true;
      if (li) li.classList.add('hwstep-done');
    }
    cb.addEventListener('change', function () {
      try { localStorage.setItem(key, cb.checked ? '1' : '0'); } catch (e) { /* ignore */ }
      if (li) li.classList.toggle('hwstep-done', cb.checked);
    });
  });

  function openHashTarget() {
    var h = location.hash.slice(1);
    if (!h) return;
    var el = document.getElementById(h);
    if (!el) return;
    var det = el.tagName === 'DETAILS' ? el : (el.closest && el.closest('details'));
    if (det) det.open = true;
    if (el.scrollIntoView) el.scrollIntoView({ block: 'center' });
  }
  window.addEventListener('hashchange', openHashTarget);
  openHashTarget();
})();
</script>
</body></html>
"""


def badge_span(token: str) -> str:
    cls = TOKENS.get(token)
    if not cls:
        return html.escape(token)
    return '<span class="badge badge-%s">%s</span>' % (cls[0], html.escape(token))


def md_inline(text: str) -> str:
    """Render a short markdown snippet (table-cell text) without the wrapping <p>."""
    out = markdown.markdown(text or "", extensions=MD_EXTENSIONS).strip()
    if out.startswith("<p>") and out.endswith("</p>"):
        out = out[3:-4]
    return out


def demote_headings(text: str, extra: int = 2) -> str:
    """Push any raw '#' ATX heading in embedded prose down `extra` levels (capped at 6) so
    it nests visually under this page's own headings instead of competing with <h1>/<h2>."""
    def repl(m: "re.Match[str]") -> str:
        return "#" * min(6, len(m.group(1)) + extra) + " "
    return re.sub(r"(?m)^(#{1,6})[ \t]+", repl, text)


def md_block_html(text: str) -> str:
    return markdown.markdown(demote_headings(text), extensions=MD_EXTENSIONS)


# ---------------------------------------------------------------------------
# docs/HW-QUEUE.md -- a hand-parsed markdown table (not handed to python-markdown) so each
# row can carry data-id, PASS/FAIL/SKIP buttons and a note <input> that markdown's own
# table renderer has no hook for.
# ---------------------------------------------------------------------------

_TABLE_SEP_RE = re.compile(r"^\|?[\s:-]+\|")


def _find_table_start(lines: list) -> int:
    for i in range(len(lines) - 1):
        if lines[i].strip().startswith("|") and _TABLE_SEP_RE.match(lines[i + 1].strip()):
            return i
    return -1


def _split_row(line: str) -> list:
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|"):
        s = s[:-1]
    return [c.strip() for c in s.split("|")]


def _parse_md_table(lines: list, start: int):
    header = _split_row(lines[start])
    rows = []
    i = start + 2
    while i < len(lines) and lines[i].strip().startswith("|"):
        rows.append(_split_row(lines[i]))
        i += 1
    return header, rows


def render_hwqueue(path: "pathlib.Path"):
    """Returns (html, build_hash, build_date). Empty html (and '?' stamps) if the file is
    missing -- this section is additive, so a temporarily-absent queue must not kill the page."""
    if not path.is_file():
        print("note: %s missing, skipping the hardware-queue section" % path, file=sys.stderr)
        return "", "?", "?"
    raw = path.read_text(encoding="utf-8")
    lines = raw.split("\n")
    ti = _find_table_start(lines)
    if ti < 0:
        print("note: no markdown table found in %s" % path, file=sys.stderr)
        return "", "?", "?"

    preamble_lines = lines[:ti]
    if preamble_lines and preamble_lines[0].lstrip().startswith("#"):
        preamble_lines = preamble_lines[1:]  # the page supplies its own <h2> for this section
    preamble = "\n".join(preamble_lines).strip("\n")

    build_m = re.search(r"Build on OneDrive:\s*`([^`]+)`\s*\(([^,)]+)", preamble)
    build_hash = build_m.group(1).strip() if build_m else "?"
    build_date = build_m.group(2).strip() if build_m else "?"

    header_cells, data_rows = _parse_md_table(lines, ti)
    colidx = {name.strip().lower(): idx for idx, name in enumerate(header_cells)}

    def col(cells: list, name: str) -> str:
        idx = colidx.get(name)
        if idx is None or idx >= len(cells):
            return ""
        return cells[idx].strip()

    rows_html = []
    for cells in data_rows:
        rid = col(cells, "id")
        if not rid:
            continue
        kind = col(cells, "kind")
        what = col(cells, "what landed")
        commit = col(cells, "commit")
        check = col(cells, "check on the cart")
        expect = col(cells, "expect")
        steps_raw = col(cells, "steps")
        status_raw = col(cells, "status")

        sec_m = re.match(r"^§([A-M])", steps_raw)
        section_letter = sec_m.group(1) if sec_m else ""
        if not steps_raw or steps_raw in ("—", "-"):
            steps_html = "—"
        elif "§" in steps_raw:
            # A cell can name several sections ("§M, §Q"), and a section can be missing
            # entirely (§N..§Q had no anchor and jumped nowhere). Link each ref that has
            # a real target; render the rest as plain text so a button never lies.
            def _ref_link(m):
                ref = m.group(1)
                if ref in HWTEST_ANCHORS:
                    return '<a href="#hw-%s">§%s</a>' % (html.escape(ref), html.escape(ref))
                return '<span title="no detailed steps written for this section">§%s</span>' % html.escape(ref)
            steps_html = re.sub(r"§([A-Za-z0-9]+)", _ref_link, html.escape(steps_raw))
        else:
            steps_html = md_inline(steps_raw)

        status_token = re.sub(r"[`\s]", "", status_raw)
        status_html = badge_span(status_token) if status_token else ""

        tds = "".join(
            "<td>%s</td>" % c
            for c in (
                md_inline(rid), md_inline(kind), md_inline(what), md_inline(commit),
                md_inline(check), md_inline(expect), steps_html, status_html,
            )
        )
        buttons = "".join(
            '<button type="button" class="hwbtn hwbtn-%s" data-v="%s">%s</button>'
            % (v.lower(), v, v)
            for v in ("PASS", "FAIL", "SKIP")
        )
        tds += '<td><div class="hwbtns">%s</div></td>' % buttons
        tds += '<td><input type="text" class="hwnote" placeholder="note (esp. on FAIL)"></td>'
        sec_attr = ' data-steps-section="%s"' % html.escape(section_letter) if section_letter else ""
        rows_html.append('<tr data-id="%s"%s>%s</tr>' % (html.escape(rid), sec_attr, tds))

    preamble_html = md_block_html(preamble) if preamble.strip() else ""
    toolbar_html = (
        '<div class="hwq-bar" id="hwq-bar">'
        '<button type="button" id="hwq-unchecked">Show unchecked only</button>'
        '<button type="button" id="hwq-copy">Copy report</button>'
        '<button type="button" id="hwq-reset">Reset</button>'
        '<span class="hwq-count" id="hwq-count"></span>'
        '<span class="hwq-count" id="hwq-filtercount"></span>'
        "</div>"
    )
    table_html = (
        '<div class="hwq-table-wrap"><table class="hwq-table" id="hwq-table">'
        "<thead><tr><th>ID</th><th>Kind</th><th>What landed</th><th>Commit</th>"
        "<th>Check on the cart</th><th>Expect</th><th>Steps</th><th>Status</th>"
        "<th>Your result</th><th>Note</th></tr></thead>"
        "<tbody>" + "".join(rows_html) + "</tbody></table></div>"
        '<textarea id="hwq-reportbox" class="hwreport hidden" readonly></textarea>'
        '<p class="hwq-hint">If "Copy report" did not copy automatically (common on iOS '
        "Safari when a page is served over plain HTTP, which this one is), tap the box "
        "above, select all, and copy by hand.</p>"
    )
    out_html = (
        '<h2 id="hwqueue">Hardware queue — what to check</h2>'
        + preamble_html + toolbar_html + table_html
    )
    return out_html, build_hash, build_date


# ---------------------------------------------------------------------------
# docs/HW-TEST-2026-09-05-GB-ARC.md -- rendered as collapsible <details> per lettered
# ("## X. ...") section, with numbered steps (A1., M6b, ...) split into their own
# checkbox-bearing <li>s. See the module docstring for the two-pass block model this
# depends on: a blank-line-delimited block is a "steps run" iff ITS FIRST LINE matches the
# step-label pattern; that is what stops prose asides like "E4 is emulator-blind..." (which
# start with a letter+digit purely by coincidence, mid-paragraph) from being mis-split.
# ---------------------------------------------------------------------------

_STEP_RE = re.compile(r"^([A-M]\d{1,2}[a-z]?)(?=[.\s(])")
_SECTION_HEADER_RE = re.compile(r"^## (.+)$")
_LETTERED_RE = re.compile(r"^([A-M])\.\s+(.*)$")


def _split_hw_sections(md_text: str):
    """[(header_text_or_None, [body_lines]), ...]; element 0 is always the preamble."""
    chunks = []
    cur_header = None
    cur_lines = []
    for line in md_text.split("\n"):
        m = _SECTION_HEADER_RE.match(line)
        if m:
            chunks.append((cur_header, cur_lines))
            cur_header = m.group(1)
            cur_lines = []
        else:
            cur_lines.append(line)
    chunks.append((cur_header, cur_lines))
    return chunks


def _split_blank_blocks(text: str) -> list:
    """Split on blank lines only -- the coarse, standard markdown block boundary."""
    blocks = []
    cur = []
    for line in text.split("\n"):
        if line.strip() == "":
            if cur:
                blocks.append(cur)
                cur = []
        else:
            cur.append(line)
    if cur:
        blocks.append(cur)
    return blocks


def _parse_section_body(text: str):
    """-> [{'type':'prose','lines':[...]}, {'type':'step','label':'A1','lines':[...]}, ...]

    Two passes, deliberately not a single per-line scan. A blank-line-delimited block is a
    "steps run" iff its FIRST line matches the step-label pattern; only then do we re-split
    it at each subsequent column-0 label. This is what stops prose asides like "E4 is
    emulator-blind..." or "D4's gate change..." (real sentences inside a "WHY..."/"MEASURED,
    NOT ASSUMED..." paragraph, starting with a letter+digit purely by coincidence, with no
    blank line to separate them from the paragraph's first line) from being mis-read as new
    steps: that paragraph's block starts with "WHY..."/"MEASURED...", which never matches, so
    the whole block stays one prose entry no matter what its later lines start with. A
    per-line scan (checking every line in isolation) gets this wrong -- caught by testing
    section K, whose own review-round cross-references ("D4", "E4") are exactly this trap.
    """
    result = []
    for raw_block in _split_blank_blocks(text):
        first = raw_block[0]
        if not first[:1].isspace() and _STEP_RE.match(first):
            cur = None
            for line in raw_block:
                indented = line[:1].isspace()
                m = None if indented else _STEP_RE.match(line)
                if not indented and m:
                    if cur is not None:
                        result.append(cur)
                    cur = {"type": "step", "label": m.group(1), "lines": [line]}
                elif cur is not None:
                    cur["lines"].append(line)
                else:  # pragma: no cover -- a block's first line always matches here
                    cur = {"type": "step", "label": m.group(1) if m else "?", "lines": [line]}
            if cur is not None:
                result.append(cur)
        else:
            result.append({"type": "prose", "lines": list(raw_block)})
    return result


def _strip_label(text: str, label: str) -> str:
    return re.sub(r"^" + re.escape(label) + r"[.\s(]?\s*", "", text, count=1)


def _render_section_body(blocks: list) -> str:
    parts = []
    i, n = 0, len(blocks)
    while i < n:
        if blocks[i]["type"] == "step":
            items = []
            while i < n and blocks[i]["type"] == "step":
                label = blocks[i]["label"]
                joined = " ".join(s.strip() for s in blocks[i]["lines"])
                text = _strip_label(joined, label)
                items.append(
                    '<li id="hw-%s"><label><input type="checkbox" class="hwstep-cb" '
                    'data-step="%s"> <b>%s.</b> %s</label></li>'
                    % (html.escape(label, quote=True), html.escape(label, quote=True),
                       html.escape(label), md_inline(text))
                )
                i += 1
            parts.append('<ul class="hwsteps">' + "".join(items) + "</ul>")
        else:
            joined = " ".join(s.strip() for s in blocks[i]["lines"])
            parts.append(md_block_html(joined))
            i += 1
    return "".join(parts)


def hwtest_anchor_ids(path: "pathlib.Path") -> set:
    """Every id="hw-..." render_hwtest() will emit, computed BEFORE the queue table renders.

    The queue's Steps column links to these. Four refs (N, O, P, Q) used to point at
    sections nobody had written, so those buttons scrolled nowhere; the table now only
    links a ref that really has a target.
    """
    ids = set()
    if not path.is_file():
        return ids
    for header, body in _split_hw_sections(path.read_text(encoding="utf-8")):
        if header:
            m = _LETTERED_RE.match(header.strip())
            if m:
                ids.add(m.group(1))
        for line in body:
            m = _STEP_RE.match(line.strip())
            if m:
                ids.add(m.group(1))
    return ids


HWTEST_ANCHORS = hwtest_anchor_ids(HWTEST)

def render_hwtest(path: "pathlib.Path") -> str:
    if not path.is_file():
        print("note: %s missing, skipping the detailed hardware-steps section" % path, file=sys.stderr)
        return ""
    raw = path.read_text(encoding="utf-8")
    chunks = _split_hw_sections(raw)

    parts = ['<section class="hwtest"><h2 id="hwtest">Detailed hardware steps</h2>']
    _, preamble_lines = chunks[0]
    preamble_text = "\n".join(preamble_lines).strip("\n")
    if preamble_text.strip():
        parts.append('<div class="hwtest-intro">'
                      + md_block_html(demote_headings(preamble_text, extra=2)) + "</div>")

    for header, body_lines in chunks[1:]:
        body_text = "\n".join(body_lines)
        blocks = _parse_section_body(body_text)
        inner = _render_section_body(blocks)
        m = _LETTERED_RE.match(header or "")
        if m:
            letter, title = m.group(1), m.group(2)
            parts.append(
                '<details id="hw-%s"><summary>%s. %s</summary><div class="hwsec-body">%s</div>'
                "</details>" % (letter, letter, html.escape(title), inner)
            )
        else:
            parts.append("<h3>%s</h3>%s" % (html.escape(header or ""), inner))
    parts.append("</section>")
    return "".join(parts)


def main() -> int:
    if not SRC.exists():
        print("missing " + str(SRC), file=sys.stderr)
        return 1
    body = markdown.markdown(SRC.read_text(encoding="utf-8"), extensions=MD_EXTENSIONS)
    # Scope the global filter bar to the feature-matrix's own tables only -- the new
    # hardware-queue table (below) manages its own PASS/FAIL/SKIP filtering independently
    # and must not be hidden/shown by the top toolbar's badge/text filters.
    body = body.replace("<table>", '<table class="matrix">')

    # <code>HW-PASS</code> -> a coloured badge, so the status column reads at a glance.
    def badge(m: "re.Match[str]") -> str:
        raw = html.unescape(m.group(1))
        if raw not in TOKENS:
            return m.group(0)
        return badge_span(raw)

    body = re.sub(r"<code>([^<]+)</code>", badge, body)

    counts = {t: body.count('badge-' + c[0] + '"') for t, c in TOKENS.items()}
    buttons = "\n  ".join(
        '<button data-tok="%s"><span class="badge badge-%s">%s</span> '
        '<span class="n">%d</span></button>' % (c[0], c[0], t, counts[t])
        for t, c in TOKENS.items()
    )
    badgecss = "\n  ".join(
        ".badge-%s { color:%s; border-color:%s55; background:%s14; }" % (c[0], c[1], c[1], c[1])
        for c in TOKENS.values()
    )

    def figure(path: pathlib.Path, caption: str) -> str:
        """Link the sheet instead of inlining it.

        Inlining 19 contact sheets as base64 made the page 3.1 MB, which is most of what
        made it feel stale and heavy. The sheets are copied next to the page (docs/site/
        shots/) and referenced by URL, so the page is ~100 KB and the images still open.
        """
        dest_dir = ROOT / "docs" / "site" / "shots"
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / path.name
        try:
            if (not dest.exists()) or dest.stat().st_mtime < path.stat().st_mtime:
                shutil.copy2(path, dest)
        except OSError:
            return ""
        age = (datetime.date.today() - datetime.date.fromtimestamp(path.stat().st_mtime)).days
        stale = (' <span style="color:#d29922">— %d days old, predates the current build</span>' % age) if age > 2 else ""
        return (
            '<figure style="margin:18px 0"><a href="shots/%s"><img src="shots/%s" loading="lazy" '
            'style="max-width:100%%;border:1px solid #21262d;border-radius:6px" alt="%s"></a>'
            '<figcaption style="color:#8b949e;font-size:13px;margin-top:6px">%s%s</figcaption></figure>'
            % (html.escape(path.name), html.escape(path.name),
               html.escape(caption), html.escape(caption), stale))

    per_feature_html = ""
    jump_list_html = ""
    if CONTACT_SHEETS_INDEX.is_file():
        features = json.loads(CONTACT_SHEETS_INDEX.read_text(encoding="utf-8"))
        sections = []
        jumps = []
        for entry in features:
            sheet_path = ROOT / entry["file"]
            if not sheet_path.exists():
                continue
            jumps.append('<a href="#shots-%s">%s</a>' % (
                html.escape(entry["id"]), html.escape(entry["title"])))
            sections.append(
                '<h3 id="shots-%s">%s <span style="color:#6e7681;font-weight:400">'
                '(%d shot%s, updated %s)</span></h3>%s'
                % (html.escape(entry["id"]), html.escape(entry["title"]),
                   entry["count"], "" if entry["count"] == 1 else "s",
                   html.escape(entry["updated"]),
                   figure(sheet_path, entry["title"])))
        if sections:
            per_feature_html = "".join(sections)
            jump_list_html = (
                '<p style="color:#8b949e;font-size:13px">Jump to feature: '
                + " &middot; ".join(jumps) + "</p>")

    older_shots = []
    for path, caption in SHOTS:
        if not path.exists():
            continue
        older_shots.append(figure(path, caption))

    shots_html = ""
    if per_feature_html or older_shots:
        # Say how old the sheets are instead of implying they are current. Every sheet on
        # this page is a picture of a build that is not the one linked at the top, and the
        # page used to present them with no date at all.
        newest = 0
        try:
            for entry in json.loads(CONTACT_SHEETS_INDEX.read_text(encoding="utf-8")):
                sp = ROOT / entry["file"]
                if sp.exists():
                    newest = max(newest, sp.stat().st_mtime)
        except Exception:
            newest = 0
        age_note = ""
        if newest:
            d = datetime.date.fromtimestamp(newest)
            days = (datetime.date.today() - d).days
            age_note = (
                '<p style="color:#d29922;font-size:13px">These sheets were captured on '
                '<b>%s</b>%s. They are kept because they are still the clearest picture of '
                'those features, not because they match the build above — re-run the chains '
                'to refresh them.</p>' % (d.isoformat(), ", %d days ago" % days if days else ""))
        shots_html = '<h2 id="shots">Screenshots</h2>' + age_note + jump_list_html + per_feature_html
        if older_shots:
            # Collapsed: these are historical and were dominating the page.
            shots_html += (
                '<details style="margin-top:18px"><summary style="cursor:pointer;color:#8b949e">'
                'Older sheets (%d, archived — 2026-08-08 to 2026-09-05)</summary>%s</details>'
                % (len(older_shots), "".join(older_shots)))

    hwqueue_html, hw_build, hw_build_date = render_hwqueue(HWQ)
    hwtest_html = render_hwtest(HWTEST)

    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    page = (TEMPLATE.replace("BADGECSS", badgecss)
                    .replace("BUTTONS", buttons)
                    .replace("HWQUEUE_HTML", hwqueue_html)
                    .replace("BODY", body)
                    .replace("HWTEST_HTML", hwtest_html)
                    .replace("SHOTS_HTML", shots_html)
                    .replace("HW_BUILD_JS", json.dumps(hw_build))
                    .replace("HW_BUILD_DATE_JS", json.dumps(hw_build_date))
                    .replace("</h1>", "</h1><p style=\"color:#8b949e;margin:0 0 18px\">Page generated " + stamp + "</p>", 1))
    OUT.write_text(page, encoding="utf-8")
    SITE.parent.mkdir(parents=True, exist_ok=True)
    SITE.write_text(page, encoding="utf-8")
    print("%s  (%d bytes)" % (OUT, OUT.stat().st_size))
    print("badges: " + ", ".join("%s=%d" % (t, n) for t, n in counts.items()))
    print("hw-queue build: %s (%s)" % (hw_build, hw_build_date))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
