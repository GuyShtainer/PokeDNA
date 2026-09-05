#!/usr/bin/env python3
"""Render docs/FEATURE-MATRIX.md as a single self-contained HTML page.

Quick viewer, not a build step: run it again after editing the markdown.
    python3 tools/gen_matrix_html.py && open docs/feature-matrix.html
"""
import base64
import datetime
import html
import json
import pathlib
import re
import sys

import markdown

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "FEATURE-MATRIX.md"
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
}

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
</style>
</head><body>
<div class="toolbar" id="bar">
  <button data-tok="" class="on">All</button>
  BUTTONS
  <input id="q" type="search" placeholder="filter rows by text...">
</div>
<div class="wrap">
BODY
SHOTS_HTML
<p class="foot">Generated from <code>docs/FEATURE-MATRIX.md</code> by
<code>tools/gen_matrix_html.py</code>. Edit the markdown, re-run the script.</p>
</div>
<script>
var bar = document.getElementById('bar'), q = document.getElementById('q'), tok = '';
function apply() {
  var text = q.value.trim().toLowerCase();
  document.querySelectorAll('tbody tr').forEach(function (tr) {
    var t = tr.textContent;
    var okTok = !tok || (tr.querySelector('.badge-' + tok) !== null);
    var okTxt = !text || t.toLowerCase().indexOf(text) !== -1;
    tr.classList.toggle('hidden', !(okTok && okTxt));
  });
  document.querySelectorAll('table').forEach(function (tb) {
    var any = tb.querySelectorAll('tbody tr:not(.hidden)').length;
    tb.classList.toggle('hidden', any === 0);
  });
}
bar.addEventListener('click', function (e) {
  if (e.target.tagName !== 'BUTTON') return;
  tok = e.target.dataset.tok;
  bar.querySelectorAll('button').forEach(function (b) { b.classList.toggle('on', b === e.target); });
  apply();
});
q.addEventListener('input', apply);
</script>
</body></html>
"""


def main() -> int:
    if not SRC.exists():
        print("missing " + str(SRC), file=sys.stderr)
        return 1
    body = markdown.markdown(
        SRC.read_text(encoding="utf-8"),
        extensions=["tables", "sane_lists", "attr_list"],
    )

    # <code>HW-PASS</code> -> a coloured badge, so the status column reads at a glance.
    def badge(m: "re.Match[str]") -> str:
        raw = html.unescape(m.group(1))
        cls = TOKENS.get(raw)
        if not cls:
            return m.group(0)
        return '<span class="badge badge-%s">%s</span>' % (cls[0], html.escape(raw))

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
        data = base64.b64encode(path.read_bytes()).decode("ascii")
        return (
            '<figure style="margin:18px 0"><img src="data:image/png;base64,%s" '
            'style="max-width:100%%;border:1px solid #21262d;border-radius:6px" alt="%s">'
            '<figcaption style="color:#8b949e;font-size:13px;margin-top:6px">%s</figcaption></figure>'
            % (data, html.escape(caption), html.escape(caption)))

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
        shots_html = '<h2 id="shots">Screenshots</h2>' + jump_list_html + per_feature_html
        if older_shots:
            if per_feature_html:
                shots_html += '<h3>Older sheets</h3>'
            shots_html += "".join(older_shots)
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    page = (TEMPLATE.replace("BADGECSS", badgecss)
                    .replace("BUTTONS", buttons)
                    .replace("BODY", body)
                    .replace("SHOTS_HTML", shots_html)
                    .replace("</h1>", "</h1><p style=\"color:#8b949e;margin:0 0 18px\">Page generated " + stamp + "</p>", 1))
    OUT.write_text(page, encoding="utf-8")
    SITE.parent.mkdir(parents=True, exist_ok=True)
    SITE.write_text(page, encoding="utf-8")
    print("%s  (%d bytes)" % (OUT, OUT.stat().st_size))
    print("badges: " + ", ".join("%s=%d" % (t, n) for t, n in counts.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
